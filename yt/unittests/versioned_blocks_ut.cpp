#include "stdafx.h"
#include "framework.h"

#include "versioned_table_client_ut.h"

#include <ytlib/new_table_client/schema.h>
#include <ytlib/new_table_client/versioned_block_writer.h>
#include <ytlib/new_table_client/versioned_block_reader.h>

#include <ytlib/transaction_client/public.h>

#include <core/compression/codec.h>

namespace NYT {
namespace NVersionedTableClient {
namespace {

using namespace NTransactionClient;
using namespace NCompression;

////////////////////////////////////////////////////////////////////////////////

class TVersionedBlocksTestBase
    : public TVersionedTableClientTestBase
{
protected:
    void CheckResult(TSimpleVersionedBlockReader& reader, const std::vector<TVersionedRow>& rows)
    {
        int i = 0;
        do {
            EXPECT_LT(i, rows.size());
            auto row = reader.GetRow(&MemoryPool);
            ExpectRowsEqual(rows[i], row);
        } while (reader.NextRow());
    }

    TTableSchema Schema;
    TKeyColumns KeyColumns;

    TSharedRef Data;
    NProto::TBlockMeta Meta;

    TChunkedMemoryPool MemoryPool;

};

////////////////////////////////////////////////////////////////////////////////

class TVersionedBlocksTestOneRow
    :public TVersionedBlocksTestBase
{
protected:
    virtual void SetUp() override
    {
        Schema.Columns() = {
            TColumnSchema("k1", EValueType::String),
            TColumnSchema("k2", EValueType::Integer),
            TColumnSchema("k3", EValueType::Double),
            TColumnSchema("v1", EValueType::Integer),
            TColumnSchema("v2", EValueType::Integer)
        };

        KeyColumns = {"k1", "k2", "k3"};

        TSimpleVersionedBlockWriter blockWriter(Schema, KeyColumns);

        TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 3, 3);
        row.BeginKeys()[0] = MakeUnversionedStringValue("a", 0);
        row.BeginKeys()[1] = MakeUnversionedIntegerValue(1, 1);
        row.BeginKeys()[2] = MakeUnversionedDoubleValue(1.5, 2);

        // v1
        row.BeginValues()[0] = MakeVersionedIntegerValue(8, 11, 3);
        row.BeginValues()[1] = MakeVersionedIntegerValue(7, 3, 3);
        // v2
        row.BeginValues()[2] = MakeVersionedSentinelValue(EValueType::Null, 5, 4);

        row.BeginTimestamps()[0] = 11;
        row.BeginTimestamps()[1] = 9 | TombstoneTimestampMask;
        row.BeginTimestamps()[2] = 3;

        blockWriter.WriteRow(row, nullptr, nullptr);

        auto block = blockWriter.FlushBlock();
        auto* codec = GetCodec(ECodec::None);

        Data = codec->Compress(block.Data);
        Meta = block.Meta;
    }

};

TEST_F(TVersionedBlocksTestOneRow, ReadByTimestamp1)
{
    // Reorder value columns in reading schema.
    std::vector<int> schemaIdMapping = {0, 1, 2, 4, 3};

    TSimpleVersionedBlockReader blockReader(
        Data,
        Meta,
        Schema,
        KeyColumns,
        schemaIdMapping,
        7);

    TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 2, 1);
    row.BeginKeys()[0] = MakeUnversionedStringValue("a", 0);
    row.BeginKeys()[1] = MakeUnversionedIntegerValue(1, 1);
    row.BeginKeys()[2] = MakeUnversionedDoubleValue(1.5, 2);
    row.BeginValues()[0] = MakeVersionedSentinelValue(EValueType::Null, 5, 3);
    row.BeginValues()[1] = MakeVersionedIntegerValue(7, 3, 4);
    row.BeginTimestamps()[0] = 3 | IncrementalTimestampMask;

    std::vector<TVersionedRow> rows;
    rows.push_back(row);

    CheckResult(blockReader, rows);
}

TEST_F(TVersionedBlocksTestOneRow, ReadByTimestamp2)
{
    // Omit last column
    std::vector<int> schemaIdMapping = {0, 1, 2, 4};

    TSimpleVersionedBlockReader blockReader(
        Data,
        Meta,
        Schema,
        KeyColumns,
        schemaIdMapping,
        9);

    TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 0, 1);
    row.BeginKeys()[0] = MakeUnversionedStringValue("a", 0);
    row.BeginKeys()[1] = MakeUnversionedIntegerValue(1, 1);
    row.BeginKeys()[2] = MakeUnversionedDoubleValue(1.5, 2);
    row.BeginTimestamps()[0] = 9 | TombstoneTimestampMask;

    std::vector<TVersionedRow> rows;
    rows.push_back(row);

    CheckResult(blockReader, rows);
}

TEST_F(TVersionedBlocksTestOneRow, ReadLastCommitted)
{
    // Omit last column
    std::vector<int> schemaIdMapping = {0, 1, 2, 4};

    TSimpleVersionedBlockReader blockReader(
        Data,
        Meta,
        Schema,
        KeyColumns,
        schemaIdMapping,
        LastCommittedTimestamp);

    TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 0, 1);
    row.BeginKeys()[0] = MakeUnversionedStringValue("a", 0);
    row.BeginKeys()[1] = MakeUnversionedIntegerValue(1, 1);
    row.BeginKeys()[2] = MakeUnversionedDoubleValue(1.5, 2);
    row.BeginTimestamps()[0] = 11;

    std::vector<TVersionedRow> rows;
    rows.push_back(row);

    CheckResult(blockReader, rows);
}

TEST_F(TVersionedBlocksTestOneRow, ReadAllCommitted)
{
    // Read only last non-key column.
    std::vector<int> schemaIdMapping = {0, 1, 2, 4};

    TSimpleVersionedBlockReader blockReader(
        Data,
        Meta,
        Schema,
        KeyColumns,
        schemaIdMapping,
        AllCommittedTimestamp);

    TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 1, 3);
    row.BeginKeys()[0] = MakeUnversionedStringValue("a", 0);
    row.BeginKeys()[1] = MakeUnversionedIntegerValue(1, 1);
    row.BeginKeys()[2] = MakeUnversionedDoubleValue(1.5, 2);

    // v2
    row.BeginValues()[0] = MakeVersionedSentinelValue(EValueType::Null, 5, 4);

    row.BeginTimestamps()[0] = 11;
    row.BeginTimestamps()[1] = 9 | TombstoneTimestampMask;
    row.BeginTimestamps()[2] = 3;

    std::vector<TVersionedRow> rows;
    rows.push_back(row);

    CheckResult(blockReader, rows);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NVersionedTableClient
} // namespace NYT
