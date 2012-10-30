#pragma once

#include "public.h"

#include <ytlib/ytree/yson_serializable.h>

namespace NYT {
namespace NFormats {

////////////////////////////////////////////////////////////////////////////////

struct TDsvFormatConfig
    : public TYsonSerializable
{
    char RecordSeparator;
    char KeyValueSeparator;
    char FieldSeparator;
    TNullable<Stroka> LinePrefix;

    bool EnableEscaping;
    char EscapingSymbol;

    // Escaping rules (is EscapingSymbol is '\\')
    //  * '\0' ---> "\0"
    //  * '\n' ---> "\n"
    //  * '\t' ---> "\t"
    //  * 'X'  ---> "\X" if X not in ['\0', '\n', '\t']

    TDsvFormatConfig()
    {
        Register("record_separator", RecordSeparator)
            .Default('\n');
        Register("key_value_separator", KeyValueSeparator)
            .Default('=');
        Register("field_separator", FieldSeparator)
            .Default('\t');
        Register("line_prefix", LinePrefix)
            .Default();
        Register("enable_escaping", EnableEscaping)
            .Default(true);
        Register("escaping_symbol", EscapingSymbol)
            .Default('\\');
    }
};

DECLARE_ENUM(EPrintAttributes,
    (Always)
    (Never)
    (OnDemand)
);

struct TJsonFormatConfig
    : public TYsonSerializable
{
    bool Pretty;
    EPrintAttributes PrintAttributes;

    TJsonFormatConfig()
    {
        Register("pretty", Pretty)
            .Default(false);
        Register("print_attributes", PrintAttributes)
            .Default(EPrintAttributes::OnDemand);
    }
};

struct TYamrFormatConfig
    : public TYsonSerializable
{
    bool HasSubkey;

    Stroka Key;
    Stroka Subkey;
    Stroka Value;

    bool Lenval;

    // Delimited specific options
    char FieldSeparator;
    char RecordSeparator;

    TYamrFormatConfig()
    {
        Register("has_subkey", HasSubkey)
            .Default(false);
        Register("key", Key)
            .Default("key");
        Register("subkey", Subkey)
            .Default("subkey");
        Register("value", Value)
            .Default("value");
        Register("lenval", Lenval)
            .Default(false);
        Register("fs", FieldSeparator)
            .Default('\t');
        Register("rs", RecordSeparator)
            .Default('\n');
    }
};

struct TYamredDsvFormatConfig
    : public TDsvFormatConfig
{
    bool HasSubkey;
    char YamrKeysSeparator;

    std::vector<Stroka> KeyColumnNames;
    std::vector<Stroka> SubkeyColumnNames;

    TYamredDsvFormatConfig()
    {
        Register("has_subkey", HasSubkey)
            .Default(false);
        Register("key_column_names", KeyColumnNames);
        Register("subkey_column_names", SubkeyColumnNames)
            .Default();
        Register("yamr_keys_separator", YamrKeysSeparator)
            .Default(' ');
    }
};


////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
