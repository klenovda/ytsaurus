#pragma once

#include "common.h"

#include "../misc/property.h"
#include "../transaction_server/common.h"

namespace NYT {
namespace NCypress {

////////////////////////////////////////////////////////////////////////////////

class TLock
{
    DECLARE_BYVAL_RO_PROPERTY(TLockId, Id);
    DECLARE_BYVAL_RO_PROPERTY(TNodeId, NodeId);
    DECLARE_BYVAL_RO_PROPERTY(NTransaction::TTransactionId, TransactionId);
    DECLARE_BYVAL_RO_PROPERTY(ELockMode, Mode);

public:
    TLock(
        const TLockId& id,
        const TNodeId& nodeId,
        const NTransaction::TTransactionId& transactionId,
        ELockMode mode);

    TAutoPtr<TLock> Clone() const;

    void Save(TOutputStream* output) const;
    static TAutoPtr<TLock> Load(const TLockId& id, TInputStream* input);

private:
    TLock(const TLock& other);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypress
} // namespace NYT

