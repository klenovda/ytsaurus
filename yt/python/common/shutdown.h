#pragma once

#include <yt/core/actions/callback.h>

namespace NYT::NPython {

////////////////////////////////////////////////////////////////////////////////

void RegisterShutdownCallback(TCallback<void()> additionalCallback, int index);
void RegisterShutdown();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NPython

