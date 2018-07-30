#pragma once

#include "cluster_tracker.h"

#include <yt/server/clickhouse/interop/api.h>

namespace NYT {
namespace NClickHouse {

////////////////////////////////////////////////////////////////////////////////

void RegisterConcatenatingTableFunctions(
    NInterop::IStoragePtr storage,
    IExecutionClusterPtr cluster);

} // namespace NClickHouse
} // namespace NYT
