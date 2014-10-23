#pragma once

#include "public.h"

#include <core/logging/log.h>

#include <ytlib/new_table_client/public.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

using NVersionedTableClient::TRowBuffer;

class TCGFragment;

////////////////////////////////////////////////////////////////////////////////

extern NLog::TLogger QueryClientLogger;

NLog::TLogger BuildLogger(const TConstQueryPtr& query);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

