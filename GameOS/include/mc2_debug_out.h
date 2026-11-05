// Platform debugger sink for the mc2fmt facade. Kept out of mclib/mc2fmt.h so the sim translation units that include
// mc2fmt.h do not see the OutputDebugStringA extern. Includes mc2fmt.h, so a file needing both takes this one include.
#pragma once

#include <cstdio>

#include "mc2fmt.h"

namespace mc2::fmt {

#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
#endif
inline void debug_out(mc2::czstring text) noexcept {
#ifdef _WIN32
    OutputDebugStringA(text ? text : "");
#else
    std::fputs(text ? text : "", stderr);
#endif
}

}  // namespace mc2::fmt
