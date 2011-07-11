#pragma once

#include "common.h"
#include "master_state_manager_rpc.h"
#include "snapshot.h"
#include "cell_manager.h"

#include "../rpc/client.h"
#include "../actions/parallel_awaiter.h"
namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotDownloader
    : private TNonCopyable
{
public:
    struct TConfig
    {
        TDuration LookupTimeout;
        TDuration ReadTimeout;
        i32 BlockSize;

        TConfig()
            : LookupTimeout(TDuration::Seconds(2))
            , ReadTimeout(TDuration::Seconds(5))
            , BlockSize(32 * 1024 * 1024)
        {}
    };

    DECLARE_ENUM(EResult,
        (OK)
        (SnapshotNotFound)
        (SnapshotUnavailable)
        (RemoteError)
        (IOError)
        (IncorrectChecksum)
    );

    TSnapshotDownloader(
        const TConfig& config,
        TCellManager::TPtr cellManager);

    EResult GetSnapshot(i32 segmentId, TSnapshotWriter* snapshotWriter);

private:
    struct TSnapshotInfo
    {
        TMasterId SourceId;
        i64 Length;
        i32 PrevRecordCount;
        ui64 Checksum;

        TSnapshotInfo() {}

        TSnapshotInfo(TMasterId owner, i64 length, i32 prevRecordCount, ui64 checksum)
            : SourceId(owner)
            , Length(length)
            , PrevRecordCount(prevRecordCount)
            , Checksum(checksum)
        {}
    };

    typedef TMasterStateManagerProxy TProxy;

    TConfig Config;
    TCellManager::TPtr CellManager;

    TSnapshotInfo GetSnapshotInfo(i32 segmentId); // also finds snapshot source
    static void OnResponse(
        TProxy::TRspGetSnapshotInfo::TPtr response,
        TParallelAwaiter::TPtr awaiter,
        TAsyncResult<TSnapshotInfo>::TPtr asyncResult,
        TMasterId masterId);
    static void OnComplete(
        i32 segmentId,
        TAsyncResult<TSnapshotInfo>::TPtr asyncResult);
    EResult DownloadSnapshot(
        i32 segmentId,
        TSnapshotInfo snapshotInfo,
        TSnapshotWriter* snapshotWriter);
    EResult WriteSnapshot(
        i32 segmentId,
        i64 snapshotLength,
        i32 sourceId,
        TOutputStream &output);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
