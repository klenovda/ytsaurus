#include "chunk_list_pool.h"
#include "private.h"
#include "config.h"

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/api/client.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/rpc/helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NCypressClient;
using namespace NObjectClient;
using namespace NTransactionClient;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NApi;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

TChunkListPool::TChunkListPool(
    TSchedulerConfigPtr config,
    INativeClientPtr client,
    IInvokerPtr controllerInvoker,
    const TOperationId& operationId,
    const TTransactionId& transactionId)
    : Config_(config)
    , Client_(client)
    , ControllerInvoker_(controllerInvoker)
    , OperationId_(operationId)
    , TransactionId_(transactionId)
    , ChunkListReleaseExecutor_(New<TPeriodicExecutor>(
        controllerInvoker,
        BIND(&TChunkListPool::Release, MakeWeak(this), std::vector<TChunkListId>()),
        Config_->ChunkListReleaseBatchDelay))
    , Logger(OperationLogger)
{
    YCHECK(config);
    YCHECK(client);
    YCHECK(controllerInvoker);

    Logger.AddTag("OperationId: %v", operationId);
}

bool TChunkListPool::HasEnough(TCellTag cellTag, int requestedCount)
{
    VERIFY_INVOKER_AFFINITY(ControllerInvoker_);

    auto& data = CellMap_[cellTag];
    int currentSize = static_cast<int>(data.Ids.size());
    if (currentSize >= requestedCount + Config_->ChunkListWatermarkCount) {
        // Enough chunk lists. Above the watermark even after extraction.
        return true;
    } else {
        // Additional chunk lists are definitely needed but still could be a success.
        AllocateMore(cellTag);
        return currentSize >= requestedCount;
    }
}

TChunkListId TChunkListPool::Extract(TCellTag cellTag)
{
    VERIFY_INVOKER_AFFINITY(ControllerInvoker_);

    auto& data = CellMap_[cellTag];

    YCHECK(!data.Ids.empty());
    auto id = data.Ids.back();
    data.Ids.pop_back();

    LOG_DEBUG("Chunk list extracted from pool (ChunkListId: %v, CellTag: %v, RemainingCount: %v)",
        id,
        cellTag,
        data.Ids.size());

    return id;
}

void TChunkListPool::Reinstall(const TChunkListId& id)
{
    auto cellTag = CellTagFromId(id);
    auto& data = CellMap_[cellTag];
    data.Ids.push_back(id);
    LOG_DEBUG("Reinstalled chunk list into the pool (ChunkListId: %v, CellTag: %v, RemainingCount: %v)",
        id,
        cellTag,
        static_cast<int>(data.Ids.size()));
}

void TChunkListPool::Release(const std::vector<TChunkListId>& ids)
{
    VERIFY_INVOKER_AFFINITY(ControllerInvoker_);

    for (const auto& id : ids) {
        ChunksToRelease_[CellTagFromId(id)].push_back(id);
    }

    if (ChunksToRelease_.empty()) {
        return;
    }

    auto now = TInstant::Now();
    bool delayElapsed = LastReleaseTime_ + Config_->ChunkListReleaseBatchDelay > now;

    int count = 0;
    for (const auto& pair : ChunksToRelease_) {
        count += pair.second.size();
    }
    bool sizeExceeded = count >= Config_->DesiredChunkListsPerRelease;

    if (!delayElapsed && !sizeExceeded) {
        return;
    }

    LastReleaseTime_ = now;
    for (const auto& pair : ChunksToRelease_) {
        auto cellTag = pair.first;
        const auto& ids = pair.second;

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
        TChunkServiceProxy proxy(channel);

        int index = 0;
        while (index < ids.size()) {
            auto batchReq = proxy.ExecuteBatch();
            for (int i = 0; i < Config_->DesiredChunkListsPerRelease && index < ids.size(); ++i) {
                auto req = batchReq->add_unstage_chunk_tree_subrequests();
                ToProto(req->mutable_chunk_tree_id(), ids[index]);
                req->set_recursive(true);
                ++index;
            }

            // Fire-and-forget.
            // The subscriber is only needed to log the outcome.
            batchReq->Invoke().Subscribe(
                BIND(&TChunkListPool::OnChunkListsReleased, MakeStrong(this), cellTag)
                    .Via(ControllerInvoker_));
        }
    }

    ChunksToRelease_.clear();
}

void TChunkListPool::AllocateMore(TCellTag cellTag)
{
    auto& data = CellMap_[cellTag];

    int count = data.LastSuccessCount < 0
        ? Config_->ChunkListPreallocationCount
        : static_cast<int>(data.LastSuccessCount * Config_->ChunkListAllocationMultiplier);

    count = std::min(count, Config_->MaxChunkListAllocationCount);

    if (data.RequestInProgress) {
        LOG_DEBUG("Cannot allocate more chunk lists for pool, another request is in progress (CellTag: %v)",
            cellTag);
        return;
    }

    LOG_INFO("Allocating more chunk lists for pool (CellTag: %v, Count: %v)",
        cellTag,
        count);

    auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
    TChunkServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();
    GenerateMutationId(batchReq);

    auto req = batchReq->add_create_chunk_lists_subrequests();
    ToProto(req->mutable_transaction_id(), TransactionId_);
    req->set_count(count);

    data.RequestInProgress = true;

    batchReq->Invoke().Subscribe(
        BIND(&TChunkListPool::OnChunkListsCreated, MakeWeak(this), cellTag)
            .Via(ControllerInvoker_));
}

void TChunkListPool::OnChunkListsCreated(
    TCellTag cellTag,
    const TChunkServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
{
    auto& data = CellMap_[cellTag];

    YCHECK(data.RequestInProgress);
    data.RequestInProgress = false;

    auto error = GetCumulativeError(batchRspOrError);
    if (!error.IsOK()) {
        LOG_ERROR(batchRspOrError, "Error allocating chunk lists for pool (CellTag: %v)",
            cellTag);
        return;
    }

    const auto& batchRsp = batchRspOrError.Value();
    const auto& rsp = batchRsp->create_chunk_lists_subresponses(0);
    auto ids = FromProto<std::vector<TChunkListId>>(rsp.chunk_list_ids());
    data.Ids.insert(data.Ids.end(), ids.begin(), ids.end());
    data.LastSuccessCount = ids.size();

    LOG_INFO("Allocated more chunk lists for pool (CellTag: %v, Count: %v)",
        cellTag,
        data.LastSuccessCount);
}

void TChunkListPool::OnChunkListsReleased(
    TCellTag cellTag,
    const TChunkServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
{
    // NB: We only look at the topmost error and ignore subresponses.
    if (!batchRspOrError.IsOK()) {
        LOG_WARNING(batchRspOrError, "Error releasing chunk lists from pool (CellTag: %v)",
            cellTag);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
