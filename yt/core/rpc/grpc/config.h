#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

#include <contrib/libs/grpc/include/grpc/grpc_security_constants.h>

namespace NYT {
namespace NRpc {
namespace NGrpc {

////////////////////////////////////////////////////////////////////////////////

class TPemBlobConfig
    : public NYTree::TYsonSerializable
{
public:
    TNullable<TString> FileName;
    TNullable<TString> Value;

    TPemBlobConfig()
    {
        RegisterParameter("file_name", FileName)
            .Optional();
        RegisterParameter("value", Value)
            .Optional();

        RegisterPostprocessor([&] {
            if (FileName && Value) {
                THROW_ERROR_EXCEPTION("Cannot specify both \"file_name\" and \"value\"");
            }
            if (!FileName && !Value) {
                THROW_ERROR_EXCEPTION("Must specify either \"file_name\" or \"value\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TPemBlobConfig)

////////////////////////////////////////////////////////////////////////////////

class TSslPemKeyCertPairConfig
    : public NYTree::TYsonSerializable
{
public:
    TPemBlobConfigPtr PrivateKey;
    TPemBlobConfigPtr CertChain;
        
    TSslPemKeyCertPairConfig()
    {
        RegisterParameter("private_key", PrivateKey)
            .Optional();
        RegisterParameter("cert_chain", CertChain)
            .Optional();
    }
};

DEFINE_REFCOUNTED_TYPE(TSslPemKeyCertPairConfig)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EClientCertificateRequest,
    ((DontRequestClientCertificate)(GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE))
    ((RequestClientCertificateButDontVerify)(GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_BUT_DONT_VERIFY))
    ((RequestClientCertificateAndVerify)(GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY))
    ((RequestAndRequireClientCertificateButDontVerify)(GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY))
    ((RequestAndRequireClientCertificateAndVerify)(GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY))
);

////////////////////////////////////////////////////////////////////////////////

class TServerCredentialsConfig
    : public NYTree::TYsonSerializable
{
public:
    TPemBlobConfigPtr PemRootCerts;
    std::vector<TSslPemKeyCertPairConfigPtr> PemKeyCertPairs;
    EClientCertificateRequest ClientCertificateRequest;

    TServerCredentialsConfig()
    {
        RegisterParameter("pem_root_certs", PemRootCerts)
            .Optional();
        RegisterParameter("pem_key_cert_pairs", PemKeyCertPairs);
        RegisterParameter("client_certificate_request", ClientCertificateRequest)
            .Default(EClientCertificateRequest::RequestAndRequireClientCertificateAndVerify);
    }
};

DEFINE_REFCOUNTED_TYPE(TServerCredentialsConfig)

////////////////////////////////////////////////////////////////////////////////

class TServerAddressConfig
    : public NYTree::TYsonSerializable
{
public:
    TString Address;
    TServerCredentialsConfigPtr Credentials;

    TServerAddressConfig()
    {
        RegisterParameter("address", Address);
        RegisterParameter("credentials", Credentials)
            .Optional();
    }
};

DEFINE_REFCOUNTED_TYPE(TServerAddressConfig)

////////////////////////////////////////////////////////////////////////////////

class TServerConfig
    : public NYTree::TYsonSerializable
{
public:
    std::vector<TServerAddressConfigPtr> Addresses;
    THashMap<TString, NYTree::INodePtr> GrpcArguments;

    TServerConfig()
    {
        RegisterParameter("addresses", Addresses);
        RegisterParameter("grpc_arguments", GrpcArguments)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TServerConfig)

////////////////////////////////////////////////////////////////////////////////

class TChannelCredentialsConfig
    : public NYTree::TYsonSerializable
{
public:
    TPemBlobConfigPtr PemRootCerts;
    TSslPemKeyCertPairConfigPtr PemKeyCertPair;

    TChannelCredentialsConfig()
    {
        RegisterParameter("pem_root_certs", PemRootCerts)
            .Optional();
        RegisterParameter("pem_key_cert_pair", PemKeyCertPair);
    }
};

DEFINE_REFCOUNTED_TYPE(TChannelCredentialsConfig)

////////////////////////////////////////////////////////////////////////////////

class TChannelConfig
    : public NYTree::TYsonSerializable
{
public:
    TString Address;
    TChannelCredentialsConfigPtr Credentials;
    THashMap<TString, NYTree::INodePtr> GrpcArguments;

    TChannelConfig()
    {
        RegisterParameter("address", Address);
        RegisterParameter("credentials", Credentials)
            .Optional();
        RegisterParameter("grpc_arguments", GrpcArguments)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TChannelConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NGrpc
} // namespace NRpc
} // namespace NYT
