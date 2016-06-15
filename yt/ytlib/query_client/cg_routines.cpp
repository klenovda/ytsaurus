#include "cg_routines.h"
#include "callbacks.h"
#include "cg_types.h"
#include "evaluation_helpers.h"
#include "helpers.h"
#include "query_statistics.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/schemaful_writer.h>
#include <yt/ytlib/table_client/unordered_schemaful_reader.h>
#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/farm_hash.h>

#include <yt/core/profiling/scoped_timer.h>

#include <contrib/libs/re2/re2/re2.h>

#include <mutex>

#include <string.h>

namespace llvm {

template <bool Cross>
class TypeBuilder<google::re2::RE2*, Cross>
    : public TypeBuilder<void*, Cross>
{ };

} // namespace llvm

////////////////////////////////////////////////////////////////////////////////

namespace NYT {
namespace NQueryClient {
namespace NRoutines {

using namespace NConcurrency;
using namespace NTableClient;

static const auto& Logger = QueryClientLogger;

struct TOutputBufferTag
{ };

////////////////////////////////////////////////////////////////////////////////

void WriteRow(TRow row, TExecutionContext* context, TWriteOpClosure* closure)
{
    CHECK_STACK();

    auto* statistics = context->Statistics;

    if (statistics->RowsWritten >= context->Limit) {
        throw TInterruptedCompleteException();
    }

    if (statistics->RowsWritten >= context->OutputRowLimit) {
        throw TInterruptedIncompleteException();
    }

    ++statistics->RowsWritten;

    auto& batch = closure->OutputRowsBatch;

    const auto& rowBuffer = closure->OutputBuffer;

    Y_ASSERT(batch->size() < batch->capacity());
    batch->push_back(rowBuffer->Capture(row));

    if (batch.size() == batch.capacity()) {
        auto& writer = context->Writer;
        bool shouldNotWait;
        {
            NProfiling::TAggregatingTimingGuard timingGuard(&statistics->WriteTime);
            shouldNotWait = writer->Write(batch);
        }

        if (!shouldNotWait) {
            NProfiling::TAggregatingTimingGuard timingGuard(&statistics->AsyncTime);
            WaitFor(writer->GetReadyEvent())
                .ThrowOnError();
        }
        batch.clear();
        rowBuffer->Clear();
    }
}

void ScanOpHelper(
    TExecutionContext* context,
    void** consumeRowsClosure,
    void (*consumeRows)(void** closure, TRow* rows, i64 size))
{
    auto& reader = context->Reader;

    std::vector<TRow> rows;
    rows.reserve(RowsetProcessingSize);

    auto* statistics = context->Statistics;

    while (true) {
        bool hasMoreData;
        {
            NProfiling::TAggregatingTimingGuard timingGuard(&statistics->ReadTime);
            hasMoreData = reader->Read(&rows);
        }

        bool shouldWait = rows.empty();

        // Remove null rows.
        rows.erase(
            std::remove_if(rows.begin(), rows.end(), [] (TRow row) {
                return !row;
            }),
            rows.end());

        if (statistics->RowsRead + rows.size() >= context->InputRowLimit) {
            YCHECK(statistics->RowsRead <= context->InputRowLimit);
            rows.resize(context->InputRowLimit - statistics->RowsRead);
            statistics->IncompleteInput = true;
            hasMoreData = false;
        }
        statistics->RowsRead += rows.size();

        consumeRows(consumeRowsClosure, rows.data(), rows.size());
        rows.clear();
        context->IntermediateBuffer->Clear();

        if (!hasMoreData) {
            break;
        }

        if (shouldWait) {
            NProfiling::TAggregatingTimingGuard timingGuard(&statistics->AsyncTime);
            WaitFor(reader->GetReadyEvent())
                .ThrowOnError();
        }
    }
}

void InsertJoinRow(
    TExecutionContext* context,
    TRowBuffer* buffer,
    TJoinClosure* closure,
    TMutableRow* keyPtr,
    TRow row)
{
    CHECK_STACK();

    i64 chainIndex = closure->ChainedRows.size();
    closure->ChainedRows.emplace_back(buffer->Capture(row), -1);

    if (chainIndex >= context->JoinRowLimit) {
        throw TInterruptedIncompleteException();
    }

    TMutableRow key = *keyPtr;
    auto inserted = closure->Lookup.insert(std::make_pair(key, std::make_pair(chainIndex, false)));
    if (inserted.second) {
        closure->Keys.push_back(key);
        for (int index = 0; index < closure->KeySize; ++index) {
            buffer->Capture(&key[index]);
        }
        *keyPtr = buffer->Allocate(closure->KeySize);
    } else {
        auto& startIndex = inserted.first->second.first;
        closure->ChainedRows.back().second = startIndex;
        startIndex = chainIndex;
    }
}

void JoinOpHelper(
    TExecutionContext* context,
    TJoinEvaluator* joinEvaluator,
    THasherFunction* lookupHasher,
    TComparerFunction* lookupEqComparer,
    TComparerFunction* lookupLessComparer,
    int keySize,
    void** collectRowsClosure,
    void (*collectRows)(
        void** closure,
        TJoinClosure* joinClosure,
        TRowBuffer* buffer),
    void** consumeRowsClosure,
    void (*consumeRows)(void** closure, TRow* rows, i64 size))
{
    TJoinClosure closure(lookupHasher, lookupEqComparer, keySize);

    auto buffer = New<TRowBuffer>();

    try {
        // Collect join ids.
        collectRows(collectRowsClosure, &closure, buffer.Get());
    } catch (const TInterruptedIncompleteException&) {
        // Set incomplete and continue
        context->Statistics->IncompleteOutput = true;
    }

    LOG_DEBUG("Sorting %v join keys",
        closure.Keys.size());

    std::sort(closure.Keys.begin(), closure.Keys.end(), lookupLessComparer);

    LOG_DEBUG("Collected %v join keys from %v rows",
        closure.Keys.size(),
        closure.ChainedRows.size());

    (*joinEvaluator)(
        context,
        lookupHasher,
        lookupEqComparer,
        closure.Lookup,
        std::move(closure.Keys),
        std::move(closure.ChainedRows),
        buffer,
        consumeRowsClosure,
        consumeRows);
}

const TRow* InsertGroupRow(
    TExecutionContext* context,
    TRowBuffer* buffer,
    TGroupByClosure* closure,
    TMutableRow row)
{
    CHECK_STACK();

    auto inserted = closure->Lookup.insert(row);

    if (inserted.second) {
        if (closure->GroupedRows.size() >= context->GroupRowLimit) {
            throw TInterruptedIncompleteException();
        }

        closure->GroupedRows.push_back(row);
        for (int index = 0; index < closure->KeySize; ++index) {
            buffer->Capture(&row[index]);
        }

        if (closure->CheckNulls) {
            for (int index = 0; index < closure->KeySize; ++index) {
                if (row[index].Type == EValueType::Null) {
                    THROW_ERROR_EXCEPTION("Null values in group key");
                }
            }
        }
    }

    return &*inserted.first;
}

void GroupOpHelper(
    TExecutionContext* context,
    THasherFunction* groupHasher,
    TComparerFunction* groupComparer,
    int keySize,
    bool checkNulls,
    void** collectRowsClosure,
    void (*collectRows)(
        void** closure,
        TGroupByClosure* groupByClosure,
        TRowBuffer* buffer),
    void** consumeRowsClosure,
    void (*consumeRows)(void** closure, TRow* rows, i64 size))
{
    TGroupByClosure closure(groupHasher, groupComparer, keySize, checkNulls);

    auto buffer = New<TRowBuffer>();

    try {
        collectRows(collectRowsClosure, &closure, buffer.Get());
    } catch (const TInterruptedIncompleteException&) {
        // Set incomplete and continue
        context->Statistics->IncompleteOutput = true;
    }

    LOG_DEBUG("Collected %v group rows",
        closure.GroupedRows.size());

    for (size_t index = 0; index < closure.GroupedRows.size(); index += RowsetProcessingSize) {
        auto size = std::min(RowsetProcessingSize, closure.GroupedRows.size() - index);
        consumeRows(consumeRowsClosure, closure.GroupedRows.data() + index, size);
        context->IntermediateBuffer->Clear();
    }
}

void AllocatePermanentRow(TExecutionContext* context, TRowBuffer* buffer, int valueCount, TMutableRow* row)
{
    CHECK_STACK();

    *row = buffer->Allocate(valueCount);
}

void AllocateIntermediateRow(TExpressionContext* context, int valueCount, TMutableRow* row)
{
    *row = context->IntermediateBuffer->Allocate(valueCount);
}

void AddRow(TTopCollector* topCollector, TRow row)
{
    topCollector->AddRow(row);
}

void OrderOpHelper(
    TExecutionContext* context,
    TComparerFunction* comparer,
    void** collectRowsClosure,
    void (*collectRows)(void** closure, TTopCollector* topCollector),
    void** consumeRowsClosure,
    void (*consumeRows)(void** closure, TRow* rows, i64 size),
    int rowSize)
{
    auto limit = context->Limit;

    TTopCollector topCollector(limit, comparer);
    collectRows(collectRowsClosure, &topCollector);
    auto rows = topCollector.GetRows(rowSize);

    for (size_t index = 0; index < rows.size(); index += RowsetProcessingSize) {
        auto size = std::min(RowsetProcessingSize, rows.size() - index);
        consumeRows(consumeRowsClosure, rows.data() + index, size);
        context->IntermediateBuffer->Clear();
    }
}

void WriteOpHelper(
    TExecutionContext* context,
    void** collectRowsClosure,
    void (*collectRows)(void** closure, TWriteOpClosure* writeOpClosure))
{
    TWriteOpClosure closure;

    closure.OutputBuffer = New<TRowBuffer>(TOutputBufferTag());
    closure.OutputRowsBatch.reserve(RowsetProcessingSize);

    try {
        collectRows(collectRowsClosure, &closure);
    } catch (const TInterruptedIncompleteException&) {
        // Set incomplete and continue
        context->Statistics->IncompleteOutput = true;
    } catch (const TInterruptedCompleteException&) {
        // Continue
    }

    LOG_DEBUG("Flushing writer");
    if (!closure.OutputRowsBatch.empty()) {
        bool shouldNotWait;
        {
            NProfiling::TAggregatingTimingGuard timingGuard(&context->Statistics->WriteTime);
            shouldNotWait = context->Writer->Write(closure.OutputRowsBatch);
        }

        if (!shouldNotWait) {
            NProfiling::TAggregatingTimingGuard timingGuard(&context->Statistics->AsyncTime);
            WaitFor(context->Writer->GetReadyEvent())
                .ThrowOnError();
        }
    }

    LOG_DEBUG("Closing writer");
    {
        NProfiling::TAggregatingTimingGuard timingGuard(&context->Statistics->AsyncTime);
        WaitFor(context->Writer->Close())
            .ThrowOnError();
    }
}

char* AllocateBytes(TExpressionContext* context, size_t byteCount)
{
    return context
        ->IntermediateBuffer
        ->GetPool()
        ->AllocateUnaligned(byteCount);
}

char* AllocatePermanentBytes(TExecutionContext* context, size_t byteCount)
{
    CHECK_STACK();

    return context
        ->PermanentBuffer
        ->GetPool()
        ->AllocateUnaligned(byteCount);
}

////////////////////////////////////////////////////////////////////////////////

char IsRowInArray(
    TExpressionContext* context,
    TComparerFunction* comparer,
    TRow row,
    TSharedRange<TRow>* rows)
{
    CHECK_STACK();

    return std::binary_search(rows->Begin(), rows->End(), row, comparer);
}

size_t StringHash(
    const char* data,
    ui32 length)
{
    return FarmHash(data, length);
}

// FarmHash and MurmurHash hybrid to hash TRow.
ui64 SimpleHash(const TUnversionedValue* begin, const TUnversionedValue* end)
{
    const ui64 MurmurHashConstant = 0xc6a4a7935bd1e995ULL;

    // Append fingerprint to hash value. Like Murmurhash.
    const auto hash64 = [&, MurmurHashConstant] (ui64 data, ui64 value) {
        value ^= FarmFingerprint(data);
        value *= MurmurHashConstant;
        return value;
    };

    // Hash string. Like Murmurhash.
    const auto hash = [&, MurmurHashConstant] (const void* voidData, int length, ui64 seed) {
        ui64 result = seed;
        const ui64* ui64Data = reinterpret_cast<const ui64*>(voidData);
        const ui64* ui64End = ui64Data + (length / 8);

        while (ui64Data < ui64End) {
            auto data = *ui64Data++;
            result = hash64(data, result);
        }

        const char* charData = reinterpret_cast<const char*>(ui64Data);

        if (length & 4) {
            result ^= (*reinterpret_cast<const ui32*>(charData) << (length & 3));
            charData += 4;
        }
        if (length & 2) {
            result ^= (*reinterpret_cast<const ui16*>(charData) << (length & 1));
            charData += 2;
        }
        if (length & 1) {
            result ^= *reinterpret_cast<const ui8*>(charData);
        }

        result *= MurmurHashConstant;
        result ^= (result >> 47);
        result *= MurmurHashConstant;
        result ^= (result >> 47);
        return result;
    };

    ui64 result = end - begin;

    for (auto value = begin; value != end; value++) {
        switch(value->Type) {
            case EValueType::Int64:
                result = hash64(value->Data.Int64, result);
                break;
            case EValueType::Uint64:
                result = hash64(value->Data.Uint64, result);
                break;
            case EValueType::Boolean:
                result = hash64(value->Data.Boolean, result);
                break;
            case EValueType::String:
                result = hash(
                    value->Data.String,
                    value->Length,
                    result);
                break;
            case EValueType::Null:
                result = hash64(0, result);
                break;
            default:
                YUNREACHABLE();
        }
    }

    return result;
}

ui64 FarmHashUint64(ui64 value)
{
    return FarmFingerprint(value);
}

void ThrowException(const char* error)
{
    THROW_ERROR_EXCEPTION("Error while executing UDF: %s", error);
}

void ThrowQueryException(const char* error)
{
    THROW_ERROR_EXCEPTION("Error while executing query: %s", error);
}

google::re2::RE2* RegexCreate(TUnversionedValue* regexp)
{
    return new google::re2::RE2(google::re2::StringPiece(regexp->Data.String, regexp->Length));
}

void RegexDestroy(google::re2::RE2* re2)
{
    delete re2;
}

ui8 RegexFullMatch(google::re2::RE2* re2, TUnversionedValue* string)
{
    YCHECK(string->Type == EValueType::String);

    return google::re2::RE2::FullMatch(
        google::re2::StringPiece(string->Data.String, string->Length),
        *re2);
}

ui8 RegexPartialMatch(google::re2::RE2* re2, TUnversionedValue* string)
{
    YCHECK(string->Type == EValueType::String);

    return google::re2::RE2::PartialMatch(
        google::re2::StringPiece(string->Data.String, string->Length),
        *re2);
}

void CopyString(TExecutionContext* context, TUnversionedValue* result, const std::string& str)
{
    char* data = AllocateBytes(context, str.size());
    memcpy(data, str.c_str(), str.size());
    result->Type = EValueType::String;
    result->Length = str.size();
    result->Data.String = data;
}

void RegexReplaceFirst(
    TExecutionContext* context,
    google::re2::RE2* re2,
    TUnversionedValue* string,
    TUnversionedValue* rewrite,
    TUnversionedValue* result)
{
    YCHECK(string->Type == EValueType::String);
    YCHECK(rewrite->Type == EValueType::String);

    google::re2::string str(string->Data.String, string->Length);
    google::re2::RE2::Replace(
        &str,
        *re2,
        google::re2::StringPiece(rewrite->Data.String, rewrite->Length));

    CopyString(context, result, str);
}


void RegexReplaceAll(
    TExecutionContext* context,
    google::re2::RE2* re2,
    TUnversionedValue* string,
    TUnversionedValue* rewrite,
    TUnversionedValue* result)
{
    YCHECK(string->Type == EValueType::String);
    YCHECK(rewrite->Type == EValueType::String);

    google::re2::string str(string->Data.String, string->Length);
    google::re2::RE2::GlobalReplace(
        &str,
        *re2,
        google::re2::StringPiece(rewrite->Data.String, rewrite->Length));

    CopyString(context, result, str);
}

void RegexExtract(
    TExecutionContext* context,
    google::re2::RE2* re2,
    TUnversionedValue* string,
    TUnversionedValue* rewrite,
    TUnversionedValue* result)
{
    YCHECK(string->Type == EValueType::String);
    YCHECK(rewrite->Type == EValueType::String);

    google::re2::string str;
    google::re2::RE2::Extract(
        google::re2::StringPiece(string->Data.String, string->Length),
        *re2,
        google::re2::StringPiece(rewrite->Data.String, rewrite->Length),
        &str);

    CopyString(context, result, str);
}

void RegexEscape(
    TExecutionContext* context,
    TUnversionedValue* string,
    TUnversionedValue* result)
{
    auto str = google::re2::RE2::QuoteMeta(
        google::re2::StringPiece(string->Data.String, string->Length));

    CopyString(context, result, str);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRoutines

////////////////////////////////////////////////////////////////////////////////

using NCodegen::TRoutineRegistry;

void RegisterQueryRoutinesImpl(TRoutineRegistry* registry)
{
#define REGISTER_ROUTINE(routine) \
    registry->RegisterRoutine(#routine, NRoutines::routine)
    REGISTER_ROUTINE(WriteRow);
    REGISTER_ROUTINE(InsertGroupRow);
    REGISTER_ROUTINE(ScanOpHelper);
    REGISTER_ROUTINE(WriteOpHelper);
    REGISTER_ROUTINE(InsertJoinRow);
    REGISTER_ROUTINE(JoinOpHelper);
    REGISTER_ROUTINE(GroupOpHelper);
    REGISTER_ROUTINE(StringHash);
    REGISTER_ROUTINE(AllocatePermanentRow);
    REGISTER_ROUTINE(AllocateIntermediateRow);
    REGISTER_ROUTINE(AllocatePermanentBytes);
    REGISTER_ROUTINE(AllocateBytes);
    REGISTER_ROUTINE(IsRowInArray);
    REGISTER_ROUTINE(SimpleHash);
    REGISTER_ROUTINE(FarmHashUint64);
    REGISTER_ROUTINE(AddRow);
    REGISTER_ROUTINE(OrderOpHelper);
    REGISTER_ROUTINE(ThrowException);
    REGISTER_ROUTINE(ThrowQueryException);
    REGISTER_ROUTINE(RegexCreate);
    REGISTER_ROUTINE(RegexDestroy);
    REGISTER_ROUTINE(RegexFullMatch);
    REGISTER_ROUTINE(RegexPartialMatch);
    REGISTER_ROUTINE(RegexReplaceFirst);
    REGISTER_ROUTINE(RegexReplaceAll);
    REGISTER_ROUTINE(RegexExtract);
    REGISTER_ROUTINE(RegexEscape);
#undef REGISTER_ROUTINE

    registry->RegisterRoutine("memcmp", std::memcmp);
}

TRoutineRegistry* GetQueryRoutineRegistry()
{
    static TRoutineRegistry registry;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, &RegisterQueryRoutinesImpl, &registry);
    return &registry;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

