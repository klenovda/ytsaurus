#include "evaluator.h"
#include "private.h"
#include "config.h"
#include "evaluation_helpers.h"
#include "folding_profiler.h"
#include "helpers.h"
#include "plan_fragment.h"
#include "query_statistics.h"

#include <yt/ytlib/table_client/schemaful_writer.h>

#include <yt/core/misc/async_cache.h>

#include <yt/core/profiling/scoped_timer.h>

#include <llvm/ADT/FoldingSet.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Threading.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

using namespace NConcurrency;

struct TEvaluatorBufferTag
{ };

////////////////////////////////////////////////////////////////////////////////

class TCachedCGQuery
    : public TAsyncCacheValueBase<
        llvm::FoldingSetNodeID,
        TCachedCGQuery>
{
public:
    TCachedCGQuery(const llvm::FoldingSetNodeID& id, TCGQueryCallback&& function)
        : TAsyncCacheValueBase(id)
        , Function_(std::move(function))
    { }

    TCGQueryCallback GetQueryCallback()
    {
        return Function_;
    }

private:
    TCGQueryCallback Function_;
};

typedef TIntrusivePtr<TCachedCGQuery> TCachedCGQueryPtr;

class TEvaluator::TImpl
    : public TAsyncSlruCacheBase<llvm::FoldingSetNodeID, TCachedCGQuery>
{
public:
    explicit TImpl(TExecutorConfigPtr config)
        : TAsyncSlruCacheBase(config->CGCache)
    { }

    TQueryStatistics Run(
        TConstQueryPtr query,
        ISchemafulReaderPtr reader,
        ISchemafulWriterPtr writer,
        const TExecuteQuery& executeCallback,
        const TConstFunctionProfilerMapPtr& functionProfilers,
        const TConstAggregateProfilerMapPtr& aggregateProfilers,
        bool enableCodeCache)
    {
        TRACE_CHILD("QueryClient", "Evaluate") {
            TRACE_ANNOTATION("fragment_id", query->Id);
            auto queryFingerprint = InferName(query, true);
            TRACE_ANNOTATION("query_fingerprint", queryFingerprint);

            auto Logger = BuildLogger(query);

            LOG_DEBUG("Executing query (Fingerprint: %v, InputSchema: %v, RenamedSchema: %v, ResultSchema: %v)",
                queryFingerprint,
                NYTree::ConvertToYsonString(query->TableSchema, NYson::EYsonFormat::Text).Data(),
                NYTree::ConvertToYsonString(query->RenamedTableSchema, NYson::EYsonFormat::Text).Data(),
                NYTree::ConvertToYsonString(query->GetTableSchema(), NYson::EYsonFormat::Text).Data());

            TQueryStatistics statistics;
            TDuration wallTime;

            try {
                NProfiling::TAggregatingTimingGuard timingGuard(&wallTime);

                TCGVariables fragmentParams;
                auto cgQuery = Codegen(
                    query,
                    fragmentParams,
                    functionProfilers,
                    aggregateProfilers,
                    statistics,
                    enableCodeCache);

                LOG_DEBUG("Evaluating plan fragment");

                auto permanentBuffer = New<TRowBuffer>(TEvaluatorBufferTag());
                auto outputBuffer = New<TRowBuffer>(TEvaluatorBufferTag());
                auto intermediateBuffer = New<TRowBuffer>(TEvaluatorBufferTag());

                std::vector<TRow> outputBatchRows;
                outputBatchRows.reserve(RowsetProcessingSize);

                // NB: function contexts need to be destroyed before cgQuery since it hosts destructors.
                TExecutionContext executionContext;
                executionContext.Reader = reader;

                executionContext.PermanentBuffer = permanentBuffer;
                executionContext.OutputBuffer = outputBuffer;
                executionContext.IntermediateBuffer = intermediateBuffer;
                executionContext.Writer = writer;
                executionContext.OutputRowsBatch = &outputBatchRows;
                executionContext.Statistics = &statistics;
                executionContext.InputRowLimit = query->InputRowLimit;
                executionContext.OutputRowLimit = query->OutputRowLimit;
                executionContext.GroupRowLimit = query->OutputRowLimit;
                executionContext.JoinRowLimit = query->OutputRowLimit;
                executionContext.Limit = query->Limit;

                // Used in joins
                executionContext.ExecuteCallback = executeCallback;

                if (!query->JoinClauses.empty()) {
                    YCHECK(executeCallback);
                }

                LOG_DEBUG("Evaluating query");

                try {
                    CallCGQueryPtr(
                        cgQuery,
                        fragmentParams.GetOpaqueData(),
                        &executionContext);
                } catch (const TInterruptedIncompleteException&) {
                    // Set incomplete and continue
                    executionContext.Statistics->IncompleteOutput = true;
                } catch (const TInterruptedCompleteException&) {
                    // Continue
                }

                LOG_DEBUG("Flushing writer");
                if (!outputBatchRows.empty()) {
                    bool shouldNotWait;
                    {
                        NProfiling::TAggregatingTimingGuard timingGuard(&statistics.WriteTime);
                        shouldNotWait = writer->Write(outputBatchRows);
                    }

                    if (!shouldNotWait) {
                        NProfiling::TAggregatingTimingGuard timingGuard(&statistics.AsyncTime);
                        WaitFor(writer->GetReadyEvent())
                            .ThrowOnError();
                    }
                }

                LOG_DEBUG("Closing writer");
                {
                    NProfiling::TAggregatingTimingGuard timingGuard(&statistics.AsyncTime);
                    WaitFor(writer->Close())
                        .ThrowOnError();
                }

                LOG_DEBUG("Finished evaluating plan fragment ("
                    "PermanentBufferCapacity: %v, "
                    "OutputBufferCapacity: %v, "
                    "IntermediateBufferCapacity: %v)",
                    permanentBuffer->GetCapacity(),
                    outputBuffer->GetCapacity(),
                    intermediateBuffer->GetCapacity());

            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Query evaluation failed") << ex;
            }

            statistics.SyncTime = wallTime - statistics.AsyncTime;
            statistics.ExecuteTime = statistics.SyncTime - statistics.ReadTime - statistics.WriteTime;

            LOG_DEBUG("Query statistics (%v)", statistics);

            TRACE_ANNOTATION("rows_read", statistics.RowsRead);
            TRACE_ANNOTATION("rows_written", statistics.RowsWritten);
            TRACE_ANNOTATION("sync_time", statistics.SyncTime);
            TRACE_ANNOTATION("async_time", statistics.AsyncTime);
            TRACE_ANNOTATION("execute_time", statistics.ExecuteTime);
            TRACE_ANNOTATION("read_time", statistics.ReadTime);
            TRACE_ANNOTATION("write_time", statistics.WriteTime);
            TRACE_ANNOTATION("codegen_time", statistics.CodegenTime);
            TRACE_ANNOTATION("incomplete_input", statistics.IncompleteInput);
            TRACE_ANNOTATION("incomplete_output", statistics.IncompleteOutput);

            return statistics;
        }
    }

private:
    TCGQueryCallback Codegen(
        TConstQueryPtr query,
        TCGVariables& variables,
        const TConstFunctionProfilerMapPtr& functionProfilers,
        const TConstAggregateProfilerMapPtr& aggregateProfilers,
        TQueryStatistics& statistics,
        bool enableCodeCache)
    {
        llvm::FoldingSetNodeID id;

        auto makeCodegenQuery = Profile(query, &id, &variables, functionProfilers, aggregateProfilers);

        auto Logger = BuildLogger(query);

        auto compileWithLogging = [&] () {
            TRACE_CHILD("QueryClient", "Compile") {
                NProfiling::TAggregatingTimingGuard timingGuard(&statistics.CodegenTime);
                LOG_DEBUG("Started compiling fragment");
                auto cgQuery = New<TCachedCGQuery>(id, makeCodegenQuery());
                LOG_DEBUG("Finished compiling fragment");
                return cgQuery;
            }
        };

        TCachedCGQueryPtr cgQuery;
        if (enableCodeCache) {
            auto cookie = BeginInsert(id);
            if (cookie.IsActive()) {
                LOG_DEBUG("Codegen cache miss");

                try {
                    cookie.EndInsert(compileWithLogging());
                } catch (const std::exception& ex) {
                    cookie.Cancel(TError(ex).Wrap("Failed to compile a query fragment"));
                }
            }

            cgQuery = WaitFor(cookie.GetValue())
                .ValueOrThrow();
        } else {
            LOG_DEBUG("Codegen cache disabled");

            cgQuery = compileWithLogging();
        }

        return cgQuery->GetQueryCallback();
    }

    static void CallCGQuery(
        const TCGQueryCallback& cgQuery,
        void* const* opaqueValues,
        TExecutionContext* executionContext)
    {
        cgQuery(opaqueValues, executionContext);
    }

    void(*volatile CallCGQueryPtr)(
        const TCGQueryCallback& cgQuery,
        void* const* opaqueValues,
        TExecutionContext* executionContext) = CallCGQuery;

};

