#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/config.h>

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/cell_scheduler/bootstrap.h>
#include <yt/server/cell_scheduler/config.h>

#include <yt/server/job_proxy/job_proxy.h>

#include <yt/server/hydra/private.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/chunk_client/dispatcher.h>

#include <yt/ytlib/monitoring/http_server.h>
#include <yt/ytlib/monitoring/monitoring_manager.h>

#include <yt/ytlib/scheduler/config.h>

#include <yt/ytlib/shutdown.h>

#include <yt/build/build.h>

#include <yt/core/logging/config.h>
#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/crash_handler.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/address.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/tools/tools.h>

#include <yt/core/tracing/trace_manager.h>

#include <util/folder/dirut.h>

#include <util/system/sigset.h>

#include <contrib/tclap/yt_helpers.h>

#ifdef _unix_
    #include <sys/resource.h>
#endif
#ifdef _linux_
    #include <grp.h>
#endif

namespace NYT {

using namespace NYTree;
using namespace NYson;
using namespace NElection;
using namespace NScheduler;
using namespace NJobProxy;
using namespace NTools;

////////////////////////////////////////////////////////////////////////////////

static NLogging::TLogger Logger("Server");

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EExitCode,
    ((OK)(0))
    ((OptionsError)(1))
    ((BootstrapError)(2))
    ((ExecutorError)(3))
);

////////////////////////////////////////////////////////////////////////////////

struct TArgsParser
{
public:
    TArgsParser()
        : CmdLine("Command line", ' ', GetVersion())
        , WorkingDirectory("", "working-dir", "working directory", false, "", "DIR")
        , Config("", "config", "configuration file", false, "", "FILE")
        , ConfigTemplate("", "config-template", "print configuration file template")
        , Node("", "node", "start cell node")
        , Master("", "master", "start cell master")
        , DumpMasterSnapshot("", "dump-master-snapshot", "load a given master snapshot and dump its content to stderr", false, "", "FILE")
        , ValidateMasterSnapshot("", "validate-master-snapshot", "validate a given master snapshot", false, "", "FILE")
        , Scheduler("", "scheduler", "start scheduler")
        , JobProxy("", "job-proxy", "start job proxy")
        , JobId("", "job-id", "job id (for job proxy mode)", false, "", "ID")
        , OperationId("", "operation-id", "operation id (for job proxy mode)", false, "", "ID")
#ifdef _unix_
        , Tool("", "tool", "tool id", false, "", "ID")
        , Spec("", "spec", "tool spec", false, "", "SPEC")
#ifdef _linux_
        , CGroups("", "cgroup", "run in cgroup", false, "")
#endif
        , Executor("", "executor", "start a user job")
        , Shell("", "shell", "start a shell in job sandbox, agrument is FD for PTY", false, -1, "NUM")
        , PreparePipes("", "prepare-pipe", "prepare pipe descriptor  (for executor mode)", false, "FD")
        , EnableCoreDump("", "enable-core-dump", "enable core dump (for executor mode)")
        , Uid("", "uid", "set uid  (for executor and shell mode)", false, -1, "NUM")
        , Environment("", "env", "set environment variable  (for executor and shell mode)", false, "ENV")
        , Command("", "command", "command (for executor mode)", false, "", "COMMAND")
#endif
    {
        CmdLine.add(WorkingDirectory);
        CmdLine.add(Config);
        CmdLine.add(ConfigTemplate);
        CmdLine.add(Node);
        CmdLine.add(Master);
        CmdLine.add(DumpMasterSnapshot);
        CmdLine.add(ValidateMasterSnapshot);
        CmdLine.add(Scheduler);
        CmdLine.add(JobProxy);
        CmdLine.add(JobId);
        CmdLine.add(OperationId);
#ifdef _unix_
        CmdLine.add(Tool);
        CmdLine.add(Spec);
#ifdef _linux_
        CmdLine.add(CGroups);
#endif
        CmdLine.add(Executor);
        CmdLine.add(Shell);
        CmdLine.add(PreparePipes);
        CmdLine.add(EnableCoreDump);
        CmdLine.add(Uid);
        CmdLine.add(Environment);
        CmdLine.add(Command);
#endif
    }

