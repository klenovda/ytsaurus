#include "stdafx.h"
#include "async_change_log.h"

#include "../misc/foreach.h"
#include "../misc/singleton.h"
#include "../actions/action_util.h"
#include "../misc/thread_affinity.h"

#include <util/system/thread.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

// TODO: Extract these settings to the global configuration.
static i32 UnflushedBytesThreshold = 1 << 20;
static i32 UnflushedRecordsThreshold = 100000;

////////////////////////////////////////////////////////////////////////////////

class TChangeLogQueue
    : public TRefCountedBase
{
public:
    typedef TIntrusivePtr<TChangeLogQueue> TPtr;
    typedef TAsyncChangeLog::TAppendResult TAppendResult;

    // Guarded by TImpl::SpinLock
    TAtomic UseCount;

    TChangeLogQueue(TChangeLog::TPtr changeLog)
        : UseCount(0)
        , ChangeLog(changeLog)
        , FlushedRecordCount(changeLog->GetRecordCount())
        , Result(New<TAppendResult>())
        , UnflushedBytes(0)
    { }

    TAppendResult::TPtr Append(i32 recordId, const TSharedRef& data)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAppendResult::TPtr result;

        bool doFlush = false;
        {
            TGuard<TSpinLock> guard(SpinLock);
            if (recordId != DoGetRecordCount()) {
                LOG_FATAL("Unexpected record id in changelog (expected: %d, got: %d, ChangeLogId: %d)",
                    DoGetRecordCount(),
                    recordId,
                    ChangeLog->GetId());
            }

            RecordsToAppend.push_back(data);
            UnflushedBytes += data.Size();

            if (UnflushedBytes >= UnflushedBytesThreshold ||
                RecordsToAppend.ysize() >= UnflushedRecordsThreshold)
            {
                LOG_DEBUG("Async changelog unflushed threshold reached (ChangeLogId: %d)",
                    ChangeLog->GetId());
                doFlush = true;
            }

            result = Result;
        }

        if (doFlush)
            Flush();

        return result;
    }

    void Flush()
    {
        VERIFY_THREAD_AFFINITY(Flush);

        TAppendResult::TPtr result;
        {
            TGuard<TSpinLock> guard(SpinLock);

            YASSERT(RecordsToFlush.empty());

            ChangeLog->Append(FlushedRecordCount, RecordsToAppend);
            RecordsToFlush.swap(RecordsToAppend);

            result = Result;
            Result = New<TAppendResult>();
            
            UnflushedBytes = 0;
        }

        ChangeLog->Flush();
        result->Set(TVoid());

        {
            TGuard<TSpinLock> guard(SpinLock);
            FlushedRecordCount += RecordsToFlush.ysize();
            RecordsToFlush.clear();
        }
    }

    void WaitFlush()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TAppendResult::TPtr result;
        {
            TGuard<TSpinLock> guard(SpinLock);
            if (RecordsToFlush.empty() && RecordsToAppend.empty()) {
                return;
            }
            result = Result;
        }
        result->Get();
    }

    int GetRecordCount()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(SpinLock);
        return DoGetRecordCount();
    }

    bool IsEmpty()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(SpinLock);
        return RecordsToAppend.ysize() == 0 && RecordsToFlush.ysize() == 0;
    }

    // Can return less records than recordCount
    void Read(i32 firstRecordId, i32 recordCount, yvector<TSharedRef>* result)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        i32 flushedRecordCount;

        {
            TGuard<TSpinLock> guard(SpinLock);
            flushedRecordCount = FlushedRecordCount;            
            
            CopyRecords(
                FlushedRecordCount,
                RecordsToFlush,
                firstRecordId,
                recordCount,
                result);

            CopyRecords(
                FlushedRecordCount + RecordsToFlush.ysize(),
                RecordsToAppend,
                firstRecordId,
                recordCount,
                result);
        }

        if (firstRecordId < flushedRecordCount) {
            yvector<TSharedRef> buffer;
            i32 neededRecordCount =
                Min(recordCount, flushedRecordCount - firstRecordId);
            ChangeLog->Read(firstRecordId, neededRecordCount, &buffer);
            YASSERT(buffer.ysize() == neededRecordCount);

            buffer.insert(
                buffer.end(),
                result->begin(),
                result->end());

            result->swap(buffer);
        }
    }

