#include "proxy_coordinator.h"

#include <yt/ytlib/security_client/public.h>

#include <yt/core/rpc/public.h>
#include <yt/core/rpc/service.h>

#include <atomic>

namespace NYT::NRpcProxy {

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TProxyCoordinator
    : public IProxyCoordinator
{
public:
    virtual bool SetBannedState(bool banned) override;
    virtual bool GetBannedState() const override;

    virtual void SetBanMessage(const TString& message) override;
    virtual TString GetBanMessage() const override;

    virtual bool SetAvailableState(bool available) override;
    virtual bool GetAvailableState() const override;

    virtual void ValidateOperable() const override;

private:
    std::atomic<bool> IsBanned_ = {false};
    std::atomic<bool> IsAvailable_ = {false};

    TSpinLock BanSpinLock_;
    TString BanMessage_;
};

bool TProxyCoordinator::SetBannedState(bool banned)
{
    return IsBanned_.exchange(banned, std::memory_order_relaxed) != banned;
}

bool TProxyCoordinator::GetBannedState() const
{
    return IsBanned_.load(std::memory_order_relaxed);
}

void TProxyCoordinator::SetBanMessage(const TString& message)
{
    auto guard = Guard(BanSpinLock_);
    BanMessage_ = message;
}

TString TProxyCoordinator::GetBanMessage() const
{
    auto guard = Guard(BanSpinLock_);
    return BanMessage_;
}

bool TProxyCoordinator::SetAvailableState(bool available)
{
    return IsAvailable_.exchange(available, std::memory_order_relaxed) != available;
}

bool TProxyCoordinator::GetAvailableState() const
{
    return IsAvailable_.load(std::memory_order_relaxed);
}

void TProxyCoordinator::ValidateOperable() const
{
    if (!GetAvailableState()) {
        THROW_ERROR_EXCEPTION(NRpc::EErrorCode::Unavailable, "Proxy cannot synchronize with cluster");
    }
    if (GetBannedState()) {
        THROW_ERROR_EXCEPTION(NApi::NRpcProxy::EErrorCode::ProxyBanned, "Proxy has been banned")
            << TErrorAttribute("message", GetBanMessage());
    }
}

////////////////////////////////////////////////////////////////////////////////

IProxyCoordinatorPtr CreateProxyCoordinator()
{
    return New<TProxyCoordinator>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpcProxy
