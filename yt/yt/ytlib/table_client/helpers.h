#pragma once

#include "public.h"

#include <yt/yt/client/table_client/versioned_row.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/adapters.h>
#include <yt/yt/client/table_client/columnar_statistics.h>
#include <yt/yt/client/table_client/column_sort_schema.h>
#include <yt/yt/client/table_client/table_output.h>
#include <yt/yt/client/table_client/row_batch.h>

#include <yt/yt/client/chunk_client/public.h>

#include <yt/yt/client/formats/public.h>

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/ytlib/chunk_client/chunk_owner_ypath_proxy.h>
#include <yt/yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/yt/ytlib/chunk_client/data_source.h>
#include <yt/yt/ytlib/chunk_client/data_sink.h>

#include <yt/yt/ytlib/cypress_client/public.h>

#include <yt/yt/ytlib/object_client/public.h>

#include <yt/yt/ytlib/scheduler/public.h>

#include <yt/yt/core/yson/lexer.h>
#include <yt/yt/core/yson/public.h>


namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

NApi::ITableReaderPtr CreateApiFromSchemalessChunkReaderAdapter(
    ISchemalessChunkReaderPtr underlyingReader);

////////////////////////////////////////////////////////////////////////////////

void PipeReaderToWriter(
    const ISchemalessChunkReaderPtr& reader,
    const IUnversionedRowsetWriterPtr& writer,
    const TPipeReaderToWriterOptions& options);

////////////////////////////////////////////////////////////////////////////////

// NB: not using TYsonString here to avoid copying.
TUnversionedValue MakeUnversionedValue(
    TStringBuf ysonString,
    int id,
    NYson::TStatelessLexer& lexer);

////////////////////////////////////////////////////////////////////////////////

//! Checks whether chunk with `chunkSortColumns' sort columns
//! can belong to sorted table with `tableSortColumns' sort columns and
//! given key uniqueness.
//! Table is sorted without key uniqueness requirement iff all of its
//! chunks are sorted, boundary keys of neighbouring chunks are properly ordered when
//! compared by first `tableSortColumns' values (replacing missing values with nulls if necessary)
//! and for all chunks `chunkSortColumns' is prefix of `tableSortColumns' or
//! `tableSortColumns' is prefix of `chunkSortColumns'.
//! Table is sorted with key uniqueness requirement iff all of its chunks are sorted
//! and have unique keys, boundary keys of neighbouring chunks are properly ordered
//! and different and `chunkSortColumns' is a prefix of `tableSortColumns'.
void ValidateSortColumns(
    const TSortColumns& tableSortColumns,
    const TSortColumns& chunkSortColumns,
    bool requireUniqueKeys);

//! Same as `ValidateSortColumns' but does not check column names.
void ValidateKeyColumnCount(
    int tableKeyColumnCount,
    int chunkKeyColumnCount,
    bool requireUniqueKeys);

TColumnFilter CreateColumnFilter(
    const std::optional<std::vector<TString>>& columns,
    const TNameTablePtr& nameTable);

////////////////////////////////////////////////////////////////////////////////

NScheduler::NProto::TOutputResult GetWrittenChunksBoundaryKeys(
    const ISchemalessMultiChunkWriterPtr& writer);

std::pair<TLegacyOwningKey, TLegacyOwningKey> GetChunkBoundaryKeys(
    const NTableClient::NProto::TBoundaryKeysExt& boundaryKeysExt,
    int keyColumnCount);
std::pair<TLegacyOwningKey, TLegacyOwningKey> GetChunkBoundaryKeys(
    const NChunkClient::NProto::TChunkMeta& chunkMeta,
    int keyColumnCount);

////////////////////////////////////////////////////////////////////////////////

void ValidateDynamicTableTimestamp(
    const NYPath::TRichYPath& path,
    bool dynamic,
    const TTableSchema& schema,
    const NYTree::IAttributeDictionary& attributes,
    bool forceDisableDynamicStoreRead = false);

