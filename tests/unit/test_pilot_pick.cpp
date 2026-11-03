// getBestPilot out-of-pool path: a pilot that failed the available/in-use filters must never be returned.
#include "doctest.h"
#include <span>
#include "../../code/pilot_pick.h"

namespace {
struct FakePilot { bool available; bool inUse; bool isAvailable() const { return available; } };
FakePilot* pick(FakePilot** v, size_t n)
{
    return pickOutOfPoolPilot<FakePilot>(std::span<FakePilot* const>(v, n), [](FakePilot* p) { return p->inUse; });
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
