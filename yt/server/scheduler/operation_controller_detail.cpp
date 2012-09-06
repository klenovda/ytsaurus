#include "stdafx.h"
#include "operation_controller_detail.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "job_resources.h"

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/chunk_client//chunk_list_ypath_proxy.h>

#include <ytlib/object_client/object_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/convert.h>

#include <ytlib/formats/format.h>

#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>
#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/table_client/key.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NFormats;
using namespace NJobProxy;

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TTask::TTask(TOperationControllerBase* controller)
    : Controller(controller)
    , CachedPendingJobCount(0)
    , Logger(Controller->Logger)
{ }

int TOperationControllerBase::TTask::GetPendingJobCountDelta()
{
    int oldValue = CachedPendingJobCount;
    int newValue = GetPendingJobCount();
    CachedPendingJobCount = newValue;
    return newValue - oldValue;
}

i64 TOperationControllerBase::TTask::GetLocality(const Stroka& address) const
{
    return ChunkPool->GetLocality(address);
}

bool TOperationControllerBase::TTask::IsStrictlyLocal() const
{
    return false;
}

int TOperationControllerBase::TTask::GetPriority() const
{
    return 0;
}

void TOperationControllerBase::TTask::AddStripe(TChunkStripePtr stripe)
{
    ChunkPool->Add(stripe);
    AddInputLocalityHint(stripe);
    AddPendingHint();
}

void TOperationControllerBase::TTask::AddStripes(const std::vector<TChunkStripePtr>& stripes)
{
    FOREACH (auto stripe, stripes) {
        AddStripe(stripe);
    }
}

TJobPtr TOperationControllerBase::TTask::ScheduleJob(ISchedulingContext* context)
{
    if (!Controller->CheckAvailableChunkLists(GetChunkListCountPerJob())) {
        return NULL;
    }

    auto jip = New<TJobInProgress>(this);

    auto address = context->GetNode()->GetAddress();
    auto dataSizeThreshold = GetJobDataSizeThreshold();
    jip->PoolResult = ChunkPool->Extract(address, dataSizeThreshold);

    LOG_DEBUG("Job chunks extracted (TotalCount: %d, LocalCount: %d, ExtractedDataSize: %" PRId64 ", DataSizeThreshold: %s)",
        jip->PoolResult->TotalChunkCount,
        jip->PoolResult->LocalChunkCount,
        jip->PoolResult->TotalDataSize,
        ~ToString(dataSizeThreshold));

    jip->Job = context->StartJob(Controller->Operation);
    auto* jobSpec = jip->Job->GetSpec();

    BuildJobSpec(jip, jobSpec);
    *jobSpec->mutable_resource_utilization() = GetRequestedResourcesForJip(jip);

    Controller->RegisterJobInProgress(jip);

    OnJobStarted(jip);

    return jip->Job;
}

bool TOperationControllerBase::TTask::IsPending() const
{
    return ChunkPool->IsPending();
}

bool TOperationControllerBase::TTask::IsCompleted() const
{
    return ChunkPool->IsCompleted();
}

const TProgressCounter& TOperationControllerBase::TTask::DataSizeCounter() const
{
    return ChunkPool->DataSizeCounter();
}

const TProgressCounter& TOperationControllerBase::TTask::ChunkCounter() const
{
    return ChunkPool->ChunkCounter();
}

void TOperationControllerBase::TTask::OnJobStarted(TJobInProgressPtr jip)
{
    UNUSED(jip);
}

void TOperationControllerBase::TTask::OnJobCompleted(TJobInProgressPtr jip)
{
    ChunkPool->OnCompleted(jip->PoolResult);
}

void TOperationControllerBase::TTask::OnJobFailed(TJobInProgressPtr jip)
{
    ChunkPool->OnFailed(jip->PoolResult);

    Controller->ReleaseChunkLists(jip->ChunkListIds);

    FOREACH (const auto& stripe, jip->PoolResult->Stripes) {
        AddInputLocalityHint(stripe);
    }
    AddPendingHint();
}

void TOperationControllerBase::TTask::OnTaskCompleted()
{
    LOG_DEBUG("Task completed (Task: %s)", ~GetId());
}

void TOperationControllerBase::TTask::AddPendingHint()
{
    Controller->AddTaskPendingHint(this);
}

void TOperationControllerBase::TTask::AddInputLocalityHint(TChunkStripePtr stripe)
{
    Controller->AddTaskLocalityHint(this, stripe);
}

i64 TOperationControllerBase::TTask::GetJobDataSizeThresholdGeneric(int pendingJobCount, i64 pendingDataSize)
{
    return static_cast<i64>(std::ceil((double) pendingDataSize / pendingJobCount));
}

void TOperationControllerBase::TTask::AddSequentialInputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobInProgressPtr jip)
{
    auto* inputSpec = jobSpec->add_input_specs();
    FOREACH (const auto& stripe, jip->PoolResult->Stripes) {
        AddInputChunks(inputSpec, stripe);
    }
}

void TOperationControllerBase::TTask::AddParallelInputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobInProgressPtr jip)
{
    FOREACH (const auto& stripe, jip->PoolResult->Stripes) {
        auto* inputSpec = jobSpec->add_input_specs();
        AddInputChunks(inputSpec, stripe);
    }
}

