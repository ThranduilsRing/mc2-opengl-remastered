// The allocation probe and ScopedLeakCheck exercise allocation accounting and standard owners.
#include "doctest.h"
#include "alloc_probe.h"

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

}
