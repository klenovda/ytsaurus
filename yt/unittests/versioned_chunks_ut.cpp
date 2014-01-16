#include "stdafx.h"
#include "framework.h"

#include "versioned_table_client_ut.h"

#include <ytlib/new_table_client/config.h>
#include <ytlib/new_table_client/schema.h>
#include <ytlib/new_table_client/versioned_chunk_reader.h>
#include <ytlib/new_table_client/versioned_chunk_writer.h>
#include <ytlib/new_table_client/versioned_reader.h>
#include <ytlib/new_table_client/versioned_row.h>
#include <ytlib/new_table_client/versioned_writer.h>

#include <ytlib/chunk_client/memory_reader.h>
#include <ytlib/chunk_client/memory_writer.h>

#include <ytlib/transaction_client/public.h>

#include <core/compression/public.h>

namespace NYT {
namespace NVersionedTableClient {
namespace {

using namespace NChunkClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

Stroka A("a");
Stroka B("a");

class TVersionedChunksTest
    : public TVersionedTableClientTestBase
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

        MemoryWriter = New<TMemoryWriter>();

        ChunkWriter = CreateVersionedChunkWriter(
            New<TChunkWriterConfig>(),
            New<TChunkWriterOptions>(),
            Schema,
            KeyColumns,
            MemoryWriter);

        EXPECT_TRUE(ChunkWriter->Open().Get().IsOK());
    }

    TTableSchema Schema;
    TKeyColumns KeyColumns;

    IVersionedReaderPtr ChunkReader;
    IVersionedWriterPtr ChunkWriter;

    IAsyncReaderPtr MemoryReader;
    TMemoryWriterPtr MemoryWriter;

    TChunkedMemoryPool MemoryPool;

    void CheckResult(const std::vector<TVersionedRow>& expected, const std::vector<TVersionedRow>& actual)
    {
        EXPECT_EQ(expected.size(), actual.size());
        for (int i = 0; i < expected.size(); ++i) {
            ExpectRowsEqual(expected[i], actual[i]);
        }
    }

    void FillKey(TVersionedRow row, TNullable<Stroka> k1, TNullable<i64> k2, TNullable<double> k3)
    {
        row.BeginKeys()[0] = k1 
            ? MakeUnversionedStringValue(*k1, 0)
            : MakeUnversionedSentinelValue(EValueType::Null, 0);
        row.BeginKeys()[1] = k2 
            ? MakeUnversionedIntegerValue(*k2, 1)
            : MakeUnversionedSentinelValue(EValueType::Null, 1);
        row.BeginKeys()[2] = k3 
            ? MakeUnversionedDoubleValue(*k3, 2)
            : MakeUnversionedSentinelValue(EValueType::Null, 2);
    }

    void WriteThreeRows()
    {
        std::vector<TVersionedRow> rows;
        {
            TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 3, 3);
            FillKey(row, MakeNullable(A), MakeNullable(1), MakeNullable(1.5));

            // v1
            row.BeginValues()[0] = MakeVersionedIntegerValue(8, 11, 3);
            row.BeginValues()[1] = MakeVersionedIntegerValue(7, 3, 3);
            // v2
            row.BeginValues()[2] = MakeVersionedSentinelValue(EValueType::Null, 5, 4);

            row.BeginTimestamps()[0] = 11;
            row.BeginTimestamps()[1] = 9 | TombstoneTimestampMask;
            row.BeginTimestamps()[2] = 3;

            rows.push_back(row);
        } {
            TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 3, 1);
            FillKey(row, MakeNullable(A), MakeNullable(2), Null);

            // v1
            row.BeginValues()[0] = MakeVersionedIntegerValue(2, 1, 3);
            // v2
            row.BeginValues()[1] = MakeVersionedIntegerValue(100, 10, 4);
            row.BeginValues()[2] = MakeVersionedSentinelValue(EValueType::Null, 5, 4);

            row.BeginTimestamps()[0] = 1;

            rows.push_back(row);
        } {
            TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 5, 3);
            FillKey(row, MakeNullable(B), MakeNullable(1), MakeNullable(1.5));