void TOperationControllerBase::TTask::AddTabularOutputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobInProgressPtr jip,
    int tableIndex)
{
    const auto& table = Controller->OutputTables[tableIndex];
    auto* outputSpec = jobSpec->add_output_specs();
    outputSpec->set_channels(table.Channels.Data());
    auto chunkListId = Controller->GetFreshChunkList();
    jip->ChunkListIds.push_back(chunkListId);
    *outputSpec->mutable_chunk_list_id() = chunkListId.ToProto();
}

void TOperationControllerBase::TTask::AddInputChunks(
    NScheduler::NProto::TTableInputSpec* inputSpec,
    TChunkStripePtr stripe)
{
    FOREACH (const auto& weightedChunk, stripe->Chunks) {
        auto* inputChunk = inputSpec->add_chunks();
        *inputChunk = *weightedChunk.InputChunk;
        inputChunk->set_uncompressed_data_size(weightedChunk.DataSizeOverride);
        inputChunk->set_row_count(weightedChunk.RowCountOverride);
    }
}

NProto::TNodeResources TOperationControllerBase::TTask::GetRequestedResourcesForJip(TJobInProgressPtr jip) const
{
    UNUSED(jip);
    return GetMinRequestedResources();
}

bool TOperationControllerBase::TTask::HasEnoughResources(TExecNodePtr node) const
{
    return NScheduler::HasEnoughResources(
        node->ResourceUtilization(),
        GetMinRequestedResources(),
        node->ResourceLimits());
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TOperationControllerBase(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
    : Config(config)
    , Host(host)
    , Operation(operation)
    , ObjectProxy(host->GetLeaderChannel())
    , Logger(OperationLogger)
    , Active(false)
    , Running(false)
    , LastChunkListAllocationCount(-1)
    , RunningJobCount(0)
    , CompletedJobCount(0)
    , FailedJobCount(0)
    , PendingTaskInfos(MaxTaskPriority + 1)
    , CachedPendingJobCount(0)
{
    Logger.AddTag(Sprintf("OperationId: %s", ~operation->GetOperationId().ToString()));
}

void TOperationControllerBase::Initialize()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    
    LOG_INFO("Initializing operation");

    FOREACH (const auto& path, GetInputTablePaths()) {
        TInputTable table;
        table.Path = path;
        InputTables.push_back(table);
    }

    FOREACH (const auto& path, GetOutputTablePaths()) {
        TOutputTable table;
        table.Path = path;
        if (path.Attributes().Get<bool>("overwrite", false)) {
            table.Clear = true;
        }
        OutputTables.push_back(table);
    }

    FOREACH (const auto& path, GetFilePaths()) {
        TUserFile file;
        file.Path = path;
        Files.push_back(file);
    }

    try {
        DoInitialize();
    } catch (const std::exception& ex) {
        LOG_INFO("Operation has failed to initialize\n%s", ex.what());
        Active = false;
        throw;
    }

    Active = true;

    LOG_INFO("Operation initialized");
}

void TOperationControllerBase::DoInitialize()
{ }

void TOperationControllerBase::DoBuildProgressYson(IYsonConsumer* consumer)
{
    UNUSED(consumer);
}

TFuture<void> TOperationControllerBase::Prepare()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto this_ = MakeStrong(this);
    auto pipeline = StartAsyncPipeline(Host->GetBackgroundInvoker())
        ->Add(BIND(&TThis::StartPrimaryTransaction, MakeStrong(this)))
        ->Add(BIND(&TThis::OnPrimaryTransactionStarted, MakeStrong(this)))
        ->Add(BIND(&TThis::StartSeconaryTransactions, MakeStrong(this)))
        ->Add(BIND(&TThis::OnSecondaryTransactionsStarted, MakeStrong(this)))
        ->Add(BIND(&TThis::GetObjectIds, MakeStrong(this)))
        ->Add(BIND(&TThis::OnObjectIdsReceived, MakeStrong(this)))
        ->Add(BIND(&TThis::RequestInputs, MakeStrong(this)))
        ->Add(BIND(&TThis::OnInputsReceived, MakeStrong(this)))
        ->Add(BIND(&TThis::CompletePreparation, MakeStrong(this)));
     pipeline = CustomizePreparationPipeline(pipeline);
     return pipeline
        ->Add(BIND(&TThis::OnPreparationCompleted, MakeStrong(this)))
        ->Run()
        .Apply(BIND([=] (TValueOrError<void> result) -> TFuture<void> {
            if (result.IsOK()) {
                if (this_->Active) {
                    this_->Running = true;
                }
                return MakeFuture();
            } else {
                LOG_WARNING("Operation preparation failed\n%s", ~ToString(result));
                this_->Active = false;
                this_->Host->OnOperationFailed(this_->Operation, result);
                // This promise is never fulfilled.
                return NewPromise<void>();
            }
        }));
}

TFuture<void> TOperationControllerBase::Revive()
{
    try {
        Initialize();
    } catch (const std::exception& ex) {
        OnOperationFailed(TError("Operation has failed to initialize")
            << ex);
        // This promise is never fulfilled.
        return NewPromise<void>();
    }
    return Prepare();
}

TFuture<void> TOperationControllerBase::Commit()
{
    VERIFY_THREAD_AFFINITY_ANY();

    YCHECK(Active);

    LOG_INFO("Committing operation");

    auto this_ = MakeStrong(this);
    return StartAsyncPipeline(Host->GetBackgroundInvoker())
        ->Add(BIND(&TThis::CommitOutputs, MakeStrong(this)))
        ->Add(BIND(&TThis::OnOutputsCommitted, MakeStrong(this)))
        ->Run()
        .Apply(BIND([=] (TValueOrError<void> result) -> TFuture<void> {
            Active = false;
            if (result.IsOK()) {
                LOG_INFO("Operation committed");
                return MakeFuture();
            } else {
                LOG_WARNING("Operation has failed to commit\n%s", ~ToString(result));
                this_->Host->OnOperationFailed(this_->Operation, result);
                return NewPromise<void>();
            }
        }));
}

