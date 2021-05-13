#include "chunk_manager.h"
#include "config.h"
#include "job_tracker.h"
#include "job.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>

#include <yt/yt/server/master/node_tracker_server/data_center.h>
#include <yt/yt/server/master/node_tracker_server/node_tracker.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/ytlib/node_tracker_client/helpers.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>

namespace NYT::NChunkServer {

using namespace NNodeTrackerClient::NProto;
using namespace NNodeTrackerServer;
using namespace NCellMaster;
using namespace NConcurrency;
using namespace NProfiling;
using namespace NCypressClient;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

TJobTracker::TJobTracker(
    TChunkManagerConfigPtr config,
    TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , JobThrottler_(CreateReconfigurableThroughputThrottler(
        New<TThroughputThrottlerConfig>(),
        ChunkServerLogger,
        ChunkServerProfilerRegistry.WithPrefix("/job_throttler")))
{
    InitInterDCEdges();
}

TJobTracker::~TJobTracker() = default;

void TJobTracker::Start()
{
    const auto& configManager = Bootstrap_->GetConfigManager();
    configManager->SubscribeConfigChanged(DynamicConfigChangedCallback_);
    OnDynamicConfigChanged();
}

void TJobTracker::Stop()
{
    const auto& configManager = Bootstrap_->GetConfigManager();
    configManager->UnsubscribeConfigChanged(DynamicConfigChangedCallback_);
}

void TJobTracker::OnNodeDataCenterChanged(TNode* node, TDataCenter* oldDataCenter)
{
    YT_ASSERT(node->GetDataCenter() != oldDataCenter);

    for (const auto& [jobId, job] : node->IdToJob()) {
        UpdateInterDCEdgeConsumption(job, oldDataCenter, -1);
        UpdateInterDCEdgeConsumption(job, node->GetDataCenter(), +1);
    }
}

int TJobTracker::GetCappedSecondaryCellCount()
{
    return std::max<int>(1, Bootstrap_->GetMulticellManager()->GetSecondaryCellTags().size());
}

void TJobTracker::InitInterDCEdges()
{
    UpdateInterDCEdgeCapacities();
    InitUnsaturatedInterDCEdges();
}

void TJobTracker::InitUnsaturatedInterDCEdges()
{
    UnsaturatedInterDCEdges_.clear();

    const auto defaultCapacity = GetDynamicConfig()->InterDCLimits->GetDefaultCapacity() / GetCappedSecondaryCellCount();

    const auto& nodeTracker = Bootstrap_->GetNodeTracker();

    auto updateForSrcDC = [&] (const TDataCenter* srcDataCenter) {
        auto& interDCEdgeConsumption = InterDCEdgeConsumption_[srcDataCenter];
        const auto& interDCEdgeCapacities = InterDCEdgeCapacities_[srcDataCenter];

        auto updateForDstDC = [&] (const TDataCenter* dstDataCenter) {
            if (interDCEdgeConsumption.Value(dstDataCenter, 0) <
                interDCEdgeCapacities.Value(dstDataCenter, defaultCapacity))
            {
                UnsaturatedInterDCEdges_[srcDataCenter].insert(dstDataCenter);
            }
        };

        updateForDstDC(nullptr);
        for (auto [dataCenterId, dataCenter] : nodeTracker->DataCenters()) {
            if (IsObjectAlive(dataCenter)) {
                updateForDstDC(dataCenter);
            }
        }
    };

    updateForSrcDC(nullptr);
    for (auto [dataCenterId, dataCenter] : nodeTracker->DataCenters()) {
        if (IsObjectAlive(dataCenter)) {
            updateForSrcDC(dataCenter);
        }
    }
}

void TJobTracker::UpdateInterDCEdgeConsumption(
    const TJobPtr& job,
    const TDataCenter* srcDataCenter,
    int sizeMultiplier)
{
    // TODO(aleksandra-zh): Add merge jobs here.
    if (job->GetType() != EJobType::ReplicateChunk &&
        job->GetType() != EJobType::RepairChunk)
    {
        return;
    }

    auto& interDCEdgeConsumption = InterDCEdgeConsumption_[srcDataCenter];
    const auto& interDCEdgeCapacities = InterDCEdgeCapacities_[srcDataCenter];

    const auto defaultCapacity = GetDynamicConfig()->InterDCLimits->GetDefaultCapacity() / GetCappedSecondaryCellCount();

    auto getReplicas = [&] (const TJobPtr& job) {
        switch (job->GetType()) {
            case EJobType::ReplicateChunk: {
                auto replicateJob = StaticPointerCast<TReplicationJob>(job);
                return replicateJob->TargetReplicas();
            }
            case EJobType::RepairChunk: {
                auto repairJob = StaticPointerCast<TRepairJob>(job);
                return repairJob->TargetReplicas();
            }
            default:
                YT_ABORT();
        }
    };

    i64 chunkPartSize = 0;
    switch (job->GetType()) {
        case EJobType::ReplicateChunk:
            chunkPartSize = job->ResourceUsage().replication_data_size();
            break;
        case EJobType::RepairChunk:
            chunkPartSize = job->ResourceUsage().repair_data_size();
            break;
        default:
            YT_ABORT();
    }

    for (const auto& nodePtrWithIndexes : getReplicas(job)) {
        const auto* dstDataCenter = nodePtrWithIndexes.GetPtr()->GetDataCenter();

        auto& consumption = interDCEdgeConsumption[dstDataCenter];
        consumption += sizeMultiplier * chunkPartSize;

        if (consumption < interDCEdgeCapacities.Value(dstDataCenter, defaultCapacity)) {
            UnsaturatedInterDCEdges_[srcDataCenter].insert(dstDataCenter);
        } else {
            auto it = UnsaturatedInterDCEdges_.find(srcDataCenter);
            if (it != UnsaturatedInterDCEdges_.end()) {
                it->second.erase(dstDataCenter);
                // Don't do UnsaturatedInterDCEdges_.erase(it) here - the memory
                // saving is negligible, but the slowdown may be noticeable. Plus,
                // the removal is very likely to be undone by a soon-to-follow insertion.
            }
        }
    }
}

bool TJobTracker::HasUnsaturatedInterDCEdgeStartingFrom(const TDataCenter* srcDataCenter) const
{
    auto it = UnsaturatedInterDCEdges_.find(srcDataCenter);
    if (it == UnsaturatedInterDCEdges_.end()) {
        return false;
    }
    return !it->second.empty();
}

void TJobTracker::OnDataCenterCreated(const TDataCenter* dataCenter)
{
    UpdateInterDCEdgeCapacities(true);

    const auto defaultCapacity = GetDynamicConfig()->InterDCLimits->GetDefaultCapacity() / GetCappedSecondaryCellCount();

    auto updateEdge = [&] (const TDataCenter* srcDataCenter, const TDataCenter* dstDataCenter) {
        if (InterDCEdgeConsumption_[srcDataCenter].Value(dstDataCenter, 0) <
            InterDCEdgeCapacities_[srcDataCenter].Value(dstDataCenter, defaultCapacity))
        {
            UnsaturatedInterDCEdges_[srcDataCenter].insert(dstDataCenter);
        }
    };

    updateEdge(nullptr, dataCenter);
    updateEdge(dataCenter, nullptr);

    const auto& nodeTracker = Bootstrap_->GetNodeTracker();
    for (const auto& [dstDataCenterId, otherDataCenter] : nodeTracker->DataCenters()) {
        updateEdge(dataCenter, otherDataCenter);
        updateEdge(otherDataCenter, dataCenter);
    }
}

void TJobTracker::OnDataCenterDestroyed(const TDataCenter* dataCenter)
{
    InterDCEdgeCapacities_.erase(dataCenter);
    for (auto& [srcDataCenter, dstDataCenterCapacities] : InterDCEdgeCapacities_) {
        dstDataCenterCapacities.erase(dataCenter); // may be no-op
    }

    InterDCEdgeConsumption_.erase(dataCenter);
    for (auto& [srcDataCenter, dstDataCenterConsumption] : InterDCEdgeConsumption_) {
        dstDataCenterConsumption.erase(dataCenter); // may be no-op
    }

    UnsaturatedInterDCEdges_.erase(dataCenter);
    for (auto& [srcDataCenter, dstDataCenterSet] : UnsaturatedInterDCEdges_) {
        dstDataCenterSet.erase(dataCenter); // may be no-op
    }
}

void TJobTracker::UpdateInterDCEdgeCapacities(bool force)
{
    if (!force &&
        GetCpuInstant() - InterDCEdgeCapacitiesLastUpdateTime_ <= GetDynamicConfig()->InterDCLimits->GetUpdateInterval())
    {
        return;
    }

    InterDCEdgeCapacities_.clear();

    auto capacities = GetDynamicConfig()->InterDCLimits->GetCapacities();

    const auto& nodeTracker = Bootstrap_->GetNodeTracker();

    auto updateForSrcDC = [&] (const TDataCenter* srcDataCenter) {
        const std::optional<TString>& srcDataCenterName = srcDataCenter
            ? std::optional<TString>(srcDataCenter->GetName())
            : std::nullopt;
        auto& interDCEdgeCapacities = InterDCEdgeCapacities_[srcDataCenter];
        const auto& newInterDCEdgeCapacities = capacities[srcDataCenterName];

        auto updateForDstDC = [&] (const TDataCenter* dstDataCenter) {
            const std::optional<TString>& dstDataCenterName = dstDataCenter
                ? std::optional<TString>(dstDataCenter->GetName())
                : std::nullopt;
            auto it = newInterDCEdgeCapacities.find(dstDataCenterName);
            if (it != newInterDCEdgeCapacities.end()) {
                interDCEdgeCapacities[dstDataCenter] = it->second / GetCappedSecondaryCellCount();
            }
        };

        updateForDstDC(nullptr);
        for (const auto& pair : nodeTracker->DataCenters()) {
            if (IsObjectAlive(pair.second)) {
                updateForDstDC(pair.second);
            }
        }
    };

    updateForSrcDC(nullptr);
    for (const auto& pair : nodeTracker->DataCenters()) {
        if (IsObjectAlive(pair.second)) {
            updateForSrcDC(pair.second);
        }
    }

    InterDCEdgeCapacitiesLastUpdateTime_ = GetCpuInstant();
}

const TJobTracker::TDataCenterSet& TJobTracker::GetUnsaturatedInterDCEdgesStartingFrom(const TDataCenter* dc)
{
    return UnsaturatedInterDCEdges_[dc];
}

TJobId TJobTracker::GenerateJobId() const
{
    const auto& multicellManager = Bootstrap_->GetMulticellManager();
    return MakeRandomId(EObjectType::MasterJob, multicellManager->GetCellTag());
}

void TJobTracker::RegisterJob(
    const TJobPtr& job,
    std::vector<TJobPtr>* jobsToStart,
    TNodeResources* resourceUsage)
{
    if (!job) {
        return;
    }

    *resourceUsage += job->ResourceUsage();
    jobsToStart->push_back(job);

    job->GetNode()->RegisterJob(job);

    auto jobType = job->GetType();
    ++RunningJobs_[jobType];
    ++JobsStarted_[jobType];

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto chunkId = job->GetChunkIdWithIndexes().Id;
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (chunk) {
        chunk->SetJob(job);
    }

    UpdateInterDCEdgeConsumption(job, job->GetNode()->GetDataCenter(), +1);

    JobThrottler_->Acquire(1);
}

void TJobTracker::UnregisterJob(const TJobPtr& job)
{
    job->GetNode()->UnregisterJob(job);
    auto jobType = job->GetType();
    --RunningJobs_[jobType];

    auto jobState = job->GetState();
    switch (jobState) {
        case EJobState::Completed:
            ++JobsCompleted_[jobType];
            break;
        case EJobState::Failed:
            ++JobsFailed_[jobType];
            break;
        case EJobState::Aborted:
            ++JobsAborted_[jobType];
            break;
        default:
            break;
    }
    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto chunkId = job->GetChunkIdWithIndexes().Id;
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (chunk) {
        chunk->SetJob(nullptr);
        chunkManager->ScheduleChunkRefresh(chunk);
    }

    UpdateInterDCEdgeConsumption(job, job->GetNode()->GetDataCenter(), -1);
}

void TJobTracker::ProcessJobs(
    TNode* node,
    const std::vector<TJobPtr>& currentJobs,
    std::vector<TJobPtr>* jobsToAbort,
    std::vector<TJobPtr>* jobsToRemove)
{
    // Pull capacity changes.
    UpdateInterDCEdgeCapacities();

    const auto& address = node->GetDefaultAddress();

    for (const auto& job : currentJobs) {
        auto jobId = job->GetJobId();
        auto jobType = job->GetType();

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(CellTagFromId(jobId) == multicellManager->GetCellTag());

        YT_VERIFY(TypeFromId(jobId) == EObjectType::MasterJob);

        switch (job->GetState()) {
            case EJobState::Running:
            case EJobState::Waiting: {
                if (TInstant::Now() - job->GetStartTime() > GetDynamicConfig()->JobTimeout) {
                    jobsToAbort->push_back(job);
                    YT_LOG_WARNING("Job timed out (JobId: %v, JobType: %v, Address: %v, Duration: %v, ChunkId: %v)",
                        jobId,
                        jobType,
                        address,
                        TInstant::Now() - job->GetStartTime(),
                        job->GetChunkIdWithIndexes());
                    break;
                }

                switch (job->GetState()) {
                    case EJobState::Running:
                        YT_LOG_DEBUG("Job is running (JobId: %v, JobType: %v, Address: %v, ChunkId: %v)",
                            jobId,
                            jobType,
                            address,
                            job->GetChunkIdWithIndexes());
                        break;

                    case EJobState::Waiting:
                        YT_LOG_DEBUG("Job is waiting (JobId: %v, JobType: %v, Address: %v, ChunkId: %v)",
                            jobId,
                            jobType,
                            address,
                            job->GetChunkIdWithIndexes());
                        break;

                    default:
                        YT_ABORT();
                }
                break;
            }

            case EJobState::Completed:
            case EJobState::Failed:
            case EJobState::Aborted: {
                jobsToRemove->push_back(job);
                auto rescheduleChunkRemoval = [&] {
                    if (jobType == EJobType::RemoveChunk &&
                        !job->Error().FindMatching(NChunkClient::EErrorCode::NoSuchChunk))
                    {
                        const auto& replica = job->GetChunkIdWithIndexes();
                        node->AddToChunkRemovalQueue(replica);
                    }
                };

                switch (job->GetState()) {
                    case EJobState::Completed:
                        YT_LOG_DEBUG("Job completed (JobId: %v, JobType: %v, Address: %v, ChunkId: %v)",
                            jobId,
                            jobType,
                            address,
                            job->GetChunkIdWithIndexes());
                        break;

                    case EJobState::Failed:
                        YT_LOG_WARNING(job->Error(), "Job failed (JobId: %v, JobType: %v, Address: %v, ChunkId: %v)",
                            jobId,
                            jobType,
                            address,
                            job->GetChunkIdWithIndexes());
                        rescheduleChunkRemoval();
                        break;

                    case EJobState::Aborted:
                        YT_LOG_WARNING(job->Error(), "Job aborted (JobId: %v, JobType: %v, Address: %v, ChunkId: %v)",
                            jobId,
                            jobType,
                            address,
                            job->GetChunkIdWithIndexes());
                        rescheduleChunkRemoval();
                        break;

                    default:
                        YT_ABORT();
                }
                UnregisterJob(job);
                break;
            }

            default:
                YT_ABORT();
        }
    }

    // Check for missing jobs
    THashSet<TJobPtr> currentJobSet(currentJobs.begin(), currentJobs.end());
    std::vector<TJobPtr> missingJobs;
    for (const auto& [jobId, job] : node->IdToJob()) {
        if (currentJobSet.find(job) == currentJobSet.end()) {
            missingJobs.push_back(job);
            YT_LOG_WARNING("Job is missing (JobId: %v, JobType: %v, Address: %v, ChunkId: %v)",
                jobId,
                job->GetType(),
                address,
                job->GetChunkIdWithIndexes());
        }
    }

    for (const auto& job : missingJobs) {
        UnregisterJob(job);
    }
}

bool TJobTracker::IsOverdraft() const
{
    return JobThrottler_->IsOverdraft();
}

const TDynamicChunkManagerConfigPtr& TJobTracker::GetDynamicConfig()
{
    const auto& configManager = Bootstrap_->GetConfigManager();
    return configManager->GetConfig()->ChunkManager;
}

void TJobTracker::OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/)
{
    JobThrottler_->Reconfigure(GetDynamicConfig()->JobThrottler);
}

void TJobTracker::OverrideResourceLimits(TNodeResources* resourceLimits, const TNode& node)
{
    const auto& resourceLimitsOverrides = node.ResourceLimitsOverrides();
    #define XX(name, Name) \
        if (resourceLimitsOverrides.has_##name()) { \
            resourceLimits->set_##name(std::min(resourceLimitsOverrides.name(), resourceLimits->name())); \
        }
    ITERATE_NODE_RESOURCE_LIMITS_OVERRIDES(XX)
    #undef XX
}

void TJobTracker::OnProfiling(TSensorBuffer* buffer) const
{
    for (auto jobType : TEnumTraits<EJobType>::GetDomainValues()) {
        if (jobType >= NJobTrackerClient::FirstMasterJobType && jobType <= NJobTrackerClient::LastMasterJobType) {
            buffer->PushTag({"job_type", FormatEnum(jobType)});

            buffer->AddGauge("/running_job_count", RunningJobs_[jobType]);
            buffer->AddCounter("/jobs_started", JobsStarted_[jobType]);
            buffer->AddCounter("/jobs_completed", JobsCompleted_[jobType]);
            buffer->AddCounter("/jobs_failed", JobsFailed_[jobType]);
            buffer->AddCounter("/jobs_aborted", JobsAborted_[jobType]);

            buffer->PopTag();
        }
    }

    for (const auto& srcPair : InterDCEdgeConsumption_) {
        const auto* src = srcPair.first;
        buffer->PushTag({"source_data_center", src ? src->GetName() : "null"});

        for (const auto& dstPair : srcPair.second) {
            const auto* dst = dstPair.first;
            buffer->PushTag({"destination_data_center", dst ? dst->GetName() : "null"});

            const auto consumption = dstPair.second;
            buffer->AddGauge("/inter_dc_edge_consumption", consumption);

            buffer->PopTag();
        }

        buffer->PopTag();
    }

    for (const auto& srcPair : InterDCEdgeCapacities_) {
        const auto* src = srcPair.first;
        buffer->PushTag({"source_data_center", src ? src->GetName() : "null"});

        for (const auto& dstPair : srcPair.second) {
            const auto* dst = dstPair.first;
            buffer->PushTag({"destination_data_center", dst ? dst->GetName() : "null"});

            const auto capacity = dstPair.second;
            buffer->AddGauge("/inter_dc_edge_capacity", capacity);

            buffer->PopTag();
        }

        buffer->PopTag();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
