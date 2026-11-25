#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "mech_texname_derive.h"

#include <string>

namespace {
namespace mt = mech_texname;

TEST_CASE("Imported ORM package selection and loaded texture flags") {
    const auto name = mt::importedOrmName(R"({"materials":{"ao":{"tga":"ao.tga"},"orm":{"tga":"synthetic_orm.tga"},"normal":{"tga":"normal.tga"}}})");
    CHECK(name == "synthetic_orm.tga");
    CHECK(mt::importedOrmName(R"({"materials":{"ao":{"tga":"ao.tga"},"normal":{"tga":"normal.tga"}}})").empty());
    CHECK(mt::importedOrmName(R"({"materials":{"orm":{},"normal":{"tga":"normal.tga"}}})").empty());
    CHECK(mt::importedOrmName(R"({"materials":{},"orm":{"tga":"outside.tga"}})").empty());
    CHECK(mt::importedOrmName(R"({"materials":{"orm":{"tga":""}}})").empty());
    CHECK(mt::importedOrmName(R"({"materials":{"orm":{"tga":null},"normal":{"tga":"normal.tga"}}})").empty());
    for (bool stock : {false, true})
        for (bool loaded : {false, true})
            for (bool declared : {false, true})
                CHECK(mt::useOrm(stock, loaded, declared) == (stock || (loaded && declared)));
    CHECK_FALSE(mt::useOrm(false, true, false)); // Legacy AO stays grayscale.
    CHECK_FALSE(mt::useOrm(false, false, true)); // Missing ORM stays unbound.
}

TEST_CASE("mech_texname_harness/strip_path_and_ext") {
    CHECK((mt::deriveName("textures/MadCat_Body.png", false) == "madcat_body.tga"));
    CHECK((mt::deriveName("C:\\art\\Foo.PNG", false) == "foo.tga"));
    CHECK((mt::deriveName("plain", false) == "plain.tga"));
    return;
}

TEST_CASE("mech_texname_harness/sanitize_nonalnum_to_underscore") {
    CHECK((mt::deriveName("mad cat#1", false) == "mad_cat_1.tga"));
    CHECK((mt::deriveName("a-b_c9", false) == "a-b_c9.tga"));
    return;
}

TEST_CASE("mech_texname_harness/empty_stem_returns_empty") {
    CHECK((mt::deriveName("", false).empty()));
    CHECK((mt::deriveName("dir/.png", false).empty()));
    return;
}

TEST_CASE("mech_texname_harness/alpha_prefix") {
    CHECK((mt::deriveName("leaf", true) == "a_leaf.tga"));
    CHECK((mt::deriveName("leaf", false) == "leaf.tga"));

    CHECK((mt::deriveName("a_card", true) == "a_card.tga"));
    return;
}

TEST_CASE("mech_texname_harness/clamp_fits_256") {
    std::string longstem(400, 'x');
    std::string out = mt::deriveName(longstem, false);
    CHECK((out.size() + 1 <= 256));
    CHECK((out.size() >= 4 && out.substr(out.size() - 4) == ".tga"));

    std::string outA = mt::deriveName(longstem, true);
    CHECK((outA.size() + 1 <= 256));
    CHECK((outA.compare(0, 2, "a_") == 0));
    return;
}

TEST_CASE("mech_texname_harness/dropped_mesh_filter") {
    for (const char* n : {"Mech_dmg", "torso_explode", "BodyBlip", "armINDC", "thing_uix"})
        CHECK((mt::isDroppedMeshName(n)));
    for (const char* n : {"Marauder_Body", "left_arm", "torso"})
        CHECK((!mt::isDroppedMeshName(n)));
    CHECK((!mt::isDroppedMeshName(nullptr)));
    return;
}

TEST_CASE("mech_texname_harness/embedded_index_not_resolved_here") {

    CHECK((mt::deriveName("*0", false) == "_0.tga"));
    return;
}

TEST_CASE("mech_texname_harness/demo_intentional_fail" * doctest::should_fail()) {
    CHECK((mt::deriveName("body", false) == "WRONG.tga"));
    return;
}

}

TEST_CASE("mech texture names consume bounded views") {
    const char raw[] = {'d','/','B','o','d','y','.','p','n','g','!'};
    const std::string_view path(raw, 10);
    CHECK(mech_texname::stripPath(path) == "Body.png");
    CHECK(mech_texname::deriveName(path, false) == "body.tga");
    CHECK(mech_texname::deriveName(std::string_view{}, false).empty());
}
