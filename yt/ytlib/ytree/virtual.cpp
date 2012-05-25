#include "stdafx.h"
#include "virtual.h"
#include "fluent.h"
#include "node_detail.h"
#include "yson_writer.h"
#include "ypath_detail.h"
#include "ypath_client.h"
#include "attribute_provider_detail.h"
#include "tokenizer.h"

#include <ytlib/misc/configurable.h>

namespace NYT {
namespace NYTree {

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

const int DefaultMaxSize = 1000;

////////////////////////////////////////////////////////////////////////////////

void TVirtualMapBase::DoInvoke(IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Get);
    DISPATCH_YPATH_SERVICE_METHOD(List);
    TSupportsAttributes::DoInvoke(context);
}

IYPathService::TResolveResult TVirtualMapBase::ResolveRecursive(const TYPath& path, const Stroka& verb)
{
    UNUSED(verb);

    TTokenizer tokenizer(path);
    tokenizer.ParseNext();
    Stroka key(tokenizer.CurrentToken().GetStringValue());

    auto service = GetItemService(key);
    if (!service) {
        ythrow yexception() << Sprintf("Key %s is not found", ~Stroka(key).Quote());
    }

    return TResolveResult::There(service, TYPath(tokenizer.GetCurrentSuffix()));
}

void TVirtualMapBase::GetSelf(TReqGet* request, TRspGet* response, TCtxGet* context)
{
    YASSERT(!TTokenizer(context->GetPath()).ParseNext());

    int max_size = request->Attributes().Get<int>("max_size", DefaultMaxSize);

    TStringStream stream;
    TYsonWriter writer(&stream, EYsonFormat::Binary);
    auto keys = GetKeys(max_size);
    auto size = GetSize();

    // TODO(MRoizner): use fluent
    BuildYsonFluently(&writer);

    if (keys.ysize() != size) {
        writer.OnBeginAttributes();
        writer.OnKeyedItem("incomplete");
        writer.OnStringScalar("true");
        writer.OnEndAttributes();
    }

    writer.OnBeginMap();
    FOREACH (const auto& key, keys) {
        writer.OnKeyedItem(key);
        writer.OnEntity();
    }
    writer.OnEndMap();

    response->set_value(stream.Str());
    context->Reply();
}

void TVirtualMapBase::ListSelf(TReqList* request, TRspList* response, TCtxList* context)
{
    UNUSED(request);
    YASSERT(!TTokenizer(context->GetPath()).ParseNext());

    auto keys = GetKeys();
    NYT::ToProto(response->mutable_keys(), keys);
    context->Reply();
}

void TVirtualMapBase::GetSystemAttributes(std::vector<TAttributeInfo>* attributes)
{
    attributes->push_back("count");
}

bool TVirtualMapBase::GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    if (key == "count") {
        BuildYsonFluently(consumer)
            .Scalar(static_cast<i64>(GetSize()));
        return true;
    }

    return false;
}

bool TVirtualMapBase::SetSystemAttribute(const Stroka& key, const TYson& value)
{
    UNUSED(key);
    UNUSED(value);
    return false;
}

ISystemAttributeProvider* TVirtualMapBase::GetSystemAttributeProvider()
{
    return this;
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualEntityNode
    : public TNodeBase
    , public TSupportsAttributes
    , public IEntityNode
    , public TEphemeralAttributeProvider
{
    YTREE_NODE_TYPE_OVERRIDES(Entity)

public:
    TVirtualEntityNode(IYPathServicePtr underlyingService)
        : UnderlyingService(underlyingService)
    { }

    virtual INodeFactoryPtr CreateFactory() const
    {
        YASSERT(Parent);
        return Parent->CreateFactory();
    }

    virtual ICompositeNodePtr GetParent() const
    {
        return Parent;
    }

    virtual void SetParent(ICompositeNode* parent)
    {
        Parent = parent;
    }

    virtual TResolveResult Resolve(const TYPath& path, const Stroka& verb)
    {
        // TODO(babenko): handle ugly face
        return TResolveResult::There(UnderlyingService, path);
    }

private:
    IYPathServicePtr UnderlyingService;
    ICompositeNode* Parent;
    
    // TSupportsAttributes members

    virtual IAttributeDictionary* GetUserAttributes()
    {
        return &Attributes();
    }
};

INodePtr CreateVirtualNode(IYPathServicePtr service)
{
    return New<TVirtualEntityNode>(service);
}

NYT::NYTree::INodePtr CreateVirtualNode(TYPathServiceProducer producer)
{
    return CreateVirtualNode(IYPathService::FromProducer(producer));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
