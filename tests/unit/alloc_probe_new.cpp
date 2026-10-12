// The unit tier's global operator new and delete, routed through the allocation probe so a test can ask whether the code under test
// released everything it took (tests/unit/alloc_probe.h ScopedLeakCheck). Linked only into mc2_tests: the engine replaces operator new
// in GameOS/gameos/gameos.cpp, which this binary does not link.
#include "mclib/alloc_probe.h"

#include <new>

namespace {

void* probeNew(size_t bytes)
{
	void* p = mc2_alloc_probe::allocate(bytes);
	if (!p)
		throw std::bad_alloc();
	return p;
}

} // namespace

void* operator new(size_t bytes) { return probeNew(bytes); }
void* operator new[](size_t bytes) { return probeNew(bytes); }
void* operator new(size_t bytes, const std::nothrow_t&) noexcept { return mc2_alloc_probe::allocate(bytes); }
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept { return mc2_alloc_probe::allocate(bytes); }
void operator delete(void* p) noexcept { mc2_alloc_probe::release(p); }
void operator delete[](void* p) noexcept { mc2_alloc_probe::release(p); }
void operator delete(void* p, size_t) noexcept { mc2_alloc_probe::release(p); }
void operator delete[](void* p, size_t) noexcept { mc2_alloc_probe::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { mc2_alloc_probe::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { mc2_alloc_probe::release(p); }
