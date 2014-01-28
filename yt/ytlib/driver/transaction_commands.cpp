#include "stdafx.h"
#include "transaction_commands.h"

#include <core/concurrency/fiber.h>

#include <core/ytree/fluent.h>
#include <core/ytree/attribute_helpers.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/transaction_client/transaction_manager.h>

namespace NYT {
namespace NDriver {

using namespace NYTree;
using namespace NTransactionClient;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

void TStartTransactionCommand::DoExecute()
{
    TTransactionStartOptions options;
    options.Timeout = Request->Timeout;
    options.ParentId = Request->TransactionId;
    options.MutationId = Request->MutationId;
    options.Ping = true;
    options.AutoAbort = false;
    options.PingAncestors = Request->PingAncestors;

    std::unique_ptr<IAttributeDictionary> attributes;
    if (Request->Attributes) {
        attributes = ConvertToAttributes(Request->Attributes);
        options.Attributes = attributes.get();
    }

    auto transactionManager = Context->GetClient()->GetTransactionManager();
    auto transactionOrError = WaitFor(transactionManager->Start(options));
    auto transaction = transactionOrError.GetValueOrThrow();
    transaction->Detach();

    Reply(BuildYsonStringFluently()
        .Value(transaction->GetId()));
}

////////////////////////////////////////////////////////////////////////////////

void TPingTransactionCommand::DoExecute()
{
    // Specially for evvers@ :)
    if (Request->TransactionId == NullTransactionId)
        return;

    auto transaction = GetTransaction(EAllowNullTransaction::No, EPingTransaction::No);
    auto result = WaitFor(transaction->Ping());
    THROW_ERROR_EXCEPTION_IF_FAILED(result);
}

////////////////////////////////////////////////////////////////////////////////

void TCommitTransactionCommand::DoExecute()
{
    auto transaction = GetTransaction(EAllowNullTransaction::No, EPingTransaction::No);
    auto result = WaitFor(transaction->Commit(GenerateMutationId()));
    THROW_ERROR_EXCEPTION_IF_FAILED(result);
}

////////////////////////////////////////////////////////////////////////////////

void TAbortTransactionCommand::DoExecute()
{
    auto transaction = GetTransaction(EAllowNullTransaction::No, EPingTransaction::No);
    auto result = WaitFor(transaction->Abort(GenerateMutationId()));
    THROW_ERROR_EXCEPTION_IF_FAILED(result);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