void TOperationControllerBase::OnJobRunning(TJobPtr job)
{
    UNUSED(job);
}

void TOperationControllerBase::OnJobCompleted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    --RunningJobCount;
    ++CompletedJobCount;

    auto jip = GetJobInProgress(job);
    jip->Task->OnJobCompleted(jip);
    
    RemoveJobInProgress(job);

    LogProgress();

    if (jip->Task->IsCompleted()) {
        jip->Task->OnTaskCompleted();
    }

    if (RunningJobCount == 0 && GetPendingJobCount() == 0) {
        OnOperationCompleted();
    }
}

void TOperationControllerBase::OnJobFailed(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    --RunningJobCount;
    ++FailedJobCount;

    auto jip = GetJobInProgress(job);
    jip->Task->OnJobFailed(jip);

    RemoveJobInProgress(job);

    LogProgress();

    if (FailedJobCount >= Config->FailedJobsLimit) {
        OnOperationFailed(TError("Failed jobs limit %d has been reached",
            Config->FailedJobsLimit));
    }

    FOREACH (const auto& chunkId, job->Result().failed_chunk_ids()) {
        OnChunkFailed(TChunkId::FromProto(chunkId));
    }
}

void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    if (InputChunkIds.find(chunkId) == InputChunkIds.end()) {
        LOG_WARNING("Intermediate chunk %s has failed", ~chunkId.ToString());
        OnIntermediateChunkFailed(chunkId);
    } else {
        LOG_WARNING("Input chunk %s has failed", ~chunkId.ToString());
        OnInputChunkFailed(chunkId);
    }
}

void TOperationControllerBase::OnInputChunkFailed(const TChunkId& chunkId)
{
    OnOperationFailed(TError("Unable to read input chunk %s", ~chunkId.ToString()));
}

void TOperationControllerBase::OnIntermediateChunkFailed(const TChunkId& chunkId)
{
    OnOperationFailed(TError("Unable to read intermediate chunk %s", ~chunkId.ToString()));
}

void TOperationControllerBase::Abort()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Aborting operation");

    Running = false;
    Active = false;

    AbortTransactions();

    LOG_INFO("Operation aborted");
}

TJobPtr TOperationControllerBase::ScheduleJob(ISchedulingContext* context)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
 
    if (!Running) {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        return NULL;
    }

    if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        return NULL;
    }

    // Make a course check to see if the node has enough resources.
    auto node = context->GetNode();
    if (!HasEnoughResources(
            node->ResourceUtilization(),
            GetMinRequestedResources(),
            node->ResourceLimits()))
    {
        return NULL;
    }

    auto job = DoScheduleJob(context);
    if (!job) {
        return NULL;
    }

    ++RunningJobCount;
    LogProgress();
    return job;
}

void TOperationControllerBase::UpdatePendingJobCount(TTaskPtr task)
{
    int oldValue = CachedPendingJobCount;
    int newValue = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newValue;

    LOG_DEBUG_IF(newValue != oldValue, "Pending job count updated (Task: %s, Count: %d -> %d)",
        ~task->GetId(),
        oldValue,
        newValue);
}

void TOperationControllerBase::AddTaskPendingHint(TTaskPtr task)
{
    if (!task->IsStrictlyLocal() && task->GetPendingJobCount() > 0) {
        auto* info = GetPendingTaskInfo(task);
        if (info->GlobalTasks.insert(task).second) {
            LOG_DEBUG("Task pending hint added (Task: %s)",
                ~task->GetId());
        }
    }
    UpdatePendingJobCount(task);
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    auto* info = GetPendingTaskInfo(task);
    if (info->AddressToLocalTasks[address].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %s, Address: %s)",
            ~task->GetId(),
            ~address);
    }
}

TOperationControllerBase::TPendingTaskInfo* TOperationControllerBase::GetPendingTaskInfo(TTaskPtr task)
{
    int priority = task->GetPriority();
    YASSERT(priority >= 0 && priority <= MaxTaskPriority);
    return &PendingTaskInfos[priority];
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    DoAddTaskLocalityHint(task, address);
    UpdatePendingJobCount(task);
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe)
{
    FOREACH (const auto& chunk, stripe->Chunks) {
        const auto& inputChunk = chunk.InputChunk;
        FOREACH (const auto& address, inputChunk->node_addresses()) {
            DoAddTaskLocalityHint(task, address);
        }
    }
    UpdatePendingJobCount(task);
}

