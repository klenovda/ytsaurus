#include "stdafx.h"
#include "nl_client.h"
#include "rpc.pb.h"
#include "message_rearranger.h"
#include "packet.h"

#include <ytlib/actions/action_util.h>
#include <ytlib/logging/log.h>
#include <ytlib/misc/thread_affinity.h>

#include <quality/netliba_v6/udp_http.h>

#include <util/generic/singleton.h>
#include <util/generic/list.h>
#include <util/generic/utility.h>

namespace NYT {
namespace NBus {

using namespace NNetliba;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = BusLogger;

// TODO: make configurable
static const int MaxNLCallsPerIteration = 10;
static const TDuration ClientSleepQuantum = TDuration::MilliSeconds(10);
static const TDuration MessageRearrangeTimeout = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

class TClientDispatcher;

class TNLBusClient
    : public IBusClient
{
public:
    typedef TIntrusivePtr<TNLBusClient> TPtr;

    TNLBusClient(TNLBusClientConfig* config);

    void Start();

    virtual ~TNLBusClient()
    { }

    virtual IBus::TPtr CreateBus(IMessageHandler* handler);

private:
    class TBus;
    friend class TClientDispatcher;

    TNLBusClientConfig::TPtr Config;
    TUdpAddress ServerAddress;
};

IBusClient::TPtr CreateNLBusClient(TNLBusClientConfig* config)
{
    auto client = New<TNLBusClient>(config);
    client->Start();
    return client;
}

////////////////////////////////////////////////////////////////////////////////

class TNLBusClient::TBus
    : public IBus
{
public:
    typedef TIntrusivePtr<TBus> TPtr;
    typedef yhash_set<TGuid> TRequestIdSet;

    TBus(const TUdpAddress& address, IMessageHandler* handler)
        : Address(address)
        , Handler(handler)
        , Terminated(false)
        // TODO: rearrangement is switched off, see YT-95
        //, MessageRearranger(New<TMessageRearranger>(
        //    SessionId,
        //    ~FromMethod(&TBus::OnMessageDequeued, TPtr(this)),
        //    MessageRearrangeTimeout))
    {
        RestartSession();
    }


    TUdpAddress GetAddress()
    {
        return Address;
    }

    void ProcessIncomingMessage(IMessage* message, TSequenceId sequenceId)
    {
        UNUSED(sequenceId);
        // TODO: rearrangement is switched off, see YT-95
        // MessageRearranger->EnqueueMessage(message, sequenceId);
        Handler->OnMessage(message, this);
    }

    virtual TSendResult::TPtr Send(IMessage::TPtr message);
    virtual void Terminate();


    void RestartSession()
    {
        SequenceId = 0;
        SessionId = TSessionId::Create();
    }

    const TSessionId& GetSessionId()
    {
        return SessionId;
    }


    TSequenceId GenerateSequenceId()
    {
        return SequenceId++;
    }


    TRequestIdSet& RequestIds()
    {
        return RequestIds_;
    }

    TRequestIdSet& PingIds()
    {
        return PingIds_;
    }

private:
    TUdpAddress Address;
    IMessageHandler::TPtr Handler;
    volatile bool Terminated;
    TSequenceId SequenceId;
    TSessionId SessionId;
    TRequestIdSet RequestIds_;
    TRequestIdSet PingIds_;

    // TODO: rearrangement is switched off, see YT-95
    // TMessageRearranger::TPtr MessageRearranger;
    //void OnMessageDequeued(IMessage* message);
};

////////////////////////////////////////////////////////////////////////////////

class TClientDispatcher
{
    struct TRequest
        : public TRefCounted
    {
        typedef TIntrusivePtr<TRequest> TPtr;

        TRequest(
            const TSessionId& sessionId,
            TBlob* data)
            : SessionId(sessionId)
            , Result(New<IBus::TSendResult>())
        {
            Data.swap(*data);
        }

        TSessionId SessionId;
        IBus::TSendResult::TPtr Result;
        TBlob Data;
    };

    typedef yhash_map<TSessionId, TNLBusClient::TBus::TPtr> TBusMap;
    typedef yhash_map<TGuid, TRequest::TPtr> TRequestMap;

