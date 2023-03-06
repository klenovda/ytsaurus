#include "scheduling_segment_manager.h"
#include "private.h"
#include "persistent_fair_share_tree_job_scheduler_state.h"
#include "fair_share_tree_snapshot.h"

#include <util/generic/algorithm.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NLogging;
using namespace NNodeTrackerClient;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static constexpr int LargeGpuSegmentJobGpuDemand = 8;

////////////////////////////////////////////////////////////////////////////////

namespace {

double GetNodeResourceLimit(const TFairShareTreeJobSchedulerNodeState& node, EJobResourceType resourceType)
{
    return node.Descriptor->Online
        ? GetResource(node.Descriptor->ResourceLimits, resourceType)
        : 0.0;
}

EJobResourceType GetSegmentBalancingKeyResource(ESegmentedSchedulingMode mode)
{
    switch (mode) {
        case ESegmentedSchedulingMode::LargeGpu:
            return EJobResourceType::Gpu;
        default:
            YT_ABORT();
    }
}

TNodeMovePenalty GetMovePenaltyForNode(
    const TFairShareTreeJobSchedulerNodeState& node,
    ESegmentedSchedulingMode mode)
{
    auto keyResource = GetSegmentBalancingKeyResource(mode);
    switch (keyResource) {
        case EJobResourceType::Gpu:
            return TNodeMovePenalty{
                .PriorityPenalty = node.RunningJobStatistics.TotalGpuTime -
                    node.RunningJobStatistics.PreemptibleGpuTime,
                .RegularPenalty = node.RunningJobStatistics.TotalGpuTime
            };
        default:
            return TNodeMovePenalty{
                .PriorityPenalty = node.RunningJobStatistics.TotalCpuTime -
                    node.RunningJobStatistics.PreemptibleCpuTime,
                .RegularPenalty = node.RunningJobStatistics.TotalCpuTime
            };
    }
}

void SortByPenalty(TNodeWithMovePenaltyList& nodeWithPenaltyList)
{
    SortBy(nodeWithPenaltyList, [] (const TNodeWithMovePenalty& node) { return node.MovePenalty; });
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

bool operator <(const TNodeMovePenalty& lhs, const TNodeMovePenalty& rhs)
{
    if (lhs.PriorityPenalty != rhs.PriorityPenalty) {
        return lhs.PriorityPenalty < rhs.PriorityPenalty;
    }
    return lhs.RegularPenalty < rhs.RegularPenalty;
}

TNodeMovePenalty& operator +=(TNodeMovePenalty& lhs, const TNodeMovePenalty& rhs)
{
    lhs.PriorityPenalty += rhs.PriorityPenalty;
    lhs.RegularPenalty += rhs.RegularPenalty;
    return lhs;
}

void FormatValue(TStringBuilderBase* builder, const TNodeMovePenalty& penalty, TStringBuf /*format*/)
{
    builder->AppendFormat("{PriorityPenalty: %.3f, RegularPenalty: %.3f}",
        penalty.PriorityPenalty,
        penalty.RegularPenalty);
}

TString ToString(const TNodeMovePenalty& penalty)
{
    return ToStringViaBuilder(penalty);
}

////////////////////////////////////////////////////////////////////////////////

const TSchedulingSegmentModule& TSchedulingSegmentManager::GetNodeModule(
    const std::optional<TString>& nodeDataCenter,
    const std::optional<TString>& nodeInfinibandCluster,
    ESchedulingSegmentModuleType moduleType)
{
    switch (moduleType) {
        case ESchedulingSegmentModuleType::DataCenter:
            return nodeDataCenter;
        case ESchedulingSegmentModuleType::InfinibandCluster:
            return nodeInfinibandCluster;
        default:
            YT_ABORT();
    }
}

const TSchedulingSegmentModule& TSchedulingSegmentManager::GetNodeModule(
    const TExecNodeDescriptor& nodeDescriptor,
    ESchedulingSegmentModuleType moduleType)
{
    return GetNodeModule(nodeDescriptor.DataCenter, nodeDescriptor.InfinibandCluster, moduleType);
}

TString TSchedulingSegmentManager::GetNodeTagFromModuleName(const TString& moduleName, ESchedulingSegmentModuleType moduleType)
{
    switch (moduleType) {
        case ESchedulingSegmentModuleType::DataCenter:
            return moduleName;
        case ESchedulingSegmentModuleType::InfinibandCluster:
            return Format("%v:%v", InfinibandClusterNameKey, moduleName);
        default:
            YT_ABORT();
    }
}

TSchedulingSegmentManager::TSchedulingSegmentManager(
    TString treeId,
    TFairShareStrategySchedulingSegmentsConfigPtr config,
    NLogging::TLogger logger,
    const NProfiling::TProfiler& profiler)
    : TreeId_(std::move(treeId))
    , Logger(std::move(logger))
    , Config_(std::move(config))
    , SensorProducer_(New<TBufferedProducer>())
{
    profiler.AddProducer("/segments", SensorProducer_);
}

void TSchedulingSegmentManager::UpdateSchedulingSegments(TUpdateSchedulingSegmentsContext* context)
{
    if (Config_->Mode != ESegmentedSchedulingMode::Disabled) {
        DoUpdateSchedulingSegments(context);
    } else if (PreviousMode_ != ESegmentedSchedulingMode::Disabled) {
        Reset(context);
    }

    PreviousMode_ = Config_->Mode;

    LogAndProfileSegments(context);
    BuildPersistentState(context);
}

void TSchedulingSegmentManager::InitOrUpdateOperationSchedulingSegment(
    const TFairShareTreeJobSchedulerOperationStatePtr& operationState) const
{
    YT_VERIFY(operationState->InitialAggregatedMinNeededResources.has_value());

    auto segment = [&] {
        if (auto specifiedSegment = operationState->Spec->SchedulingSegment) {
            return *specifiedSegment;
        }

        switch (Config_->Mode) {
            case ESegmentedSchedulingMode::LargeGpu: {
                bool meetsGangCriterion = operationState->IsGang || !Config_->AllowOnlyGangOperationsInLargeSegment;
                auto jobGpuDemand = operationState->InitialAggregatedMinNeededResources->GetGpu();
                bool meetsJobGpuDemandCriterion = (jobGpuDemand == LargeGpuSegmentJobGpuDemand);
                return meetsGangCriterion && meetsJobGpuDemandCriterion
                    ? ESchedulingSegment::LargeGpu
                    : ESchedulingSegment::Default;
            }
            default:
                return ESchedulingSegment::Default;
        }
    }();

    if (operationState->SchedulingSegment != segment) {
        YT_LOG_DEBUG(
            "Setting new scheduling segment for operation ("
            "Segment: %v, Mode: %v, AllowOnlyGangOperationsInLargeSegment: %v, IsGang: %v, "
            "InitialMinNeededResources: %v, SpecifiedSegment: %v)",
            segment,
            Config_->Mode,
            Config_->AllowOnlyGangOperationsInLargeSegment,
            operationState->IsGang,
            operationState->InitialAggregatedMinNeededResources,
            operationState->Spec->SchedulingSegment);

        operationState->SchedulingSegment = segment;
        operationState->SpecifiedSchedulingSegmentModules = operationState->Spec->SchedulingSegmentModules;
        if (!IsModuleAwareSchedulingSegment(segment)) {
            operationState->SchedulingSegmentModule.reset();
        }
    }
}

void TSchedulingSegmentManager::UpdateConfig(TFairShareStrategySchedulingSegmentsConfigPtr config)
{
    Config_ = std::move(config);
}

void TSchedulingSegmentManager::DoUpdateSchedulingSegments(TUpdateSchedulingSegmentsContext* context)
{
    // Process operations.
    CollectCurrentResourceAmountPerSegment(context);
    ResetOperationModuleAssignments(context);
    CollectFairResourceAmountPerSegment(context);
    AssignOperationsToModules(context);
    AdjustFairResourceAmountBySatisfactionMargins(context);

    // Process nodes.
    ValidateInfinibandClusterTags(context);
    ApplySpecifiedSegments(context);
    CheckAndRebalanceSegments(context);
}

void TSchedulingSegmentManager::Reset(TUpdateSchedulingSegmentsContext* context)
{
    PreviousMode_ = ESegmentedSchedulingMode::Disabled;
    UnsatisfiedSince_.reset();

    for (auto& [_, operation] : context->OperationStates) {
        operation->SchedulingSegmentModule.reset();
        operation->FailingToScheduleAtModuleSince.reset();
    }
    for (auto& [_, node] : context->NodeStates) {
        SetNodeSegment(&node, ESchedulingSegment::Default, context);
    }
}

void TSchedulingSegmentManager::CollectCurrentResourceAmountPerSegment(TUpdateSchedulingSegmentsContext* context) const
{
    auto keyResource = GetSegmentBalancingKeyResource(Config_->Mode);
    for (const auto& [_, node] : context->NodeStates) {
        if (!node.Descriptor) {
            continue;
        }

        auto nodeKeyResourceLimit = GetNodeResourceLimit(node, keyResource);
        const auto& nodeModule = GetNodeModule(node);
        auto& currentResourceAmount = IsModuleAwareSchedulingSegment(node.SchedulingSegment)
            ? context->CurrentResourceAmountPerSegment.At(node.SchedulingSegment).MutableAt(nodeModule)
            : context->CurrentResourceAmountPerSegment.At(node.SchedulingSegment).Mutable();
        currentResourceAmount += nodeKeyResourceLimit;
        context->TotalCapacityPerModule[nodeModule] += nodeKeyResourceLimit;
        context->NodesTotalKeyResourceLimit += nodeKeyResourceLimit;
    }

    context->RemainingCapacityPerModule = context->TotalCapacityPerModule;
    auto snapshotTotalKeyResourceLimit = GetResource(context->TreeSnapshot->ResourceLimits(), keyResource);

    YT_LOG_WARNING_IF(context->NodesTotalKeyResourceLimit != snapshotTotalKeyResourceLimit,
        "Total key resource limit from node states differs from the tree snapshot, "
        "operation scheduling segments distribution might be unfair "
        "(NodesTotalKeyResourceLimit: %v, SnapshotTotalKeyResourceLimit: %v, KeyResource: %v)",
        context->NodesTotalKeyResourceLimit,
        snapshotTotalKeyResourceLimit,
        keyResource);
}

void TSchedulingSegmentManager::ResetOperationModuleAssignments(TUpdateSchedulingSegmentsContext* context) const
{
    for (const auto& [operationId, operation] : context->OperationStates) {
        auto* element = context->TreeSnapshot->FindEnabledOperationElement(operationId);
        if (!element) {
            continue;
        }

        const auto& segment = operation->SchedulingSegment;
        if (!segment || !IsModuleAwareSchedulingSegment(*segment)) {
            // Segment may be unset due to a race, and in this case we silently ignore the operation.
            continue;
        }

        auto& schedulingSegmentModule = operation->SchedulingSegmentModule;
        if (!schedulingSegmentModule) {
            continue;
        }

        if (element->ResourceUsageAtUpdate() != element->ResourceDemand()) {
            if (!operation->FailingToScheduleAtModuleSince) {
                operation->FailingToScheduleAtModuleSince = context->Now;
            }

            if (*operation->FailingToScheduleAtModuleSince + Config_->ModuleReconsiderationTimeout < context->Now) {
                YT_LOG_DEBUG(
                    "Operation has failed to schedule all jobs for too long, revoking its module assignment "
                    "(OperationId: %v, SchedulingSegment: %v, PreviousModule: %v, ResourceUsage: %v, ResourceDemand: %v, Timeout: %v)",
                    operationId,
                    segment,
                    schedulingSegmentModule,
                    element->ResourceUsageAtUpdate(),
                    element->ResourceDemand(),
                    Config_->ModuleReconsiderationTimeout);

                // NB: We will abort all jobs that are running in the wrong module.
                schedulingSegmentModule.reset();
                operation->FailingToScheduleAtModuleSince.reset();
            }
        } else {
            operation->FailingToScheduleAtModuleSince.reset();
        }
    }
}

void TSchedulingSegmentManager::CollectFairResourceAmountPerSegment(TUpdateSchedulingSegmentsContext* context) const
{
    auto keyResource = GetSegmentBalancingKeyResource(Config_->Mode);
    for (auto& [operationId, operation] : context->OperationStates) {
        auto* element = context->TreeSnapshot->FindEnabledOperationElement(operationId);
        if (!element) {
            continue;
        }

        const auto& segment = operation->SchedulingSegment;
        if (!segment) {
            // Segment may be unset due to a race, and in this case we silently ignore the operation.
            continue;
        }

        auto& fairShareAtSegment = context->FairSharePerSegment.At(*segment);
        if (IsModuleAwareSchedulingSegment(*segment)) {
            fairShareAtSegment.MutableAt(operation->SchedulingSegmentModule) += element->Attributes().FairShare.Total[keyResource];
        } else {
            fairShareAtSegment.Mutable() += element->Attributes().FairShare.Total[keyResource];
        }
    }

    auto snapshotTotalKeyResourceLimit = GetResource(context->TreeSnapshot->ResourceLimits(), keyResource);
    for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
        if (IsModuleAwareSchedulingSegment(segment)) {
            for (const auto& schedulingSegmentModule : Config_->GetModules()) {
                auto fairResourceAmount =
                    context->FairSharePerSegment.At(segment).GetOrDefaultAt(schedulingSegmentModule) * snapshotTotalKeyResourceLimit;
                context->FairResourceAmountPerSegment.At(segment).SetAt(schedulingSegmentModule, fairResourceAmount);
                context->RemainingCapacityPerModule[schedulingSegmentModule] -= fairResourceAmount;
            }
        } else {
            auto fairResourceAmount = context->FairSharePerSegment.At(segment).GetOrDefault() * snapshotTotalKeyResourceLimit;
            context->FairResourceAmountPerSegment.At(segment).Set(fairResourceAmount);
        }
    }
}

void TSchedulingSegmentManager::AssignOperationsToModules(TUpdateSchedulingSegmentsContext* context) const
{
    auto keyResource = GetSegmentBalancingKeyResource(Config_->Mode);

    struct TOperationStateWithElement
    {
        TOperationId OperationId;
        TFairShareTreeJobSchedulerOperationState* Operation;
        TSchedulerOperationElement* Element;
    };
    std::vector<TOperationStateWithElement> operationsToAssignToModule;
    operationsToAssignToModule.reserve(context->OperationStates.size());
    for (auto& [operationId, operation] : context->OperationStates) {
        auto* element = context->TreeSnapshot->FindEnabledOperationElement(operationId);
        if (!element) {
            continue;
        }

        const auto& segment = operation->SchedulingSegment;
        if (!segment || !IsModuleAwareSchedulingSegment(*segment)) {
            continue;
        }

        // NB(eshcherbin): Demand could be zero, because needed resources update is asynchronous.
        if (element->ResourceDemand() == TJobResources()) {
            continue;
        }

        bool demandFullySatisfied = Dominates(element->Attributes().FairShare.Total + TResourceVector::Epsilon(), element->Attributes().DemandShare);
        if (operation->SchedulingSegmentModule || !demandFullySatisfied) {
            continue;
        }

        operationsToAssignToModule.push_back(TOperationStateWithElement{
            .OperationId = operationId,
            .Operation = operation.Get(),
            .Element = element,
        });
    }

    std::sort(
        operationsToAssignToModule.begin(),
        operationsToAssignToModule.end(),
        [keyResource] (const TOperationStateWithElement& lhs, const TOperationStateWithElement& rhs) {
            auto lhsSpecifiedModuleCount = lhs.Operation->SpecifiedSchedulingSegmentModules
                ? lhs.Operation->SpecifiedSchedulingSegmentModules->size()
                : 0;
            auto rhsSpecifiedModuleCount = rhs.Operation->SpecifiedSchedulingSegmentModules
                ? rhs.Operation->SpecifiedSchedulingSegmentModules->size()
                : 0;
            if (lhsSpecifiedModuleCount != rhsSpecifiedModuleCount) {
                return lhsSpecifiedModuleCount < rhsSpecifiedModuleCount;
            }

            return GetResource(rhs.Element->ResourceDemand(), keyResource) <
                GetResource(lhs.Element->ResourceDemand(), keyResource);
        });

    for (const auto& [operationId, operation, element] : operationsToAssignToModule) {
        const auto& segment = operation->SchedulingSegment;
        auto operationDemand = GetResource(element->ResourceDemand(), keyResource);

        std::function<bool(double, double)> isModuleBetter;
        double initialBestRemainingCapacity;
        switch (Config_->ModuleAssignmentHeuristic) {
            case ESchedulingSegmentModuleAssignmentHeuristic::MaxRemainingCapacity:
                isModuleBetter = [] (double remainingCapacity, double bestRemainingCapacity) {
                    return bestRemainingCapacity < remainingCapacity;
                };
                initialBestRemainingCapacity = std::numeric_limits<double>::lowest();
                break;

            case ESchedulingSegmentModuleAssignmentHeuristic::MinRemainingFeasibleCapacity:
                isModuleBetter = [operationDemand] (double remainingCapacity, double bestRemainingCapacity) {
                    return remainingCapacity >= operationDemand && bestRemainingCapacity > remainingCapacity;
                };
                initialBestRemainingCapacity = std::numeric_limits<double>::max();
                break;

            default:
                YT_ABORT();
        }

        TSchedulingSegmentModule bestModule;
        auto bestRemainingCapacity = initialBestRemainingCapacity;
        const auto& specifiedModules = operation->SpecifiedSchedulingSegmentModules;
        for (const auto& schedulingSegmentModule : Config_->GetModules()) {
            auto it = context->RemainingCapacityPerModule.find(schedulingSegmentModule);
            auto remainingCapacity = it != context->RemainingCapacityPerModule.end() ? it->second : 0.0;

            if (specifiedModules && !specifiedModules->contains(schedulingSegmentModule)) {
                continue;
            }

            if (isModuleBetter(remainingCapacity, bestRemainingCapacity)) {
                bestModule = schedulingSegmentModule;
                bestRemainingCapacity = remainingCapacity;
            }
        }

        if (!bestModule) {
            YT_LOG_INFO(
                "Failed to find a suitable module for operation "
                "(AvailableModules: %v, SpecifiedModules: %v, OperationDemand: %v, "
                "RemainingCapacityPerModule: %v, TotalCapacityPerModule: %v, OperationId: %v)",
                Config_->GetModules(),
                operation->SpecifiedSchedulingSegmentModules,
                operationDemand,
                context->RemainingCapacityPerModule,
                context->TotalCapacityPerModule,
                operationId);

            continue;
        }

        auto operationFairShare = element->Attributes().FairShare.Total[keyResource];
        context->FairSharePerSegment.At(*segment).MutableAt(operation->SchedulingSegmentModule) -= operationFairShare;
        operation->SchedulingSegmentModule = bestModule;
        context->FairSharePerSegment.At(*segment).MutableAt(operation->SchedulingSegmentModule) += operationFairShare;

        context->FairResourceAmountPerSegment.At(*segment).MutableAt(operation->SchedulingSegmentModule) += operationDemand;
        context->RemainingCapacityPerModule[operation->SchedulingSegmentModule] -= operationDemand;

        YT_LOG_DEBUG(
            "Assigned operation to a new scheduling segment module "
            "(SchedulingSegment: %v, Module: %v, SpecifiedModules: %v, "
            "OperationDemand: %v, RemainingCapacityPerModule: %v, TotalCapacityPerModule: %v, "
            "OperationId: %v)",
            segment,
            operation->SchedulingSegmentModule,
            operation->SpecifiedSchedulingSegmentModules,
            operationDemand,
            context->RemainingCapacityPerModule,
            context->TotalCapacityPerModule,
            operationId);
    }
}

void TSchedulingSegmentManager::AdjustFairResourceAmountBySatisfactionMargins(TUpdateSchedulingSegmentsContext* context) const
{
    for (auto segment: TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
        if (IsModuleAwareSchedulingSegment(segment)) {
            for (const auto& schedulingSegmentModule : Config_->GetModules()) {
                auto satisfactionMargin = Config_->SatisfactionMargins.At(segment).GetOrDefaultAt(schedulingSegmentModule);
                auto& value = context->FairResourceAmountPerSegment.At(segment).MutableAt(schedulingSegmentModule);
                value = std::max(value + satisfactionMargin, 0.0);
            }
        } else {
            auto satisfactionMargin = Config_->SatisfactionMargins.At(segment).GetOrDefault();
            auto& value = context->FairResourceAmountPerSegment.At(segment).Mutable();
            value = std::max(value + satisfactionMargin, 0.0);
        }
    }
}

void TSchedulingSegmentManager::ValidateInfinibandClusterTags(TUpdateSchedulingSegmentsContext* context) const
{
    static const TString InfinibandClusterTagPrefix = InfinibandClusterNameKey + ":";

    if (!Config_->EnableInfinibandClusterTagValidation) {
        return;
    }

    auto validateNodeDescriptor = [&] (const TExecNodeDescriptor& node) -> TError {
        if (!node.InfinibandCluster || !Config_->InfinibandClusters.contains(*node.InfinibandCluster)) {
            return TError("Node's infiniband cluster is invalid or missing")
                << TErrorAttribute("node_infiniband_cluster", node.InfinibandCluster)
                << TErrorAttribute("configured_infiniband_clusters", Config_->InfinibandClusters);
        }

        std::vector<TString> infinibandClusterTags;
        for (const auto& tag : node.Tags.GetSourceTags()) {
            if (tag.StartsWith(InfinibandClusterTagPrefix)) {
                infinibandClusterTags.push_back(tag);
            }
        }

        if (infinibandClusterTags.empty()) {
            return TError("Node has no infiniband cluster tags");
        }

        if (std::ssize(infinibandClusterTags) > 1) {
            return TError("Node has more than one infiniband cluster tags")
                << TErrorAttribute("infiniband_cluster_tags", infinibandClusterTags);
        }

        const auto& tag = infinibandClusterTags[0];
        if (tag != TSchedulingSegmentManager::GetNodeTagFromModuleName(*node.InfinibandCluster, ESchedulingSegmentModuleType::InfinibandCluster)) {
            return TError("Node's infiniband cluster tag doesn't match its infiniband cluster from annotations")
                << TErrorAttribute("infiniband_cluster", node.InfinibandCluster)
                << TErrorAttribute("infiniband_cluster_tag", tag);
        }

        return {};
    };

    for (const auto& [nodeId, node] : context->NodeStates) {
        auto error = validateNodeDescriptor(*node.Descriptor);
        if (!error.IsOK()) {
            error = error << TErrorAttribute("node_address", node.Descriptor->Address);
            context->Error = TError("Node's infiniband cluster tags validation failed in tree %Qv", TreeId_)
                << std::move(error);
            break;
        }
    }
}

void TSchedulingSegmentManager::ApplySpecifiedSegments(TUpdateSchedulingSegmentsContext* context) const
{
    for (auto& [nodeId, node] : context->NodeStates) {
        if (auto segment = node.SpecifiedSchedulingSegment) {
            SetNodeSegment(&node, *segment, context);
        }
    }
}

void TSchedulingSegmentManager::CheckAndRebalanceSegments(TUpdateSchedulingSegmentsContext* context)
{
    auto [isSegmentUnsatisfied, hasUnsatisfiedSegment] = FindUnsatisfiedSegments(context);
    if (hasUnsatisfiedSegment) {
        if (!UnsatisfiedSince_) {
            UnsatisfiedSince_ = context->Now;
        }

        YT_LOG_DEBUG(
            "Found unsatisfied scheduling segments in tree "
            "(IsSegmentUnsatisfied: %v, UnsatisfiedFor: %v, Timeout: %v, InitializationDeadline: %v)",
            isSegmentUnsatisfied,
            context->Now - *UnsatisfiedSince_,
            Config_->UnsatisfiedSegmentsRebalancingTimeout,
            InitializationDeadline_);

        auto deadline = std::max(
            *UnsatisfiedSince_ + Config_->UnsatisfiedSegmentsRebalancingTimeout,
            InitializationDeadline_);
        if (context->Now > deadline) {
            DoRebalanceSegments(context);
            UnsatisfiedSince_.reset();
        }
    } else {
        UnsatisfiedSince_.reset();
    }
}

std::pair<TSchedulingSegmentMap<bool>, bool> TSchedulingSegmentManager::FindUnsatisfiedSegments(const TUpdateSchedulingSegmentsContext* context) const
{
    TSchedulingSegmentMap<bool> isSegmentUnsatisfied;
    bool hasUnsatisfiedSegment = false;
    for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
        const auto& fairResourceAmount = context->FairResourceAmountPerSegment.At(segment);
        const auto& currentResourceAmount = context->CurrentResourceAmountPerSegment.At(segment);
        if (IsModuleAwareSchedulingSegment(segment)) {
            for (const auto& schedulingSegmentModule : Config_->GetModules()) {
                if (currentResourceAmount.GetOrDefaultAt(schedulingSegmentModule) < fairResourceAmount.GetOrDefaultAt(schedulingSegmentModule)) {
                    hasUnsatisfiedSegment = true;
                    isSegmentUnsatisfied.At(segment).SetAt(schedulingSegmentModule, true);
                }
            }
        } else if (currentResourceAmount.GetOrDefault() < fairResourceAmount.GetOrDefault()) {
            hasUnsatisfiedSegment = true;
            isSegmentUnsatisfied.At(segment).Set(true);
        }
    }

