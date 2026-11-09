#include "doctest.h"
#include "../../code/spawn_tree_clear.h"
#include <array>

namespace {
struct Object {
    ObjectClass type = TREE;
    struct Position { float x = 0, y = 0, z = 0; } position;
    float radius = 2;
    bool exists = true, tangible = true, pathLocks = true, falling = false;
    int level = 0;
    ObjectClass getObjectClass() const { return type; }
    bool getExists() const { return exists; }
    bool getTangible() const { return tangible; }
    int getMoveLevel() const { return level; }
    Position getPosition() const { return position; }
    float getExtentRadius() const { return radius; }
};
}

TEST_CASE("spawn_tree_clear/initial overlap uses contact once, including adjacent blocks") {
    Object mech; mech.type = BATTLEMECH; mech.radius = 5;
    std::array<Object, 7> objects;
    objects[1].position.x = 6; // Overlap at the edge of the mech footprint.
    objects[2].position.x = 7; // Tangent: stock contact uses strict radius test.
    objects[3].position.y = 20;
    objects[4].type = TERRAINOBJECT;
    objects[5].exists = false;
    objects[6].tangible = false; // Already falling/fallen.
    std::array<Object*, 8> trees{};
    for (size_t i = 0; i < objects.size(); ++i) trees[i] = &objects[i];
    unsigned calls = 0;
    auto contact = [&](Object* tree, Object* collider) {
        CHECK(collider == &mech);
        tree->falling = true;
        tree->tangible = false;
        ++calls;
    };
    CHECK(mc2::clearSpawnTrees(mech, std::span(trees), contact) == 2);
    CHECK(objects[0].falling);
    CHECK(objects[1].falling);
    CHECK_FALSE(objects[2].falling);
    CHECK_FALSE(objects[4].falling);
    CHECK(mc2::clearSpawnTrees(mech, std::span(trees), contact) == 0);
    CHECK(calls == 2);
}

TEST_CASE("spawn_tree_clear/absent airborne and non-contact movers preserve trees") {
    Object mech; mech.type = BATTLEMECH;
    Object tree;
    std::array<Object*, 1> trees{&tree};
    auto contact = [](auto*, auto*) { FAIL("ineligible mover contacted a tree"); };
    SUBCASE("absent") { mech.exists = false; }
    SUBCASE("intangible") { mech.tangible = false; }
    SUBCASE("airborne") { mech.level = 2; }
    SUBCASE("no path locks") { mech.pathLocks = false; }
    SUBCASE("vehicle") { mech.type = GROUNDVEHICLE; }
    CHECK(mc2::clearSpawnTrees(mech, std::span(trees), contact) == 0);
}
