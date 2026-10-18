#pragma once
// GameObject's allocation (G4, B7o.9): the CRT heap, zero-filled. Save files copy whole member arrays of an object and the
// init(false) paths skip pointer writes, so every byte no constructor sets must be defined: zero. Header-only so the unit tier
// proves the fill against a poisoned ::operator new (tests/unit/test_object_zero_new.cpp).
#include <cstddef>
#include <cstring>
#include <new>

inline void* objectZeroNew (size_t bytes)
{
	void* result = ::operator new(bytes);
	if (result)	// the engine's replacement operator new (GameOS) is not proven to throw rather than return null
		std::memset(result, 0, bytes);
	return result;
}
