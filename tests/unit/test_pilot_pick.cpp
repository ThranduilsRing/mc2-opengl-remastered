// getBestPilot out-of-pool path: a pilot that failed the available/in-use filters must never be returned.
#include "doctest.h"
#include <span>
#include "../../code/pilot_pick.h"

namespace {
struct FakePilot { bool available; bool inUse; bool isAvailable() const { return available; } };
FakePilot* pick(FakePilot** v, size_t n)
{
    return pickOutOfPoolPilot<FakePilot>(std::span<FakePilot* const>(v, n), [](FakePilot* p) { return p->inUse; }).value_or(nullptr);
}
}

TEST_CASE("pilot already in a mech is never returned (MP pools are not pre-filtered)")
{
    FakePilot only{ true, true };
    FakePilot* v[1] = { &only };
    CHECK(pick(v, 1) == nullptr);
}

TEST_CASE("dead pilot is never returned")
{
    FakePilot only{ false, false };
    FakePilot* v[1] = { &only };
    CHECK(pick(v, 1) == nullptr);
}

TEST_CASE("empty pool -> nullptr")
{
    CHECK(pick(nullptr, 0) == nullptr);
}

TEST_CASE("mix of in-use, dead and available: first available, not-in-use pilot wins")
{
    FakePilot inMech{ true, true }, dead{ false, false }, deadInMech{ false, true }, ok1{ true, false }, ok2{ true, false };
    FakePilot* v[5] = { &inMech, &dead, &deadInMech, &ok1, &ok2 };
    CHECK(pick(v, 5) == &ok1);
    FakePilot* w[3] = { &inMech, &dead, &deadInMech };
    CHECK(pick(w, 3) == nullptr);
}

TEST_CASE("recovery button hover text: no-pilot id only when the pool is empty")
{
	CHECK(recoveryHelpTextId(true, 45120, 48100) == 48100);
	CHECK(recoveryHelpTextId(false, 45120, 48100) == 45120);
}

TEST_CASE("primary pool filter excludes in-use pilots regardless of MP (no MPlayer bypass)")
{
	FakePilot inMech{ true, true }, dead{ false, false }, ok{ true, false };
	auto inUse = [](FakePilot& p) { return p.inUse; };
	CHECK_FALSE(poolEligible(inMech, inUse));
	CHECK_FALSE(poolEligible(dead, inUse));
	CHECK(poolEligible(ok, inUse));
}

// ---- the real selection (selectPoolPilot / poolHasUnusedPilot: the code getBestPilot and gotPilotsLeft run) ----
#include <deque>
namespace {
struct Pilot {
	int rank; bool available; bool inMech; bool used = false;
	bool isAvailable() const { return available; }
	bool isUsed() const { return used; }
	void setUsed(bool u) { used = u; }
};
auto inMechOf = [](Pilot& p) { return p.inMech; };
auto byRank = [](Pilot* a, Pilot* b) { return a->rank - b->rank; };
Pilot* choose(std::deque<Pilot>& pool) { return selectPoolPilot<Pilot>(pool, inMechOf, byRank); }
}

TEST_CASE("selectPoolPilot: best-ranked eligible pilot wins and is marked used")
{
	std::deque<Pilot> pool{ {1, true, false}, {9, true, false}, {5, true, false} };
	Pilot* p = choose(pool);
	REQUIRE(p != nullptr);
	CHECK(p->rank == 9);
	CHECK(p->used);
}

TEST_CASE("selectPoolPilot: pilots in a mech or unavailable are never chosen")
{
	std::deque<Pilot> pool{ {9, true, true}, {8, false, false}, {2, true, false} };
	Pilot* p = choose(pool);
	REQUIRE(p != nullptr);
	CHECK(p->rank == 2);
}

TEST_CASE("selectPoolPilot: all used -> an eligible pilot is reused; none eligible -> nullptr")
{
	std::deque<Pilot> pool{ {3, true, false, true}, {4, true, false, true} };
	CHECK(choose(pool) != nullptr);
	std::deque<Pilot> none{ {3, true, true}, {4, false, false} };
	CHECK(choose(none) == nullptr);
	std::deque<Pilot> empty;
	CHECK(choose(empty) == nullptr);
}

TEST_CASE("poolHasUnusedPilot agrees with selectPoolPilot (gotPilotsLeft)")
{
	std::deque<Pilot> pool{ {3, true, false}, {4, true, true} };
	CHECK(poolHasUnusedPilot<Pilot>(pool, inMechOf));
	choose(pool);   // hands out the one eligible pilot
	CHECK_FALSE(poolHasUnusedPilot<Pilot>(pool, inMechOf));
	std::deque<Pilot> busy{ {3, true, true} };
	CHECK_FALSE(poolHasUnusedPilot<Pilot>(busy, inMechOf));
}

TEST_CASE("MP shape: two mechs, one pilot -> the second mech gets none (MPlayer is not consulted)")
{
	std::deque<Pilot> pool{ {7, true, false} };
	Pilot* first = choose(pool);
	REQUIRE(first != nullptr);
	first->inMech = true;                 // the first mech is now live and flies that pilot
	CHECK(choose(pool) == nullptr);       // no unused pilot, and the reuse path refuses an in-mech pilot
	CHECK_FALSE(poolHasUnusedPilot<Pilot>(pool, inMechOf));
}
