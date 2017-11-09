#pragma once

#include "public.h"

#include <yt/server/job_proxy/config.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/job_prober_client/job_prober_service_proxy.h>

#include <yt/ytlib/formats/format.h>

#include <yt/core/actions/public.h>

#include <yt/core/bus/public.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/fs.h>

#include <util/stream/file.h>

namespace NYT {
namespace NExecAgent {

////////////////////////////////////////////////////////////////////////////////

struct ISlot
    : public TRefCounted
{
    //! Kill all possibly running processes and clean sandboxes.
    virtual void Cleanup() = 0;

    virtual void CancelPreparation() = 0;

    virtual TFuture<void> RunJobProxy(
        NJobProxy::TJobProxyConfigPtr config,
        const TJobId& jobId,
        const TOperationId& operationId) = 0;

    virtual TFuture<void> CreateSandboxDirectories() = 0;

    virtual TFuture<void> MakeLink(
        ESandboxKind sandboxKind,
        const TString& targetPath,
        const TString& linkName,
        bool isExecutable) = 0;

    virtual TFuture<void> MakeCopy(
        ESandboxKind sandboxKind,
        const TString& sourcePath,
        const TString& destinationName,
        bool isExecutable) = 0;

    virtual TFuture<TString> PrepareTmpfs(
        ESandboxKind sandboxKind,
        i64 size,
        TString path,
        bool enable) = 0;

    virtual TFuture<void> FinalizePreparation(TNullable<i64> diskSpaceLimit, TNullable<i64> inodeLimit) = 0;

    virtual NJobProberClient::IJobProbePtr GetJobProberClient() = 0;

    virtual NBus::TTcpBusServerConfigPtr GetBusServerConfig() const = 0;

    virtual int GetSlotIndex() const = 0;
};

DEFINE_REFCOUNTED_TYPE(ISlot)

////////////////////////////////////////////////////////////////////////////////

ISlotPtr CreateSlot(
    int slotIndex,
    TSlotLocationPtr location,
    IJobEnvironmentPtr environment,
    const TString& nodeTag);

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