TJobPtr TOperationControllerBase::DoScheduleJob(ISchedulingContext* context)
{
    // First try to find a local task for this node.
    auto now = TInstant::Now();
    auto node = context->GetNode();
    auto address = node->GetAddress();
    for (int priority = static_cast<int>(PendingTaskInfos.size()) - 1; priority >= 0; --priority) {
        auto& info = PendingTaskInfos[priority];
        auto localTasksIt = info.AddressToLocalTasks.find(address);
        if (localTasksIt == info.AddressToLocalTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask = NULL;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            i64 locality = task->GetLocality(address);
            if (locality <= 0) {
                localTasks.erase(jt);
                LOG_TRACE("Task locality hint removed (Task: %s, Address: %s)",
                    ~task->GetId(),
                    ~address);
                continue;
            }

            if (locality <= bestLocality) {
                continue;
            }

            if (!task->HasEnoughResources(node)) {
                continue;
            }

            if (task->GetPendingJobCount() == 0) {
                UpdatePendingJobCount(task);
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (bestTask) {
            auto job = bestTask->ScheduleJob(context);
            if (job) {
                auto delayedTime = bestTask->GetDelayedTime();
                LOG_DEBUG("Scheduled a local job (Task: %s, Address: %s, Priority: %d, Locality: %" PRId64 ", Delay: %s)",
                    ~bestTask->GetId(),
                    ~address,
                    priority,
                    bestLocality,
                    delayedTime ? ~ToString(now - delayedTime.Get()) : "Null");
                bestTask->SetDelayedTime(Null);
                UpdatePendingJobCount(bestTask);
                return job;
            }
        }
    }

    // Next look for other (global) tasks.
    for (int priority = static_cast<int>(PendingTaskInfos.size()) - 1; priority >= 0; --priority) {
        auto& info = PendingTaskInfos[priority];
        auto& globalTasks = info.GlobalTasks;
        auto it = globalTasks.begin();
        while (it != globalTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            if (task->GetPendingJobCount() == 0) {
                LOG_DEBUG("Task pending hint removed (Task: %s)", ~task->GetId());
                globalTasks.erase(jt);
                UpdatePendingJobCount(task);
                continue;
            }

            if (!task->HasEnoughResources(node)) {
                continue;
            }

            // Check for delayed execution.
            auto delayedTime = task->GetDelayedTime();
            auto localityTimeout = task->GetLocalityTimeout();
            if (localityTimeout != TDuration::Zero()) {
                if (!delayedTime) {
                    task->SetDelayedTime(now);
                    continue;
                }
                if (delayedTime.Get() + localityTimeout > now) {
                    continue;
                }
            }

            auto job = task->ScheduleJob(context);
            if (job) {
                LOG_DEBUG("Scheduled a non-local job (Task: %s, Address: %s, Priority: %d, Delay: %s)",
                    ~task->GetId(),
                    ~address,
                    priority,
                    delayedTime ? ~ToString(now - delayedTime.Get()) : "Null");
                UpdatePendingJobCount(task);
                return job;
            }
        }
    }

    return NULL;
}

int TOperationControllerBase::GetPendingJobCount()
{
    return CachedPendingJobCount;
}

void TOperationControllerBase::OnOperationCompleted()
{
    VERIFY_THREAD_AFFINITY_ANY();

    YCHECK(Active);
    LOG_INFO("Operation completed");

    Running = false;

    Host->OnOperationCompleted(Operation);
}

void TOperationControllerBase::OnOperationFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!Active)
        return;

    LOG_WARNING("Operation failed\n%s", ~ToString(error));

    Running = false;
    Active = false;

    Host->OnOperationFailed(Operation, error);
}

void TOperationControllerBase::AbortTransactions()
{
    LOG_INFO("Aborting transactions");

    if (PrimaryTransaction) {
        // The call is async.
        PrimaryTransaction->Abort();
    }

    // No need to abort the others.
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::CommitOutputs()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Committing outputs");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, OutputTables) {
        auto ypath = FromObjectId(table.ObjectId);
        {
            auto req = TChunkListYPathProxy::Attach(FromObjectId(table.OutputChunkListId));
            FOREACH (const auto& pair, table.OutputChunkTreeIds) {
                *req->add_children_ids() = pair.second.ToProto();
            }
            batchReq->AddRequest(req, "attach_out");
        }
        if (table.SetSorted) {
            auto req = TTableYPathProxy::SetSorted(WithTransaction(ypath, OutputTransaction->GetId()));
            ToProto(req->mutable_key_columns(), table.KeyColumns);
            batchReq->AddRequest(req, "set_out_sorted");
        }
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(InputTransaction->GetId()));
        batchReq->AddRequest(req, "commit_in_tx");
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(OutputTransaction->GetId()));
        batchReq->AddRequest(req, "commit_out_tx");
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(PrimaryTransaction->GetId()));
        batchReq->AddRequest(req, "commit_primary_tx");
    }

    // We don't need pings any longer, detach the transactions.
    PrimaryTransaction->Detach();
    InputTransaction->Detach();
    OutputTransaction->Detach();

    return batchReq->Invoke();
}

