#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NQueryClient {

constexpr int MaxExpressionDepth = 50;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_GLOBAL(const NLogging::TLogger, QueryClientLogger, "QueryClient");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient

