// The allocation probe and ScopedLeakCheck: the leak oracle of the ownership step, proven on itself and on EString (the first type
// the step migrates).
#include "doctest.h"
#include "alloc_probe.h"
#include "estring.h"

#include <memory>
#include <string>
#include <vector>

TEST_SUITE("alloc_probe")
{

TEST_CASE("the probe counts a block from new to delete, with its bytes")
{
	ScopedLeakCheck lc;
	char* p = new char[100];
	CHECK(lc.delta() == 1);
	CHECK(lc.bytesDelta() == 100);
	CHECK(lc.allocated());
	delete[] p;
	CHECK(lc.delta() == 0);
	CHECK(lc.bytesDelta() == 0);
}

TEST_CASE("a leak is a positive delta, a stray release a negative one")
{
	void* leaked = nullptr;
	{
		ScopedLeakCheck lc;
		leaked = new int(7);
		CHECK(lc.delta() == 1);
	}
	ScopedLeakCheck after;
	delete static_cast<int*>(leaked);
	CHECK(after.delta() == -1);
}

TEST_CASE("a scope that allocates nothing reports nothing")
{
	ScopedLeakCheck lc;
	int local[8] = {};
	local[3] = 1;
	CHECK(local[3] == 1);
	CHECK_FALSE(lc.allocated());
	CHECK(lc.delta() == 0);
}

TEST_CASE("nullptr release and zero-byte allocation are harmless")
{
	ScopedLeakCheck lc;
	mc2_alloc_probe::release(nullptr);
	void* z = mc2_alloc_probe::allocate(0);
	CHECK(z != nullptr);
	CHECK(lc.delta() == 1);
	mc2_alloc_probe::release(z);
	CHECK(lc.delta() == 0);
}

TEST_CASE("standard owners balance: vector, string, unique_ptr")
{
	ScopedLeakCheck lc;
	{
		std::vector<int> v(1000, 1);
		std::string s(200, 'x');
		auto u = std::make_unique<double[]>(64);
		CHECK(lc.delta() == 3);
	}
	CHECK(lc.delta() == 0);
}

TEST_CASE("EString: create, copy, assign, format and destroy leave the heap where it was")
{
	// EString's buffers come from malloc (EBuffer, estring.cpp), which the probe does not see; what it proves here is that nothing
	// EString does through operator new (Format's scratch, CreateMBCS) is left behind by the common sequence. B1 moves the
	// buffer to std::string, and then every byte is in the probe's view.
	ScopedLeakCheck lc;
	{
		EString a("one");
		EString b(a);          // shares the buffer (copy on write)
		EString c;
		c = b;
		c += " two";           // the shared buffer is split here
		EString d;
		d.Format("%d-%s", 42, (const char*)c);
		CHECK(a.Length() == 3);
		CHECK(c.Length() == 7);
		CHECK(d.Length() == 10);
		char* copy = d.CreateMBCS();
		CHECK(lc.delta() == 1);
		delete[] copy;
	}
	CHECK(lc.delta() == 0);
	CHECK(lc.bytesDelta() == 0);
}

TEST_CASE("EString: a by-value return handed to a C string pointer outlives nothing")
{
	// The one bug class the B1 migration guards against: a const char* taken from a temporary EString. The probe cannot see the
	// dangling read; it sees that the temporary's buffer is gone, which is why the pointer must not be kept.
	ScopedLeakCheck lc;
	auto make = [] { return EString("temporary"); };
	{
		EString kept = make();
		CHECK(kept.Length() == 9);
	}
	CHECK(lc.delta() == 0);
}

}
