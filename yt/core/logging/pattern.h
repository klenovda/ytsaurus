#pragma once

#include "public.h"

#include <yt/core/misc/raw_formatter.h>

namespace NYT::NLogging {

////////////////////////////////////////////////////////////////////////////////

const int MessageBufferSize = 65556;
typedef TRawFormatter<MessageBufferSize> TMessageBuffer;

void FormatMessage(TMessageBuffer* out, TStringBuf message);
void FormatDateTime(TMessageBuffer* out, TInstant dateTime);
void FormatLevel(TMessageBuffer* out, ELogLevel level);

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging::NYT
