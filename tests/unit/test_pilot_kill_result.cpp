#include "doctest.h"
#include "../../code/pilot_kill_result.h"
#include <initializer_list>

namespace {
using namespace pilot_kill_result;
enum class Class { other, mech, vehicle };
struct Object {
    Class value;
    Class getObjectClass() const { return value; }
};
Icon classifyObject(Object* object)
{
    return classify(object, Class::mech, Class::vehicle);
}
}

TEST_CASE("pilot kill result/missing and unsupported lookups have no icon or valor credit")
{
    CHECK(classifyObject(nullptr) == Icon::none);
    Object unsupported{Class::other};
    CHECK(classifyObject(&unsupported) == Icon::none);
    CHECK_FALSE(countsForValor(classifyObject(nullptr), false));
    CHECK_FALSE(countsForValor(classifyObject(&unsupported), false));
}

TEST_CASE("pilot kill result/ground mechs air movers and vehicles preserve credit")
{
    Object mech{Class::mech};
    Object vehicle{Class::vehicle};
    CHECK(classifyObject(&mech) == Icon::mech);
    CHECK(countsForValor(classifyObject(&mech), false));
    CHECK_FALSE(countsForValor(classifyObject(&mech), true));
    CHECK(classifyObject(&vehicle) == Icon::vehicle);
    CHECK_FALSE(countsForValor(classifyObject(&vehicle), false));
}

TEST_CASE("pilot kill result/valor thresholds exclude unresolved records")
{
    CHECK(medal(0) == Medal::none);
    CHECK(medal(2) == Medal::none);
    CHECK(medal(3) == Medal::valor);
    CHECK(medal(6) == Medal::valor);
    CHECK(medal(7) == Medal::uncommonValor);
    CHECK(medal(8) == Medal::uncommonValor);
    for (long resolved : {2L, 6L}) {
        const auto afterMissing = resolved + countsForValor(classifyObject(nullptr), false);
        CHECK(afterMissing == resolved);
        CHECK(medal(afterMissing) == medal(resolved));
    }
}