    TCLAP::CmdLine CmdLine;

    TCLAP::ValueArg<Stroka> WorkingDirectory;
    TCLAP::ValueArg<Stroka> Config;
    TCLAP::SwitchArg ConfigTemplate;
    TCLAP::SwitchArg Node;
    TCLAP::SwitchArg Master;
    TCLAP::ValueArg<Stroka> DumpMasterSnapshot;
    TCLAP::ValueArg<Stroka> ValidateMasterSnapshot;
    TCLAP::SwitchArg Scheduler;
    TCLAP::SwitchArg JobProxy;
    TCLAP::ValueArg<Stroka> JobId;
    TCLAP::ValueArg<Stroka> OperationId;

#ifdef _unix_
    TCLAP::ValueArg<Stroka> Tool;
    TCLAP::ValueArg<Stroka> Spec;
#ifdef _linux_
    TCLAP::MultiArg<Stroka> CGroups;
#endif
    TCLAP::SwitchArg Executor;
    TCLAP::ValueArg<int> Shell;
    TCLAP::MultiArg<int> PreparePipes;
    TCLAP::SwitchArg EnableCoreDump;
    TCLAP::ValueArg<int> Uid;
    TCLAP::MultiArg<Stroka> Environment;
    TCLAP::ValueArg<Stroka> Command;
#endif

};

////////////////////////////////////////////////////////////////////////////////

