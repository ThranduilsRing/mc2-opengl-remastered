// Review follow-up (B7 proof gaps): GameObject::operator new (gameobj.cpp) is objectZeroNew (code/object_zero_new.h). Against a
// POISONED ::operator new (alloc_probe_new.cpp fills every new block with the poison byte while it is set) the block comes back
// all zero; the negative control shows the poison is really there without the fill. ObjectType::operator new (objtype.cpp) never
// zero-filled, before or after B7o.9 (UserHeap::Malloc is gos_Malloc, no fill), so no zero-fill is asserted for it.
#include "doctest.h"
#include "object_zero_new.h"

#include <cstddef>
#include <initializer_list>

extern unsigned char g_unitNewPoison;	// alloc_probe_new.cpp: nonzero fills each new block with this byte

namespace {
bool allBytes(const void* p, size_t n, unsigned char value)
{
	const auto* b = static_cast<const unsigned char*>(p);
	for (size_t i = 0; i < n; ++i)
		if (b[i] != value)
			return false;
	return true;
}
}

TEST_CASE("objectZeroNew: a poisoned allocation comes back zero-filled (GameObject G4)")
{
	g_unitNewPoison = 0xA5;
	for (size_t size : {size_t(1), size_t(64), size_t(4096), size_t(70000)})
	{
		void* control = ::operator new(size);
		CHECK(allBytes(control, size, 0xA5));
		::operator delete(control);

		void* object = objectZeroNew(size);
		CHECK(allBytes(object, size, 0));
		::operator delete(object);
	}
	g_unitNewPoison = 0;
}