private:
    int DoGetRecordCount() const
    {
        VERIFY_SPINLOCK_AFFINITY(SpinLock);
        return FlushedRecordCount + RecordsToFlush.ysize() + RecordsToAppend.ysize();
    }

    static void CopyRecords(
        i32 firstRecordId,
        const yvector<TSharedRef>& records,
        i32 neededFirstRecordId,
        i32 neededRecordCount,
        yvector<TSharedRef>* result)
    {
        i32 size = records.ysize();
        i32 begin = neededFirstRecordId - firstRecordId;
        i32 end = neededFirstRecordId + neededRecordCount - firstRecordId;
        auto beginIt = records.begin() + Min(Max(begin, 0), size);
        auto endIt = records.begin() + Min(Max(end, 0), size);
        if (endIt != beginIt) {
            result->insert(
                result->end(),
                beginIt,
                endIt);
        }
    }
    
    TChangeLog::TPtr ChangeLog;
    
    TSpinLock SpinLock;
    i32 FlushedRecordCount;
    yvector<TSharedRef> RecordsToAppend;
    yvector<TSharedRef> RecordsToFlush;
    TAppendResult::TPtr Result;
    i32 UnflushedBytes;

    DECLARE_THREAD_AFFINITY_SLOT(Flush);
};

////////////////////////////////////////////////////////////////////////////////

class TAsyncChangeLog::TImpl
{
public:
    typedef TAsyncChangeLog::TAppendResult TAppendResult;

    static TImpl* Get()
    {
        return Singleton<TImpl>();
    }

    TImpl()
        : Thread(ThreadFunc, static_cast<void*>(this))
        , WakeupEvent(Event::rManual)
        , Finished(false)
    {
        Thread.Start();    
    }

    ~TImpl()
    {
        Shutdown();
    }

    TAppendResult::TPtr Append(
        TChangeLog::TPtr changeLog,
        i32 recordId,
        const TSharedRef& data)
    {
        LOG_TRACE("Async changelog record is enqueued (Version: %s)",
            ~TMetaVersion(changeLog->GetId(), recordId).ToString());

        auto queue = FindOrCreateQueue(changeLog);
        auto result = queue->Append(recordId, data);
        AtomicDecrement(queue->UseCount);
        WakeupEvent.Signal();
        return result;
    }

    void Read(
        TChangeLog::TPtr changeLog,
        i32 firstRecordId,
        i32 recordCount,
        yvector<TSharedRef>* result)
    {
        YASSERT(firstRecordId >= 0);
        YASSERT(recordCount >= 0);
        YASSERT(result);

        if (recordCount == 0)
            return;
        
        auto queue = FindQueue(changeLog);
        if (queue) {
            queue->Read(firstRecordId, recordCount, result);
            AtomicDecrement(queue->UseCount);
        } else {
            changeLog->Read(firstRecordId, recordCount, result);
        }
    }

    void Flush(TChangeLog::TPtr changeLog)
    {
        auto queue = FindQueue(changeLog);
        if (queue) {
            queue->WaitFlush();
            AtomicDecrement(queue->UseCount);
        }
        changeLog->Flush();
    }

    i32 GetRecordCount(TChangeLog::TPtr changeLog)
    {
        auto queue = FindQueue(changeLog);
        if (queue) {
            auto result = queue->GetRecordCount();
            AtomicDecrement(queue->UseCount);
            return result;
        } else {
            return changeLog->GetRecordCount();
        }
    }

    void Finalize(TChangeLog::TPtr changeLog)
    {
        Flush(changeLog);

        changeLog->Finalize();
        LOG_DEBUG("Async changelog finalized (ChangeLogId: %d)", changeLog->GetId());
    }

    void Truncate(TChangeLog::TPtr changeLog, i32 atRecordId)
    {
        // TODO: Later on this can be improved to asynchronous behavior by
        // getting rid of explicit synchronization.
        Flush(changeLog);

        return changeLog->Truncate(atRecordId);
    }

    void Shutdown()
    {
        Finished = true;
        WakeupEvent.Signal();
        Thread.Join();
    }

private:
    TChangeLogQueue::TPtr FindQueue(TChangeLog::TPtr changeLog) const
    {
        TGuard<TSpinLock> guard(SpinLock);
        auto it = ChangeLogQueues.find(changeLog);
        if (it == ChangeLogQueues.end())
            return NULL;

        auto& queue = it->Second();
        ++queue->UseCount;
        return queue;
    }

