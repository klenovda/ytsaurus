#include "serialize.h"

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

int GetCurrentSnapshotVersion()
{
    return 16;
}

bool ValidateSnapshotVersion(int version)
{
    return version == 16;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
