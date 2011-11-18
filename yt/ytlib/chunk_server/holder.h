#pragma once

#include "common.h"

#include "../misc/property.h"
#include "../misc/serialize.h"

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EHolderState,
    // The holder had just registered but have not reported any heartbeats yet.
    (Registered)
    // The holder is reporting heartbeats.
    // We have a proper knowledge of its chunk set.
    (Active)
);

// TODO: move impl to cpp

class THolder
{
    DECLARE_BYVAL_RO_PROPERTY(THolderId, Id);
    DECLARE_BYVAL_RO_PROPERTY(Stroka, Address);
    DECLARE_BYVAL_RW_PROPERTY(EHolderState, State);
    DECLARE_BYREF_RW_PROPERTY(THolderStatistics, Statistics);
    DECLARE_BYREF_RW_PROPERTY(yhash_set<TChunkId>, ChunkIds);
    DECLARE_BYREF_RO_PROPERTY(yvector<TJobId>, JobIds);

public:
    THolder(
        THolderId id,
        const Stroka& address,
        EHolderState state,
        const THolderStatistics& statistics)
        : Id_(id)
        , Address_(address)
        , State_(state)
        , Statistics_(statistics)
    { }

    THolder(const THolder& other)
        : Id_(other.Id_)
        , Address_(other.Address_)
        , State_(other.State_)
        , Statistics_(other.Statistics_)
        , ChunkIds_(other.ChunkIds_)
        , JobIds_(other.JobIds_)
    { }

    TAutoPtr<THolder> Clone() const
    {
        return new THolder(*this);
    }

    void Save(TOutputStream* output) const
    {
        ::Save(output, Address_);
        ::Save(output, State_);
        ::Save(output, Statistics_);
        SaveSet(output, ChunkIds_);
        ::Save(output, JobIds_);
    }

    static TAutoPtr<THolder> Load(THolderId id, TInputStream* input)
    {
        Stroka address;
        EHolderState state;
        THolderStatistics statistics;
        ::Load(input, address);
        ::Load(input, state);
        ::Load(input, statistics);
        TAutoPtr<THolder> holder = new THolder(id, address, state, statistics);
        ::Load(input, holder->ChunkIds_);
        ::Load(input, holder->JobIds_);
        return holder;
    }

    void AddJob(const TJobId& id)
    {
        JobIds_.push_back(id);
    }

    void RemoveJob(const TJobId& id)
    {
        auto it = std::find(JobIds_.begin(), JobIds_.end(), id);
        if (it != JobIds_.end()) {
            JobIds_.erase(it);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

// TODO: refactor & cleanup
struct TReplicationSink
{
    explicit TReplicationSink(const Stroka &address)
        : Address(address)
    { }

    Stroka Address;
    yhash_set<TJobId> JobIds;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
