#pragma once

#include "public.h"

#include <core/actions/callback.h>

#include <core/misc/shutdownable.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

// XXX(sandello): Facade does not have to be ref-counted.
class TThreadPool
    : public TRefCounted
    , public IShutdownable
{
public:
    TThreadPool(
        int threadCount,
        const Stroka& threadNamePrefix,
        bool enableLogging = true,
        bool enableProfiling = true);

    virtual ~TThreadPool();

    virtual void Shutdown() override;

    void Configure(int threadCount);

    IInvokerPtr GetInvoker();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TThreadPool)

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
