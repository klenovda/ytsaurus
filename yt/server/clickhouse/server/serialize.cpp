#include "serialize.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/ytlib/chunk_client/data_source.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/client/node_tracker_client/node_directory.h>
#include <yt/client/chunk_client/chunk_replica.h>
#include <yt/client/chunk_client/read_limit.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>

namespace NYT {

using namespace NYT::NYTree;
using namespace NYT::NYson;

namespace NProto {

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TExtension& spec, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("tag").Value(spec.tag())
            .Item("data").Value(spec.data())
        .EndMap();
}

void Deserialize(TExtension& spec, INodePtr node)
{
    auto mapNode = node->AsMap();

    spec = TExtension();

    spec.set_tag(ConvertTo<int>(mapNode->GetChild("tag")));
    spec.set_data(ConvertTo<TString>(mapNode->GetChild("data")));
}

void Serialize(const TExtensionSet& spec, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .List(spec.extensions());
}

void Deserialize(TExtensionSet& spec, INodePtr node)
{
    auto listNode = node->AsList();

    auto* extensions = spec.mutable_extensions();
    for (const auto& child: listNode->GetChildren()) {
        Deserialize(*extensions->Add(), child);
    }
}

}   // namespace NProto

namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TDataSource& spec, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("type").Value(spec.GetType())
            .Item("timestamp").Value(spec.GetTimestamp())
            .DoIf(spec.GetPath().HasValue(), [&] (TFluentMap fluent) {
                fluent.Item("path").Value(*spec.GetPath());
            })
            .DoIf(spec.Schema().HasValue(), [&] (TFluentMap fluent) {
                fluent.Item("schema").Value(*spec.Schema());
            })
            .DoIf(spec.Columns().HasValue(), [&] (TFluentMap fluent) {
                fluent.Item("columns").List(*spec.Columns());
            })
        .EndMap();
}

void Deserialize(TDataSource& spec, INodePtr node)
{
    auto mapNode = node->AsMap();

    auto type = ConvertTo<EDataSourceType>(mapNode->GetChild("type"));
    auto timestamp = ConvertTo<NTransactionClient::TTimestamp>(mapNode->GetChild("timestamp"));

    TNullable<TString> path;
    if (auto node = mapNode->FindChild("path")) {
        path = ConvertTo<TString>(node);
    }

    TNullable<NTableClient::TTableSchema> schema;
    if (auto node = mapNode->FindChild("schema")) {
        schema = ConvertTo<NTableClient::TTableSchema>(node);
    }

    TNullable<std::vector<TString>> columns;
    if (auto node = mapNode->FindChild("columns")) {
        columns = ConvertTo<std::vector<TString>>(node);
    }

    spec = TDataSource(
        type,
        std::move(path),
        std::move(schema),
        std::move(columns),
        timestamp);
}

void Serialize(const TDataSourceDirectory& sourceDirectory, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .List(sourceDirectory.DataSources());
}

void Deserialize(TDataSourceDirectory& sourceDirectory, INodePtr node)
{
    auto listNode = node->AsList();

    for (const auto& child: listNode->GetChildren()) {
        sourceDirectory.DataSources().push_back(
            ConvertTo<TDataSource>(child));
    }
}

void Serialize(const TDataSliceDescriptor& spec, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .List(spec.ChunkSpecs);
}

void Deserialize(TDataSliceDescriptor& spec, INodePtr node)
{
    auto listNode = node->AsList();

    spec.ChunkSpecs = ConvertTo<std::vector<NProto::TChunkSpec>>(listNode);
}

namespace NProto {

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TChunkSpec& spec, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("chunk_id").Value(NYT::FromProto<TChunkId>(spec.chunk_id()))
            .DoIf(spec.has_lower_limit(), [&] (TFluentMap fluent) {
                fluent.Item("lower_limit").Value(NYT::FromProto<NChunkClient::TReadLimit>(spec.lower_limit()));
            })
            .DoIf(spec.has_upper_limit(), [&] (TFluentMap fluent) {
                fluent.Item("upper_limit").Value(NYT::FromProto<NChunkClient::TReadLimit>(spec.upper_limit()));
            })
            .Item("replicas").DoListFor(spec.replicas(),
                [] (TFluentList fluent, ui32 packedReplica) {
                    TChunkReplica replica;
                    FromProto(&replica, packedReplica);
                    fluent.Item().Value(replica.GetNodeId());
                })
            .DoIf(spec.has_table_index(), [&] (TFluentMap fluent) {
                fluent.Item("table_index").Value(spec.table_index());
            })
            .DoIf(spec.has_erasure_codec(), [&] (TFluentMap fluent) {
                fluent.Item("erasure_codec").Value(spec.erasure_codec());
            })
            .DoIf(spec.has_table_row_index(), [&] (TFluentMap fluent) {
                fluent.Item("table_row_index").Value(spec.table_row_index());
            })
            .DoIf(spec.has_timestamp(), [&] (TFluentMap fluent) {
                fluent.Item("timestamp").Value(spec.timestamp());
            })
            .DoIf(spec.has_range_index(), [&] (TFluentMap fluent) {
                fluent.Item("range_index").Value(spec.range_index());
            })
            .DoIf(spec.has_row_count_override(), [&] (TFluentMap fluent) {
                fluent.Item("row_count_override").Value(spec.row_count_override());
            })
            .DoIf(spec.has_data_weight_override(), [&] (TFluentMap fluent) {
                fluent.Item("data_weight_override").Value(spec.data_weight_override());
            })
            .DoIf(spec.has_data_slice_tag(), [&] (TFluentMap fluent) {
                fluent.Item("data_slice_tag").Value(spec.data_slice_tag());
            })
            .DoIf(spec.has_chunk_index(), [&] (TFluentMap fluent) {
                fluent.Item("chunk_index").Value(spec.chunk_index());
            })
            .DoIf(spec.has_chunk_meta(), [&] (TFluentMap fluent) {
                fluent.Item("chunk_meta").Value(spec.chunk_meta());
            })
        .EndMap();
}

void Deserialize(TChunkSpec& spec, INodePtr node)
{
    auto mapNode = node->AsMap();

    spec = TChunkSpec();

    ToProto(spec.mutable_chunk_id(), ConvertTo<TChunkId>(mapNode->GetChild("chunk_id")));

    if (auto node = mapNode->FindChild("lower_limit")) {
        ToProto(spec.mutable_lower_limit(), ConvertTo<NChunkClient::TReadLimit>(node));
    }

    if (auto node = mapNode->FindChild("upper_limit")) {
        ToProto(spec.mutable_upper_limit(), ConvertTo<NChunkClient::TReadLimit>(node));
    }

    if (auto node = mapNode->FindChild("replicas")) {
        auto* replicas = spec.mutable_replicas();
        for (ui32 nodeId: ConvertTo<std::vector<ui32>>(node)) {
            TChunkReplica replica(nodeId, 0, 0);
            ToProto(replicas->Add(), replica);
        }
    }

    if (auto node = mapNode->FindChild("table_index")) {
        spec.set_table_index(ConvertTo<ui32>(node));
    }

    if (auto node = mapNode->FindChild("erasure_codec")) {
        spec.set_erasure_codec(ConvertTo<ui32>(node));
    }

    if (auto node = mapNode->FindChild("table_row_index")) {
        spec.set_table_row_index(ConvertTo<ui64>(node));
    }

    if (auto node = mapNode->FindChild("timestamp")) {
        spec.set_timestamp(ConvertTo<ui64>(node));
    }

    if (auto node = mapNode->FindChild("range_index")) {
        spec.set_range_index(ConvertTo<ui32>(node));
    }

    if (auto node = mapNode->FindChild("row_count_override")) {
        spec.set_row_count_override(ConvertTo<ui64>(node));
    }

    if (auto node = mapNode->FindChild("data_weight_override")) {
        spec.set_data_weight_override(ConvertTo<ui64>(node));
    }

    if (auto node = mapNode->FindChild("data_slice_tag")) {
        spec.set_data_slice_tag(ConvertTo<ui64>(node));
    }

    if (auto node = mapNode->FindChild("chunk_index")) {
        spec.set_chunk_index(ConvertTo<ui64>(node));
    }

    if (auto node = mapNode->FindChild("chunk_meta")) {
        Deserialize(*spec.mutable_chunk_meta(), node);
    }
}

void Serialize(const TChunkMeta& spec, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("type").Value(spec.type())
            .Item("version").Value(spec.version())
            .Item("extensions").Value(spec.extensions())
        .EndMap();
}

void Deserialize(TChunkMeta& spec, INodePtr node)
{
    auto mapNode = node->AsMap();

    spec = TChunkMeta();

    spec.set_type(ConvertTo<int>(mapNode->GetChild("type")));
    spec.set_version(ConvertTo<int>(mapNode->GetChild("version")));

    Deserialize(*spec.mutable_extensions(), mapNode->GetChild("extensions"));
}

}   // namespace NProto
}   // namespace NChunkClient

namespace NNodeTrackerClient {

////////////////////////////////////////////////////////////////////////////////

void Deserialize(TNodeDirectory& nodeDirectory, INodePtr node)
{
    auto listNode = node->AsList();

    for (const auto& child: listNode->GetChildren()) {
        auto mapNode = child->AsMap();
        nodeDirectory.AddDescriptor(
            ConvertTo<TNodeId>(mapNode->GetChild("node_id")),
            TNodeDescriptor(ConvertTo<TAddressMap>(mapNode->GetChild("addresses"))));
    }
}

}   // namespace NNodeTrackerClient
}   // namespace NYT
