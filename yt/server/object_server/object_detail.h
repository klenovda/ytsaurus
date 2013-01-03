#pragma once

#include "public.h"
#include "object.h"
#include "object_proxy.h"
#include "object_manager.h"

#include <ytlib/misc/property.h>

#include <ytlib/meta_state/map.h>

#include <ytlib/ytree/ypath_detail.h>
#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/system_attribute_provider.h>
#include <ytlib/ytree/ypath.pb.h>

#include <ytlib/object_client/object_ypath.pb.h>

#include <server/cell_master/public.h>

#include <server/transaction_server/public.h>

#include <server/security_server/public.h>


namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

class TStagedObjectBase
    : public TUnversionedObjectBase
{
    DEFINE_BYVAL_RW_PROPERTY(NTransactionServer::TTransaction*, StagingTransaction);
    DEFINE_BYVAL_RW_PROPERTY(NSecurityServer::TAccount*, StagingAccount);

public:
    explicit TStagedObjectBase(const TObjectId& id);

    void Save(const NCellMaster::TSaveContext& context) const;
    void Load(const NCellMaster::TLoadContext& context);

    //! Returns True if the object is the staging area of some transaction.
    bool IsStaged() const;

};

////////////////////////////////////////////////////////////////////////////////

// We need definition of this class in header because we want to inherit it.
class TUserAttributeDictionary
    : public NYTree::IAttributeDictionary
{
public:
    TUserAttributeDictionary(TObjectManagerPtr objectManager, const TObjectId& objectId);

    // NYTree::IAttributeDictionary members
    virtual std::vector<Stroka> List() const override;
    virtual TNullable<NYTree::TYsonString> FindYson(const Stroka& key) const override;
    virtual void SetYson(const Stroka& key, const NYTree::TYsonString& value) override;
    virtual bool Remove(const Stroka& key) override;

protected:
    TObjectManagerPtr ObjectManager;
    TObjectId ObjectId;

};

////////////////////////////////////////////////////////////////////////////////

class TObjectProxyBase
    : public virtual NYTree::TSupportsAttributes
    , public virtual NYTree::ISystemAttributeProvider
    , public virtual IObjectProxy
{
public:
    TObjectProxyBase(NCellMaster::TBootstrap* bootstrap, TObjectBase* object);
    ~TObjectProxyBase();

    // IObjectProxy members
    virtual const TObjectId& GetId() const override;
    virtual NYTree::IAttributeDictionary& Attributes() override;
    virtual const NYTree::IAttributeDictionary& Attributes() const override;
    virtual void Invoke(NRpc::IServiceContextPtr context) override;
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* consumer,
        const NYTree::TAttributeFilter& filter) const override;

protected:
    NCellMaster::TBootstrap* Bootstrap;
    TObjectBase* Object;
    TAutoPtr<NYTree::IAttributeDictionary> UserAttributes;

    DECLARE_RPC_SERVICE_METHOD(NObjectClient::NProto, GetId);

    //! Returns the full object id that coincides with #Id
    //! for non-versioned objects and additionally includes transaction id for
    //! versioned ones.
    virtual TVersionedObjectId GetVersionedId() const;

    void GuardedInvoke(NRpc::IServiceContextPtr context);
    virtual void DoInvoke(NRpc::IServiceContextPtr context) override;
    virtual bool IsWriteRequest(NRpc::IServiceContextPtr context) const override;

    // NYTree::TSupportsAttributes members
    virtual NYTree::IAttributeDictionary* GetUserAttributes() override;
    virtual ISystemAttributeProvider* GetSystemAttributeProvider() override;

    virtual TAutoPtr<NYTree::IAttributeDictionary> DoCreateUserAttributes();

    // NYTree::ISystemAttributeProvider members
    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) const override;
    virtual bool GetSystemAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) const override;
    virtual TAsyncError GetSystemAttributeAsync(const Stroka& key, NYson::IYsonConsumer* consumer) const override;
    virtual bool SetSystemAttribute(const Stroka& key, const NYTree::TYsonString& value) override;

    bool IsRecovery() const;
    bool IsLeader() const;
    void ValidateActiveLeader() const;
    void ForwardToLeader(NRpc::IServiceContextPtr context);
    void OnLeaderResponse(NRpc::IServiceContextPtr context, NBus::IMessagePtr responseMessage);

};

////////////////////////////////////////////////////////////////////////////////

class TNonversionedObjectProxyNontemplateBase
    : public TObjectProxyBase
{
public:
    TNonversionedObjectProxyNontemplateBase(
        NCellMaster::TBootstrap* bootstrap,
        TObjectBase* object);

    virtual bool IsWriteRequest(NRpc::IServiceContextPtr context) const override;

protected:
    virtual void DoInvoke(NRpc::IServiceContextPtr context) override;

    virtual void GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context) override;

    virtual void ValidateRemoval();

    DECLARE_RPC_SERVICE_METHOD(NYTree::NProto, Remove);

};

////////////////////////////////////////////////////////////////////////////////

template <class TObject>
class TNonversionedObjectProxyBase
    : public TNonversionedObjectProxyNontemplateBase
{
public:
    typedef typename NMetaState::TMetaStateMap<TObjectId, TObject> TMap;

    TNonversionedObjectProxyBase(
        NCellMaster::TBootstrap* bootstrap,
        TObject* object,
        TMap* map)
        : TNonversionedObjectProxyNontemplateBase(bootstrap, object)
        , Map(map)
    {
        YASSERT(map);
    }

protected:
    TMap* Map;


    const TObject* GetThisTypedImpl() const
    {
        return static_cast<const TObject*>(Object);
    }

    TObject* GetThisTypedImpl()
    {
        return static_cast<TObject*>(Object);
    }

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