void TOperationControllerBase::OnOutputsCommitted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error committing outputs");

    {
        auto rsps = batchRsp->GetResponses("attach_out");
        FOREACH (auto rsp, rsps) {
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error attaching chunk trees");
        }
    }

    {
        auto rsps = batchRsp->GetResponses("set_out_sorted");
        FOREACH (auto rsp, rsps) {
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error marking output table as sorted");
        }
    }

    {
        auto rsp = batchRsp->GetResponse("commit_in_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing input transaction");
    }

    {
        auto rsp = batchRsp->GetResponse("commit_out_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing output transaction");
    }

    {
        auto rsp = batchRsp->GetResponse("commit_primary_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing primary transaction");
    }

    LOG_INFO("Outputs committed");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::StartPrimaryTransaction()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Starting primary transaction");

    auto batchReq = ObjectProxy.ExecuteBatch();

    {
        auto req = TTransactionYPathProxy::CreateObject(
            Operation->GetTransactionId() == NullTransactionId
            ? RootTransactionPath
            : FromObjectId(Operation->GetTransactionId()));
        req->set_type(EObjectType::Transaction);
        batchReq->AddRequest(req, "start_primary_tx");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnPrimaryTransactionStarted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error starting primary transaction");

    {
        auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_primary_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error starting primary transaction");
        auto id = TTransactionId::FromProto(rsp->object_id());
        LOG_INFO("Primary transaction is %s", ~id.ToString());
        PrimaryTransaction = Host->GetTransactionManager()->Attach(id, true);
    }
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::StartSeconaryTransactions()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Starting secondary transactions");

    auto batchReq = ObjectProxy.ExecuteBatch();

    {
        auto req = TTransactionYPathProxy::CreateObject(FromObjectId(PrimaryTransaction->GetId()));
        req->set_type(EObjectType::Transaction);
        batchReq->AddRequest(req, "start_in_tx");
    }

    {
        auto req = TTransactionYPathProxy::CreateObject(FromObjectId(PrimaryTransaction->GetId()));
        req->set_type(EObjectType::Transaction);
        batchReq->AddRequest(req, "start_out_tx");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnSecondaryTransactionsStarted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error starting secondary transactions");

    {
        auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_in_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error starting input transaction");
        auto id = TTransactionId::FromProto(rsp->object_id());
        LOG_INFO("Input transaction is %s", ~id.ToString());
        InputTransaction = Host->GetTransactionManager()->Attach(id, true);
    }

    {
        auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_out_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error starting output transaction");
        auto id = TTransactionId::FromProto(rsp->object_id());
        LOG_INFO("Output transaction is %s", ~id.ToString());
        OutputTransaction = Host->GetTransactionManager()->Attach(id, true);
    }
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::GetObjectIds()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Getting object ids");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto req = TObjectYPathProxy::GetId(WithTransaction(table.Path.GetPath(), InputTransaction->GetId()));
        req->set_allow_nonempty_path_suffix(true);
        batchReq->AddRequest(req, "get_in_id");
    }

    FOREACH (const auto& table, OutputTables) {
        auto req = TObjectYPathProxy::GetId(WithTransaction(table.Path.GetPath(), InputTransaction->GetId()));
        // TODO(babenko): should we allow this?
        req->set_allow_nonempty_path_suffix(true);
        batchReq->AddRequest(req, "get_out_id");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnObjectIdsReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error getting object ids");

    {
        auto getInIdRsps = batchRsp->GetResponses<TObjectYPathProxy::TRspGetId>("get_in_id");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                auto rsp = getInIdRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error getting id for input table %s", ~table.Path.GetPath()));
                table.ObjectId = TObjectId::FromProto(rsp->object_id());
            }
        }
    }

    {
        auto getOutIdRsps = batchRsp->GetResponses<TObjectYPathProxy::TRspGetId>("get_out_id");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            {
                auto rsp = getOutIdRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error getting id for output table %s",
                        ~table.Path.GetPath()));
                table.ObjectId = TObjectId::FromProto(rsp->object_id());
            }
        }
    }

    LOG_INFO("Object ids received");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::RequestInputs()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Requesting inputs");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto ypath = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(WithTransaction(ypath, InputTransaction->GetId()));
            req->set_mode(ELockMode::Snapshot);
            batchReq->AddRequest(req, "lock_in");
        }
        {
            // NB: Use table.Path, not YPath here, otherwise path suffix is ignored.
            auto req = TTableYPathProxy::Fetch(WithTransaction(table.Path.GetPath(), InputTransaction->GetId()));
            req->set_fetch_node_addresses(true);
            req->set_fetch_all_meta_extensions(true);
            req->set_negate(table.NegateFetch);
            batchReq->AddRequest(req, "fetch_in");
        }
        {
            auto req = TYPathProxy::Get(WithTransaction(ypath, InputTransaction->GetId()) + "/@sorted");
            batchReq->AddRequest(req, "get_in_sorted");
        }
        {
            auto req = TYPathProxy::Get(WithTransaction(ypath, InputTransaction->GetId()) + "/@sorted_by");
            batchReq->AddRequest(req, "get_in_sorted_by");
        }
    }

    FOREACH (const auto& table, OutputTables) {
        auto ypath = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(WithTransaction(ypath, OutputTransaction->GetId()));
            req->set_mode(table.Clear ? ELockMode::Exclusive : ELockMode::Shared);
            batchReq->AddRequest(req, "lock_out");
        }
        {
            auto req = TYPathProxy::Get(WithTransaction(ypath, Operation->GetTransactionId()) + "/@channels");
            batchReq->AddRequest(req, "get_out_channels");
        }
        {
            auto req = TYPathProxy::Get(WithTransaction(ypath, OutputTransaction->GetId()) + "/@row_count");
            batchReq->AddRequest(req, "get_out_row_count");
        }
        {
            auto req = TTableYPathProxy::Clear(WithTransaction(ypath, OutputTransaction->GetId()));
            // Even if |Clear| is False we still add a dummy request
            // to keep "clear_out" requests aligned with output tables.            // 
            batchReq->AddRequest(table.Clear ? req : NULL, "clear_out");
        }
        {
            auto req = TTableYPathProxy::GetChunkListForUpdate(WithTransaction(ypath, OutputTransaction->GetId()));
            batchReq->AddRequest(req, "get_out_chunk_list");
        }
    }

    FOREACH (const auto& file, Files) {
        auto ypath = file.Path.GetPath();
        {
            auto req = TFileYPathProxy::Fetch(WithTransaction(ypath, Operation->GetTransactionId()));
            batchReq->AddRequest(req, "fetch_files");
        }
    }

    RequestCustomInputs(batchReq);

    return batchReq->Invoke();
}

