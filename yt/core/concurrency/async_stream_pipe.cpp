#include "async_stream_pipe.h"

namespace NYT {
namespace NConcurrency {

///////////////////////////////////////////////////////////////////////////////

struct TAsyncStreamPipeTag
{ };

///////////////////////////////////////////////////////////////////////////////

TAsyncStreamPipe::TItem::TItem(TSharedRef sharedRef, TPromise<void> writeComplete)
    : SharedRef_(std::move(sharedRef))
    , WriteComplete_(std::move(writeComplete))
{ }

///////////////////////////////////////////////////////////////////////////////

TFuture<void> TAsyncStreamPipe::Write(const TSharedRef& buffer)
{
    if (!buffer) {
        // Empty buffer has special meaning in our queue, so we don't write it.
        return VoidFuture;
    }

    auto writeComplete = NewPromise<void>();
    Queue_.Enqueue(TItem(TSharedRef::MakeCopy<TAsyncStreamPipeTag>(buffer), writeComplete));
    return writeComplete;
}

TFuture<TSharedRef> TAsyncStreamPipe::Read()
{
    auto result = Queue_.Dequeue();
    return result.Apply(BIND([] (TItem item) {
        item.WriteComplete_.Set();
        return item.SharedRef_;
    }));
}

TFuture<void> TAsyncStreamPipe::Close()
{
    Queue_.Enqueue(TItem(TSharedRef(), NewPromise<void>()));
    return VoidFuture;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
