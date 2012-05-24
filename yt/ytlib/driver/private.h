#pragma once

#include <ytlib/misc/common.h>
#include <ytlib/logging/log.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

struct ICommand;
struct ICommandContext;

////////////////////////////////////////////////////////////////////////////////

extern NLog::TLogger DriverLogger;

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
