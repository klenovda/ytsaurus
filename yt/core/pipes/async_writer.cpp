#include "async_writer.h"
#include "private.h"
#include "io_dispatcher.h"
#include "io_dispatcher_impl.h"
#include "pipe.h"

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/proc.h>

#include <yt/contrib/libev/ev++.h>

#include <errno.h>

namespace NYT {
namespace NPipes {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = PipesLogger;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EWriterState,
    (Active)
    (Closed)
    (Failed)
    (Aborted)
);

class TAsyncWriterImpl
    : public TRefCounted
{
public:
    explicit TAsyncWriterImpl(int fd)
        : FD_(fd)
    {
        BIND([=, this_ = MakeStrong(this)] () {
            FDWatcher_.set(FD_, ev::WRITE);
            FDWatcher_.set(TIODispatcher::Get()->Impl_->GetEventLoop());
            FDWatcher_.set<TAsyncWriterImpl, &TAsyncWriterImpl::OnWrite>(this);
            FDWatcher_.start();
        })
        .Via(TIODispatcher::Get()->Impl_->GetInvoker())
        .Run();
    }

    ~TAsyncWriterImpl()
    {
        YCHECK(State_ != EWriterState::Active);
    }

    int GetHandle() const
    {
        return FD_;
    }

    TFuture<void> Write(const TSharedRef& buffer)
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YCHECK(buffer.Size() > 0);

        auto promise = NewPromise<void>();
        BIND([=, this_ = MakeStrong(this)] () {
            YCHECK(WriteResultPromise_.IsSet());
            WriteResultPromise_ = promise;

            switch (State_) {
                case EWriterState::Aborted:
                    WriteResultPromise_.Set(TError(EErrorCode::Aborted, "Writer aborted")
                        << TErrorAttribute("fd", FD_));
                    return;

                case EWriterState::Failed:
                    WriteResultPromise_.Set(TError("Writer failed")
                        << TErrorAttribute("fd", FD_));
                    return;

                case EWriterState::Closed:
                    WriteResultPromise_.Set(TError("Writer closed")
                        << TErrorAttribute("fd", FD_));
                    return;

                case EWriterState::Active:
                    Buffer_ = buffer;
                    Position_ = 0;

                    if (!FDWatcher_.is_active()) {
                        FDWatcher_.start();
                    }

                    break;

                default:
                    YUNREACHABLE();
            };
        })
        .Via(TIODispatcher::Get()->Impl_->GetInvoker())
        .Run();

        return promise.ToFuture();
    }

    TFuture<void> Close()
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YCHECK(WriteResultPromise_.IsSet());

        return BIND([=, this_ = MakeStrong(this)] () {
            if (State_ != EWriterState::Active)
                return;

            State_ = EWriterState::Closed;
            FDWatcher_.stop();
            SafeClose(FD_, false);
            FD_ = TPipe::InvalidFD;
        })
        .AsyncVia(TIODispatcher::Get()->Impl_->GetInvoker())
        .Run();
    }

    TFuture<void> Abort()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND([=, this_ = MakeStrong(this)] () {
            if (State_ != EWriterState::Active)
                return;

            State_ = EWriterState::Aborted;
            FDWatcher_.stop();
            WriteResultPromise_.TrySet(TError(EErrorCode::Aborted, "Writer aborted")
               << TErrorAttribute("fd", FD_));

            YCHECK(TryClose(FD_, false));
            FD_ = TPipe::InvalidFD;
        })
        .AsyncVia(TIODispatcher::Get()->Impl_->GetInvoker())
        .Run();
    }

private:
    int FD_;

    //! \note Thread-unsafe. Must be accessed from ev-thread only.
    ev::io FDWatcher_;

    TPromise<void> WriteResultPromise_ = MakePromise(TError());

    EWriterState State_ = EWriterState::Active;

    TSharedRef Buffer_;
    int Position_ = 0;

    DECLARE_THREAD_AFFINITY_SLOT(EventLoop);


    void OnWrite(ev::io&, int eventType)
    {
        VERIFY_THREAD_AFFINITY(EventLoop);
        YCHECK((eventType & ev::WRITE) == ev::WRITE);

        YCHECK(State_ == EWriterState::Active);

        if (Position_ < Buffer_.Size()) {
            DoWrite();
        } else {
            FDWatcher_.stop();
        }
    }

    void DoWrite()
    {
#ifndef _win_
        YCHECK(Position_ < Buffer_.Size());

        int size;
        do {
            size = ::write(FD_, Buffer_.Begin() + Position_, Buffer_.Size() - Position_);
        } while (size == -1 && errno == EINTR);

        if (size == -1) {
            if (errno == EAGAIN) {
                return;
            }

            auto error = TError("Writer failed")
                << TErrorAttribute("fd", FD_)
                << TError::FromSystem();
            LOG_ERROR(error);

            YCHECK(TryClose(FD_, false));
            FD_ = TPipe::InvalidFD;

            State_ = EWriterState::Failed;
            FDWatcher_.stop();
            WriteResultPromise_.Set(error);
            return;
        }

        YCHECK(size > 0);
        Position_ += size;

        if (Position_ == Buffer_.Size()) {
            WriteResultPromise_.Set(TError());
        }
#else
    THROW_ERROR_EXCEPTION("Unsupported platform");
#endif
    }

};

DEFINE_REFCOUNTED_TYPE(TAsyncWriterImpl);

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

TAsyncWriter::TAsyncWriter(int fd)
    : Impl_(New<NDetail::TAsyncWriterImpl>(fd))
{ }

TAsyncWriter::~TAsyncWriter()
{
    // Abort does not fail.
    Impl_->Abort();
}

int TAsyncWriter::GetHandle() const
{
    return Impl_->GetHandle();
}

TFuture<void> TAsyncWriter::Write(const TSharedRef& buffer)
{
    return Impl_->Write(buffer);
}

TFuture<void> TAsyncWriter::Close()
{
    return Impl_->Close();
}

TFuture<void> TAsyncWriter::Abort()
{
    return Impl_->Abort();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