////////////////////////////////////////////////////////////////////////////////

std::vector<NChunkClient::TInputChunkPtr> CollectTableInputChunks(
    const NYPath::TRichYPath& path,
    const NApi::NNative::IClientPtr& client,
    const NNodeTrackerClient::TNodeDirectoryPtr& nodeDirectory,
    const NChunkClient::TFetchChunkSpecConfigPtr& config,
    NObjectClient::TTransactionId transactionId,
    bool fetchHeavyColumnStatisticsExt,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

//! Helpers for updating columnar statistics with versioned and unversioned rows.
void UpdateColumnarStatistics(NProto::TColumnarStatisticsExt& columnarStatisticsExt, TUnversionedRow row);
void UpdateColumnarStatistics(NProto::TColumnarStatisticsExt& columnarStatisticsExt, TVersionedRow row);

////////////////////////////////////////////////////////////////////////////////

void CheckUnavailableChunks(EUnavailableChunkStrategy strategy, std::vector<NChunkClient::NProto::TChunkSpec>* chunkSpecs);

////////////////////////////////////////////////////////////////////////////////

ui32 GetHeavyColumnStatisticsHash(ui32 salt, const TString& columnName);

TColumnarStatistics GetColumnarStatistics(
    const NProto::THeavyColumnStatisticsExt& statistics,
    const std::vector<TString>& columnNames);

////////////////////////////////////////////////////////////////////////////////

//! Helper class for storing virtual values in schemaless reader.
//! It allows extracting them as vector of TUnversionedValue as well as
//! filling them as a sequence of RLE-encoded IUnversionedColumnarRowBatch::TColumn.
class TReaderVirtualValues
{
public:
    DEFINE_BYREF_RO_PROPERTY(std::vector<TUnversionedValue>, Values);

public:
    TReaderVirtualValues() = default;

    void AddValue(TUnversionedValue value, TLogicalTypePtr logicalType);

    //! Return number of columns that are required for representing virtual
    //! column #virtualColumnIndex (including inner columns for RLE encoding).
    int GetBatchColumnCount(int virtualColumnIndex) const;

    //! Return total number of columns that are required for representing all virtual columns.
    int GetTotalColumnCount() const;

    void FillColumns(
        TMutableRange<IUnversionedColumnarRowBatch::TColumn> columnRange,
        int virtualColumnIndex,
        ui64 startIndex,
        ui64 valueCount) const;

private:
    static const ui64 Zero_;

    std::vector<TLogicalTypePtr> LogicalTypes_;

    void FillRleColumn(IUnversionedColumnarRowBatch::TColumn* rleColumn, int virtualColumnIndex) const;

    void FillMainColumn(
        IUnversionedColumnarRowBatch::TColumn* mainColumn,
        const IUnversionedColumnarRowBatch::TColumn* rleColumn,
        int virtualColumnIndex,
        ui64 startIndex,
        ui64 valueCount) const;
};

////////////////////////////////////////////////////////////////////////////////

NProto::THeavyColumnStatisticsExt GetHeavyColumnStatisticsExt(
    const NProto::TColumnarStatisticsExt& columnarStatisticsExt,
    const std::function<TString(int index)>& getNameByIndex,
    int columnCount,
    int maxHeavyColumns);

////////////////////////////////////////////////////////////////////////////////

void AddTagsFromDataSource(const NYTree::IAttributeDictionaryPtr& baggage, const NChunkClient::TDataSource& dataSource);
void AddTagsFromDataSink(const NYTree::IAttributeDictionaryPtr& baggage, const NChunkClient::TDataSink& dataSink);

void PackBaggageFromDataSource(const NTracing::TTraceContextPtr& context, const NChunkClient::TDataSource& dataSource);
void PackBaggageFromDataSink(const NTracing::TTraceContextPtr& context, const NChunkClient::TDataSink& dataSink);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient

#define HELPERS_INL_H_
#include "helpers-inl.h"
#undef HELPERS_INL_H_
