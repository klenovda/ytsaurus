#include "cancelable_context.h"
#include "callback.h"
#include "invoker_detail.h"
#include "invoker_util.h"

#include <yt/core/concurrency/scheduler.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TCancelableContext::TCancelableInvoker
    : public TInvokerWrapper
{
public:
    TCancelableInvoker(
        TCancelableContextPtr context,
        IInvokerPtr underlyingInvoker)
        : TInvokerWrapper(std::move(underlyingInvoker))
        , Context_(std::move(context))
    {
        YT_VERIFY(Context_);
    }

    virtual void Invoke(TClosure callback) override
    {
        YT_ASSERT(callback);

        if (Context_->Canceled_) {
            return;
        }

        return UnderlyingInvoker_->Invoke(BIND_DONT_CAPTURE_TRACE_CONTEXT([=, this_ = MakeStrong(this), callback = std::move(callback)] {
            if (Context_->Canceled_) {
                return;
            }

            TCurrentInvokerGuard guard(this_);
            callback.Run();
        }));
    }

private:
    const TCancelableContextPtr Context_;

};

////////////////////////////////////////////////////////////////////////////////

bool TCancelableContext::IsCanceled() const
{
    return Canceled_;
}

void TCancelableContext::Cancel(const TError& error)
{
    THashSet<TWeakPtr<TCancelableContext>> propagateToContexts;
    THashSet<TAwaitable> propagateToAwaitables;
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (Canceled_) {
            return;
        }
        CancelationError_ = error;
        Canceled_ = true;
        PropagateToContexts_.swap(propagateToContexts);
        PropagateToAwaitables_.swap(propagateToAwaitables);
    }

    Handlers_.FireAndClear(error);

    for (const auto& weakContext : propagateToContexts) {
        auto context = weakContext.Lock();
        if (context) {
            context->Cancel(error);
        }
    }

    for (const auto& awaitable : propagateToAwaitables) {
        awaitable.Cancel(error);
    }
}

IInvokerPtr TCancelableContext::CreateInvoker(IInvokerPtr underlyingInvoker)
{
    return New<TCancelableInvoker>(this, std::move(underlyingInvoker));
}

void TCancelableContext::SubscribeCanceled(const TCallback<void(const TError&)>& callback)
{
    TGuard<TSpinLock> guard(SpinLock_);
    if (Canceled_) {
        guard.Release();
        callback.Run(CancelationError_);
        return;
    }
    Handlers_.Subscribe(callback);
}

void TCancelableContext::UnsubscribeCanceled(const TCallback<void(const TError&)>& /*callback*/)
{
    YT_ABORT();
}

void TCancelableContext::PropagateTo(const TCancelableContextPtr& context)
{
    auto weakContext = MakeWeak(context);

    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (Canceled_) {
            guard.Release();
            context->Cancel(CancelationError_);
            return;
        }
        PropagateToContexts_.insert(context);
    }

    context->SubscribeCanceled(BIND_DONT_CAPTURE_TRACE_CONTEXT([=, weakThis = MakeWeak(this)] (const TError& /*error*/) {
        if (auto this_ = weakThis.Lock()) {
            TGuard<TSpinLock> guard(SpinLock_);
            PropagateToContexts_.erase(context);
        }
    }));
}

void TCancelableContext::PropagateTo(const TAwaitable& awaitable)
{
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (Canceled_) {
            guard.Release();
            awaitable.Cancel(CancelationError_);
            return;
        }

        PropagateToAwaitables_.insert(awaitable);
    }

    awaitable.Subscribe(BIND_DONT_CAPTURE_TRACE_CONTEXT([=, weakThis = MakeWeak(this)] () {
        if (auto this_ = weakThis.Lock()) {
            TGuard<TSpinLock> guard(SpinLock_);
            PropagateToAwaitables_.erase(awaitable);
        }
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
