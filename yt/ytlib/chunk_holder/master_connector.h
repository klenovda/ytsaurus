#pragma once

#include "common.h"
#include "chunk_store.h"
#include "replicator.h"

#include "../chunk_manager/chunk_manager_rpc.h"

namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

//! Mediates connection between the holder and its master.
/*!
 *  This class is responsible for registering the holder and sending
 *  heartbeats. In particular, it reports chunk deltas to the master
 *  and handles scheduled chunk removals.
 */
class TMasterConnector
    : public TRefCountedBase
{
public:
    typedef TIntrusivePtr<TMasterConnector> TPtr;
    typedef TChunkHolderConfig TConfig;

    //! Creates an instance.
    TMasterConnector(
        const TConfig& config,
        TChunkStore::TPtr chunkStore,
        TReplicator::TPtr replicator,
        IInvoker::TPtr serviceInvoker);

    //! Registers a chunk that was just fully uploaded.
    /*!
     *  This call places the chunk into a list and reports its arrival
     *  to the master upon a next heartbeat.
     */
    void RegisterAddedChunk(TChunk::TPtr chunk);

    //! Registers a chunk that was just removed.
    /*!
     *  This call places the chunk into a list and reports its removal
     *  to the master upon a next heartbeat.
     */
    void RegisterRemovedChunk(TChunk::TPtr chunk);
    
private:
    typedef NChunkManager::TChunkManagerProxy TProxy;
    typedef TProxy::EErrorCode EErrorCode;
    typedef yhash_set<TChunk::TPtr, TIntrusivePtrHash<TChunk> > TChunks;

    //! Special id value indicating that the holder is not registered.
    static const int InvalidHolderId = -1;

    TConfig Config;
    
    TChunkStore::TPtr ChunkStore;
    TReplicator::TPtr Replicator;

    //! All state modifications are carried out via this invoker.
    IInvoker::TPtr ServiceInvoker;
    
    //! Indicates if the holder is currently registered at the master.
    bool Registered;
    
    //! Indicates if the holder must send the delta of its chunk set during the next heartbeat.
    /*! 
     *  When the holder is just registered, this flag is false.
     *  It becomes true upon a first successful heartbeat.
     *  The delta is stored in #AddedChunks and #RemovedChunks.
     */
    bool IncrementalHeartbeat;

    //! Current id assigned by the master, #InvalidHolderId if not registered.
    int HolderId;

    //! Proxy for the master.
    THolder<TProxy> Proxy;
    
    //! Local address of the holder.
    /*!
     *  This address is computed during initialization by combining the host name (returned by #HostName)
     *  and the port number in #Config.
     */
    Stroka Address;
    
    //! Chunks that were added since the last successful heartbeat.
    TChunks AddedSinceLastSuccess;

    //! Chunks that were removed since the last successful heartbeat.
    TChunks RemovedSinceLastSuccess;

    //! Chunks that were reported added at the last heartbeat (for which no reply is received yet).
    TChunks ReportedAdded;

    //! Chunks that were reported removed at the last heartbeat (for which no reply is received yet).
    TChunks ReportedRemoved;

    //! Initializes #Proxy.
    void InitializeProxy();

    //! Computes #Address.
    void InitializeAddress();

    //! Schedules a heartbeat via TDelayedInvoker.
    void ScheduleHeartbeat();

    //! Invoked when a heartbeat must be sent.
    void OnHeartbeat();

    //! Sends out a registration request.
    void SendRegister();

    //! Handles registration response.
    void OnRegisterResponse(TProxy::TRspRegisterHolder::TPtr response);

    //! Sends out a heartbeat.
    void SendHeartbeat();

    //! Handles heartbeat response.
    void OnHeartbeatResponse(TProxy::TRspHolderHeartbeat::TPtr response);

    //! Handles error during a registration or a heartbeat.
    void OnDisconnected();

    //! Constructs a protobuf chunk info for a given chunk.
    static NChunkManager::NProto::TChunkInfo GetInfo(TChunk::TPtr chunk);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
