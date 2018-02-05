#include "snapshot_downloader.h"

#include <yt/server/scheduler/config.h>

#include <yt/server/cell_scheduler/bootstrap.h>

#include <yt/ytlib/api/native_client.h>

#include <yt/ytlib/scheduler/helpers.h>

namespace NYT {
namespace NControllerAgent {

using namespace NApi;
using namespace NConcurrency;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

TSnapshotDownloader::TSnapshotDownloader(
    TControllerAgentConfigPtr config,
    NCellScheduler::TBootstrap* bootstrap,
    const TOperationId& operationId)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , OperationId_(operationId)
    , Logger(NLogging::TLogger(MasterConnectorLogger)
        .AddTag("OperationId: %v", operationId))
{
    YCHECK(Config_);
    YCHECK(Bootstrap_);
}

TSharedRef TSnapshotDownloader::Run(const NYTree::TYPath& snapshotPath)
{
    LOG_INFO("Starting downloading snapshot");

    const auto& client = Bootstrap_->GetMasterClient();

    TFileReaderOptions options;
    options.Config = Config_->SnapshotReader;

    auto reader = WaitFor(client->CreateFileReader(snapshotPath, options))
        .ValueOrThrow();

    LOG_INFO("Snapshot reader opened");

    std::vector<TSharedRef> blocks;
    while (true) {
        auto blockOrError = WaitFor(reader->Read());
        auto block = blockOrError.ValueOrThrow();
        if (!block)
            break;
        blocks.push_back(block);
    }

    LOG_INFO("Snapshot downloaded successfully");

    struct TSnapshotDataTag { };
    return MergeRefsToRef<TSnapshotDataTag>(blocks);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT
