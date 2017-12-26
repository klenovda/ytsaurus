#include "common.h"

namespace NYT {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

TString ToString(EValueType type)
{
    switch (type) {
        case VT_INT8:    return "int8";
        case VT_INT16:   return "int16";
        case VT_INT32:   return "int32";
        case VT_INT64:   return "int64";
        case VT_UINT8:   return "uint8";
        case VT_UINT16:  return "uint16";
        case VT_UINT32:  return "uint32";
        case VT_UINT64:  return "uint64";
        case VT_DOUBLE:  return "double";
        case VT_BOOLEAN: return "boolean";
        case VT_STRING:  return "string";
        case VT_UTF8:    return "utf8";
        case VT_ANY:     return "any";
        default:
            ythrow yexception() << "Invalid value type " << static_cast<int>(type);
    }
}

TNode ToNode(const TColumnSchema& columnSchema)
{
    TNode result = TNode::CreateMap();

    result["name"] = columnSchema.Name_;
    result["type"] = ToString(columnSchema.Type_);
    if (columnSchema.SortOrder_) {
        result["sort_order"] = ::ToString(*columnSchema.SortOrder_);
    }
    if (columnSchema.Lock_) {
        result["lock"] = ::ToString(*columnSchema.Lock_);
    }
    if (columnSchema.Expression_) {
        result["expression"] = *columnSchema.Expression_;
    }
    if (columnSchema.Aggregate_) {
        result["aggregate"] = *columnSchema.Aggregate_;
    }
    if (columnSchema.Group_) {
        result["group"] = *columnSchema.Group_;
    }

    result["required"] = columnSchema.Required_;

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////


TTableSchema& TTableSchema::AddColumn(const TString& name, EValueType type) &
{
    Columns_.push_back(TColumnSchema().Name(name).Type(type));
    return *this;
}

TTableSchema TTableSchema::AddColumn(const TString& name, EValueType type) &&
{
    return std::move(AddColumn(name, type));
}

TTableSchema& TTableSchema::AddColumn(const TString& name, EValueType type, ESortOrder sortOrder) &
{
    Columns_.push_back(TColumnSchema().Name(name).Type(type).SortOrder(sortOrder));
    return *this;
}

TTableSchema TTableSchema::AddColumn(const TString& name, EValueType type, ESortOrder sortOrder) &&
{
    return std::move(AddColumn(name, type, sortOrder));
}

TNode TTableSchema::ToNode() const
{
    TNode result = TNode::CreateList();
    result.Attributes()["strict"] = Strict_;
    result.Attributes()["unique_keys"] = UniqueKeys_;
    for (const auto& column : Columns_) {
        result.Add(NDetail::ToNode(column));
    }
    return result;
}

bool IsTrivial(const TReadLimit& readLimit)
{
    return !readLimit.Key_ && !readLimit.RowIndex_ && !readLimit.Offset_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
