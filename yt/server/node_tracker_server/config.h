#pragma once

#include "public.h"

#include <core/ytree/yson_serializable.h>

namespace NYT {
namespace NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

class TNodeTrackerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration RegisteredNodeTimeout;
    TDuration OnlineNodeTimeout;

    int MaxConcurrentNodeRegistrations;
    int MaxConcurrentNodeUnregistrations;

    TNodeTrackerConfig()
    {
        RegisterParameter("registered_node_timeout", RegisteredNodeTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("online_node_timeout", OnlineNodeTimeout)
            .Default(TDuration::Seconds(60));

        RegisterParameter("max_concurrent_node_registrations", MaxConcurrentNodeRegistrations)
            .Default(5)
            .GreaterThan(0);
        RegisterParameter("max_concurrent_node_unregistrations", MaxConcurrentNodeUnregistrations)
            .Default(5)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TNodeTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