    TThread Thread;
    volatile bool Terminated;
    // IRequester has to be stored by Arcadia's IntrusivePtr.
    ::TIntrusivePtr<IRequester> Requester;

    TBusMap BusMap;
    TRequestMap RequestMap;

    TLockFreeQueue<TRequest::TPtr> RequestQueue;
    TLockFreeQueue<TNLBusClient::TBus::TPtr> BusRegisterQueue;
    TLockFreeQueue<TNLBusClient::TBus::TPtr> BusUnregisterQueue;

    static void* ThreadFunc(void* param)
    {
        auto* dispatcher = reinterpret_cast<TClientDispatcher*>(param);
        dispatcher->ThreadMain();
        return NULL;
    }

    void ThreadMain()
    {
        while (!Terminated) {
            // NB: "&", not "&&" since we want every type of processing to happen on each iteration.
            if (!ProcessBusRegistrations() &
                !ProcessBusUnregistrations() &
                !ProcessOutcomingRequests() &
                !ProcessIncomingNLRequests() &
                !ProcessIncomingNLResponses())
            {
                LOG_TRACE("Client is idle");
                GetEvent().WaitT(ClientSleepQuantum);
            }
        }
    }

    bool ProcessBusRegistrations()
    {
        LOG_TRACE("Processing client bus registrations");

        bool result = false;
        TNLBusClient::TBus::TPtr bus;
        while (BusRegisterQueue.Dequeue(&bus)) {
            result = true;
            RegisterBus(~bus);
        }
        return result;
    }

    bool ProcessBusUnregistrations()
    {
        LOG_TRACE("Processing client bus unregistrations");

        bool result = false;
        TNLBusClient::TBus::TPtr bus;
        while (BusUnregisterQueue.Dequeue(&bus)) {
            result = true;
            UnregisterBus(~bus);
        }
        return result;
    }

    void RegisterBus(TNLBusClient::TBus* bus)
    {
        const auto& sessionId = bus->GetSessionId();
        BusMap.insert(MakePair(sessionId, bus));
        LOG_DEBUG("Bus is registered (SessionId: %s, Bus: %p, Address: %s)",
            ~sessionId.ToString(),
            bus,
            ~GetAddressAsString(bus->GetAddress()));
    }

    void UnregisterBus(TNLBusClient::TBus* bus)
    {
        const auto& sessionId = bus->GetSessionId();

        LOG_DEBUG("Bus is unregistered (SessionId: %s, Bus: %p)",
            ~sessionId.ToString(),
            bus);

        FOREACH(const auto& requestId, bus->RequestIds()) {
            Requester->CancelRequest((TGUID) requestId);
            RequestMap.erase(requestId);
        }
        bus->RequestIds().clear();

        TBlob ackData;
        CreatePacket(sessionId, TPacketHeader::EType::Ack, &ackData);

        FOREACH(const auto& requestId, bus->PingIds()) {
            Requester->SendResponse(requestId, &ackData);
        }
        bus->PingIds().clear();

        BusMap.erase(sessionId);
    }

    bool ProcessIncomingNLResponses()
    {
        LOG_TRACE("Processing incoming client NetLiba responses");

        int callCount = 0;
        while (callCount < MaxNLCallsPerIteration) {
            TAutoPtr<TUdpHttpResponse> nlResponse = Requester->GetResponse();
            if (!nlResponse)
                break;

            ++callCount;
            ProcessIncomingNLResponse(~nlResponse);
        }
        return callCount > 0;
    }

    void ProcessIncomingNLResponse(NNetliba::TUdpHttpResponse* nlResponse)
    {
        if (nlResponse->Ok != TUdpHttpResponse::OK)
        {
            ProcessFailedNLResponse(nlResponse);
            return;
        }

        auto* header = ParsePacketHeader<TPacketHeader>(nlResponse->Data);
        if (!header)
            return;

        switch (header->Type) {
            case TPacketHeader::EType::Ack:
                ProcessAck(header, nlResponse);
                break;

            case TPacketHeader::EType::Message:
                //ProcessAck(header, nlResponse);
                ProcessMessage(header, nlResponse);
                break;

            case TPacketHeader::EType::BrokenSession:
                ProcessBrokenSession(header, nlResponse);
                break;

            default:
                LOG_ERROR("Invalid response packet type (RequestId: %s, Type: %s)",
                    ~TGuid(nlResponse->ReqId).ToString(),
                    ~header->Type.ToString());
        }
    }