EExitCode GuardedMain(int argc, const char* argv[])
{
    TThread::CurrentThreadSetName("Bootstrap");

    srand(time(nullptr));

    TArgsParser parser;
    parser.CmdLine.parse(argc, argv);

    // Figure out the mode: cell master, cell node, scheduler or job proxy.
    bool isMaster = parser.Master.getValue();
    bool isMasterSnapshotDump = parser.DumpMasterSnapshot.isSet();
    bool isMasterSnapshotValidate = parser.ValidateMasterSnapshot.isSet();
    bool isNode = parser.Node.getValue();
    bool isScheduler = parser.Scheduler.getValue();
    bool isJobProxy = parser.JobProxy.getValue();

#ifdef _unix_
    Stroka toolName = parser.Tool.getValue();
    bool isExecutor = parser.Executor.getValue();
    bool isShell = parser.Shell.isSet();
#endif

    bool printConfigTemplate = parser.ConfigTemplate.getValue();

    Stroka configFileName = parser.Config.getValue();

    Stroka workingDirectory = parser.WorkingDirectory.getValue();

    int modeCount = 0;
    if (isNode) {
        ++modeCount;
    }
    if (isMaster) {
        ++modeCount;
    }
    if (isMasterSnapshotDump) {
        ++modeCount;
    }
    if (isMasterSnapshotValidate) {
        ++modeCount;
    }
    if (isScheduler) {
        ++modeCount;
    }
    if (isJobProxy) {
        ++modeCount;
    }

#ifdef _unix_
    if (!toolName.Empty()) {
        ++modeCount;
    }
    if (isExecutor) {
        ++modeCount;
    }
    if (isShell) {
        ++modeCount;
    }
#endif

    if (modeCount != 1) {
        TCLAP::StdOutput().usage(parser.CmdLine);
        return EExitCode::OptionsError;
    }

    if (!workingDirectory.empty()) {
        NFs::SetCurrentWorkingDirectory(workingDirectory);
    }

    if (isJobProxy) {
        CloseAllDescriptors();
        CreateStderrFile("stderr");
    }

#ifdef _unix_
    if (!toolName.Empty()) {
        NYson::TYsonString spec(parser.Spec.getValue());
        auto result = ExecuteTool(toolName, spec);
        Cout << result.Data();
        // NB: no shutdown, some initialization may still be in progress.
        Cout.Flush();
        _exit(static_cast<int>(EExitCode::OK));
    }
#endif

    INodePtr configNode;

    if (isExecutor || isShell) {
        // Don't start any other singleton or parse config in executor mode.
        NLogging::TLogManager::Get()->Configure(NLogging::TLogConfig::CreateQuiet());
    } else if (!printConfigTemplate) {
        if (configFileName.empty()) {
            THROW_ERROR_EXCEPTION("Missing --config option");
        }

        // Parse configuration file.
        try {
            TIFStream configStream(configFileName);
            configNode = ConvertToNode(&configStream);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing server configuration")
                << ex;
        }

        // Deserialize as a generic server config.
        auto genericConfig = New<TServerConfig>();
        genericConfig->Load(configNode);

        // Configure singletons.

        if (!isMasterSnapshotDump && !isMasterSnapshotValidate) {
            NLogging::TLogManager::Get()->Configure(configFileName, "/logging");
        } else {
            NLogging::TLogManager::Get()->Configure(NLogging::TLogConfig::CreateQuiet());
        }

        TAddressResolver::Get()->Configure(genericConfig->AddressResolver);
        if (!TAddressResolver::Get()->IsLocalHostNameOK()) {
            THROW_ERROR_EXCEPTION("Could not determine the local host FQDN");
        }

        NChunkClient::TDispatcher::Get()->Configure(genericConfig->ChunkClientDispatcher);

        NTracing::TTraceManager::Get()->Configure(configFileName, "/tracing");

        NProfiling::TProfileManager::Get()->Start();
    }

#ifdef _linux_
    bool enableCGroups = true;
    if (isNode) {
        auto config = New<NCellNode::TCellNodeConfig>();

        try {
            config->Load(configNode);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing cell node configuration")
                << ex;
        }
        enableCGroups = config->ExecAgent->EnableCGroups;
    }

    auto cgroups = parser.CGroups.getValue();
    if (enableCGroups) {
        for (const auto& path : cgroups) {
            NCGroup::TNonOwningCGroup cgroup(path);
            cgroup.EnsureExistance();
            cgroup.AddCurrentTask();
        }
    } else {
        if (!cgroups.empty()) {
            LOG_WARNING("CGroups are explicitely disabled in config; ignoring --cgroup parameter");
        }
    }
#endif

#ifdef _unix_
    if (isExecutor) {
        TThread::CurrentThreadSetName("ExecutorMain");
        if (parser.Command.getValue().empty()) {
            THROW_ERROR_EXCEPTION("Missing or empty --command option");
        }

        const int permissions = S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH;
        for (auto fd : parser.PreparePipes.getValue()) {
            SetPermissions(fd, permissions);
        }

        if (!parser.EnableCoreDump.getValue()) {
            // Disable core dump for user jobs when option is not present.
            struct rlimit rlimit = {0, 0};

            auto res = setrlimit(RLIMIT_CORE, &rlimit);
            if (res) {
                auto errorMessage = Format("Failed to disable core dumps\n%v", TError::FromSystem());
                fprintf(stderr, "%s", errorMessage.c_str());
                return EExitCode::ExecutorError;
            }
        }
    }
    if (isShell) {
        TThread::CurrentThreadSetName("ShellMain");
        auto pty = parser.Shell.getValue();
        if (pty < 0) {
            THROW_ERROR_EXCEPTION("Invalid argument for --shell option");
        }
        CloseAllDescriptors({pty});
        setsid();
        SafeLoginTty(pty);
    }

    if (isExecutor || isShell) {
        auto uid = parser.Uid.getValue();
        if (uid > 0) {
            // Set unprivileged uid and gid for user process.
            YCHECK(setuid(0) == 0);
            YCHECK(setgroups(0, nullptr) == 0);

#ifdef _linux_
            YCHECK(setresgid(uid, uid, uid) == 0);
            YCHECK(setresuid(uid, uid, uid) == 0);
#else
            YCHECK(setgid(uid) == 0);
            YCHECK(setuid(uid) == 0);
#endif
        }

        std::vector<char*> env;
        for (auto envVar : parser.Environment.getValue()) {
            env.push_back(const_cast<char*>(envVar.c_str()));
        }
        env.push_back(const_cast<char*>("SHELL=/bin/bash"));
        env.push_back(nullptr);

        std::vector<const char*> args;
        args.push_back("/bin/bash");
        Stroka command;
        if (isExecutor) {
            // :; is added avoid fork/exec (oneshot) optimization
            command = ":; " + parser.Command.getValue();
            args.push_back("-c");
            args.push_back(command.c_str());
        }
        args.push_back(nullptr);

        TryExecve(
            "/bin/bash",
            args.data(),
            env.data());

        return EExitCode::ExecutorError;
    }
#endif

    // Start an appropriate server.
    if (isNode) {
        TThread::CurrentThreadSetName("NodeMain");

        if (printConfigTemplate) {
            auto config = New<NCellNode::TCellNodeConfig>();
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellNode::TBootstrap(configNode);
        bootstrap->Run();
    }

    if (isMaster || isMasterSnapshotDump || isMasterSnapshotValidate) {
        TThread::CurrentThreadSetName("MasterMain");

        if (printConfigTemplate) {
            auto config = New<NCellMaster::TCellMasterConfig>();
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellMaster::TBootstrap(configNode);
        bootstrap->Initialize();
        if (isMaster) {
            bootstrap->Run();
        } else if (isMasterSnapshotDump) {
            bootstrap->TryLoadSnapshot(parser.DumpMasterSnapshot.getValue(), true);
        } else if (isMasterSnapshotValidate) {
            bootstrap->TryLoadSnapshot(parser.ValidateMasterSnapshot.getValue(), false);
        } else {
            YUNREACHABLE();
        }
    }

    if (isScheduler) {
        TThread::CurrentThreadSetName("SchedulerMain");

        if (printConfigTemplate) {
            auto config = New<NCellScheduler::TCellSchedulerConfig>();
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellScheduler::TBootstrap(configNode);
        bootstrap->Run();
    }

    if (isJobProxy) {
        TThread::CurrentThreadSetName("JobProxyMain");

        if (printConfigTemplate) {
            auto config = New<NJobProxy::TJobProxyConfig>();
            TYsonWriter writer(&Cout, EYsonFormat::Pretty);
            config->Save(&writer);
            return EExitCode::OK;
        }

        TJobId jobId;
        try {
            jobId = TGuid::FromString(parser.JobId.getValue());
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing job id")
                << ex;
        }


        // NB: There are some cyclic references here:
        // JobProxy <-> Job
        // JobProxy <-> JobProberService
        // But we (currently) don't care.
        auto jobProxy = New<TJobProxy>(configNode, jobId);
        jobProxy->Run();

        // XXX(sandello): Job proxy does not shutdown cleanly (there are cyclic references holding some threads)
        // so we just wipe out the process.
        _exit(static_cast<int>(EExitCode::OK));
    }

    return EExitCode::OK;
}

EExitCode Main(int argc, const char* argv[])
{
    InstallCrashSignalHandler();

#ifdef _unix_
    sigset_t sigset;
    SigEmptySet(&sigset);
    SigAddSet(&sigset, SIGHUP);
    SigProcMask(SIG_BLOCK, &sigset, NULL);

    signal(SIGPIPE, SIG_IGN);

    uid_t ruid, euid;
#ifdef _linux_
    uid_t suid;
    YCHECK(getresuid(&ruid, &euid, &suid) == 0);
#else
    ruid = getuid();
    euid = geteuid();
#endif
    if (euid == 0) {
        YCHECK(setgroups(0, nullptr) == 0);
        // if effective uid == 0 (e. g. set-uid-root), make
        // saved = effective, effective = real
#ifdef _linux_
        YCHECK(setresuid(ruid, ruid, euid) == 0);
#else
        YCHECK(setuid(euid) == 0);
        YCHECK(seteuid(ruid) == 0);
        YCHECK(setruid(ruid) == 0);
#endif
    }
#endif /* _unix_ */

    EExitCode exitCode;
    try {
        exitCode = GuardedMain(argc, argv);
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Server startup failed");
        exitCode = EExitCode::BootstrapError;
    }

    NHydra::ShutdownHydraIOInvoker();
    Shutdown();

    return exitCode;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

int main(int argc, const char* argv[])
{
    return static_cast<int>(NYT::Main(argc, argv));
}
