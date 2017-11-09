#pragma once

#include "public.h"
#include "private.h"

#include <yt/server/cell_scheduler/public.h>

#include <yt/server/misc/fork_executor.h>

#include <yt/ytlib/api/public.h>

#include <yt/core/pipes/pipe.h>

#include <yt/core/profiling/profiler.h>

#include <util/system/file.h>

namespace NYT {
namespace NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

struct TSnapshotJob
    : public TIntrinsicRefCounted
{
    TOperationId OperationId;
    IOperationControllerPtr Controller;
    NPipes::TAsyncReaderPtr Reader;
    std::unique_ptr<TFile> OutputFile;
    int SnapshotIndex = -1;
    bool Suspended = false;
};

DEFINE_REFCOUNTED_TYPE(TSnapshotJob)

////////////////////////////////////////////////////////////////////////////////

class TSnapshotBuilder
    : public TForkExecutor
{
public:
    TSnapshotBuilder(
        TSchedulerConfigPtr config,
        TOperationIdToControllerMap controllers,
        NApi::IClientPtr client,
        IInvokerPtr IOInvoker);

    TFuture<void> Run();

private:
    const TSchedulerConfigPtr Config_;
    const TOperationIdToControllerMap Controllers_;
    const NApi::IClientPtr Client_;
    const IInvokerPtr IOInvoker_;
    const IInvokerPtr ControlInvoker_;

    std::vector<TSnapshotJobPtr> Jobs_;

    NProfiling::TProfiler Profiler;

    //! This method is called after controller is suspended.
    //! It is used to set flag Suspended in corresponding TSnapshotJob.
    void OnControllerSuspended(const TSnapshotJobPtr& job);

    virtual TDuration GetTimeout() const override;
    virtual void RunParent() override;
    virtual void RunChild() override;

    TFuture<std::vector<TError>> UploadSnapshots();
    void UploadSnapshot(const TSnapshotJobPtr& job);

    bool ControllersSuspended_ = false;
};

DEFINE_REFCOUNTED_TYPE(TSnapshotBuilder)

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT
