#include "stdafx.h"
#include "helpers.h"

#include <ytlib/ypath/token.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYPath;

////////////////////////////////////////////////////////////////////

TYPath GetOperationPath(const TOperationId& operationId)
{
    return
        "//sys/operations/" +
        ToYPathLiteral(ToString(operationId));
}

TYPath GetJobsPath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId) +
        "/jobs";
}

TYPath GetJobPath(const TOperationId& operationId, const TJobId& jobId)
{
    return
        GetJobsPath(operationId) + "/" +
        ToYPathLiteral(ToString(jobId));
}

TYPath GetStdErrPath(const TOperationId& operationId, const TJobId& jobId)
{
    return
        GetJobPath(operationId, jobId)
        + "/stderr";
}

TYPath GetSnapshotPath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId)
        + "/snapshot";
}

TYPath GetLivePreviewOutputPath(const TOperationId& operationId, int tableIndex)
{
    return
        GetOperationPath(operationId)
        + "/output_" + ToString(tableIndex);
}

TYPath GetLivePreviewIntermediatePath(const TOperationId& operationId)
{
    return
        GetOperationPath(operationId)
        + "/intermediate";
}

bool IsOperationFinished(EOperationState state)
{
    return
        state == EOperationState::Completed ||
        state == EOperationState::Aborted ||
        state == EOperationState::Failed;
}

bool IsOperationFinishing(EOperationState state)
{
    return
        state == EOperationState::Completing ||
        state == EOperationState::Aborting ||
        state == EOperationState::Failing;
}

bool IsOperationInProgress(EOperationState state)
{
    return
        state == EOperationState::Initializing ||
        state == EOperationState::Preparing ||
        state == EOperationState::Reviving ||
        state == EOperationState::Running ||
        state == EOperationState::Suspended ||
        state == EOperationState::Completing ||
        state == EOperationState::Failing ||
        state == EOperationState::Suspended;
}

bool IsOperationActive(EOperationState state)
{
    return
        state == EOperationState::Running ||
        state == EOperationState::Suspended;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

