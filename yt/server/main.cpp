#include "stdafx.h"
#include "cell_master_bootstrap.h"
#include "scheduler_bootstrap.h"

#include <ytlib/misc/enum.h>
#include <ytlib/rpc/rpc_manager.h>
#include <ytlib/logging/log_manager.h>
#include <ytlib/ytree/serialize.h>
#include <ytlib/chunk_holder/bootstrap.h>

namespace NYT {

using namespace NLastGetopt;
using namespace NYTree;
using namespace NElection;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("Server");

DECLARE_ENUM(EExitCode,
    ((OK)(0))
    ((OptionsError)(1))
    ((BootstrapError)(2))
);

EExitCode GuardedMain(int argc, const char* argv[])
{
    // Configure options parser.
    TOpts opts;

    opts.AddHelpOption();

    const auto& chunkHolderOpt = opts.AddLongOption("chunk-holder", "start chunk holder")
        .NoArgument()
        .Optional();

    const auto& cellMasterOpt = opts.AddLongOption("cell-master", "start cell master")
        .NoArgument()
        .Optional();

    const auto& schedulerOpt = opts.AddLongOption("scheduler", "start scheduler")
        .NoArgument()
        .Optional();

    int port = -1;
    opts.AddLongOption("port", "port to listen")
        .Optional()
        .RequiredArgument("PORT")
        .StoreResult(&port);

    TPeerId peerId = InvalidPeerId;
    opts.AddLongOption("id", "peer id")
        .Optional()
        .RequiredArgument("ID")
        .StoreResult(&peerId);

    Stroka configFileName;
    opts.AddLongOption("config", "configuration file")
        .RequiredArgument("FILE")
        .StoreResult(&configFileName);

    TOptsParseResult results(&opts, argc, argv);

    // Figure out the mode: cell master or chunk holder.
    bool isCellMaster = results.Has(&cellMasterOpt);
    bool isChunkHolder = results.Has(&chunkHolderOpt);
    bool isScheduler = results.Has(&schedulerOpt);

    int modeCount = 0;
    if (isChunkHolder) {
        ++modeCount;
    }
    if (isCellMaster) {
        ++modeCount;
    }

    if (isScheduler) {
        ++modeCount;
    }

    if (modeCount != 1) {
        opts.PrintUsage(results.GetProgramName());
        return EExitCode::OptionsError;
    }

    // Configure logging.
    NLog::TLogManager::Get()->Configure(configFileName, "logging");

    // Parse configuration file.
    INodePtr configNode;
    try {
        TIFStream configStream(configFileName);
        configNode = DeserializeFromYson(&configStream);
    } catch (const std::exception& ex) {
        ythrow yexception() << Sprintf("Error reading server configuration\n%s",
            ex.what());
    }

    // Start an appropriate server.
    if (isChunkHolder) {
        auto config = New<NChunkHolder::TBootstrap::TConfig>();
        try {
            config->Load(~configNode);

            // Override RPC port.
            if (port >= 0) {
                config->RpcPort = port;
            }

            config->Validate();
        } catch (const std::exception& ex) {
            ythrow yexception() << Sprintf("Error parsing chunk holder configuration\n%s",
                ex.what());
        }


        NChunkHolder::TBootstrap bootstrap(configFileName, ~config);
        bootstrap.Run();
    }

    if (isCellMaster) {
        auto config = New<TCellMasterBootstrap::TConfig>();
        try {
            config->Load(~configNode);
            
            // Override peer id.
            if (peerId != InvalidPeerId) {
                config->MetaState->Cell->Id = peerId;
            }

            config->Validate();
        } catch (const std::exception& ex) {
            ythrow yexception() << Sprintf("Error parsing cell master configuration\n%s",
                ex.what());
        }

        TCellMasterBootstrap cellMasterBootstrap(configFileName, ~config);
        cellMasterBootstrap.Run();
    }

    if (isScheduler) {
        auto config = New<TSchedulerBootstrap::TConfig>();
        try {
            config->LoadAndValidate(~configNode);
        } catch (const std::exception& ex) {
            ythrow yexception() << Sprintf("Error parsing cell master configuration\n%s",
                ex.what());
        }

        TSchedulerBootstrap schedulerBootstrap(configFileName, ~config);
        schedulerBootstrap.Run();
    }

    // Actually this will never happen.
    return EExitCode::OK;
}

int Main(int argc, const char* argv[])
{
    int exitCode;
    try {
        exitCode = GuardedMain(argc, argv);
    }
    catch (const std::exception& ex) {
        LOG_ERROR("Server startup failed\n%s", ex.what());
        exitCode = EExitCode::BootstrapError;
    }

    // TODO: refactor system shutdown
    NLog::TLogManager::Get()->Shutdown();
    NRpc::TRpcManager::Get()->Shutdown();
    TDelayedInvoker::Shutdown();

    return exitCode;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

int main(int argc, const char* argv[])
{
    return NYT::Main(argc, argv);
}