    void ProcessFailedNLResponse(TUdpHttpResponse* nlResponse)
    {
        TGuid requestId = nlResponse->ReqId;
        auto requestIt = RequestMap.find(requestId);
        if (requestIt == RequestMap.end()) {
            LOG_DEBUG("An obsolete request failed (RequestId: %s)",
                ~requestId.ToString());
            return;
        }

        auto request = requestIt->second;

        LOG_DEBUG("Request failed (SessionId: %s, RequestId: %s)",
            ~request->SessionId.ToString(),
            ~requestId.ToString());

        request->Result->Set(ESendResult::Failed);
        RequestMap.erase(requestIt);
    }

    bool ProcessIncomingNLRequests()
    {
        LOG_TRACE("Processing incoming client NetLiba requests");

        int callCount = 0;
        while (callCount < MaxNLCallsPerIteration) {
            TAutoPtr<TUdpHttpRequest> nlRequest = Requester->GetRequest();
            if (!nlRequest)
                break;

            ++callCount;
            ProcessIncomingNLRequest(~nlRequest);
        }
        return callCount > 0;
    }

    void ProcessIncomingNLRequest(TUdpHttpRequest* nlRequest)
    {
        auto* header = ParsePacketHeader<TPacketHeader>(nlRequest->Data);
        if (!header)
            return;

        switch (header->Type) {
            case TPacketHeader::EType::Ping:
                ProcessPing(header, nlRequest);
                break;

            case TPacketHeader::EType::Message:
                ProcessMessage(header, nlRequest);
                break;

            default:
                LOG_ERROR("Invalid request packet type (RequestId: %s, Type: %s)",
                    ~TGuid(nlRequest->ReqId).ToString(),
                    ~header->Type.ToString());
            return;
        }
    }

    void ProcessAck(TPacketHeader* header, TUdpHttpResponse* nlResponse)
    {
        TGuid requestId = nlResponse->ReqId;
        auto requestIt = RequestMap.find(requestId);
        if (requestIt == RequestMap.end()) {
            LOG_DEBUG("Ack for obsolete request received (SessionId: %s, RequestId: %s)",
                ~header->SessionId.ToString(),
                ~requestId.ToString());
            return;
        }

        LOG_DEBUG("Request ack received (SessionId: %s, RequestId: %s)",
            ~header->SessionId.ToString(),
            ~requestId.ToString());

        auto& request = requestIt->second;
        request->Result->Set(ESendResult::OK);
        RequestMap.erase(requestIt);
    }

    void ProcessBrokenSession(TPacketHeader* header, TUdpHttpResponse* nlResponse)
    {
        const auto& sessionId = header->SessionId;
        TGuid requestId = nlResponse->ReqId;
        auto requestIt = RequestMap.find(requestId);
        auto busIt = BusMap.find(sessionId);
        if (requestIt == RequestMap.end() || busIt == BusMap.end()) {
            LOG_DEBUG("Broken session reply for obsolete request received (SessionId: %s, RequestId: %s)",
                ~sessionId.ToString(),
                ~requestId.ToString());
            return;
        }

        auto& bus = busIt->second; 
        auto& request = requestIt->second;

        request->Result->Set(ESendResult::Failed);
        bus->RestartSession();
        const auto& newSessionId = bus->GetSessionId();
        YVERIFY(BusMap.insert(MakePair(newSessionId, bus)).second);
        BusMap.erase(busIt);
        RequestMap.erase(requestIt);

        LOG_DEBUG("Broken session reply received, session restarted (SessionId: %s, RequestId: %s, NewSessionId: %s)",
            ~sessionId.ToString(),
            ~requestId.ToString(),
            ~newSessionId.ToString());
    }

    void ProcessMessage(TPacketHeader* header, TUdpHttpRequest* nlRequest)
    {
        DoProcessMessage(header, nlRequest->ReqId, MoveRV(nlRequest->Data), true);
    }

    void ProcessMessage(TPacketHeader* header, TUdpHttpResponse* nlResponse)
    {
        DoProcessMessage(header, nlResponse->ReqId, MoveRV(nlResponse->Data), false);
    }

