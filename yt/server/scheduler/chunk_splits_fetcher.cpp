#include "stdafx.h"
#include "chunk_splits_fetcher.h"
#include "private.h"

#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/rpc/channel_cache.h>

#include <ytlib/misc/protobuf_helpers.h>

#include <ytlib/scheduler/config.h>

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NTableClient::NProto;

////////////////////////////////////////////////////////////////////

static NRpc::TChannelCache ChannelCache;

////////////////////////////////////////////////////////////////////

TChunkSplitsFetcher::TChunkSplitsFetcher(
    TSchedulerConfigPtr config,
    TMergeOperationSpecBasePtr spec,
    const TOperationId& operationId,
    const TKeyColumns& keyColumns)
    : Config(config)
    , Spec(spec)
    , KeyColumns(keyColumns)
    , Logger(OperationLogger)
{
    YCHECK(Config->MergeJobMaxSliceDataSize > 0);
    Logger.AddTag(Sprintf("OperationId: %s", ~ToString(operationId)));
}

NLog::TTaggedLogger& TChunkSplitsFetcher::GetLogger()
{
    return Logger;
}

void TChunkSplitsFetcher::Prepare(const std::vector<TRefCountedInputChunkPtr>& chunks)
{
    LOG_INFO("Started fetching chunk splits (ChunkCount: %d)",
        static_cast<int>(chunks.size()));
}

const std::vector<TRefCountedInputChunkPtr>& TChunkSplitsFetcher::GetChunkSplits()
{
    return ChunkSplits;
}

void TChunkSplitsFetcher::CreateNewRequest(const TNodeDescriptor& descriptor)
{
    auto channel = ChannelCache.GetChannel(descriptor.Address);
    auto retryingChannel = CreateRetryingChannel(Config->NodeChannel, channel);
    TDataNodeServiceProxy proxy(retryingChannel);
    proxy.SetDefaultTimeout(Config->NodeRpcTimeout);

    CurrentRequest = proxy.GetChunkSplits();
    CurrentRequest->set_min_split_size(Config->MergeJobMaxSliceDataSize);
    ToProto(CurrentRequest->mutable_key_columns(), KeyColumns);
}

bool TChunkSplitsFetcher::AddChunkToRequest(
    TNodeId nodeId,
    TRefCountedInputChunkPtr chunk)
{
    auto chunkId = EncodeChunkId(*chunk, nodeId);

    i64 dataSize;
    GetStatistics(*chunk, &dataSize);

    if (dataSize < Config->MergeJobMaxSliceDataSize) {
        LOG_DEBUG("Chunk split added (ChunkId: %s, TableIndex: %d)",
            ~ToString(chunkId),
            chunk->table_index());
        ChunkSplits.push_back(chunk);
        return false;
    } else {
        auto* requestChunk = CurrentRequest->add_input_chunks();
        *requestChunk = *chunk;
        // Makes sense for erasure chunks only.
        ToProto(requestChunk->mutable_chunk_id(), chunkId);
        return true;
    }
}

TFuture<TChunkSplitsFetcher::TResponsePtr> TChunkSplitsFetcher::InvokeRequest()
{
    auto req = CurrentRequest;
    CurrentRequest.Reset();
    return req->Invoke();
}

TError TChunkSplitsFetcher::ProcessResponseItem(
    TResponsePtr rsp,
    int index,
    TRefCountedInputChunkPtr inputChunk)
{
    YCHECK(rsp->IsOK());

    const auto& responseChunks = rsp->splitted_chunks(index);
    if (responseChunks.has_error()) {
        return FromProto(responseChunks.error());
    }

    LOG_TRACE("Received %d chunk splits for chunk #%d",
        responseChunks.input_chunks_size(),
        index);

    FOREACH (auto& responseChunk, responseChunks.input_chunks()) {
        auto split = New<TRefCountedInputChunk>(std::move(responseChunk));
        // Adjust chunk id (makes sense for erasure chunks only).
        auto chunkId = FromProto<TChunkId>(split->chunk_id());
        auto chunkIdWithIndex = DecodeChunkId(chunkId);
        ToProto(split->mutable_chunk_id(), chunkIdWithIndex.Id);
        split->set_table_index(inputChunk->table_index());
        ChunkSplits.push_back(split);
    }

    return TError();
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