            // v1
            row.BeginValues()[0] = MakeVersionedIntegerValue(9, 15, 3);
            row.BeginValues()[1] = MakeVersionedIntegerValue(8, 12, 3);
            row.BeginValues()[2] = MakeVersionedIntegerValue(7, 3, 3);
            // v2
            row.BeginValues()[3] = MakeVersionedSentinelValue(EValueType::Null, 12, 4);
            row.BeginValues()[4] = MakeVersionedSentinelValue(EValueType::Null, 8, 4);

            row.BeginTimestamps()[0] = 20 | TombstoneTimestampMask;
            row.BeginTimestamps()[1] = 3;
            row.BeginTimestamps()[2] = 2 | TombstoneTimestampMask;

            rows.push_back(row);
        }

        ChunkWriter->Write(rows);

        EXPECT_TRUE(ChunkWriter->Close().Get().IsOK());

        // Initialize reader.
        MemoryReader = New<TMemoryReader>(
            std::move(MemoryWriter->GetChunkMeta()),
            std::move(MemoryWriter->GetBlocks()));
    }

};

TEST_F(TVersionedChunksTest, ReadLastCommitted)
{
    std::vector<TVersionedRow> expected;
    {
        TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 1, 1);
        FillKey(row, MakeNullable(A), MakeNullable(1), MakeNullable(1.5));

        // v1
        row.BeginValues()[0] = MakeVersionedIntegerValue(8, 11, 3);
        row.BeginTimestamps()[0] = 11;

        expected.push_back(row);
    } {
        TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 2, 1);
        FillKey(row, MakeNullable(A), MakeNullable(2), Null);

        // v1
        row.BeginValues()[0] = MakeVersionedIntegerValue(2, 1, 3);
        // v2
        row.BeginValues()[1] = MakeVersionedIntegerValue(100, 10, 4);

        row.BeginTimestamps()[0] = 1 | IncrementalTimestampMask;

        expected.push_back(row);
    } {
        TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 0, 1);
        FillKey(row, MakeNullable(B), MakeNullable(1), MakeNullable(1.5));
        row.BeginTimestamps()[0] = 20 | TombstoneTimestampMask;

        expected.push_back(row);
    }

    WriteThreeRows();

    auto chunkMeta = New<TCachableVersionedChunkMeta>(
        MemoryReader,
        Schema,
        KeyColumns);

    EXPECT_TRUE(chunkMeta->Load().Get().IsOK());

    auto chunkReader = CreateVersionedChunkReader(
        New<TChunkReaderConfig>(),
        MemoryReader,
        chunkMeta,
        TReadLimit(),
        TReadLimit());

    EXPECT_TRUE(chunkReader->Open().Get().IsOK());

    std::vector<TVersionedRow> actual;
    actual.reserve(10);

    EXPECT_FALSE(chunkReader->Read(&actual));

    CheckResult(expected, actual);
}

TEST_F(TVersionedChunksTest, ReadByTimestamp)
{
    std::vector<TVersionedRow> expected;
    {
        TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 1, 1);
        FillKey(row, MakeNullable(A), MakeNullable(2), Null);

        // v1
        row.BeginValues()[0] = MakeVersionedIntegerValue(2, 1, 3);
        row.BeginTimestamps()[0] = 1 | IncrementalTimestampMask;

        expected.push_back(row);
    } {
        TVersionedRow row = TVersionedRow::Allocate(&MemoryPool, 3, 0, 1);
        FillKey(row, MakeNullable(B), MakeNullable(1), MakeNullable(1.5));
        row.BeginTimestamps()[0] = 2 | TombstoneTimestampMask;

        expected.push_back(row);
    }

    WriteThreeRows();

    auto chunkMeta = New<TCachableVersionedChunkMeta>(
        MemoryReader,
        Schema,
        KeyColumns);

    EXPECT_TRUE(chunkMeta->Load().Get().IsOK());

    auto chunkReader = CreateVersionedChunkReader(
        New<TChunkReaderConfig>(),
        MemoryReader,
        chunkMeta,
        TReadLimit(),
        TReadLimit(),
        2); // timestamp

    EXPECT_TRUE(chunkReader->Open().Get().IsOK());

    std::vector<TVersionedRow> actual;
    actual.reserve(10);

    EXPECT_FALSE(chunkReader->Read(&actual));

    CheckResult(expected, actual);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NVersionedTableClient
} // namespace NYT
