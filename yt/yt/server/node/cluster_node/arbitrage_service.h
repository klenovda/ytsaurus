#include "public.h"

#include <yt/yt/core/rpc/public.h>

namespace NYT::NClusterNode {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateArbitrageService(IBootstrapBase* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClusterNode
