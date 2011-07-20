#pragma once

#include <util/system/yassert.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Evaluates the expression #expr. In debug mode throws an error if (#expr == false)
#define YVERIFY(expr) \
do { \
    try { \
        if (EXPECT_FALSE( !(expr) )) { \
            if (YaIsDebuggerPresent()) __debugbreak(); \
            else assert(0&&(expr)); \
        } \
    } catch (...) { \
        if (YaIsDebuggerPresent()) __debugbreak(); \
        else assert(false && "Exception during verification"); \
    } \
} while (0)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
