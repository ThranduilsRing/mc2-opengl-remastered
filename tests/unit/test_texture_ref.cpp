// tests/unit/test_texture_ref.cpp
//
// WHAT: the ownership contract of mc2::BasicTextureRef (mclib/texture_ref.h): one release per adopted reference, none for an empty or
// handed-out one, moves transfer the reference.
// WHY: the owner replaces two hand-called release functions; a double release frees a texture under its other user and a missed one leaks
// it, so each way out of the owner has its own case.

#include "doctest.h"

#include "texture_ref.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace
{
std::vector<std::uint32_t>& released ()
{
	static std::vector<std::uint32_t> log;
	return log;
}

struct RecordRelease
{
	void operator() (std::uint32_t node) const noexcept { released().push_back(node); }
};

using Ref = mc2::BasicTextureRef<RecordRelease>;

struct Fresh
{
	Fresh () { released().clear(); }
};
} // namespace

TEST_CASE("a TextureRef owner releases its node once when it dies")
{
	Fresh fresh;
	{
		Ref ref(7);
		CHECK(ref);
		CHECK(ref.get() == 7);
	}
	REQUIRE(released().size() == 1);
	CHECK(released()[0] == 7);
}

TEST_CASE("an empty TextureRef releases nothing")
{
	Fresh fresh;
	{
		Ref empty;
		CHECK(!empty);
		CHECK(empty.get() == mc2::kNoTextureNode);
		Ref none(mc2::kNoTextureNode);
		CHECK(!none);
	}
	CHECK(released().empty());
}

TEST_CASE("a moved TextureRef hands the reference over and the source releases nothing")
{
	Fresh fresh;
	{
		Ref a(3);
		Ref b(std::move(a));
		CHECK(!a);
		CHECK(b.get() == 3);
	}
	REQUIRE(released().size() == 1);
	CHECK(released()[0] == 3);
}

TEST_CASE("move assignment releases the node the target held")
{
	Fresh fresh;
	{
		Ref a(1);
		Ref b(2);
		b = std::move(a);
		REQUIRE(released().size() == 1);
		CHECK(released()[0] == 2);
		CHECK(b.get() == 1);
		CHECK(!a);
	}
	REQUIRE(released().size() == 2);
	CHECK(released()[1] == 1);
}

TEST_CASE("reset releases the old node and adopts the new one; a self move changes nothing")
{
	Fresh fresh;
	{
		Ref a(4);
		a.reset(5);
		REQUIRE(released().size() == 1);
		CHECK(released()[0] == 4);
		Ref& self = a;
		a = std::move(self);
		CHECK(a.get() == 5);
		CHECK(released().size() == 1);
		a.reset();
		REQUIRE(released().size() == 2);
		CHECK(released()[1] == 5);
		CHECK(!a);
	}
	CHECK(released().size() == 2);
}

TEST_CASE("release() gives the node up without a release call")
{
	Fresh fresh;
	std::uint32_t node = 0;
	{
		Ref a(9);
		node = a.release();
		CHECK(!a);
	}
	CHECK(node == 9);
	CHECK(released().empty());
}
