// getBestPilot out-of-pool path: a pilot that failed the available/in-use filters must never be returned.
#include "doctest.h"
#include <span>
#include "../../code/pilot_pick.h"

namespace { struct FakePilot { bool available; bool inUse; }; }

TEST_CASE("one-pilot pool, pilot already in a mech: filtered list empty -> nullptr")
{
    FakePilot only{ true, true };   // filtered out by warriorInUse
    FakePilot* filtered[1] = { nullptr };
    long count = 0;
    if (only.available && !only.inUse) filtered[count++] = &only;
    CHECK(pickOutOfPoolPilot<FakePilot>(std::span<FakePilot* const>(filtered, count)) == nullptr);
}

TEST_CASE("one-pilot pool, pilot dead: filtered list empty -> nullptr")
{
    FakePilot only{ false, false };
    FakePilot* filtered[1] = { nullptr };
    long count = 0;
    if (only.available && !only.inUse) filtered[count++] = &only;
    CHECK(pickOutOfPoolPilot<FakePilot>(std::span<FakePilot* const>(filtered, count)) == nullptr);
}

TEST_CASE("one filtered pilot (used flag only) is reused")
{
    FakePilot only{ true, false };
    FakePilot* filtered[1] = { &only };
    CHECK(pickOutOfPoolPilot<FakePilot>(std::span<FakePilot* const>(filtered, 1)) == &only);
}
