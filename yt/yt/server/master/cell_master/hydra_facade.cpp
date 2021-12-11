#include "hydra_facade.h"
#include "private.h"
#include "automaton.h"
#include "config.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>

#include <yt/yt/server/master/cypress_server/cypress_manager.h>
#include <yt/yt/server/master/cypress_server/node_detail.h>

#include <yt/yt/server/lib/election/election_manager.h>
#include <yt/yt/server/lib/election/distributed_election_manager.h>
#include <yt/yt/server/lib/election/election_manager_thunk.h>

#include <yt/yt/server/lib/hive/transaction_supervisor.h>
#include <yt/yt/server/lib/hive/hive_manager.h>

#include <yt/yt/server/lib/hydra_common/changelog.h>
#include <yt/yt/server/lib/hydra_common/composite_automaton.h>
#include <yt/yt/server/lib/hydra_common/snapshot.h>
#include <yt/yt/server/lib/hydra_common/private.h>

#include <yt/yt/server/lib/hydra/distributed_hydra_manager.h>
#include <yt/yt/server/lib/hydra/local_hydra_janitor.h>
#include <yt/yt/server/lib/hydra/private.h>

#include <yt/yt/server/master/object_server/object.h>
#include <yt/yt/server/master/object_server/private.h>

#include <yt/yt/server/master/security_server/acl.h>
#include <yt/yt/server/master/security_server/group.h>
#include <yt/yt/server/master/security_server/security_manager.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/yt/ytlib/election/cell_manager.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/fair_share_action_queue.h>

#include <yt/yt/core/actions/cancelable_context.h>

#include <yt/yt/core/misc/proc.h>

#include <yt/yt/core/rpc/bus/channel.h>
#include <yt/yt/core/rpc/response_keeper.h>
#include <yt/yt/core/rpc/server.h>

#include <yt/yt/core/ypath/token.h>

#include <yt/yt/core/ytree/ypath_client.h>
#include <yt/yt/core/ytree/ypath_proxy.h>

namespace NYT::NCellMaster {

using namespace NConcurrency;
using namespace NRpc;
using namespace NElection;
using namespace NHydra;
using namespace NHiveServer;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CellMasterLogger;

////////////////////////////////////////////////////////////////////////////////

class THydraFacade::TImpl
    : public TRefCounted
{
public:
    TImpl(TTestingTag)
        : Bootstrap_(nullptr)
    { }

    TImpl(
        TCellMasterConfigPtr config,
        TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
    {
        YT_VERIFY(Config_);
        YT_VERIFY(Bootstrap_);

        AutomatonQueue_ = CreateEnumIndexedFairShareActionQueue<EAutomatonThreadQueue>(
            "Automaton",
            GetAutomatonThreadBuckets());
        VERIFY_INVOKER_THREAD_AFFINITY(AutomatonQueue_->GetInvoker(EAutomatonThreadQueue::Default), AutomatonThread);

        NObjectServer::SetupMasterBootstrap(bootstrap);

        BIND([=] {
            NObjectServer::SetupAutomatonThread();
            NObjectServer::SetupEpochContext(EpochContext_);
        })
            .AsyncVia(AutomatonQueue_->GetInvoker(EAutomatonThreadQueue::Default))
            .Run()
            .Get()
            .ThrowOnError();

        Automaton_ = New<TMasterAutomaton>(Bootstrap_);

        TransactionTrackerQueue_ = New<TActionQueue>("TxTracker");

        ResponseKeeper_ = New<TResponseKeeper>(
            Config_->HydraManager->ResponseKeeper,
            GetAutomatonInvoker(EAutomatonThreadQueue::ResponseKeeper),
            NObjectServer::ObjectServerLogger,
            NObjectServer::ObjectServerProfiler);

        auto electionManagerThunk = New<TElectionManagerThunk>();

        TDistributedHydraManagerOptions hydraManagerOptions{
            .UseFork = true,
            .ResponseKeeper = ResponseKeeper_
        };
        TDistributedHydraManagerDynamicOptions hydraManagerDynamicOptions{
            .AbandonLeaderLeaseDuringRecovery = true
        };
        HydraManager_ = CreateDistributedHydraManager(
            Config_->HydraManager,
            Bootstrap_->GetControlInvoker(),
            GetAutomatonInvoker(EAutomatonThreadQueue::Mutation),
            Automaton_,
            Bootstrap_->GetRpcServer(),
            electionManagerThunk,
            Bootstrap_->GetCellManager()->GetCellId(),
            Bootstrap_->GetChangelogStoreFactory(),
            Bootstrap_->GetSnapshotStore(),
            hydraManagerOptions,
            hydraManagerDynamicOptions);

        HydraManager_->SubscribeStartLeading(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
        HydraManager_->SubscribeStopLeading(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));

        HydraManager_->SubscribeStartFollowing(BIND(&TImpl::OnStartEpoch, MakeWeak(this)));
        HydraManager_->SubscribeStopFollowing(BIND(&TImpl::OnStopEpoch, MakeWeak(this)));

        for (auto queue : TEnumTraits<EAutomatonThreadQueue>::GetDomainValues()) {
            auto unguardedInvoker = GetAutomatonInvoker(queue);
            GuardedInvokers_[queue] = HydraManager_->CreateGuardedAutomatonInvoker(unguardedInvoker);
        }

        ElectionManager_ = CreateDistributedElectionManager(
            Config_->ElectionManager,
            Bootstrap_->GetCellManager(),
            Bootstrap_->GetControlInvoker(),
            HydraManager_->GetElectionCallbacks(),
            Bootstrap_->GetRpcServer());

        electionManagerThunk->SetUnderlying(ElectionManager_);

        LocalJanitor_ = New<TLocalHydraJanitor>(
            Config_->Snapshots->Path,
            Config_->Changelogs->Path,
            Config_->HydraManager,
            GetHydraIOInvoker());
    }

    void Initialize()
    {
        ElectionManager_->Initialize();

        HydraManager_->Initialize();

        LocalJanitor_->Start();
    }

    void LoadSnapshot(
        ISnapshotReaderPtr reader,
        bool dump,
        bool enableTotalWriteCountReport,
        const TSerializationDumperConfigPtr& dumpConfig)
    {
        WaitFor(reader->Open())
            .ThrowOnError();

        Automaton_->SetSerializationDumpEnabled(dump);
        Automaton_->SetEnableTotalWriteCountReport(enableTotalWriteCountReport);
        if (dumpConfig) {
            Automaton_->SetLowerWriteCountDumpLimit(dumpConfig->LowerLimit);
            Automaton_->SetUpperWriteCountDumpLimit(dumpConfig->UpperLimit);
        }
        HydraManager_->ValidateSnapshot(reader);
    }

    const TMasterAutomatonPtr& GetAutomaton() const
    {
        return Automaton_;
    }

    const IElectionManagerPtr& GetElectionManager() const
    {
        return ElectionManager_;
    }

    const IHydraManagerPtr& GetHydraManager() const
    {
        return HydraManager_;
    }

    const TResponseKeeperPtr& GetResponseKeeper() const
    {
        return ResponseKeeper_;
    }

    IInvokerPtr GetAutomatonInvoker(EAutomatonThreadQueue queue) const
    {
        return AutomatonQueue_->GetInvoker(queue);
    }

    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue) const
    {
        return EpochInvokers_[queue];
    }

    IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue) const
    {
        return GuardedInvokers_[queue];
    }