    TChangeLogQueue::TPtr FindOrCreateQueue(TChangeLog::TPtr changeLog)
    {
        TGuard<TSpinLock> guard(SpinLock);
        TChangeLogQueue::TPtr queue;

        auto it = ChangeLogQueues.find(changeLog);
        if (it != ChangeLogQueues.end()) {
            queue = it->Second();
        } else {
            queue = New<TChangeLogQueue>(changeLog);
            YVERIFY(ChangeLogQueues.insert(MakePair(changeLog, queue)).Second());
        }

        ++queue->UseCount;
        return queue;
    }

    void FlushQueues()
    {
        // Take a snapshot.
        yvector<TChangeLogQueue::TPtr> queues;
        {
            TGuard<TSpinLock> guard(SpinLock);
            
            if (ChangeLogQueues.empty()) {
                // Hash map from arcadia/util does not support iteration over
                // the empty map with iterators. It crashes with a dump assertion
                // deeply within implementation details.
                return;
            }

            FOREACH(const auto& it, ChangeLogQueues) {
                queues.push_back(it.second);
            }
        }

        // Flush the queues.
        FOREACH(auto& queue, queues) {
            queue->Flush();
        }
    }

    // Returns if there are any queues in the map
    bool CleanQueues()
    {
        TGuard<TSpinLock> guard(SpinLock);
        
        TChangeLogQueueMap::iterator it, jt;
        for (it = ChangeLogQueues.begin(); it != ChangeLogQueues.end(); /**/) {
            jt = it++;
            auto queue = jt->second;
            if (queue->UseCount == 0 && queue->IsEmpty()) {
                LOG_DEBUG("Async changelog queue was swept (ChangeLogId: %d)", jt->first->GetId());
                ChangeLogQueues.erase(jt);
            }
        }

        return !ChangeLogQueues.empty();
    }

    // Returns if there are any queues in the map
    bool FlushAndClean()
    {
        FlushQueues();
        return CleanQueues();
    }

    static void* ThreadFunc(void* param)
    {
        auto* impl = (TImpl*) param;
        impl->ThreadMain();
        return NULL;
    }
    
    void ThreadMain()
    {
        NThread::SetCurrentThreadName("AsyncChangeLog");

        while (!Finished) {
            if (FlushAndClean())
                continue;

            WakeupEvent.Reset();

            if (FlushAndClean())
                continue;

            if (!Finished) {
                WakeupEvent.Wait();
            }
        }
    }

    typedef yhash_map<TChangeLog::TPtr, TChangeLogQueue::TPtr> TChangeLogQueueMap;

    TChangeLogQueueMap ChangeLogQueues;
    TSpinLock SpinLock;
    TThread Thread;
    Event WakeupEvent;
    volatile bool Finished;
};

////////////////////////////////////////////////////////////////////////////////

TAsyncChangeLog::TAsyncChangeLog(TChangeLog::TPtr changeLog)
    : ChangeLog(changeLog)
{
    YASSERT(changeLog);
}

TAsyncChangeLog::~TAsyncChangeLog()
{ }

TAsyncChangeLog::TAppendResult::TPtr TAsyncChangeLog::Append(
    i32 recordId,
    const TSharedRef& data)
{
    return TImpl::Get()->Append(ChangeLog, recordId, data);
}

void TAsyncChangeLog::Finalize()
{
    TImpl::Get()->Finalize(ChangeLog);
}

void TAsyncChangeLog::Flush()
{
    TImpl::Get()->Flush(ChangeLog);
}

void TAsyncChangeLog::Read(i32 firstRecordId, i32 recordCount, yvector<TSharedRef>* result)
{
    TImpl::Get()->Read(ChangeLog, firstRecordId, recordCount, result);
}

i32 TAsyncChangeLog::GetId() const
{
    return ChangeLog->GetId();
}

i32 TAsyncChangeLog::GetRecordCount() const
{
    return TImpl::Get()->GetRecordCount(ChangeLog);
}

i32 TAsyncChangeLog::GetPrevRecordCount() const
{
    return ChangeLog->GetPrevRecordCount();
}

bool TAsyncChangeLog::IsFinalized() const
{
    return ChangeLog->IsFinalized();
}

void TAsyncChangeLog::Truncate(i32 atRecordId)
{
    return TImpl::Get()->Truncate(ChangeLog, atRecordId);
}

void TAsyncChangeLog::Shutdown()
{
    TImpl::Get()->Shutdown();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

