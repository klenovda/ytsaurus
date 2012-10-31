#include "stdafx.h"
#include "chunk_tree_statistics.h"

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

void TChunkTreeStatistics::Accumulate(const TChunkTreeStatistics& other)
{
    RowCount += other.RowCount;
    UncompressedSize += other.UncompressedSize;
    CompressedSize += other.CompressedSize;
    ChunkCount += other.ChunkCount;
    Rank = Max(Rank, other.Rank);
}

////////////////////////////////////////////////////////////////////////////////

void Save(const TChunkTreeStatistics& statistics, const NCellMaster::TSaveContext& context)
{
    auto* output = context.GetOutput();
    ::Save(output, statistics.RowCount);
    ::Save(output, statistics.UncompressedSize);
    ::Save(output, statistics.CompressedSize);
    ::Save(output, statistics.ChunkCount);
    ::Save(output, statistics.Rank);
}

void Load(TChunkTreeStatistics& statistics, const NCellMaster::TLoadContext& context)
{
    auto* input = context.GetInput();
    ::Load(input, statistics.RowCount);
    ::Load(input, statistics.UncompressedSize);
    ::Load(input, statistics.CompressedSize);
    ::Load(input, statistics.ChunkCount);
    ::Load(input, statistics.Rank);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
