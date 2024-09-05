#pragma once

#include "public.h"

#include <yt/yt/client/table_client/comparator.h>
#include <yt/yt/client/table_client/unversioned_row.h>

#include <library/cpp/yt/small_containers/compact_vector.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IPartitioner
    : public virtual TRefCounted
{
    virtual int GetPartitionCount() const = 0;
    virtual int GetPartitionIndex(TUnversionedRow row) const = 0;
};

DEFINE_REFCOUNTED_TYPE(IPartitioner)

////////////////////////////////////////////////////////////////////////////////

//! Create a partitioner that partitions rows by #wirePivots. If pivots are K_1 < K_2 < K_3 < ... < K_n,
//! then partitions are formed by intervals (-oo, K_1), [K_1, K_2), ..., [K_{n - 1}, K_n), [K_n, +oo).
//! During partitioning keys are compared using #comparator, long rows are trimmed to comparator length.
IPartitionerPtr CreateOrderedPartitioner(const TSharedRef& wirePivots, TComparator comparator);

IPartitionerPtr CreateOrderedPartitioner(std::vector<TOwningKeyBound> partitionLowerBound, TComparator comparator);

////////////////////////////////////////////////////////////////////////////////

IPartitionerPtr CreateHashPartitioner(int partitionCount, int keyColumnCount, TFingerprint salt);

////////////////////////////////////////////////////////////////////////////////

//! Create a partitioner that extracts the partition ID from the specified column in the row.
//! NB(apollo1321): This partitioner will be used in the shuffle service.
IPartitionerPtr CreateColumnBasedPartitioner(int partitionCount, int partitionColumnId);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