void TOperationControllerBase::OnInputsReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error requesting inputs");

    {
        auto fetchInRsps = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>("fetch_in");
        auto lockInRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_in");
        auto getInSortedRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_in_sorted");
        auto getInKeyColumns = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_in_sorted_by");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                auto rsp = lockInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error locking input table %s", ~table.Path.GetPath()));
                LOG_INFO("Input table %s was locked successfully",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = fetchInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error fetching input input table %s", ~table.Path.GetPath()));
                table.FetchResponse = rsp;
                FOREACH (const auto& chunk, rsp->chunks()) {
                    auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
                    if (chunk.node_addresses_size() == 0) {
                        THROW_ERROR_EXCEPTION("Chunk %s in input table %s is lost",
                            ~chunkId.ToString(),
                            ~table.Path.GetPath());
                    }
                    InputChunkIds.insert(chunkId);
                }
                LOG_INFO("Input table %s has %d chunks",
                    ~table.Path.GetPath(),
                    rsp->chunks_size());
            }
            {
                auto rsp = getInSortedRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error getting \"sorted\" attribute for input table %s", ~table.Path.GetPath()));
                table.Sorted = ConvertTo<bool>(TYsonString(rsp->value()));
                LOG_INFO("Input table %s is %s",
                    ~table.Path.GetPath(),
                    table.Sorted ? "sorted" : "not sorted");
            }
            if (table.Sorted) {
                auto rsp = getInKeyColumns[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error getting \"sorted_by\" attribute for input table %s",
                        ~table.Path.GetPath()));
                table.KeyColumns = ConvertTo< std::vector<Stroka> >(TYsonString(rsp->value()));
                LOG_INFO("Input table %s has key columns %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns, EYsonFormat::Text).Data());
            }
        }
    }

    {
        auto lockOutRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_out");
        auto clearOutRsps = batchRsp->GetResponses<TTableYPathProxy::TRspClear>("clear_out");
        auto getOutChunkListRsps = batchRsp->GetResponses<TTableYPathProxy::TRspGetChunkListForUpdate>("get_out_chunk_list");
        auto getOutChannelsRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_out_channels");
        auto getOutRowCountRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_out_row_count");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            {
                auto rsp = lockOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error locking output table %s",
                        ~table.Path.GetPath()));
                LOG_INFO("Output table %s was locked successfully",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = getOutChannelsRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error getting channels for output table %s",
                        ~table.Path.GetPath()));
                table.Channels = TYsonString(rsp->value());
                LOG_INFO("Output table %s has channels %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.Channels, EYsonFormat::Text).Data());
            }
            {
                auto rsp = getOutRowCountRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error getting \"row_count\" attribute for output table %s",
                        ~table.Path.GetPath()));
                table.InitialRowCount = ConvertTo<i64>(TYsonString(rsp->value()));
            }
            if (table.Clear) {
                auto rsp = clearOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error clearing output table %s",
                        ~table.Path.GetPath()));
                LOG_INFO("Output table %s was cleared successfully",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = getOutChunkListRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*
                    rsp,
                    Sprintf("Error getting output chunk list for table %s",
                        ~table.Path.GetPath()));
                table.OutputChunkListId = TChunkListId::FromProto(rsp->chunk_list_id());
                LOG_INFO("Output table %s has output chunk list %s",
                    ~table.Path.GetPath(),
                    ~table.OutputChunkListId.ToString());
            }
        }
    }

    {
        auto fetchFilesRsps = batchRsp->GetResponses<TFileYPathProxy::TRspFetch>("fetch_files");
        for (int index = 0; index < static_cast<int>(Files.size()); ++index) {
            auto& file = Files[index];
            {
                auto rsp = fetchFilesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching files");
                file.FetchResponse = rsp;
                LOG_INFO("File %s consists of chunk %s",
                    ~file.Path.GetPath(),
                    ~TChunkId::FromProto(rsp->chunk_id()).ToString());
            }
        }
    }

    OnCustomInputsRecieved(batchRsp);

    LOG_INFO("Inputs received");
}

void TOperationControllerBase::RequestCustomInputs(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
{
    UNUSED(batchReq);
}

void TOperationControllerBase::OnCustomInputsRecieved(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    UNUSED(batchRsp);
}

void TOperationControllerBase::CompletePreparation()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Completing preparation");

    ChunkListPool = New<TChunkListPool>(
        Host->GetLeaderChannel(),
        Host->GetControlInvoker(),
        Operation,
        PrimaryTransaction->GetId());

    LastChunkListAllocationCount = Config->ChunkListPreallocationCount;
    YCHECK(ChunkListPool->Allocate(LastChunkListAllocationCount));
}

void TOperationControllerBase::OnPreparationCompleted()
{
    if (!Active)
        return;

    LOG_INFO("Preparation completed");
}

TAsyncPipeline<void>::TPtr TOperationControllerBase::CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline)
{
    return pipeline;
}

void TOperationControllerBase::ReleaseChunkList(const TChunkListId& id)
{
    std::vector<TChunkListId> ids;
    ids.push_back(id);
    ReleaseChunkLists(ids);
}