    return {std::move(isSegmentUnsatisfied), hasUnsatisfiedSegment};
}

void TSchedulingSegmentManager::DoRebalanceSegments(TUpdateSchedulingSegmentsContext* context) const
{
    YT_LOG_DEBUG("Rebalancing node scheduling segments in tree");

    auto keyResource = GetSegmentBalancingKeyResource(Config_->Mode);

    TSchedulingSegmentMap<int> addedNodeCountPerSegment;
    TSchedulingSegmentMap<int> removedNodeCountPerSegment;
    TNodeMovePenalty totalPenalty;
    std::vector<std::pair<TNodeWithMovePenalty, ESchedulingSegment>> movedNodes;

    auto trySatisfySegment = [&] (
        ESchedulingSegment segment,
        double& currentResourceAmount,
        double fairResourceAmount,
        TNodeWithMovePenaltyList& availableNodes)
    {
        while (currentResourceAmount < fairResourceAmount) {
            if (availableNodes.empty()) {
                break;
            }

            // NB(eshcherbin): |availableNodes| is sorted in order of decreasing penalty.
            auto [nextAvailableNode, nextAvailableNodeMovePenalty] = availableNodes.back();
            availableNodes.pop_back();

            auto resourceAmountOnNode = GetNodeResourceLimit(*nextAvailableNode, keyResource);
            auto oldSegment = nextAvailableNode->SchedulingSegment;

            SetNodeSegment(nextAvailableNode, segment, context);
            movedNodes.emplace_back(
                TNodeWithMovePenalty{.Node = nextAvailableNode, .MovePenalty = nextAvailableNodeMovePenalty},
                segment);
            totalPenalty += nextAvailableNodeMovePenalty;

            const auto& schedulingSegmentModule = GetNodeModule(*nextAvailableNode);
            currentResourceAmount += resourceAmountOnNode;
            if (IsModuleAwareSchedulingSegment(segment)) {
                ++addedNodeCountPerSegment.At(segment).MutableAt(schedulingSegmentModule);
            } else {
                ++addedNodeCountPerSegment.At(segment).Mutable();
            }

            if (IsModuleAwareSchedulingSegment(oldSegment)) {
                context->CurrentResourceAmountPerSegment.At(oldSegment).MutableAt(schedulingSegmentModule) -= resourceAmountOnNode;
                ++removedNodeCountPerSegment.At(oldSegment).MutableAt(schedulingSegmentModule);
            } else {
                context->CurrentResourceAmountPerSegment.At(oldSegment).Mutable() -= resourceAmountOnNode;
                ++removedNodeCountPerSegment.At(oldSegment).Mutable();
            }
        }
    };

    // Every node has a penalty for moving it to another segment. We collect a set of movable nodes
    // iteratively by taking the node with the lowest penalty until the remaining nodes can no longer
    // satisfy the fair resource amount determined by the strategy.
    // In addition, the rest of the nodes are called aggressively movable if the current segment is not module-aware.
    // The intuition is that we should be able to compensate for a loss of such a node from one module by moving
    // a node from another module to the segment.
    THashMap<TSchedulingSegmentModule, TNodeWithMovePenaltyList> movableNodesPerModule;
    THashMap<TSchedulingSegmentModule, TNodeWithMovePenaltyList> aggressivelyMovableNodesPerModule;
    GetMovableNodes(
        context,
        &movableNodesPerModule,
        &aggressivelyMovableNodesPerModule);

    // First, we try to satisfy all module-aware segments, one module at a time.
    // During this phase we are allowed to use the nodes from |aggressivelyMovableNodesPerModule|
    // if |movableNodesPerModule| is exhausted.
    for (const auto& schedulingSegmentModule : Config_->GetModules()) {
        for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
            if (!IsModuleAwareSchedulingSegment(segment)) {
                continue;
            }

            auto& currentResourceAmount = context->CurrentResourceAmountPerSegment.At(segment).MutableAt(schedulingSegmentModule);
            auto fairResourceAmount = context->FairResourceAmountPerSegment.At(segment).GetOrDefaultAt(schedulingSegmentModule);
            trySatisfySegment(segment, currentResourceAmount, fairResourceAmount, movableNodesPerModule[schedulingSegmentModule]);
            trySatisfySegment(segment, currentResourceAmount, fairResourceAmount, aggressivelyMovableNodesPerModule[schedulingSegmentModule]);
        }
    }

    TNodeWithMovePenaltyList movableNodes;
    for (const auto& [_, movableNodesAtModule] : movableNodesPerModule) {
        std::move(movableNodesAtModule.begin(), movableNodesAtModule.end(), std::back_inserter(movableNodes));
    }
    SortByPenalty(movableNodes);

    // Second, we try to satisfy all other segments using the remaining movable nodes.
    // Note that some segments might have become unsatisfied during the first phase
    // if we used any nodes from |aggressivelyMovableNodesPerModule|.
    for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
        if (IsModuleAwareSchedulingSegment(segment)) {
            continue;
        }

        auto& currentResourceAmount = context->CurrentResourceAmountPerSegment.At(segment).Mutable();
        auto fairResourceAmount = context->FairResourceAmountPerSegment.At(segment).GetOrDefault();
        trySatisfySegment(segment, currentResourceAmount, fairResourceAmount, movableNodes);
    }

    auto [isSegmentUnsatisfied, hasUnsatisfiedSegment] = FindUnsatisfiedSegments(context);
    YT_LOG_WARNING_IF(hasUnsatisfiedSegment,
        "Failed to satisfy all scheduling segments during rebalancing (IsSegmentUnsatisfied: %v)",
        isSegmentUnsatisfied);

    YT_LOG_DEBUG(
        "Finished node scheduling segments rebalancing "
        "(TotalMovedNodeCount: %v, AddedNodeCountPerSegment: %v, RemovedNodeCountPerSegment: %v, "
        "NewResourceAmountPerSegment: %v, TotalPenalty: %v)",
        movedNodes.size(),
        addedNodeCountPerSegment,
        removedNodeCountPerSegment,
        context->CurrentResourceAmountPerSegment,
        totalPenalty);

    for (const auto& [nodeWithPenalty, newSegment] : movedNodes) {
        YT_LOG_DEBUG("Moving node to a new scheduling segment (Address: %v, OldSegment: %v, NewSegment: %v, Penalty: %v)",
            nodeWithPenalty.Node->Descriptor->Address,
            nodeWithPenalty.Node->SchedulingSegment,
            newSegment,
            nodeWithPenalty.MovePenalty);
    }
}

