#include "message_rearranger.h"

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

TMessageRearranger::TMessageRearranger(
    IParamAction<IMessage::TPtr>::TPtr onMessage,
    TDuration maxDelay)
    : OnMessage(onMessage)
    , Timeout(maxDelay)
    , ExpectedSequenceId(-1)
{ }

void TMessageRearranger::ArrangeMessage(IMessage::TPtr message, TSequenceId sequenceId)
{
    TGuard<TSpinLock> guard(SpinLock);
    if (sequenceId == ExpectedSequenceId) {
        TDelayedInvoker::Get()->Cancel(TimeoutCookie);
        OnMessage->Do(message);
        // TODO: extract method
        TimeoutCookie = TDelayedInvoker::Get()->Submit(
            FromMethod(&TMessageRearranger::OnExpired, this),
            Timeout);
        ExpectedSequenceId = sequenceId + 1;
    } else {
        if (MessageMap.empty()) {
            // TODO: extract method
            TimeoutCookie = TDelayedInvoker::Get()->Submit(
                FromMethod(&TMessageRearranger::OnExpired, this),
                Timeout);
        }
        MessageMap[sequenceId] = message;
    }
}

void TMessageRearranger::OnExpired()
{
    yvector<IMessage::TPtr> readyMessages;
    
    {
        TGuard<TSpinLock> guard(SpinLock);

        if (MessageMap.empty())
            return;

        ExpectedSequenceId = MessageMap.begin()->first;
        while (MessageMap.begin()->first == ExpectedSequenceId) {
            TMessageMap::iterator it = MessageMap.begin();
            readyMessages.push_back(it->second);
            MessageMap.erase(it);
            ++ExpectedSequenceId;
        }
    }

    for (yvector<IMessage::TPtr>::iterator it = readyMessages.begin();
         it != readyMessages.end();
         ++it)
    {
        OnMessage->Do(*it);
    }

    // TODO: extract method
    TimeoutCookie = TDelayedInvoker::Get()->Submit(
        FromMethod(&TMessageRearranger::OnExpired, this),
        Timeout);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
