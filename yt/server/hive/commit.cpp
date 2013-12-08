#include "stdafx.h"
#include "commit.h"

#include <core/misc/serialize.h>

#include <server/hydra/composite_automaton.h>

namespace NYT {
namespace NHive {

using namespace NHydra;

////////////////////////////////////////////////////////////////////////////////

TCommit::TCommit(const TTransactionId& transationId)
    : TransactionId_(transationId)
{
    Init();
}

TCommit::TCommit(
    bool persistent,
    const TTransactionId& transationId,
    const std::vector<TCellGuid>& participantCellGuids)
    : TransactionId_(transationId)
    , ParticipantCellGuids_(participantCellGuids)
{
    Init();
}

TFuture<TErrorOr<TTimestamp>> TCommit::GetResult()
{
    return Result_;
}

void TCommit::SetResult(const TErrorOr<TTimestamp>& result)
{
    Result_.Set(result);
}

bool TCommit::IsDistributed() const
{
    return !ParticipantCellGuids_.empty();
}

void TCommit::Save(TSaveContext& context) const
{
    using NYT::Save;

    Save(context, TransactionId_);
    Save(context, ParticipantCellGuids_);
    Save(context, PreparedParticipantCellGuids_);
}

void TCommit::Load(TLoadContext& context)
{
    using NYT::Load;

    Load(context, TransactionId_);
    Load(context, ParticipantCellGuids_);
    Load(context, PreparedParticipantCellGuids_);
}

void TCommit::Init()
{
    Result_ = NewPromise<TErrorOr<TTimestamp>>();
}

////////////////////////////////////////////////////////////////////////////////


} // namespace NHive
} // namespace NYT
