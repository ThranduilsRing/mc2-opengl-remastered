// GameOS formatting compatibility header and platform debugger sink.
#pragma once

#include "mc2format.h"

namespace mc2::fmt {

// Keep the platform debugger dependency outside the pure formatting interface.
#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char*);
#endif
inline void debug_out(const char* text) noexcept {
#ifdef _WIN32
    OutputDebugStringA(text ? text : "");
#else
    std::fputs(text ? text : "", stderr);
#endif
}

}  // namespace mc2::fmt
