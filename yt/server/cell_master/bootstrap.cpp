#include "bootstrap.h"
#include "private.h"
#include "config.h"
#include "hydra_facade.h"
#include "world_initializer.h"
#include "multicell_manager.h"

#include <yt/server/chunk_server/chunk_manager.h>
#include <yt/server/chunk_server/chunk_service.h>
#include <yt/server/chunk_server/cypress_integration.h>
#include <yt/server/chunk_server/job_tracker_service.h>

#include <yt/server/cypress_server/cypress_integration.h>
#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/file_server/file_node.h>

#include <yt/server/hive/hive_manager.h>
#include <yt/server/hive/transaction_manager.h>
#include <yt/server/hive/transaction_supervisor.h>
#include <yt/server/hive/cell_directory_synchronizer.h>

#include <yt/server/hydra/changelog.h>
#include <yt/server/hydra/file_snapshot_store.h>
#include <yt/server/hydra/local_changelog_store.h>
#include <yt/server/hydra/local_snapshot_service.h>
#include <yt/server/hydra/local_snapshot_store.h>
#include <yt/server/hydra/snapshot.h>

#include <yt/server/journal_server/journal_node.h>
#include <yt/server/journal_server/journal_manager.h>

#include <yt/server/misc/build_attributes.h>

#include <yt/server/node_tracker_server/cypress_integration.h>
#include <yt/server/node_tracker_server/node_tracker.h>
#include <yt/server/node_tracker_server/node_tracker_service.h>

#include <yt/server/object_server/object_manager.h>
#include <yt/server/object_server/object_service.h>
#include <yt/server/object_server/sys_node.h>

#include <yt/server/orchid/cypress_integration.h>

#include <yt/server/security_server/cypress_integration.h>
#include <yt/server/security_server/security_manager.h>

#include <yt/server/table_server/table_node.h>

#include <yt/server/tablet_server/cypress_integration.h>
#include <yt/server/tablet_server/tablet_manager.h>

#include <yt/server/transaction_server/cypress_integration.h>
#include <yt/server/transaction_server/timestamp_manager.h>
#include <yt/server/transaction_server/transaction_manager.h>

#include <yt/server/election/election_manager.h>

#include <yt/ytlib/election/cell_manager.h>

#include <yt/ytlib/hive/cell_directory.h>

#include <yt/ytlib/monitoring/http_integration.h>
#include <yt/ytlib/monitoring/monitoring_manager.h>

#include <yt/ytlib/orchid/orchid_service.h>

#include <yt/ytlib/transaction_client/remote_timestamp_provider.h>
#include <yt/ytlib/transaction_client/timestamp_provider.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/bus/config.h>
#include <yt/core/bus/server.h>
#include <yt/core/bus/tcp_server.h>

#include <yt/core/misc/ref_counted_tracker.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/rpc/bus_channel.h>
#include <yt/core/rpc/bus_server.h>
#include <yt/core/rpc/server.h>

#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/tree_builder.h>
#include <yt/core/ytree/virtual.h>
#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/ypath_service.h>