    IInvokerPtr GetTransactionTrackerInvoker() const
    {
        return TransactionTrackerQueue_->GetInvoker();
    }

    const NObjectServer::TEpochContextPtr& GetEpochContext() const
    {
        return EpochContext_;
    }

    void BlockAutomaton()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YT_ASSERT(!AutomatonBlocked_);
        AutomatonBlocked_ = true;

        YT_LOG_TRACE("Automaton thread blocked");
    }

    void UnblockAutomaton()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YT_ASSERT(AutomatonBlocked_);
        AutomatonBlocked_ = false;

        YT_LOG_TRACE("Automaton thread unblocked");
    }

    bool IsAutomatonLocked() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return AutomatonBlocked_;
    }

    void VerifyPersistentStateRead()
    {
#ifdef YT_ENABLE_THREAD_AFFINITY_CHECK
        if (IsAutomatonLocked()) {
            auto automatonThreadId = AutomatonThread_Slot.GetBoundThreadId();
            YT_VERIFY(automatonThreadId != InvalidThreadId);
            YT_VERIFY(GetCurrentThreadId() != automatonThreadId);
        } else {
            VERIFY_THREAD_AFFINITY(AutomatonThread);
        }
#endif
    }


    void RequireLeader() const
    {
        if (!HydraManager_->IsLeader()) {
            if (HasMutationContext()) {
                // Just a precaution, not really expected to happen.
                auto error = TError("Request can only be served at leaders");
                YT_LOG_ALERT_IF(HydraManager_->IsMutationLoggingEnabled(), error);
                THROW_ERROR error;
            } else {
                throw TLeaderFallbackException();
            }
        }
    }

    void Reconfigure(const TDynamicCellMasterConfigPtr& newConfig)
    {
        AutomatonQueue_->Reconfigure(newConfig->AutomatonThreadBucketWeights);
    }

    IInvokerPtr CreateEpochInvoker(IInvokerPtr underlyingInvoker)
    {
        VerifyPersistentStateRead();

        return EpochCancelableContext_->CreateInvoker(std::move(underlyingInvoker));
    }

