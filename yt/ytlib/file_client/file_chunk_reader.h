#pragma once

#include "public.h"

#include <core/misc/async_stream_state.h>

#include <core/compression/codec.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/reader.h>
#include <ytlib/chunk_client/chunk_spec.h>

#include <core/logging/log.h>

namespace NYT {
namespace NFileClient {

////////////////////////////////////////////////////////////////////////////////

class TFileChunkReaderProvider
    : public TRefCounted
{
public:
    TFileChunkReaderProvider(
        NChunkClient::TSequentialReaderConfigPtr config,
        NChunkClient::IBlockCachePtr uncompressedBlockCache);

    TFileChunkReaderPtr CreateReader(
        const NChunkClient::NProto::TChunkSpec& chunkSpec,
        NChunkClient::IReaderPtr chunkReader);

    void OnReaderOpened(
        TFileChunkReaderPtr reader,
        NChunkClient::NProto::TChunkSpec& chunkSpec);

    void OnReaderFinished(TFileChunkReaderPtr reader);

    bool KeepInMemory() const;

private:
    NChunkClient::TSequentialReaderConfigPtr Config;
    NChunkClient::IBlockCachePtr UncompressedBlockCache;

};

DEFINE_REFCOUNTED_TYPE(TFileChunkReaderProvider)

////////////////////////////////////////////////////////////////////////////////

class TFileChunkReaderFacade
    : public TNonCopyable
{
public:
    TFileChunkReaderFacade(TFileChunkReader* reader);

    TSharedRef GetBlock() const;

private:
    TFileChunkReader* Reader;

};

////////////////////////////////////////////////////////////////////////////////

class TFileChunkReader
    : public TRefCounted
{
public:
    typedef TFileChunkReaderProvider TProvider;
    typedef TFileChunkReaderFacade TFacade;

    TFileChunkReader(
        NChunkClient::TSequentialReaderConfigPtr sequentialConfig,
        NChunkClient::IReaderPtr chunkReader,
        NChunkClient::IBlockCachePtr uncompressedBlockCache,
        NCompression::ECodec codecId,
        i64 startOffset,
        i64 endOffset);

    TAsyncError AsyncOpen();

    bool FetchNext();
    TAsyncError GetReadyEvent();

    const TFacade* GetFacade() const;

    //! Must be called after AsyncOpen has finished.
    TFuture<void> GetFetchingCompleteEvent();

    // Called by facade.
    TSharedRef GetBlock() const;

private:
    NChunkClient::TSequentialReaderConfigPtr SequentialConfig;
    NChunkClient::IReaderPtr ChunkReader;
    NChunkClient::IBlockCachePtr UncompressedBlockCache;
    NCompression::ECodec CodecId;
    i64 StartOffset;
    i64 EndOffset;

    NChunkClient::TSequentialReaderPtr SequentialReader;

    TFacade Facade;

    TAsyncStreamState State;

    NLog::TLogger Logger;

    void OnNextBlock(TError error);
    void OnGotMeta(NChunkClient::IReader::TGetMetaResult result);

};

DEFINE_REFCOUNTED_TYPE(TFileChunkReader)

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
