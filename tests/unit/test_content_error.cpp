#include "doctest.h"
#include "content_error.h"

TEST_CASE("content_error/stock and broken fixture counts") {
    using namespace mc2::content;
    resetMission();
    reportMission("stock_fixture");
    CHECK(count(Kind::Texture) == 0);
    CHECK(count(Kind::ObjectType) == 0);
    error(Kind::Texture, "broken.txm", "invalid size");
    error(Kind::Texture, "broken.txm", "invalid size");
    error(Kind::Shape, "missing.ase", "missing shape");
    error(Kind::MissionData, "pilot.fit", "missing profile");
    CHECK(count(Kind::Texture) == 1);
    CHECK(count(Kind::Shape) == 1);
    CHECK(count(Kind::MissionData) == 1);
    CHECK(count(Kind::Sound) == 0);
    reportMission("broken_fixture");
    resetMission();
    CHECK(count(Kind::Texture) == 0);
    error(Kind::Texture, "broken.txm", "invalid size");
    error(Kind::Texture, "broken.txm", "read error");
    error(Kind::Texture, "other.txm", "invalid size");
    CHECK(count(Kind::Texture) == 3);
    resetMission();
}
