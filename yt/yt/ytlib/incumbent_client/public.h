#pragma once

#include <yt/yt/core/misc/public.h>

namespace NYT::NIncumbentClient {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EIncumbentType,
    ((ChunkReplicator)        (0))
    ((CellJanitor)            (1))
);

////////////////////////////////////////////////////////////////////////////////

int GetIncumbentShardCount(EIncumbentType type);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NIncumbentClient