void TSchedulingSegmentManager::GetMovableNodes(
    TUpdateSchedulingSegmentsContext* context,
    THashMap<TSchedulingSegmentModule, TNodeWithMovePenaltyList>* movableNodesPerModule,
    THashMap<TSchedulingSegmentModule, TNodeWithMovePenaltyList>* aggressivelyMovableNodesPerModule) const
{
    auto keyResource = GetSegmentBalancingKeyResource(Config_->Mode);
    TSchedulingSegmentMap<std::vector<TNodeId>> nodeIdsPerSegment;
    for (const auto& [nodeId, node] : context->NodeStates) {
        auto& nodeIds = nodeIdsPerSegment.At(node.SchedulingSegment);
        if (IsModuleAwareSchedulingSegment(node.SchedulingSegment)) {
            nodeIds.MutableAt(GetNodeModule(node)).push_back(nodeId);
        } else {
            nodeIds.Mutable().push_back(nodeId);
        }
    }

    auto collectMovableNodes = [&] (double currentResourceAmount, double fairResourceAmount, const std::vector<TNodeId>& nodeIds) {
        TNodeWithMovePenaltyList segmentNodes;
        segmentNodes.reserve(nodeIds.size());
        for (auto nodeId : nodeIds) {
            auto& node = GetOrCrash(context->NodeStates, nodeId);
            segmentNodes.push_back(TNodeWithMovePenalty{
                .Node = &node,
                .MovePenalty = GetMovePenaltyForNode(node, Config_->Mode),
            });
        }

        SortByPenalty(segmentNodes);

        for (const auto& nodeWithMovePenalty : segmentNodes) {
            const auto* node = nodeWithMovePenalty.Node;
            if (node->SpecifiedSchedulingSegment) {
                continue;
            }

            const auto& schedulingSegmentModule = GetNodeModule(*node);
            auto resourceAmountOnNode = GetNodeResourceLimit(*node, keyResource);
            currentResourceAmount -= resourceAmountOnNode;
            if (currentResourceAmount >= fairResourceAmount) {
                (*movableNodesPerModule)[schedulingSegmentModule].push_back(nodeWithMovePenalty);
            } else if (!IsModuleAwareSchedulingSegment(node->SchedulingSegment)) {
                (*aggressivelyMovableNodesPerModule)[schedulingSegmentModule].push_back(nodeWithMovePenalty);
            }
        }
    };

    for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
        const auto& currentResourceAmount = context->CurrentResourceAmountPerSegment.At(segment);
        const auto& fairResourceAmount = context->FairResourceAmountPerSegment.At(segment);
        const auto& nodeIds = nodeIdsPerSegment.At(segment);
        if (IsModuleAwareSchedulingSegment(segment)) {
            for (const auto& schedulingSegmentModule : Config_->GetModules()) {
                collectMovableNodes(
                    currentResourceAmount.GetOrDefaultAt(schedulingSegmentModule),
                    fairResourceAmount.GetOrDefaultAt(schedulingSegmentModule),
                    nodeIds.GetOrDefaultAt(schedulingSegmentModule));
            }
        } else {
            collectMovableNodes(
                currentResourceAmount.GetOrDefault(),
                fairResourceAmount.GetOrDefault(),
                nodeIds.GetOrDefault());
        }
    }

    auto sortAndReverseMovableNodes = [&] (auto& movableNodes) {
        for (const auto& schedulingSegmentModule : Config_->GetModules()) {
            SortByPenalty(movableNodes[schedulingSegmentModule]);
            std::reverse(movableNodes[schedulingSegmentModule].begin(), movableNodes[schedulingSegmentModule].end());
        }
    };
    sortAndReverseMovableNodes(*movableNodesPerModule);
    sortAndReverseMovableNodes(*aggressivelyMovableNodesPerModule);
}

