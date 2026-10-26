#pragma once

#ifdef _MSC_VER
#include <intrin.h>
#define MC2_SEH_TRY __try
#define MC2_SEH_EXCEPT(filter) __except(filter)
#else
#include <cstdint>
#include <x86intrin.h>
#define __forceinline inline __attribute__((always_inline))
#define __cdecl
#define __stdcall
#define _stdcall
// POSIX does not turn access violations into C++ exceptions. Invalid memory
// remains a fatal error; ordinary C++ exceptions can still reach the handler.
#define MC2_SEH_TRY try
#define MC2_SEH_EXCEPT(filter) catch (...)
inline unsigned char _BitScanForward(unsigned long* index, unsigned long mask)
{
    const auto bits = static_cast<std::uint32_t>(mask);
    if (!bits) return 0;
    *index = __builtin_ctz(bits);
    return 1;
}
#endif