private:
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    const TCellMasterConfigPtr Config_;
    TBootstrap* const Bootstrap_;

    IElectionManagerPtr ElectionManager_;

    IEnumIndexedFairShareActionQueuePtr<EAutomatonThreadQueue> AutomatonQueue_;
    TMasterAutomatonPtr Automaton_;
    IHydraManagerPtr HydraManager_;

    TActionQueuePtr TransactionTrackerQueue_;

    TResponseKeeperPtr ResponseKeeper_;

    TLocalHydraJanitorPtr LocalJanitor_;

    TEnumIndexedVector<EAutomatonThreadQueue, IInvokerPtr> GuardedInvokers_;
    TEnumIndexedVector<EAutomatonThreadQueue, IInvokerPtr> EpochInvokers_;

    NObjectServer::TEpochContextPtr EpochContext_ = New<NObjectServer::TEpochContext>();

    std::atomic<bool> AutomatonBlocked_ = false;

    TCancelableContextPtr EpochCancelableContext_;

    void OnStartEpoch()
    {
        EpochCancelableContext_ = HydraManager_->GetAutomatonCancelableContext();
        for (auto queue : TEnumTraits<EAutomatonThreadQueue>::GetDomainValues()) {
            EpochInvokers_[queue] = CreateEpochInvoker(GetAutomatonInvoker(queue));
        }

        NObjectServer::BeginEpoch();
    }

    void OnStopEpoch()
    {
        std::fill(EpochInvokers_.begin(), EpochInvokers_.end(), nullptr);

        NObjectServer::EndEpoch();
        EpochCancelableContext_ = nullptr;
    }

    static THashMap<EAutomatonThreadBucket, std::vector<EAutomatonThreadQueue>> GetAutomatonThreadBuckets()
    {
        THashMap<EAutomatonThreadBucket, std::vector<EAutomatonThreadQueue>> buckets;
        buckets[EAutomatonThreadBucket::Gossips] = {
            EAutomatonThreadQueue::TabletGossip,
            EAutomatonThreadQueue::NodeTrackerGossip,
            EAutomatonThreadQueue::MulticellGossip,
            EAutomatonThreadQueue::SecurityGossip,
        };
        return buckets;
    }
};

////////////////////////////////////////////////////////////////////////////////

THydraFacade::THydraFacade(TTestingTag tag)
    : Impl_(New<TImpl>(tag))
{ }

THydraFacade::THydraFacade(
    TCellMasterConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

THydraFacade::~THydraFacade()
{ }

void THydraFacade::Initialize()
{
    Impl_->Initialize();
}

void THydraFacade::LoadSnapshot(
    ISnapshotReaderPtr reader,
    bool dump,
    bool enableTotalWriteCountReport,
    const TSerializationDumperConfigPtr& dumpConfig)
{
    Impl_->LoadSnapshot(reader, dump, enableTotalWriteCountReport, dumpConfig);
}

const TMasterAutomatonPtr& THydraFacade::GetAutomaton() const
{
    return Impl_->GetAutomaton();
}

const IElectionManagerPtr& THydraFacade::GetElectionManager() const
{
    return Impl_->GetElectionManager();
}

const IHydraManagerPtr& THydraFacade::GetHydraManager() const
{
    return Impl_->GetHydraManager();
}

const TResponseKeeperPtr& THydraFacade::GetResponseKeeper() const
{
    return Impl_->GetResponseKeeper();
}

IInvokerPtr THydraFacade::GetAutomatonInvoker(EAutomatonThreadQueue queue) const
{
    return Impl_->GetAutomatonInvoker(queue);
}

IInvokerPtr THydraFacade::GetEpochAutomatonInvoker(EAutomatonThreadQueue queue) const
{
    return Impl_->GetEpochAutomatonInvoker(queue);
}

IInvokerPtr THydraFacade::GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue) const
{
    return Impl_->GetGuardedAutomatonInvoker(queue);
}

IInvokerPtr THydraFacade::GetTransactionTrackerInvoker() const
{
    return Impl_->GetTransactionTrackerInvoker();
}

void THydraFacade::BlockAutomaton()
{
    Impl_->BlockAutomaton();
}

void THydraFacade::UnblockAutomaton()
{
    Impl_->UnblockAutomaton();
}

bool THydraFacade::IsAutomatonLocked() const
{
    return Impl_->IsAutomatonLocked();
}

void THydraFacade::VerifyPersistentStateRead()
{
    Impl_->VerifyPersistentStateRead();
}

void THydraFacade::RequireLeader() const
{
    Impl_->RequireLeader();
}

void THydraFacade::Reconfigure(const TDynamicCellMasterConfigPtr& newConfig)
{
    Impl_->Reconfigure(newConfig);
}

IInvokerPtr THydraFacade::CreateEpochInvoker(IInvokerPtr underlyingInvoker)
{
    return Impl_->CreateEpochInvoker(std::move(underlyingInvoker));
}

const NObjectServer::TEpochContextPtr& THydraFacade::GetEpochContext() const
{
    return Impl_->GetEpochContext();
}

////////////////////////////////////////////////////////////////////////////////

TAutomatonBlockGuard::TAutomatonBlockGuard(THydraFacadePtr hydraFacade)
    : HydraFacade_(std::move(hydraFacade))
{
    HydraFacade_->BlockAutomaton();
}

TAutomatonBlockGuard::~TAutomatonBlockGuard()
{
    HydraFacade_->UnblockAutomaton();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster

