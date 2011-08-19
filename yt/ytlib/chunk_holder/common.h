#pragma once

#include "chunk_holder.pb.h"
#include "chunk_manager_rpc.pb.h"

#include "../chunk_client/common.h"
#include "../election/leader_lookup.h"
#include "../misc/guid.h"
#include "../misc/common.h"
#include "../logging/log.h"

namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

//! Describes a configuration of TChunkHolder.
struct TChunkHolderConfig
{
    //! Maximum number blocks in cache.
    int MaxCachedBlocks;

    //! Maximum number opened files in cache.
    int MaxCachedFiles;

    //! Upload session timeout.
    /*!
     * Some activity must be happening in a session regularly (i.e. new
     * blocks uploaded or sent to other chunk holders). Otherwise
     * the session expires.
     */
    TDuration SessionTimeout;
    
    //! Paths to storage locations.
    yvector<Stroka> Locations;

    //! Masters configuration.
    /*!
     *  If no master addresses are given, the holder will operate in a standalone mode.
     */
    TLeaderLookup::TConfig Masters; 
    
    //! Period between consequent heartbeats.
    TDuration HeartbeatPeriod;

    //! Timeout for RPC requests.
    TDuration RpcTimeout;

    //! Port number to listen.
    int Port;

    //! Constructs a default instance.
    /*!
     *  By default, no master connection is configured. The holder will operate in
     *  a standalone mode, which only makes sense for testing purposes.
     */
    TChunkHolderConfig()
        : MaxCachedBlocks(1024)
        , MaxCachedFiles(256)
        , SessionTimeout(TDuration::Seconds(15))
        , HeartbeatPeriod(TDuration::Seconds(15))
        , RpcTimeout(TDuration::Seconds(5))
        , Port(9000)
    {
        Locations.push_back(".");
    }

    //! Reads configuration from JSON.
    void Read(TJsonObject* json);
};

////////////////////////////////////////////////////////////////////////////////

// TODO: to statistics.h/cpp
struct THolderStatistics
{
    THolderStatistics()
        : AvailableSpace(0)
        , UsedSpace(0)
        , ChunkCount(0)
    { }

    i64 AvailableSpace;
    i64 UsedSpace;
    i32 ChunkCount;

    static THolderStatistics FromProto(const NChunkManager::NProto::THolderStatistics& proto)
    {
        THolderStatistics result;
        result.AvailableSpace = proto.GetAvailableSpace();
        result.UsedSpace = proto.GetUsedSpace();
        result.ChunkCount = proto.GetChunkCount();
        return result;
    }

    NChunkManager::NProto::THolderStatistics ToProto() const
    {
        NChunkManager::NProto::THolderStatistics result;
        result.SetAvailableSpace(AvailableSpace);
        result.SetUsedSpace(UsedSpace);
        result.SetChunkCount(ChunkCount);
        return result;
    }

    Stroka ToString() const
    {
        return Sprintf("AvailableSpace: %" PRId64 ", UsedSpace: %" PRId64 ", ChunkCount: %d",
            AvailableSpace,
            UsedSpace,
            ChunkCount);
    }
};

////////////////////////////////////////////////////////////////////////////////

extern NLog::TLogger ChunkHolderLogger;

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
