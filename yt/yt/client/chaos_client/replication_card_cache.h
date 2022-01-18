#pragma once

#include "public.h"
#include "replication_card.h"

namespace NYT::NChaosClient {

////////////////////////////////////////////////////////////////////////////////

struct TReplicationCardCacheKey
{
    TReplicationCardId CardId;
    bool RequestHistory = false;
    bool RequestCoordinators = false;
    bool RequestProgress = false;

    operator size_t() const;
    bool operator == (const TReplicationCardCacheKey& other) const = default;
};

void FormatValue(TStringBuilderBase* builder, const TReplicationCardCacheKey& key, TStringBuf /*spec*/);
TString ToString(const TReplicationCardCacheKey& key);

////////////////////////////////////////////////////////////////////////////////

struct IReplicationCardCache
    : public virtual TRefCounted
{
    virtual TFuture<TReplicationCardPtr> GetReplicationCard(const TReplicationCardCacheKey& key) = 0;
    virtual void Clear() = 0;
};

DEFINE_REFCOUNTED_TYPE(IReplicationCardCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosClient

