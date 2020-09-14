#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/data_slice_descriptor.h>

#include <yt/ytlib/api/native/public.h>

#include <yt/client/chunk_client/config.h>

#include <yt/client/table_client/name_table.h>

#include <yt/client/ypath/rich.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Helper struct for #CreateAppropriateSchemalessMultiChunkReader.
struct TTableReadSpec
{
    NChunkClient::TDataSourceDirectoryPtr DataSourceDirectory;
    std::vector<NChunkClient::TDataSliceDescriptor> DataSliceDescriptors;
};

////////////////////////////////////////////////////////////////////////////////

struct TFetchSingleTableReadSpecOptions
{
    // Required arguments.
    NYPath::TRichYPath RichPath;
    NApi::NNative::IClientPtr Client;

    // Optional arguments.
    NTransactionClient::TTransactionId TransactionId;
    NChunkClient::TReadSessionId ReadSessionId = NChunkClient::TReadSessionId::Create();
    NChunkClient::TGetUserObjectBasicAttributesOptions GetUserObjectBasicAttributesOptions;
    NChunkClient::TFetchChunkSpecConfigPtr FetchChunkSpecConfig = New<NChunkClient::TFetchChunkSpecConfig>();
    bool FetchParityReplicas = true;
    EUnavailableChunkStrategy UnavailableChunkStrategy = NTableClient::EUnavailableChunkStrategy::ThrowError;
    // Name table and column filter may be used for reading partitioned tables to identify which virtual
    // columns to put into table read spec.
    TNameTablePtr NameTable = New<TNameTable>();
    TColumnFilter ColumnFilter;
    TPartitionedTableHarvesterConfigPtr PartitionedTableHarvesterConfig;
};

//! Helper for fetching single table identified by TRichYPath.
//! By the moment this comment is written, it is used in NApi::TTableReader
//! and in CHYT YT-based external dictionaries.
TTableReadSpec FetchSingleTableReadSpec(const TFetchSingleTableReadSpecOptions& options);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
