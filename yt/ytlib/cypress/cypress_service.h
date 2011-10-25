#pragma once

#include "common.h"
#include "cypress_service_rpc.h"
#include "cypress_manager.h"

#include "../rpc/server.h"
#include "../meta_state/meta_state_service.h"
#include "../transaction_manager/transaction_manager.h"

namespace NYT {
namespace NCypress {

////////////////////////////////////////////////////////////////////////////////

class TCypressService
    : public NMetaState::TMetaStateServiceBase
{
public:
    typedef TIntrusivePtr<TCypressService> TPtr;

    //! Creates an instance.
    TCypressService(
        TCypressManager::TPtr cypressManager,
        TTransactionManager::TPtr transactionManager,
        IInvoker::TPtr serviceInvoker,
        NRpc::TServer::TPtr server);

private:
    typedef TCypressService TThis;
    typedef TCypressServiceProxy::EErrorCode EErrorCode;
    typedef NRpc::TTypedServiceException<EErrorCode> TServiceException;

    TCypressManager::TPtr CypressManager;
    TTransactionManager::TPtr TransactionManager;

    void RegisterMethods();

    void ValidateTransactionId(const TTransactionId& transactionId);
    
    void ExecuteRecoverable(
        const TTransactionId& transactionId,
        NRpc::TServiceContext::TPtr context,
        IAction::TPtr action);
    void ExecuteUnrecoverable(
        const TTransactionId& transactionId,
        NRpc::TServiceContext::TPtr context,
        IAction::TPtr action);

    RPC_SERVICE_METHOD_DECL(NProto, Get);
    RPC_SERVICE_METHOD_DECL(NProto, Set);
    RPC_SERVICE_METHOD_DECL(NProto, Remove);
    RPC_SERVICE_METHOD_DECL(NProto, Lock);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypress
} // namespace NYT