    void DoProcessMessage(TPacketHeader* header, const TGuid& requestId, TBlob&& data, bool isRequest)
    {
        int dataSize = data.ysize();
        const auto& sessionId = header->SessionId;

        auto busIt = BusMap.find(sessionId);
        if (busIt == BusMap.end()) {
            LOG_DEBUG("Message for an obsolete session is dropped (SessionId: %s, RequestId: %s, PacketSize: %d)",
                ~sessionId.ToString(),
                ~requestId.ToString(),
                dataSize);
            return;
        }

        IMessage::TPtr message;
        TSequenceId sequenceId;;
        if (!DecodeMessagePacket(MoveRV(data), &message, &sequenceId)) {
            LOG_WARNING("Error parsing message packet (RequestId: %s)", ~requestId.ToString());
            return;
        }

        LOG_DEBUG("Message received (IsRequest: %d, SessionId: %s, RequestId: %s, SequenceId: %" PRId64 ", PacketSize: %d)",
            (int) isRequest,
            ~sessionId.ToString(),
            ~requestId.ToString(),
            sequenceId,
            dataSize);

        auto& bus = busIt->second;
        bus->ProcessIncomingMessage(~message, sequenceId);

        if (!isRequest) {
            RequestMap.erase(requestId);
        } else {
            TBlob ackData;
            CreatePacket(sessionId, TPacketHeader::EType::Ack, &ackData);
            Requester->SendResponse((TGUID) requestId, &ackData);

            LOG_DEBUG("Ack sent (SessionId: %s, RequestId: %s)",
                ~sessionId.ToString(),
                ~requestId.ToString());
        }
    }

    void ProcessPing(TPacketHeader* header, TUdpHttpRequest* nlRequest)
    {
        TGuid requestId = nlRequest->ReqId;
        auto busIt = BusMap.find(header->SessionId);
        if (busIt == BusMap.end()) {
            LOG_DEBUG("Ping for an obsolete session received (SessionId: %s, RequestId: %s)",
                ~header->SessionId.ToString(),
                ~requestId.ToString());

            TBlob data;
            CreatePacket(header->SessionId, TPacketHeader::EType::Ack, &data);
            Requester->SendResponse(nlRequest->ReqId, &data);

            return;
        }

        LOG_DEBUG("Ping received (SessionId: %s, RequestId: %s)",
            ~header->SessionId.ToString(),
            ~requestId.ToString());

        // Don't reply to a ping, just register it.
        auto& bus = busIt->second;
        bus->PingIds().insert(requestId);
    }

    bool ProcessOutcomingRequests()
    {
        LOG_TRACE("Processing outcoming client NetLiba requests");

        int callCount = 0;
        while (callCount < MaxNLCallsPerIteration) {
            TRequest::TPtr request;
            if (!RequestQueue.Dequeue(&request))
                break;

            ++callCount;
            ProcessOutcomingRequest(request);
        }
        return callCount > 0;
    }

    void ProcessOutcomingRequest(TRequest::TPtr request)
    {
        auto busIt = BusMap.find(request->SessionId);
        if (busIt == BusMap.end()) {
            // Process all pending registrations and try once again.
            ProcessBusRegistrations();
            busIt = BusMap.find(request->SessionId);
            if (busIt == BusMap.end()) {
                // Still no luck.
                LOG_DEBUG("Request via an obsolete session is dropped (SessionId: %s, Request: %p)",
                    ~request->SessionId.ToString(),
                    ~request);
                return;
            }
        }

        auto& bus = busIt->second;

        TGuid requestId = Requester->SendRequest(bus->GetAddress(), "", &request->Data);

        bus->RequestIds().insert(requestId);
        RequestMap.insert(MakePair(requestId, request));

        LOG_DEBUG("Request sent (SessionId: %s, RequestId: %s, Request: %p)",
            ~request->SessionId.ToString(),
            ~requestId.ToString(),
            ~request);
    }

