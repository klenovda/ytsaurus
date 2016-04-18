#include "cache_reader.h"

#include "chunk_meta_extensions.h"
#include "chunk_reader.h"
#include "block_cache.h"

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;

///////////////////////////////////////////////////////////////////////////////

class TCacheReader
    : public IChunkReader
{
public:
    TCacheReader(
        const TChunkId& chunkId,
        IBlockCachePtr blockCache)
        : ChunkId_(chunkId)
        , BlockCache_(std::move(blockCache))
    {  }

    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(
        const TWorkloadDescriptor& /*workloadDescriptor*/,
        const std::vector<int>& blockIndexes) override
    {
        std::vector<TSharedRef> blocks;
        for (auto index : blockIndexes) {
            TBlockId blockId(ChunkId_, index);
            auto block = BlockCache_->Find(blockId, EBlockType::CompressedData);
            if (!block) {
                return MakeFuture<std::vector<TSharedRef>>(TError("Block %v is not found in the compressed data cache", blockId));
            }

            blocks.push_back(block);
        }
        return MakeFuture(std::move(blocks));
    }

    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(
        const TWorkloadDescriptor& /*workloadDescriptor*/,
        int firstBlockIndex,
        int blockCount) override
    {
        std::vector<TSharedRef> blocks;
        for (int index = 0; index < blockCount; ++index) {
            TBlockId blockId(ChunkId_, firstBlockIndex + index);
            auto block = BlockCache_->Find(blockId, EBlockType::CompressedData);
            if (!block) {
                return MakeFuture<std::vector<TSharedRef>>(TError("Block %v is not found in the compressed data cache", blockId));
            }

            blocks.push_back(block);
        }

        return MakeFuture(std::move(blocks));
    }

    virtual TFuture<TChunkMeta> GetMeta(
        const TWorkloadDescriptor& /*workloadDescriptor*/,
        const TNullable<int>& partitionTag,
        const TNullable<std::vector<int>>& extensionTags) override
    {
        // Cache-based readers shouldn't ask meta from chunk reader.
        YUNREACHABLE();
    }

    virtual TChunkId GetChunkId() const override
    {
        return ChunkId_;
    }

private:
    const TChunkId ChunkId_;
    const IBlockCachePtr BlockCache_;
};

IChunkReaderPtr CreateCacheReader(
    const TChunkId& chunkId,
    IBlockCachePtr blockCache)
{
    return New<TCacheReader>(chunkId, std::move(blockCache));
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