namespace NYT {
namespace NCellMaster {

using namespace NBus;
using namespace NRpc;
using namespace NYTree;
using namespace NElection;
using namespace NHydra;
using namespace NHive;
using namespace NNodeTrackerServer;
using namespace NTransactionServer;
using namespace NChunkServer;
using namespace NJournalServer;
using namespace NObjectServer;
using namespace NObjectClient;
using namespace NCypressServer;
using namespace NMonitoring;
using namespace NOrchid;
using namespace NProfiling;
using namespace NConcurrency;
using namespace NFileServer;
using namespace NTableServer;
using namespace NJournalServer;
using namespace NSecurityServer;
using namespace NTabletServer;

using NElection::TCellId;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CellMasterLogger;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(INodePtr configNode)
    : ConfigNode_(configNode)
{ }

// Neither remove it nor move it to the header.
TBootstrap::~TBootstrap()
{ }

TCellMasterConfigPtr TBootstrap::GetConfig() const
{
    return Config_;
}

bool TBootstrap::IsPrimaryMaster() const
{
    return PrimaryMaster_;
}

bool TBootstrap::IsSecondaryMaster() const
{
    return SecondaryMaster_;
}

bool TBootstrap::IsMulticell() const
{
    return Multicell_;
}

const TCellId& TBootstrap::GetCellId() const
{
    return CellId_;
}

TCellId TBootstrap::GetCellId(TCellTag cellTag) const
{
    return ReplaceCellTagInId(PrimaryCellId_, cellTag);
}

TCellTag TBootstrap::GetCellTag() const
{
    return CellTag_;
}

const TCellId& TBootstrap::GetPrimaryCellId() const
{
    return PrimaryCellId_;
}

TCellTag TBootstrap::GetPrimaryCellTag() const
{
    return PrimaryCellTag_;
}

const TCellTagList& TBootstrap::GetSecondaryCellTags() const
{
    return SecondaryCellTags_;
}

TMulticellManagerPtr TBootstrap::GetMulticellManager() const
{
    return MulticellManager_;
}

IServerPtr TBootstrap::GetRpcServer() const
{
    return RpcServer_;
}

TCellManagerPtr TBootstrap::GetCellManager() const
{
    return CellManager_;
}

IChangelogStoreFactoryPtr TBootstrap::GetChangelogStoreFactory() const
{
    return ChangelogStoreFactory_;
}

ISnapshotStorePtr TBootstrap::GetSnapshotStore() const
{
    return SnapshotStore_;
}

TNodeTrackerPtr TBootstrap::GetNodeTracker() const
{
    return NodeTracker_;
}

TTransactionManagerPtr TBootstrap::GetTransactionManager() const
{
    return TransactionManager_;
}

TTransactionSupervisorPtr TBootstrap::GetTransactionSupervisor() const
{
    return TransactionSupervisor_;
}

TCypressManagerPtr TBootstrap::GetCypressManager() const
{
    return CypressManager_;
}

THydraFacadePtr TBootstrap::GetHydraFacade() const
{
    return HydraFacade_;
}

TWorldInitializerPtr TBootstrap::GetWorldInitializer() const
{
    return WorldInitializer_;
}

TObjectManagerPtr TBootstrap::GetObjectManager() const
{
    return ObjectManager_;
}

TChunkManagerPtr TBootstrap::GetChunkManager() const
{
    return ChunkManager_;
}

NJournalServer::TJournalManagerPtr TBootstrap::GetJournalManager() const
{
    return JournalManager_;
}

TSecurityManagerPtr TBootstrap::GetSecurityManager() const
{
    return SecurityManager_;
}

TTabletManagerPtr TBootstrap::GetTabletManager() const
{
    return TabletManager_;
}

THiveManagerPtr TBootstrap::GetHiveManager() const
{
    return HiveManager_;
}

TCellDirectoryPtr TBootstrap::GetCellDirectory() const
{
    return CellDirectory_;
}

IInvokerPtr TBootstrap::GetControlInvoker() const
{
    return ControlQueue_->GetInvoker();
}

void TBootstrap::Initialize()
{
    srand(time(nullptr));

    ControlQueue_ = New<TActionQueue>("Control");

    BIND(&TBootstrap::DoInitialize, this)
        .AsyncVia(GetControlInvoker())
        .Run()
        .Get()
        .ThrowOnError();
}

void TBootstrap::Run()
{
    BIND(&TBootstrap::DoRun, this)
        .AsyncVia(GetControlInvoker())
        .Run()
        .Get()
        .ThrowOnError();
    Sleep(TDuration::Max());
}

void TBootstrap::TryLoadSnapshot(const Stroka& fileName, bool dump)
{
    BIND(&TBootstrap::DoLoadSnapshot, this, fileName, dump)
        .AsyncVia(HydraFacade_->GetAutomatonInvoker())
        .Run()
        .Get()
        .ThrowOnError();
    _exit(0);
}

TPeerId TBootstrap::ComputePeerId(TCellConfigPtr config, const Stroka& localAddress)
{
    for (TPeerId id = 0; id < config->Peers.size(); ++id) {
        const auto& peerAddress = config->Peers[id].Address;
        if (peerAddress && to_lower(*peerAddress) == to_lower(localAddress)) {
            return id;
        }
    }
    return InvalidPeerId;
}

void TBootstrap::DoInitialize()
{
    try {
        Config_ = ConvertTo<TCellMasterConfigPtr>(ConfigNode_);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing cell master configuration")
            << ex;
    }
    
    Config_->PrimaryMaster->ValidateAllPeersPresent();
    for (auto cellConfig : Config_->SecondaryMasters) {
        cellConfig->ValidateAllPeersPresent();
    }

    auto localAddress = BuildServiceAddress(
        TAddressResolver::Get()->GetLocalHostName(),
        Config_->RpcPort);

    TCellConfigPtr localCellConfig;
    TPeerId localPeerId;

    auto primaryId = ComputePeerId(Config_->PrimaryMaster, localAddress);
    if (primaryId == InvalidPeerId) {
        for (auto cellConfig : Config_->SecondaryMasters) {
            auto secondaryId = ComputePeerId(cellConfig, localAddress);
            if (secondaryId != InvalidPeerId) {
                SecondaryMaster_ = true;
                localCellConfig = cellConfig;
                localPeerId = secondaryId;
                break;
            }
        }
    } else {
        PrimaryMaster_ = true;
        localCellConfig = Config_->PrimaryMaster;
        localPeerId = primaryId;
    }

    if (!PrimaryMaster_ && !SecondaryMaster_) {
        THROW_ERROR_EXCEPTION("Local address %v is not recognized as a valid master address",
            localAddress);
    }

    Multicell_ = !Config_->SecondaryMasters.empty();

    CellId_ = localCellConfig->CellId;
    CellTag_ = CellTagFromId(CellId_);

    PrimaryCellId_ = Config_->PrimaryMaster->CellId;
    PrimaryCellTag_ = CellTagFromId(PrimaryCellId_);

    for (const auto& cellConfig : Config_->SecondaryMasters) {
        SecondaryCellTags_.push_back(CellTagFromId(cellConfig->CellId));
    }

    if (PrimaryMaster_) {
        LOG_INFO("Running as primary master (CellId: %v, CellTag: %v, SecondaryCellTags: %v, PeerId: %v)",
            CellId_,
            CellTag_,
            SecondaryCellTags_,
            localPeerId);
    } else if (SecondaryMaster_) {
        LOG_INFO("Running as secondary master (CellId: %v, CellTag: %v, PrimaryCellTag: %v, PeerId: %v)",
            CellId_,
            CellTag_,
            PrimaryCellTag_,
            localPeerId);
    }

    CellDirectory_ = New<TCellDirectory>(
        Config_->CellDirectory,
        GetBusChannelFactory(),
        NNodeTrackerClient::InterconnectNetworkName);

    YCHECK(CellDirectory_->ReconfigureCell(Config_->PrimaryMaster));
    for (const auto& cellConfig : Config_->SecondaryMasters) {
        YCHECK(CellDirectory_->ReconfigureCell(cellConfig));
    }

    HttpServer_.reset(new NHttp::TServer(Config_->MonitoringPort));

    auto busServerConfig = TTcpBusServerConfig::CreateTcp(Config_->RpcPort);
    auto busServer = CreateTcpBusServer(busServerConfig);

    RpcServer_ = CreateBusServer(busServer);

    CellManager_ = New<TCellManager>(
        localCellConfig,
        GetBusChannelFactory(),
        localPeerId);

    ChangelogStoreFactory_ = CreateLocalChangelogStoreFactory(
        "ChangelogFlush",
        Config_->Changelogs);

    auto fileSnapshotStore = New<TFileSnapshotStore>(
        Config_->Snapshots);

    SnapshotStore_ = CreateLocalSnapshotStore(
        Config_->HydraManager,
        CellManager_,
        fileSnapshotStore);

    HydraFacade_ = New<THydraFacade>(Config_, this);

    MulticellManager_ = New<TMulticellManager>(Config_->MulticellManager, this);

    WorldInitializer_ = New<TWorldInitializer>(Config_, this);

    if (SecondaryMaster_) {
        CellDirectorySynchronizer_ = New<TCellDirectorySynchronizer>(
            Config_->CellDirectorySynchronizer,
            CellDirectory_,
            PrimaryCellId_);
    }

    HiveManager_ = New<THiveManager>(
        Config_->HiveManager,
        CellDirectory_,
        CellId_,
        HydraFacade_->GetAutomatonInvoker(EAutomatonThreadQueue::RpcService),
        HydraFacade_->GetHydraManager(),
        HydraFacade_->GetAutomaton());

    // NB: This is exactly the order in which parts get registered and there are some
    // dependencies in Clear methods.
    ObjectManager_ = New<TObjectManager>(Config_->ObjectManager, this);

    SecurityManager_ = New<TSecurityManager>(Config_->SecurityManager, this);

    TransactionManager_ = New<TTransactionManager>(Config_->TransactionManager, this);

    NodeTracker_ = New<TNodeTracker>(Config_->NodeTracker, this);

    CypressManager_ = New<TCypressManager>(Config_->CypressManager, this);

    ChunkManager_ = New<TChunkManager>(Config_->ChunkManager, this);

    JournalManager_ = New<NJournalServer::TJournalManager>(Config_->JournalManager, this);

    TabletManager_ = New<TTabletManager>(Config_->TabletManager, this);

    auto timestampManager = New<TTimestampManager>(
        Config_->TimestampManager,
        HydraFacade_->GetAutomatonInvoker(),
        HydraFacade_->GetHydraManager(),
        HydraFacade_->GetAutomaton());

    auto timestampProvider = CreateRemoteTimestampProvider(
        Config_->TimestampProvider,
        GetBusChannelFactory());

    TransactionSupervisor_ = New<TTransactionSupervisor>(
        Config_->TransactionSupervisor,
        HydraFacade_->GetAutomatonInvoker(EAutomatonThreadQueue::RpcService),
        HydraFacade_->GetHydraManager(),
        HydraFacade_->GetAutomaton(),
        HydraFacade_->GetResponseKeeper(),
        HiveManager_,
        TransactionManager_,
        timestampProvider);

    fileSnapshotStore->Initialize();
    ObjectManager_->Initialize();
    SecurityManager_->Initialize();
    TransactionManager_->Initialize();
    NodeTracker_->Initialize();
    CypressManager_->Initialize();
    ChunkManager_->Initialize();
    TabletManager_->Initialize();
    if (CellDirectorySynchronizer_) {
        CellDirectorySynchronizer_->Start();
    }

    MonitoringManager_ = New<TMonitoringManager>();
    MonitoringManager_->Register(
        "/ref_counted",
        TRefCountedTracker::Get()->GetMonitoringProducer());
    MonitoringManager_->Register(
        "/hydra",
        HydraFacade_->GetHydraManager()->GetMonitoringProducer());
    MonitoringManager_->Register(
        "/election",
        HydraFacade_->GetElectionManager()->GetMonitoringProducer());

    auto orchidFactory = GetEphemeralNodeFactory();
    auto orchidRoot = orchidFactory->CreateMap();
    SetNodeByYPath(
        orchidRoot,
        "/monitoring",
        CreateVirtualNode(MonitoringManager_->GetService()));
    SetNodeByYPath(
        orchidRoot,
        "/profiling",
        CreateVirtualNode(TProfileManager::Get()->GetService()));
    SetNodeByYPath(
        orchidRoot,
        "/config",
        ConfigNode_);

    SetBuildAttributes(orchidRoot, "master");

    RpcServer_->RegisterService(CreateOrchidService(orchidRoot, GetControlInvoker())); // null realm
    RpcServer_->RegisterService(timestampManager->GetRpcService()); // null realm
    RpcServer_->RegisterService(HiveManager_->GetRpcService()); // cell realm
    RpcServer_->RegisterService(TransactionSupervisor_->GetRpcService()); // cell realm
    RpcServer_->RegisterService(New<TLocalSnapshotService>(CellId_, fileSnapshotStore)); // cell realm
    RpcServer_->RegisterService(CreateNodeTrackerService(Config_->NodeTracker, this)); // master hydra service
    RpcServer_->RegisterService(CreateObjectService(this)); // master hydra service
    RpcServer_->RegisterService(CreateJobTrackerService(this)); // master hydra service
    RpcServer_->RegisterService(CreateChunkService(this)); // master hydra service

    CypressManager_->RegisterHandler(CreateSysNodeTypeHandler(this));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::ChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::LostChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::LostVitalChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::UnderreplicatedChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::OverreplicatedChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::DataMissingChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::ParityMissingChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::QuorumMissingChunkMap));
    CypressManager_->RegisterHandler(CreateChunkMapTypeHandler(this, EObjectType::UnsafelyPlacedChunkMap));
    CypressManager_->RegisterHandler(CreateChunkListMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateTransactionMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateTopmostTransactionMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateLockMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateOrchidTypeHandler(this));
    CypressManager_->RegisterHandler(CreateClusterNodeNodeTypeHandler(this));
    CypressManager_->RegisterHandler(CreateClusterNodeMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateRackMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateFileTypeHandler(this));
    CypressManager_->RegisterHandler(CreateTableTypeHandler(this));
    CypressManager_->RegisterHandler(CreateJournalTypeHandler(this));
    CypressManager_->RegisterHandler(CreateAccountMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateUserMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateGroupMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateTabletCellNodeTypeHandler(this));
    CypressManager_->RegisterHandler(CreateTabletMapTypeHandler(this));
    CypressManager_->RegisterHandler(CreateTabletCellBundleMapTypeHandler(this));

    HttpServer_->Register(
        "/orchid",
        NMonitoring::GetYPathHttpHandler(orchidRoot->Via(GetControlInvoker())));

    RpcServer_->Configure(Config_->RpcServer);
}

void TBootstrap::DoRun()
{
    HydraFacade_->Initialize();

    MonitoringManager_->Start();

    LOG_INFO("Listening for HTTP requests on port %v", Config_->MonitoringPort);
    HttpServer_->Start();

    LOG_INFO("Listening for RPC requests on port %v", Config_->RpcPort);
    RpcServer_->Start();
}

void TBootstrap::DoLoadSnapshot(const Stroka& fileName, bool dump)
{
    auto reader = CreateFileSnapshotReader(fileName, InvalidSegmentId, false);
    HydraFacade_->LoadSnapshot(reader, dump);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
