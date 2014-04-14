#include "stdafx.h"

#include "multi_chunk_reader_base.h"

#include "block_cache.h"
#include "chunk_meta_extensions.h"
#include "chunk_reader_base.h"
#include "chunk_spec.h"
#include "config.h"
#include "dispatcher.h"
#include "erasure_reader.h"
#include "private.h"
#include "replication_reader.h"

#include <ytlib/node_tracker_client/node_directory.h>

#include <core/concurrency/scheduler.h>
#include <core/concurrency/parallel_awaiter.h>

#include <core/erasure/codec.h>

#include <core/misc/protobuf_helpers.h>

namespace NYT {
namespace NChunkClient {

using namespace NConcurrency;
using namespace NErasure;
using namespace NProto;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

int CalculatePrefetchWindow(const std::vector<TChunkSpec>& sortedChunkSpecs, TMultiChunkReaderConfigPtr config)
{
    int prefetchWindow = 0;
    i64 bufferSize = 0;
    while (prefetchWindow < sortedChunkSpecs.size()) {
        auto& chunkSpec = sortedChunkSpecs[prefetchWindow];
        i64 currentSize;
        GetStatistics(chunkSpec, &currentSize);
        auto miscExt = GetProtoExtension<TMiscExt>(chunkSpec.chunk_meta().extensions());

        // block that possibly exceeds group size + block used by upper level chunk reader.
        i64 chunkBufferSize = ChunkReaderMemorySize + 2 * miscExt.max_block_size();

        if (currentSize > miscExt.max_block_size()) {
            chunkBufferSize += config->WindowSize + config->GroupSize;
        } 

        if (bufferSize + chunkBufferSize > config->MaxBufferSize) {
            break;
        } else {
            bufferSize += chunkBufferSize;
            ++prefetchWindow;
        }
    }
    // Don't allow overcommit during prefetching, so exclude the last chunk.
    prefetchWindow = std::max(prefetchWindow - 1, 0);
    prefetchWindow = std::min(prefetchWindow, MaxPrefetchWindow);
    return prefetchWindow;
}

////////////////////////////////////////////////////////////////////////////////

TNontemplateMultiChunkReaderBase::TNontemplateMultiChunkReaderBase(
    TMultiChunkReaderConfigPtr config,
    TMultiChunkReaderOptionsPtr options,
    NRpc::IChannelPtr masterChannel,
    IBlockCachePtr blockCache,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const std::vector<NProto::TChunkSpec>& chunkSpecs)
    : Logger(ChunkReaderLogger)
    , Options_(options)
    , ChunkSpecs_(chunkSpecs)
    , CompletionError_(NewPromise<TError>())
    , BlockCache_(blockCache)
    , MasterChannel_(masterChannel)
    , NodeDirectory_(nodeDirectory)
    , PrefetchReaderIndex_(0)
    , FetchingCompletedAwaiter_(New<TParallelAwaiter>(GetSyncInvoker()))
    , IsOpen_(false)
    , OpenedReaderCount_(0)
{
    Logger.AddTag(Sprintf("Reader: %p", this));

    Config_ = CloneYsonSerializable(config);

    CurrentSession_.Reset();

    LOG_DEBUG("Creating multi chunk reader for %d chunks", static_cast<int>(ChunkSpecs_.size()));

    if (ChunkSpecs_.empty()) {
        CompletionError_.Set(TError());
        return;
    }

    if (Options_->KeepInMemory) {
        PrefetchWindow_ = MaxPrefetchWindow;
    } else {
        auto sortedChunkSpecs = ChunkSpecs_;
        std::sort(
            sortedChunkSpecs.begin(), 
            sortedChunkSpecs.end(), 
            [] (const TChunkSpec& lhs, const TChunkSpec& rhs)
            {
                i64 lhsDataSize, rhsDataSize;
                GetStatistics(lhs, &lhsDataSize);
                GetStatistics(rhs, &rhsDataSize);

                return lhsDataSize > rhsDataSize;
            });

        i64 smallestDataSize;
        GetStatistics(sortedChunkSpecs.back(), &smallestDataSize);

        if (smallestDataSize < Config_->WindowSize + Config_->GroupSize) {
            // Here we limit real consumption to correspond the estimated.
            Config_->WindowSize = std::max(smallestDataSize / 2, (i64) 1);
            Config_->GroupSize = std::max(smallestDataSize / 2, (i64) 1);
        }

        PrefetchWindow_ = CalculatePrefetchWindow(sortedChunkSpecs, Config_);
    }

    LOG_DEBUG("Created multi chunk reader (PrefetchWindow: %d)", PrefetchWindow_);
}

TAsyncError TNontemplateMultiChunkReaderBase::Open()
{
    YCHECK(!IsOpen_);
    IsOpen_ = true;
    if (CompletionError_.IsSet())
        return CompletionError_.ToFuture();

    return BIND(&TNontemplateMultiChunkReaderBase::DoOpen, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run();
}

TAsyncError TNontemplateMultiChunkReaderBase::GetReadyEvent()
{
    return ReadyEvent_;
}

TDataStatistics TNontemplateMultiChunkReaderBase::GetDataStatistics() const
{
    TGuard<TSpinLock> guard(DataStatisticsLock_);
    auto dataStatistics = DataStatistics_;
    for (auto reader : ActiveReaders_) {
        dataStatistics += reader->GetDataStatistics();
    }
    return dataStatistics;
}

bool TNontemplateMultiChunkReaderBase::IsFetchingCompleted() const
{
    return FetchingCompletedAwaiter_->IsCompleted() &&
        (FetchingCompletedAwaiter_->GetRequestCount() == FetchingCompletedAwaiter_->GetResponseCount());
}

void TNontemplateMultiChunkReaderBase::OpenPrefetchChunks()
{
    for (int i = 0; i < PrefetchWindow_; ++i) {
        OpenNextChunk();
    }
}

void TNontemplateMultiChunkReaderBase::OpenNextChunk()
{
    BIND(
        &TNontemplateMultiChunkReaderBase::DoOpenNextChunk, 
        MakeWeak(this))
    .Via(TDispatcher::Get()->GetReaderInvoker())
    .Run();
}

void TNontemplateMultiChunkReaderBase::DoOpenNextChunk()
{
    if (CompletionError_.IsSet())
        return;

    if (PrefetchReaderIndex_ >= ChunkSpecs_.size())
        return;

    int chunkIndex = PrefetchReaderIndex_++;
    auto& chunkSpec = ChunkSpecs_[chunkIndex];

    LOG_DEBUG("Opening chunk (ChunkIndex: %d)", chunkIndex);
    auto remoteReader = CreateRemoteReader(chunkSpec);

    auto reader = CreateTemplateReader(chunkSpec, remoteReader);
    auto error = WaitFor(reader->Open());

    if (!error.IsOK()) {
        CompletionError_.TrySet(error);
        RegisterFailedChunk(chunkIndex);
        return;
    }

    OnReaderOpened(reader, chunkIndex);

    FetchingCompletedAwaiter_->Await(reader->GetFetchingCompletedEvent());
    if (++OpenedReaderCount_ == ChunkSpecs_.size()) {
        FetchingCompletedAwaiter_->Complete();
    }

    TGuard<TSpinLock> guard(DataStatisticsLock_);
    YCHECK(ActiveReaders_.insert(reader).second);
}

IAsyncReaderPtr TNontemplateMultiChunkReaderBase::CreateRemoteReader(const TChunkSpec& chunkSpec)
{
    auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
    auto replicas = NYT::FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());

    LOG_DEBUG("Creating remote reader (ChunkId: %s)", ~ToString(chunkId));

    if (!IsErasureChunkId(chunkId)) {
        return CreateReplicationReader(
            Config_,
            BlockCache_,
            MasterChannel_,
            NodeDirectory_,
            Null,
            chunkId,
            replicas);
    }
     
    std::sort(
        replicas.begin(),
        replicas.end(),
        [] (TChunkReplica lhs, TChunkReplica rhs) {
            return lhs.GetIndex() < rhs.GetIndex();
        });

    auto erasureCodecId = ECodec(chunkSpec.erasure_codec());
    auto* erasureCodec = GetCodec(erasureCodecId);
    auto dataPartCount = erasureCodec->GetDataPartCount();

    std::vector<IAsyncReaderPtr> readers;
    readers.reserve(dataPartCount);

    auto it = replicas.begin();
    while (it != replicas.end() && it->GetIndex() < dataPartCount) {
        auto jt = it;
        while (jt != replicas.end() && it->GetIndex() == jt->GetIndex()) {
            ++jt;
        }

        TChunkReplicaList partReplicas(it, jt);
        auto partId = ErasurePartIdFromChunkId(chunkId, it->GetIndex());
        auto reader = CreateReplicationReader(
            Config_,
            BlockCache_,
            MasterChannel_,
            NodeDirectory_,
            Null,
            partId,
            partReplicas);
        readers.push_back(reader);

        it = jt;
    }
    
    YCHECK(readers.size() == dataPartCount);
    return CreateNonReparingErasureReader(readers);
}

void TNontemplateMultiChunkReaderBase::OnReaderFinished()
{
    if (Options_->KeepInMemory) {
        FinishedReaders_.push_back(CurrentSession_.ChunkReader);
    }

    TGuard<TSpinLock> guard(DataStatisticsLock_);
    DataStatistics_ += CurrentSession_.ChunkReader->GetDataStatistics();
    YCHECK(ActiveReaders_.erase(CurrentSession_.ChunkReader));

    CurrentSession_.Reset();

    OpenNextChunk();
}

bool TNontemplateMultiChunkReaderBase::OnEmptyRead(bool readerFinished)
{
    if (readerFinished) {
        OnReaderFinished();
        return !CompletionError_.IsSet();
    } else {
        OnReaderBlocked();
        return true;
    }
}

void TNontemplateMultiChunkReaderBase::OnError()
{ }

void TNontemplateMultiChunkReaderBase::RegisterFailedChunk(int chunkIndex)
{   
    auto chunkId = NYT::FromProto<TChunkId>(ChunkSpecs_[chunkIndex].chunk_id());
    LOG_WARNING("Chunk reader failed (ChunkId: %s)", ~ToString(chunkId));

    OnError();

    TGuard<TSpinLock> guard(FailedChunksLock_);
    FailedChunks_.push_back(chunkId);
}

////////////////////////////////////////////////////////////////////////////////

TNontemplateSequentialMultiChunkReaderBase::TNontemplateSequentialMultiChunkReaderBase(
    TMultiChunkReaderConfigPtr config,
    TMultiChunkReaderOptionsPtr options,
    NRpc::IChannelPtr masterChannel,
    NChunkClient::IBlockCachePtr blockCache,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const std::vector<NProto::TChunkSpec>& chunkSpecs)
    : TNontemplateMultiChunkReaderBase(
        config, 
        options, 
        masterChannel, 
        blockCache, 
        nodeDirectory, 
        chunkSpecs)
    , NextReaderIndex_(0)
{
    NextReaders_.reserve(ChunkSpecs_.size());
    for (int i = 0; i < ChunkSpecs_.size(); ++i) {
        NextReaders_.push_back(NewPromise<IChunkReaderBasePtr>());
    }
}

TError TNontemplateSequentialMultiChunkReaderBase::DoOpen()
{
    OpenPrefetchChunks();
    return WaitForNextReader();
}

void TNontemplateSequentialMultiChunkReaderBase::OnReaderOpened(IChunkReaderBasePtr chunkReader, int chunkIndex)
{
    // May have already been set in case of error.
    NextReaders_[chunkIndex].TrySet(chunkReader);
}

void TNontemplateSequentialMultiChunkReaderBase::OnReaderBlocked()
{
    ReadyEvent_ = BIND(
        &TNontemplateSequentialMultiChunkReaderBase::WaitForCurrentReader, 
        MakeStrong(this))
    .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
    .Run();
}

void TNontemplateSequentialMultiChunkReaderBase::OnReaderFinished()
{
    TNontemplateMultiChunkReaderBase::OnReaderFinished();
 
    if (NextReaderIndex_ < ChunkSpecs_.size()) {
        ReadyEvent_ = BIND(
            &TNontemplateSequentialMultiChunkReaderBase::WaitForNextReader, 
            MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run();
    } else {
        CompletionError_.TrySet(TError());
        ReadyEvent_ = CompletionError_.ToFuture();
    }
}

TError TNontemplateSequentialMultiChunkReaderBase::WaitForNextReader()
{
    CurrentSession_.ChunkSpecIndex = NextReaderIndex_;
    CurrentSession_.ChunkReader = WaitFor(NextReaders_[NextReaderIndex_].ToFuture());
    
    OnReaderSwitched();

    ++NextReaderIndex_;

    return CompletionError_.IsSet() ? CompletionError_.Get() : TError();
}

TError TNontemplateSequentialMultiChunkReaderBase::WaitForCurrentReader()
{
    auto error = WaitFor(CurrentSession_.ChunkReader->GetReadyEvent());
    if (!error.IsOK()) {
        CompletionError_.TrySet(error);
        RegisterFailedChunk(CurrentSession_.ChunkSpecIndex);
    }

    return error;
}

void TNontemplateSequentialMultiChunkReaderBase::OnError()
{
    // This is to avoid infinite waiting and memory leaks.
    for (auto& nextReader : NextReaders_) {
        nextReader.TrySet(nullptr);
    }
}

////////////////////////////////////////////////////////////////////////////////

/*
void TNontemplateParallelMultiChunkReaderBase::OnReaderBlocked()
{
    BIND(
        &TNontemplateSequentialMultiChunkReaderBase::WaitForCurrentReader, 
        MakeStrong(this))
    .Via(TDispatcher::Get()->GetReaderInvoker())
    .Run();

    ReadyEvent_ = BIND(
        &TNontemplateSequentialMultiChunkReaderBase::WaitForReadyReader, 
        MakeStrong(this))
    .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
    .Run();
}

void TNontemplateSequentialMultiChunkReaderBase::OnReaderFinished()
{
    OpenNextChunk();
 
    ++FinishedReaderCount_;

    if (FinishedReaderCount_ < ChunkSpecs_.size()) {
        ReadyEvent_ = BIND(
            &TNontemplateSequentialMultiChunkReaderBase::WaitForReadyReader, 
            MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run();
    } else {
        CompletionError_.TrySet(TError());
        ReadyEvent_ = CompletionError_.ToFuture();
    }
}
*/

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