void TOperationControllerBase::ReleaseChunkLists(const std::vector<TChunkListId>& ids)
{
    auto batchReq = ObjectProxy.ExecuteBatch();
    FOREACH (const auto& id, ids) {
        auto req = TTransactionYPathProxy::ReleaseObject();
        *req->mutable_object_id() = id.ToProto();
        batchReq->AddRequest(req);
    }

    // Fire-and-forget.
    // The subscriber is only needed to log the outcome.
    batchReq->Invoke().Subscribe(
        BIND(&TThis::OnChunkListsReleased, MakeStrong(this)));
}

void TOperationControllerBase::OnChunkListsReleased(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    if (batchRsp->IsOK()) {
        LOG_INFO("Chunk lists released successfully");
    } else {
        LOG_WARNING("Error releasing chunk lists\n%s", ~ToString(batchRsp->GetError()));
    }
}

std::vector<TRefCountedInputChunkPtr> TOperationControllerBase::CollectInputTablesChunks()
{
    // TODO(babenko): set row_attributes
    std::vector<TRefCountedInputChunkPtr> result;
    FOREACH (const auto& table, InputTables) {
        FOREACH (const auto& inputChunk, table.FetchResponse->chunks()) {
            result.push_back(New<TRefCountedInputChunk>(inputChunk));
        }
    }
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::PrepareChunkStripes(
    const std::vector<TRefCountedInputChunkPtr>& inputChunks,
    TNullable<int> jobCount,
    i64 jobSliceDataSize)
{
    i64 sliceDataSize = jobSliceDataSize;
    if (jobCount) {
        i64 totalDataSize = 0;
        FOREACH (auto inputChunk, inputChunks) {
            totalDataSize += inputChunk->uncompressed_data_size();
        }
        sliceDataSize = std::min(sliceDataSize, totalDataSize / jobCount.Get() + 1);
    }

    YCHECK(sliceDataSize > 0);

    LOG_DEBUG("Preparing chunk stripes (ChunkCount: %d, JobCount: %s, JobSliceDataSize: %" PRId64 ", SliceDataSize: %" PRId64 ")",
        static_cast<int>(inputChunks.size()),
        ~ToString(jobCount),
        jobSliceDataSize,
        sliceDataSize);

    std::vector<TChunkStripePtr> result;

    // Ensure that no input chunk has size larger than sliceSize.
    FOREACH (auto inputChunk, inputChunks) {
        auto chunkId = TChunkId::FromProto(inputChunk->slice().chunk_id());
        if (inputChunk->uncompressed_data_size() > sliceDataSize) {
            int sliceCount = (int) std::ceil((double) inputChunk->uncompressed_data_size() / (double) sliceDataSize);
            auto slicedInputChunks = SliceChunkEvenly(*inputChunk, sliceCount);
            FOREACH (auto slicedInputChunk, slicedInputChunks) {
                auto stripe = New<TChunkStripe>(slicedInputChunk);
                result.push_back(stripe);
            }
            LOG_DEBUG("Slicing chunk (ChunkId: %s, SliceCount: %d)",
                ~chunkId.ToString(),
                sliceCount);
        } else {
            auto stripe = New<TChunkStripe>(inputChunk);
            result.push_back(stripe);
            LOG_DEBUG("Taking whole chunk (ChunkId: %s)",
                ~chunkId.ToString());
        }
    }

    LOG_DEBUG("Chunk stripes prepared (StripeCount: %d)",
        static_cast<int>(result.size()));

    return result;
}

std::vector<Stroka> TOperationControllerBase::CheckInputTablesSorted(const TNullable< std::vector<Stroka> >& keyColumns)
{
    YCHECK(!InputTables.empty());

    FOREACH (const auto& table, InputTables) {
        if (!table.Sorted) {
            THROW_ERROR_EXCEPTION("Input table %s is not sorted",
                ~table.Path.GetPath());
        }
    }

    if (keyColumns) {
        FOREACH (const auto& table, InputTables) {
            if (!CheckKeyColumnsCompatible(table.KeyColumns, keyColumns.Get())) {
                THROW_ERROR_EXCEPTION("Input table %s has key columns %s that are not compatible with the requested key columns %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns, EYsonFormat::Text).Data(),
                    ~ConvertToYsonString(keyColumns.Get(), EYsonFormat::Text).Data());
            }
        }
        return keyColumns.Get();
    } else {
        const auto& referenceTable = InputTables[0];
        FOREACH (const auto& table, InputTables) {
            if (table.KeyColumns != referenceTable.KeyColumns) {
                THROW_ERROR_EXCEPTION("Key columns do not match: input table %s is sorted by %s while input table %s is sorted by %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns, EYsonFormat::Text).Data(),
                    ~referenceTable.Path.GetPath(),
                    ~ConvertToYsonString(referenceTable.KeyColumns, EYsonFormat::Text).Data());
            }
        }
        return referenceTable.KeyColumns;
    }
}

bool TOperationControllerBase::CheckKeyColumnsCompatible(
    const std::vector<Stroka>& fullColumns,
    const std::vector<Stroka>& prefixColumns)
{
    if (fullColumns.size() < prefixColumns.size()) {
        return false;
    }

    for (int index = 0; index < static_cast<int>(prefixColumns.size()); ++index) {
        if (fullColumns[index] != prefixColumns[index]) {
            return false;
        }
    }

    return true;
}

void TOperationControllerBase::CheckOutputTablesEmpty()
{
    FOREACH (const auto& table, OutputTables) {
        if (table.InitialRowCount > 0) {
            THROW_ERROR_EXCEPTION("Output table %s is not empty",
                ~table.Path.GetPath());
        }
    }
}

void TOperationControllerBase::ScheduleClearOutputTables()
{
    FOREACH (auto& table, OutputTables) {
        table.Clear = true;
    }
}

void TOperationControllerBase::ScheduleSetOutputTablesSorted(const std::vector<Stroka>& keyColumns)
{
    FOREACH (auto& table, OutputTables) {
        table.SetSorted = true;
        table.KeyColumns = keyColumns;
    }
}

void TOperationControllerBase::RegisterOutputChunkTree(
    const NChunkServer::TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex)
{
    auto& table = OutputTables[tableIndex];
    table.OutputChunkTreeIds.insert(std::make_pair(key, chunkTreeId));

    LOG_DEBUG("Output chunk tree registered (Table: %d, ChunkTreeId: %s, Key: %d)",
        tableIndex,
        ~chunkTreeId.ToString(),
        key);
}

bool TOperationControllerBase::CheckAvailableChunkLists(int requestedCount)
{
    if (ChunkListPool->GetSize() >= requestedCount + Config->ChunkListWatermarkCount) {
        // Enough chunk lists. Above the watermark even after extraction.
        return true;
    }

    // Additional chunk lists are definitely needed but still could be a success.
    bool success = ChunkListPool->GetSize() >= requestedCount;

    int allocateCount = static_cast<int>(LastChunkListAllocationCount * Config->ChunkListAllocationMultiplier);
    if (ChunkListPool->Allocate(allocateCount)) {
        LastChunkListAllocationCount = allocateCount;
    }

    return success;
}

TChunkListId TOperationControllerBase::GetFreshChunkList()
{
    return ChunkListPool->Extract();
}

void TOperationControllerBase::RegisterJobInProgress(TJobInProgressPtr jip)
{
    YCHECK(JobsInProgress.insert(MakePair(jip->Job, jip)).second);
}

TOperationControllerBase::TJobInProgressPtr TOperationControllerBase::GetJobInProgress(TJobPtr job)
{
    auto it = JobsInProgress.find(job);
    YCHECK(it != JobsInProgress.end());
    return it->second;
}

void TOperationControllerBase::RemoveJobInProgress(TJobPtr job)
{
    YCHECK(JobsInProgress.erase(job) == 1);
}

void TOperationControllerBase::BuildProgressYson(IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("jobs").BeginMap()
                .Item("total").Scalar(CompletedJobCount + RunningJobCount + GetPendingJobCount())
                .Item("pending").Scalar(GetPendingJobCount())
                .Item("running").Scalar(RunningJobCount)
                .Item("completed").Scalar(CompletedJobCount)
                .Item("failed").Scalar(FailedJobCount)
            .EndMap()
            .Do(BIND(&TThis::DoBuildProgressYson, Unretained(this)))
        .EndMap();
}

void TOperationControllerBase::BuildResultYson(IYsonConsumer* consumer)
{
    auto error = FromProto(Operation->Result().error());
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("error").Scalar(error)
        .EndMap();
}

std::vector<TRichYPath> TOperationControllerBase::GetFilePaths() const
{
    return std::vector<TRichYPath>();
}

int TOperationControllerBase::SuggestJobCount(
    i64 totalDataSize,
    i64 minDataSizePerJob,
    i64 maxDataSizePerJob,
    TNullable<int> configJobCount,
    int chunkCount)
{
    int minSuggestion = static_cast<int>(std::ceil((double) totalDataSize / maxDataSizePerJob));
    int maxSuggestion = static_cast<int>(std::ceil((double) totalDataSize / minDataSizePerJob));
    int result = configJobCount.Get(minSuggestion);
    result = std::min(result, chunkCount);
    result = std::min(result, maxSuggestion);
    result = std::max(result, 1);
    return result;
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* proto,
    TUserJobSpecPtr config,
    const std::vector<TUserFile>& files)
{
    proto->set_shell_command(config->Command);

    {
        // Set input and output format.
        TFormat inputFormat(EFormatType::Yson);
        TFormat outputFormat(EFormatType::Yson);

        if (config->Format) {
            inputFormat = outputFormat = TFormat::FromYson(config->Format);
        }

        if (config->InputFormat) {
            inputFormat = TFormat::FromYson(config->InputFormat);
        }

        if (config->OutputFormat) {
            outputFormat = TFormat::FromYson(config->OutputFormat);
        }

        proto->set_input_format(inputFormat.ToYson().Data());
        proto->set_output_format(outputFormat.ToYson().Data());
    }

    // TODO(babenko): think about per-job files
    FOREACH (const auto& file, files) {
        *proto->add_files() = *file.FetchResponse;
    }
}

TJobIOConfigPtr TOperationControllerBase::BuildJobIOConfig(
    TJobIOConfigPtr schedulerConfig,
    INodePtr specConfigNode)
{
    return UpdateYsonSerializable(schedulerConfig, specConfigNode);
}

void TOperationControllerBase::InitIntermediateOutputConfig(TJobIOConfigPtr config)
{
    // Don't replicate intermediate output.
    config->TableWriter->ReplicationFactor = 1;
    config->TableWriter->UploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->EnableNodeCaching = true;

    // Don't move intermediate chunks.
    config->TableWriter->ChunksMovable = false;
}

void TOperationControllerBase::InitIntermediateInputConfig(TJobIOConfigPtr config)
{
    // Disable master requests.
    config->TableReader->AllowFetchingSeedsFromMaster = false;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

