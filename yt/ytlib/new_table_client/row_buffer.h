#pragma once

#include "public.h"

#include <core/misc/chunked_memory_pool.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Holds data for a bunch of rows.
/*!
 *  Internally, implemented as a pair of chunked pools: one for aligned
 *  data (row headers and row values) and another for unaligned data (string values).
 */
class TRowBuffer
{
public:
    explicit TRowBuffer(
        i64 alignedPoolChunkSize = 4 * 1024,
        i64 unalignedPoolChunkSize = 4 * 1024,
        double maxPoolSmallBlockRatio = 0.25);

    TChunkedMemoryPool* GetAlignedPool();
    const TChunkedMemoryPool* GetAlignedPool() const;

    TChunkedMemoryPool* GetUnalignedPool();
    const TChunkedMemoryPool* GetUnalignedPool() const;

    TUnversionedRow Capture(TUnversionedRow row);
    std::vector<TUnversionedRow> Capture(const std::vector<TUnversionedRow>& rows);

private:
    TChunkedMemoryPool AlignedPool_;
    TChunkedMemoryPool UnalignedPool_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