    Event& GetEvent()
    {
        return Requester->GetAsyncEvent();
    }

public:
    TClientDispatcher()
        : Thread(ThreadFunc, (void*) this)
        , Terminated(false)
    {
        Requester = CreateHttpUdpRequester(0);
        if (!Requester) {
            ythrow yexception() << "Failed to create a client NetLiba requester";
        }

        Thread.Start();

        LOG_DEBUG("Client dispatcher is started");
    }

    ~TClientDispatcher()
    {
        Shutdown();
    }

    static TClientDispatcher* Get()
    {
        return Singleton<TClientDispatcher>();
    }

    void Shutdown()
    {
        Terminated = true;
        Thread.Join();

        // NB: This doesn't actually stop NetLiba threads.
        Requester->StopNoWait();

        // TODO: Consider moving somewhere else.
        StopAllNetLibaThreads();

        // NB: cannot use log here
    }

    IBus::TSendResult::TPtr EnqueueRequest(TNLBusClient::TBus* bus, IMessage* message)
    {
        auto sequenceId = bus->GenerateSequenceId();
        const auto& sessionId = bus->GetSessionId();

        TBlob data;
        if (!EncodeMessagePacket(message, sessionId, sequenceId, &data))
            ythrow yexception() << "Failed to encode a message";

        int dataSize = data.ysize();
        auto request = New<TRequest>(sessionId, &data);
        RequestQueue.Enqueue(request);
        GetEvent().Signal();

        LOG_DEBUG("Request enqueued (SessionId: %s, Request: %p, SequenceId: %" PRId64 ", PacketSize: %d)",
            ~sessionId.ToString(),
            ~request,
            sequenceId,
            dataSize);

        return request->Result;
    }

    void EnqueueBusRegister(const TNLBusClient::TBus::TPtr& bus)
    {
        const auto& sessionId = bus->GetSessionId();

        BusRegisterQueue.Enqueue(bus);
        GetEvent().Signal();

        LOG_DEBUG("Bus registration enqueued (SessionId: %s, Bus: %p)",
            ~sessionId.ToString(),
            ~bus);
    }

    void EnqueueBusUnregister(const TNLBusClient::TBus::TPtr& bus)
    {
        const auto& sessionId = bus->GetSessionId();

        BusUnregisterQueue.Enqueue(bus);
        GetEvent().Signal();

        LOG_DEBUG("Bus unregistration enqueued (SessionId: %s, Bus: %p)",
            ~sessionId.ToString(),
            ~bus);
    }

    Stroka GetDebugInfo()
    {
        return
            "ClientDispatcher info:\n" + Requester->GetDebugInfo() + "\n" +
            "Pending data size: " + ToString(Requester->GetPendingDataSize());
    }
};

////////////////////////////////////////////////////////////////////////////////

// TODO: hack
void ShutdownClientDispatcher()
{
    TClientDispatcher::Get()->Shutdown();
}

Stroka GetClientDispatcherDebugInfo()
{
    return TClientDispatcher::Get()->GetDebugInfo();
}

////////////////////////////////////////////////////////////////////////////////

TNLBusClient::TNLBusClient(TNLBusClientConfig* config)
    : Config(config)
{ }

void TNLBusClient::Start()
{
    ServerAddress = CreateAddress(Config->Address, 0);
    if (ServerAddress == TUdpAddress()) {
        ythrow yexception() << Sprintf("Failed to resolve the address %s",
            ~Config->Address.Quote());
    }
}

IBus::TPtr TNLBusClient::CreateBus(IMessageHandler* handler)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(handler);

    auto bus = New<TBus>(ServerAddress, handler);
    TClientDispatcher::Get()->EnqueueBusRegister(bus);
    return bus;
}

////////////////////////////////////////////////////////////////////////////////

IBus::TSendResult::TPtr TNLBusClient::TBus::Send(IMessage::TPtr message)
{
    VERIFY_THREAD_AFFINITY_ANY();
    // NB: We may actually need a barrier here but
    // since Terminate is used for debugging purposes mainly, we omit it.
    YASSERT(!Terminated);

    return TClientDispatcher::Get()->EnqueueRequest(this, ~message);
}

void TNLBusClient::TBus::Terminate()
{
    if (Terminated)
        return;

    Terminated = true;

    // TODO: rearrangement is switched off, see YT-95
    //MessageRearranger.Reset();
    TClientDispatcher::Get()->EnqueueBusUnregister(this);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
