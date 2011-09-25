#pragma once

#include "common.h"
#include "client.h"
#include "message.h"

#include "../misc/hash.h"
#include "../misc/metric.h"
#include "../logging/log.h"

#include <util/generic/yexception.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

class TServiceException 
    : public yexception
{
public:
    TServiceException(EErrorCode errorCode = EErrorCode::ServiceError)
        : ErrorCode(errorCode)
    { }

    EErrorCode GetErrorCode() const
    {
        return ErrorCode;
    }

private:
    EErrorCode ErrorCode;

};

////////////////////////////////////////////////////////////////////////////////

class TServiceContext;

////////////////////////////////////////////////////////////////////////////////

struct IService
    : public virtual TRefCountedBase
{
    typedef TIntrusivePtr<IService> TPtr;

    virtual Stroka GetServiceName() const = 0;
    virtual Stroka GetLoggingCategory() const = 0;

    virtual void OnBeginRequest(TIntrusivePtr<TServiceContext> context) = 0;
    virtual void OnEndRequest(TIntrusivePtr<TServiceContext> context) = 0;

    virtual Stroka GetDebugInfo() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TServiceContext
    : public TRefCountedBase
{
public:
    typedef TIntrusivePtr<TServiceContext> TPtr;

    TServiceContext(
        IService::TPtr service,
        TRequestId requestId,
        Stroka methodName,
        NBus::IMessage::TPtr message,
        NBus::IBus::TPtr replyBus);
    
    void Reply(EErrorCode errorCode = EErrorCode::OK);

    TSharedRef GetRequestBody() const;
    void SetResponseBody(TBlob* responseBody);

    const yvector<TSharedRef>& GetRequestAttachments() const;
    void SetResponseAttachments(yvector<TSharedRef>* attachments);

    Stroka GetServiceName() const;
    Stroka GetMethodName() const;
    const TRequestId& GetRequestId() const;

    NBus::IBus::TPtr GetReplyBus() const;

    void SetRequestInfo(const Stroka& info);
    Stroka GetRequestInfo() const;

    void SetResponseInfo(const Stroka& info);
    Stroka GetResponseInfo();

    IAction::TPtr Wrap(IAction::TPtr action);

protected:
    DECLARE_ENUM(EState,
        (Received)
        (Replied)
    );

    EState State;
    IService::TPtr Service;
    TRequestId RequestId;
    Stroka MethodName;
    NBus::IBus::TPtr ReplyBus;
    TSharedRef RequestBody;
    yvector<TSharedRef> RequestAttachments;
    NLog::TLogger ServiceLogger;

    TBlob ResponseBody;
    yvector<TSharedRef> ResponseAttachments;

    Stroka RequestInfo;
    Stroka ResponseInfo;

private:
    void DoReply(EErrorCode errorCode);
    void WrapThunk(IAction::TPtr action) throw();

    void LogException(NLog::ELogLevel level, EErrorCode errorCode, Stroka what);
    void LogRequestInfo();
    void LogResponseInfo(EErrorCode errorCode);

    static void AppendInfo(Stroka& lhs, Stroka rhs);
};

////////////////////////////////////////////////////////////////////////////////

template<class TRequestMessage, class TResponseMessage>
class TTypedServiceRequest
    : public TRequestMessage
    , private TNonCopyable
{
public:
    TTypedServiceRequest(const yvector<TSharedRef>& attachments)
        : Attachments_(attachments)
    { }

    yvector<TSharedRef>& Attachments()
    {
        return Attachments_;
    }

private:
    yvector<TSharedRef> Attachments_;

};

////////////////////////////////////////////////////////////////////////////////

template<class TRequestMessage, class TResponseMessage>
class TTypedServiceResponse
    : public TResponseMessage
    , private TNonCopyable
{
public:
    yvector<TSharedRef>& Attachments()
    {
        return Attachments_;
    }

private:
    yvector<TSharedRef> Attachments_;

};

////////////////////////////////////////////////////////////////////////////////

// TODO: move impl to inl?
template<class TRequestMesssage, class TResponseMessage>
class TTypedServiceContext
    : public TRefCountedBase
{
public:
    typedef TIntrusivePtr< TTypedServiceContext<TRequestMesssage, TResponseMessage> > TPtr;
    typedef TTypedServiceRequest<TRequestMesssage, TResponseMessage> TTypedRequest;
    typedef TTypedServiceResponse<TRequestMesssage, TResponseMessage> TTypedResponse;

    TTypedServiceContext(TServiceContext::TPtr context)
        : Logger(RpcLogger)
        , Context(context)
        , Request_(context->GetRequestAttachments())
    {
        if (!DeserializeMessage(&Request_, context->GetRequestBody()))
            ythrow TServiceException(EErrorCode::ProtocolError) <<
                "Can't deserialize request body";
    }

    TTypedRequest& Request()
    {
        return Request_;
    }

    TTypedResponse& Response()
    {
        return Response_;
    }

    void Reply(EErrorCode errorCode = EErrorCode::OK)
    {
        TBlob responseData;
        if (!SerializeMessage(&Response_, &responseData)) {
            LOG_FATAL("Error serializing response");
        }
        Context->SetResponseBody(&responseData);
        Context->SetResponseAttachments(&Response_.Attachments());
        Context->Reply(errorCode);
    }

    IAction::TPtr Wrap(typename IParamAction<TPtr>::TPtr paramAction)
    {
        return Context->Wrap(paramAction->Bind(TPtr(this)));
    }
    
    void SetRequestInfo(const Stroka& info)
    {
        Context->SetRequestInfo(info);
    }

    void SetRequestInfo(const char* format, ...)
    {
        Stroka info;
        va_list params;
        va_start(params, format);
        vsprintf(info, format, params);
        va_end(params);
        Context->SetRequestInfo(info);
    }

    Stroka GetRequestInfo() const
    {
        return Context->GetRequestInfo();
    }

    void SetResponseInfo(const Stroka& info)
    {
        Context->SetResponseInfo(info);
    }

    void SetResponseInfo(const char* format, ...)
    {
        Stroka info;
        va_list params;
        va_start(params, format);
        vsprintf(info, format, params);
        va_end(params);
        Context->SetResponseInfo(info);
    }

    Stroka GetResponseInfo()
    {
        return Context->GetResponseInfo();
    }

    TServiceContext::TPtr GetUntypedContext() const
    {
        return Context;
    }

private:
    NLog::TLogger& Logger;
    TServiceContext::TPtr Context;
    TTypedRequest Request_;
    TTypedResponse Response_;

};

////////////////////////////////////////////////////////////////////////////////

class TServiceBase
    : public IService
{
public:
    Stroka GetDebugInfo() const;

protected:
    typedef IParamAction<TIntrusivePtr<TServiceContext> > THandler;

    struct TMethodInfo
    {
        TMethodInfo(Stroka methodName, THandler::TPtr handler)
            : MethodName(methodName)
            , Handler(handler)
        {
            YASSERT(~handler != NULL);
        }

        Stroka MethodName;
        THandler::TPtr Handler;
    };

    TServiceBase(
        IInvoker::TPtr serviceInvoker,
        Stroka serviceName,
        Stroka loggingCategory);

    void RegisterMethod(const TMethodInfo& info);

    NLog::TLogger ServiceLogger;
    IInvoker::TPtr ServiceInvoker;

private:
    struct TRuntimeMethodInfo
    {
        TMethodInfo Info;
        TMetric ExecutionTime;

        TRuntimeMethodInfo(const TMethodInfo& info)
            : Info(info)
            // TODO: configure properly
            , ExecutionTime(0, 1000, 10)
        { }
    };

    struct TOutstandingRequest
    {
        TOutstandingRequest(TRuntimeMethodInfo* runtimeInfo, const TInstant& startTime)
            : RuntimeInfo(runtimeInfo)
            , StartTime(startTime)
        {
            YASSERT(runtimeInfo != NULL);
        }

        TRuntimeMethodInfo* RuntimeInfo;
        TInstant StartTime;
    };
    
    Stroka ServiceName;
    yhash_map<Stroka, TRuntimeMethodInfo> RuntimeMethodInfos;
    yhash_map<TServiceContext::TPtr, TOutstandingRequest> OutstandingRequests;

    virtual void OnBeginRequest(TServiceContext::TPtr context);
    virtual void OnEndRequest(TServiceContext::TPtr context);

    virtual Stroka GetLoggingCategory() const;
    virtual Stroka GetServiceName() const;

};

////////////////////////////////////////////////////////////////////////////////

#define RPC_SERVICE_METHOD_DECL(ns, method) \
    typedef ::NYT::NRpc::TTypedServiceRequest<ns::TReq##method, ns::TRsp##method> TReq##method; \
    typedef ::NYT::NRpc::TTypedServiceResponse<ns::TReq##method, ns::TRsp##method> TRsp##method; \
    typedef ::NYT::NRpc::TTypedServiceContext<ns::TReq##method, ns::TRsp##method> TCtx##method; \
    \
    void method##Thunk(::NYT::NRpc::TServiceContext::TPtr context); \
    \
    void method( \
        TCtx##method::TTypedRequest* request, \
        TCtx##method::TTypedResponse* response, \
        TCtx##method::TPtr context)

#define RPC_SERVICE_METHOD_IMPL(type, method) \
    void type::method##Thunk(::NYT::NRpc::TServiceContext::TPtr context) \
    { \
        TCtx##method::TPtr typedContext = New<TCtx##method>(context); \
        method( \
            &typedContext->Request(), \
            &typedContext->Response(), \
            typedContext); \
    } \
    \
    void type::method( \
        TCtx##method::TTypedRequest* request, \
        TCtx##method::TTypedResponse* response, \
        TCtx##method::TPtr context)

#define RPC_SERVICE_METHOD_INFO(method) \
    TMethodInfo(#method, FromMethod(&TThis::method##Thunk, this)) \

// TODO: not used, consider dropping
#define USE_RPC_SERVICE_METHOD_LOGGER() \
    ::NYT::NLog::TPrefixLogger Logger( \
        ServiceLogger, \
        context->GetMethodName() + ": ")
        
////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