////////////////////////////////////////////////////////////////////////////////

TEvaluator::TEvaluator(TExecutorConfigPtr config)
    : Impl_(New<TImpl>(std::move(config)))
{ }

TEvaluator::~TEvaluator()
{ }

TQueryStatistics TEvaluator::RunWithExecutor(
    TConstQueryPtr query,
    ISchemafulReaderPtr reader,
    ISchemafulWriterPtr writer,
    TExecuteQuery executeCallback,
    TConstFunctionProfilerMapPtr functionProfilers,
    TConstAggregateProfilerMapPtr aggregateProfilers,
    bool enableCodeCache)
{
    return Impl_->Run(
        std::move(query),
        std::move(reader),
        std::move(writer),
        std::move(executeCallback),
        functionProfilers,
        aggregateProfilers,
        enableCodeCache);
}

TQueryStatistics TEvaluator::Run(
    TConstQueryPtr query,
    ISchemafulReaderPtr reader,
    ISchemafulWriterPtr writer,
    TConstFunctionProfilerMapPtr functionProfilers,
    TConstAggregateProfilerMapPtr aggregateProfilers,
    bool enableCodeCache)
{
    return RunWithExecutor(
        std::move(query),
        std::move(reader),
        std::move(writer),
        nullptr,
        std::move(functionProfilers),
        std::move(aggregateProfilers),
        enableCodeCache);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
