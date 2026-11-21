#include "doctest.h"
#include "../../GameOS/gameos/mech_feed_ready.h"

TEST_CASE("MechFeedReady: empty flush clears both flags") {
    MechFeedReady r;
    r.beginFlush(); r.publish(true);
    CHECK(r.frame); CHECK(r.shadow);
    r.beginFlush(); r.endEmpty();
    CHECK_FALSE(r.frame); CHECK_FALSE(r.shadow);
}

TEST_CASE("MechFeedReady: populated then populated keeps both true") {
    MechFeedReady r;
    r.beginFlush(); r.publish(true);
    r.beginFlush(); r.publish(true);
    CHECK(r.frame); CHECK(r.shadow);
}

TEST_CASE("MechFeedReady: empty then populated, and reset") {
    MechFeedReady r;
    r.beginFlush(); r.endEmpty();
    r.beginFlush(); r.publish(true);
    CHECK(r.frame); CHECK(r.shadow);
    r.reset();
    CHECK_FALSE(r.frame); CHECK_FALSE(r.shadow);
}
