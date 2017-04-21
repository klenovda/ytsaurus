#pragma once

#include "common.h"
#include "error.h"
#include "local_address.h"

#include <yt/core/actions/future.h>

#include <yt/core/ytree/yson_serializable.h>

#ifdef _WIN32
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Constructs an address of the form |hostName:port|.
Stroka BuildServiceAddress(const TStringBuf& hostName, int port);

//! Parses service address into host name and port number.
//! Both #hostName and #port can be |NULL|.
//! Throws if the address is malformed.
void ParseServiceAddress(
    const TStringBuf& address,
    TStringBuf* hostName,
    int* port);

//! Extracts port number from a service address.
//! Throws if the address is malformed.
int GetServicePort(const TStringBuf& address);

//! Extracts host name from a service address.
TStringBuf GetServiceHostName(const TStringBuf& address);

////////////////////////////////////////////////////////////////////////////////

//! Configuration for TAddressResolver singleton.
class TAddressResolverConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnableIPv4;
    bool EnableIPv6;
    TNullable<Stroka> LocalHostFqdn;
    TDuration AddressExpirationTime;

    TAddressResolverConfig()
    {
        RegisterParameter("enable_ipv4", EnableIPv4)
            .Default(true);
        RegisterParameter("enable_ipv6", EnableIPv6)
            .Default(true);
        RegisterParameter("localhost_fqdn", LocalHostFqdn)
            .Default();
        RegisterParameter("address_expiration_time", AddressExpirationTime)
            .Default(TDuration::Minutes(1));
    }
};

typedef TIntrusivePtr<TAddressResolverConfig> TAddressResolverConfigPtr;

////////////////////////////////////////////////////////////////////////////////

//! An opaque wrapper for |sockaddr| type.
class TNetworkAddress
{
public:
    TNetworkAddress();
    TNetworkAddress(const TNetworkAddress& other, int port);
    explicit TNetworkAddress(const sockaddr& other, socklen_t length = 0);
    TNetworkAddress(int family, const char* addr, size_t size);

    sockaddr* GetSockAddr();
    const sockaddr* GetSockAddr() const;
    socklen_t GetLength() const;

    static TErrorOr<TNetworkAddress> TryParse(const TStringBuf& address);
    static TNetworkAddress Parse(const TStringBuf& address);

private:
    sockaddr_storage Storage;
    socklen_t Length;

    static socklen_t GetGenericLength(const sockaddr& sockAddr);
};

Stroka ToString(const TNetworkAddress& address, bool withPort = true);

bool operator == (const TNetworkAddress& lhs, const TNetworkAddress& rhs);
bool operator != (const TNetworkAddress& lhs, const TNetworkAddress& rhs);

////////////////////////////////////////////////////////////////////////////////

//! Performs asynchronous host name resolution.
class TAddressResolver
{
public:
    ~TAddressResolver();

    //! Returns the singleton instance.
    static TAddressResolver* Get();

    //! Destroys the singleton instance.
    static void StaticShutdown();

    //! Shuts down all internals of address resolver.
    void Shutdown();

    //! Resolves #address asynchronously.
    /*!
     *  Calls |getaddrinfo| and returns the first entry belonging to |AF_INET| or |AF_INET6| family.
     *  Caches successful resolutions.
     */
    TFuture<TNetworkAddress> Resolve(const Stroka& address);

    //! Return |true| if the local host FQDN can be properly determined.
    bool IsLocalHostNameOK();

    //! Returns |true| if #address matches one of local host addresses.
    bool IsLocalAddress(const TNetworkAddress& address);

    //! Removes all cached resolutions.
    void PurgeCache();

    //! Updates resolver configuration.
    void Configure(TAddressResolverConfigPtr config);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

    TAddressResolver();

    Y_DECLARE_SINGLETON_FRIEND();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