const TSchedulingSegmentModule& TSchedulingSegmentManager::GetNodeModule(const TFairShareTreeJobSchedulerNodeState& node) const
{
    YT_ASSERT(node.Descriptor.has_value());

    return GetNodeModule(*node.Descriptor, Config_->ModuleType);
}

void TSchedulingSegmentManager::SetNodeSegment(
    TFairShareTreeJobSchedulerNodeState* node,
    ESchedulingSegment segment,
    TUpdateSchedulingSegmentsContext* context) const
{
    if (node->SchedulingSegment == segment) {
        return;
    }

    node->SchedulingSegment = segment;
    context->MovedNodes.push_back(TSetNodeSchedulingSegmentOptions{
        .NodeId = node->Descriptor->Id,
        .Segment = segment,
    });
}

void TSchedulingSegmentManager::LogAndProfileSegments(const TUpdateSchedulingSegmentsContext* context) const
{
    auto mode = Config_->Mode;
    bool segmentedSchedulingEnabled = mode != ESegmentedSchedulingMode::Disabled;
    if (segmentedSchedulingEnabled) {
        YT_LOG_DEBUG(
            "Scheduling segments state in tree "
            "(Mode: %v, Modules: %v, KeyResource: %v, FairSharePerSegment: %v, TotalKeyResourceLimit: %v, "
            "TotalCapacityPerModule: %v, FairResourceAmountPerSegment: %v, CurrentResourceAmountPerSegment: %v)",
            mode,
            Config_->GetModules(),
            GetSegmentBalancingKeyResource(mode),
            context->FairSharePerSegment,
            context->NodesTotalKeyResourceLimit,
            context->TotalCapacityPerModule,
            context->FairResourceAmountPerSegment,
            context->CurrentResourceAmountPerSegment);
    } else {
        YT_LOG_DEBUG("Segmented scheduling is disabled in tree, skipping");
    }

    TSensorBuffer sensorBuffer;
    if (segmentedSchedulingEnabled) {
        for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
            auto profileResourceAmountPerSegment = [&] (const TString& sensorName, const TSegmentToResourceAmount& resourceAmountMap) {
                const auto& valueAtSegment = resourceAmountMap.At(segment);
                if (IsModuleAwareSchedulingSegment(segment)) {
                    for (const auto& schedulingSegmentModule : Config_->GetModules()) {
                        TWithTagGuard guard(&sensorBuffer, "module", ToString(schedulingSegmentModule));
                        sensorBuffer.AddGauge(sensorName, valueAtSegment.GetOrDefaultAt(schedulingSegmentModule));
                    }
                } else {
                    sensorBuffer.AddGauge(sensorName, valueAtSegment.GetOrDefault());
                }
            };

            TWithTagGuard guard(&sensorBuffer, "segment", FormatEnum(segment));
            profileResourceAmountPerSegment("/fair_resource_amount", context->FairResourceAmountPerSegment);
            profileResourceAmountPerSegment("/current_resource_amount", context->CurrentResourceAmountPerSegment);
        }
    } else {
        for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
            TWithTagGuard guard(&sensorBuffer, "segment", FormatEnum(segment));
            if (IsModuleAwareSchedulingSegment(segment)) {
                guard.AddTag("module", ToString(NullModule));
            }

            sensorBuffer.AddGauge("/fair_resource_amount", 0.0);
            sensorBuffer.AddGauge("/current_resource_amount", 0.0);
        }
    }

    for (const auto& schedulingSegmentModule : Config_->GetModules()) {
        auto it = context->TotalCapacityPerModule.find(schedulingSegmentModule);
        auto moduleCapacity = it != context->TotalCapacityPerModule.end() ? it->second : 0.0;

        TWithTagGuard guard(&sensorBuffer, "module", ToString(schedulingSegmentModule));
        sensorBuffer.AddGauge("/module_capacity", moduleCapacity);
    }

    SensorProducer_->Update(std::move(sensorBuffer));
}

void TSchedulingSegmentManager::BuildPersistentState(TUpdateSchedulingSegmentsContext* context) const
{
    context->PersistentState = New<TPersistentSchedulingSegmentsState>();

    for (auto [nodeId, node] : context->NodeStates) {
        if (node.SchedulingSegment == ESchedulingSegment::Default) {
            continue;
        }

        EmplaceOrCrash(
            context->PersistentState->NodeStates,
            nodeId,
            TPersistentNodeSchedulingSegmentState{
                .Segment = node.SchedulingSegment,
                .Address = node.Descriptor->Address,
            });
    }

    for (const auto& [operationId, operationState] : context->OperationStates) {
        if (operationState->SchedulingSegmentModule) {
            EmplaceOrCrash(
                context->PersistentState->OperationStates,
                operationId,
                TPersistentOperationSchedulingSegmentState{
                    .Module = operationState->SchedulingSegmentModule,
                });
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
