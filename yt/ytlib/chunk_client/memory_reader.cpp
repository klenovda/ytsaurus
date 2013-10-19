#include "memory_reader.h"
#include "chunk_meta_extensions.h"

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

TMemoryReader::TMemoryReader(std::vector<TSharedRef>&& blocks, NProto::TChunkMeta&& meta)
    : Blocks(blocks)
    , Meta(meta)
{ }

auto TMemoryReader::AsyncReadBlocks(const std::vector<int>& blockIndexes) -> TAsyncReadResult
{
    std::vector<TSharedRef> blocks;
    for (auto index: blockIndexes) {
        YCHECK(index < Blocks.size());
        blocks.push_back(Blocks[index]);
    }

    return MakeFuture(TReadResult(std::move(blocks)));
}

auto TMemoryReader::AsyncGetChunkMeta(
    const TNullable<int>& partitionTag,
    const std::vector<int>* tags) -> TAsyncGetMetaResult
{
    YCHECK(!partitionTag);

    return MakeFuture(TGetMetaResult(tags ? FilterChunkMetaExtensions(Meta, *tags) : Meta));
}

TChunkId TMemoryReader::GetChunkId() const
{
    // ToDo(psushin): make YUNIMPLEMENTED, after fixing sequential reader.
    return NullChunkId;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
