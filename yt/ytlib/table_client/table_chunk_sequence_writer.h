﻿#pragma once

#include "public.h"
#include "chunk_sequence_writer_base.h"
#include "table_chunk_writer.h"

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TTableChunkSequenceWriter
    : public TChunkSequenceWriterBase<TTableChunkWriter>
{
public:
    typedef TChunkSequenceWriterBase<TTableChunkWriter> TBase;
    using TBase::TryWriteRowUnsafe;

    TTableChunkSequenceWriter(
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        NRpc::IChannelPtr masterChannel,
        const NTransactionClient::TTransactionId& transactionId,
        const NChunkClient::TChunkListId& parentChunkListId);

    ~TTableChunkSequenceWriter();

    virtual bool TryWriteRow(const TRow& row) override;

    // Used internally by jobs that generate sorted output.
    bool TryWriteRowUnsafe(const TRow& row, const TNonOwningKey& key);

    virtual TAsyncError AsyncClose() override;

    // Stores the first and the last key of the written sequence of rows.
    // Last key is collected upon call to AsyncClose.
    const NProto::TBoundaryKeysExt& GetBoundaryKeys() const;

private:

    NProto::TBoundaryKeysExt BoundaryKeys;

    virtual void InitCurrentSession(TSession nextSession) override;
    virtual void PrepareChunkWriter(TSession* newSession) override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
