#pragma once

#include "common.h"
#include "transaction_manager.h"
#include "transaction_service_rpc.h"

#include "../meta_state/meta_state_service.h"
#include "../rpc/service.h"
#include "../rpc/server.h"

namespace NYT {
namespace NTransaction {

////////////////////////////////////////////////////////////////////////////////
    
class TTransactionService
    : public NMetaState::TMetaStateServiceBase
{
public:
    typedef TIntrusivePtr<TTransactionService> TPtr;

    //! Creates an instance.
    TTransactionService(
        TTransactionManager::TPtr transactionManager,
        IInvoker::TPtr serviceInvoker,
        NRpc::TServer::TPtr server);

private:
    typedef TTransactionService TThis;
    typedef TTransactionServiceProxy::EErrorCode EErrorCode;
    typedef NRpc::TTypedServiceException<EErrorCode> TServiceException;

    TTransactionManager::TPtr TransactionManager;

    void RegisterMethods();

    void ValidateTransactionId(const TTransactionId& id);

    RPC_SERVICE_METHOD_DECL(NProto, StartTransaction);
    RPC_SERVICE_METHOD_DECL(NProto, CommitTransaction);
    RPC_SERVICE_METHOD_DECL(NProto, AbortTransaction);
    RPC_SERVICE_METHOD_DECL(NProto, RenewTransactionLease);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransaction
} // namespace NYT
