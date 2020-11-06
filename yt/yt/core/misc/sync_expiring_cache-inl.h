#pragma once
#ifndef SYNC_EXPIRING_CACHE_INL_H_
#error "Direct inclusion of this file is not allowed, include sync_expiring_cache.h"
// For the sake of sane code completion.
#include "sync_expiring_cache.h"
#endif

#include <yt/core/concurrency/periodic_executor.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue>
TSyncExpiringCache<TKey, TValue>::TSyncExpiringCache(
    TCallback<TValue(const TKey&)> calculateValueAction,
    TDuration expirationTimeout,
    IInvokerPtr invoker)
    : CalculateValueAction_(std::move(calculateValueAction))
    , ExpirationTimeout_(NProfiling::DurationToCpuDuration(expirationTimeout))
    , EvictionExecutor_(New<NConcurrency::TPeriodicExecutor>(
        invoker,
        BIND(&TSyncExpiringCache::DeleteExpiredItems, MakeWeak(this)),
        expirationTimeout))
{
    EvictionExecutor_->Start();
}

template <class TKey, class TValue>
TValue TSyncExpiringCache<TKey, TValue>::Get(const TKey& key)
{
    auto now = NProfiling::GetCpuInstant();

    {
        NConcurrency::TReaderGuard guard(MapLock_);

        auto it = Map_.find(key);
        if (it != Map_.end()) {
            auto& entry = it->second;
            if (now <= entry.LastUpdateTime + ExpirationTimeout_.load()) {
                entry.LastAccessTime = now;
                return entry.Value;
            }
        }
    }

    auto result = CalculateValueAction_.Run(key);

    {
        NConcurrency::TWriterGuard guard(MapLock_);

        auto it = Map_.find(key);
        if (it != Map_.end()) {
            it->second = {now, now, std::move(result)};
        } else {
            auto emplaceResult = Map_.emplace(key, TEntry({now, now, std::move(result)}));
            YT_VERIFY(emplaceResult.second);
            it = emplaceResult.first;
        }

        return it->second.Value;
    }
}

template <class TKey, class TValue>
std::optional<TValue> TSyncExpiringCache<TKey, TValue>::Find(const TKey& key)
{
    auto now = NProfiling::GetCpuInstant();

    NConcurrency::TReaderGuard guard(MapLock_);

    auto it = Map_.find(key);
    if (it != Map_.end()) {
        auto& entry = it->second;
        if (now <= entry.LastUpdateTime + ExpirationTimeout_.load()) {
            entry.LastAccessTime = now;
            return entry.Value;
        }
    }

    return std::nullopt;
}

template <class TKey, class TValue>
void TSyncExpiringCache<TKey, TValue>::Set(const TKey& key, TValue value)
{
    NConcurrency::TWriterGuard guard(MapLock_);

    auto now = NProfiling::GetCpuInstant();
    Map_[key] = {now, now, std::move(value)};
}

template <class TKey, class TValue>
void TSyncExpiringCache<TKey, TValue>::Clear()
{
    decltype(Map_) map;
    {
        NConcurrency::TWriterGuard guard(MapLock_);
        Map_.swap(map);
    }
}

template <class TKey, class TValue>
void TSyncExpiringCache<TKey, TValue>::SetExpirationTimeout(TDuration expirationTimeout)
{
    ExpirationTimeout_.store(NProfiling::DurationToCpuDuration(expirationTimeout));
}

template <class TKey, class TValue>
void TSyncExpiringCache<TKey, TValue>::DeleteExpiredItems()
{
    auto deadline = NProfiling::GetCpuInstant() - ExpirationTimeout_.load();

    std::vector<TKey> keysToRemove;
    {
        NConcurrency::TReaderGuard guard(MapLock_);
        for (const auto& [key, entry] : Map_) {
            if (entry.LastAccessTime < deadline) {
                keysToRemove.push_back(key);
            }
        }
    }

    if (keysToRemove.empty()) {
        return;
    }

    std::vector<TValue> valuesToRemove;
    valuesToRemove.reserve(keysToRemove.size());
    {
        NConcurrency::TWriterGuard guard(MapLock_);
        for (const auto& key : keysToRemove) {
            auto it = Map_.find(key);
            auto& entry = it->second;
            if (entry.LastAccessTime < deadline) {
                valuesToRemove.push_back(std::move(entry.Value));
                Map_.erase(it);
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
