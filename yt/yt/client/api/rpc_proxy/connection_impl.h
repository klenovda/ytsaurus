#pragma once

#include "public.h"

#include <yt/client/api/connection.h>

#include <yt/client/api/sticky_transaction_pool.h>

#include <yt/core/concurrency/public.h>

#include <yt/core/rpc/public.h>

// TODO(prime@): Create http endpoint for discovery that works without authentication.
#include <yt/core/misc/atomic_object.h>

#include <yt/core/service_discovery/public.h>

#include <yt/core/logging/log.h>

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

class TConnection
    : public NApi::IConnection
{
public:
    explicit TConnection(TConnectionConfigPtr config);
    ~TConnection();

    NRpc::IChannelPtr CreateChannel(bool sticky);

    const TConnectionConfigPtr& GetConfig();

    // IConnection implementation
    virtual NObjectClient::TCellTag GetCellTag() override;
    virtual const TString& GetLoggingTag() override;
    virtual const TString& GetClusterId() override;

    virtual IInvokerPtr GetInvoker() override;

    virtual NApi::IClientPtr CreateClient(const NApi::TClientOptions& options) override;
    virtual NHiveClient::ITransactionParticipantPtr CreateTransactionParticipant(
        NHiveClient::TCellId cellId,
        const NApi::TTransactionParticipantOptions& options) override;

    virtual void ClearMetadataCaches() override;

    virtual void Terminate() override;

private:
    friend class TClient;
    friend class TTransaction;
    friend class TTimestampProvider;

    const TConnectionConfigPtr Config_;

    const TGuid ConnectionId_;
    const TString LoggingTag_;
    const TString ClusterId_;
    const NLogging::TLogger Logger;
    const NConcurrency::TActionQueuePtr ActionQueue_;
    const NRpc::IChannelFactoryPtr ChannelFactory_;
    const NRpc::TDynamicChannelPoolPtr ChannelPool_;
    const NConcurrency::TPeriodicExecutorPtr UpdateProxyListExecutor_;

    NRpc::IChannelPtr DiscoveryChannel_;

    // TODO(prime@): Create http endpoint for discovery that works without authentication.
    TAtomicObject<TString> DiscoveryToken_;

    NServiceDiscovery::IServiceDiscoveryPtr ServiceDiscovery_;

    std::vector<TString> DiscoverProxiesViaRpc();
    std::vector<TString> DiscoverProxiesViaHttp();
    std::vector<TString> DiscoverProxiesViaServiceDiscovery();

    void OnProxyListUpdate();
};

DEFINE_REFCOUNTED_TYPE(TConnection)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
