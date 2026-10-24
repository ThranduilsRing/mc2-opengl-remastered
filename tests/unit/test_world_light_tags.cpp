#include "doctest.h"
#include "../../mclib/spotlight_diag.h"
#include <cstdint>

namespace {
using DWORD = uint32_t;
struct Light {};
using TG_LightPtr = Light*;
constexpr int MAX_LIGHTS_IN_WORLD = 4;
// Compile the production slot methods; only the renderer callback is replaced.
struct CameraSlots {
    TG_LightPtr worldLights[MAX_LIGHTS_IN_WORLD]{};
    long numLights = 0;
    int updates = 0;
    void updateLights() { ++updates; }
#include "camera_world_lights.inc"
};
using namespace mc2_spotlight_diag;
struct Tags {
    Tags() { reset(); }
    ~Tags() { reset(); }
};
}

TEST_CASE("WorldLightTags/slot reuse discards the previous camera's mover tag") {
    Tags cleanup;
    CameraSlots camera;
    Light oldMover, building;
    tag_slot(0, Mech, &oldMover);
    REQUIRE(camera.addWorldLight(&building) == 0);
    CHECK_FALSE(is_e_slot(0, nullptr));
    CHECK(slot_owner(0) == nullptr);
    tag_slot(0, Bldg, &building);
    SourceClass src = Mech;
    REQUIRE(is_e_slot(0, &src));
    CHECK(src == Bldg);
    camera.removeWorldLight(0, &building);
    CHECK_FALSE(is_e_slot(0, nullptr));
    CHECK(camera.updates == 1);
}

TEST_CASE("WorldLightTags/removal follows the light when the saved slot is stale") {
    Tags cleanup;
    CameraSlots camera;
    Light building, mover;
    REQUIRE(camera.addWorldLight(&building) == 0);
    REQUIRE(camera.addWorldLight(&mover) == 1);
    tag_slot(0, Bldg, &building);
    tag_slot(1, Gv, &mover);
    camera.removeWorldLight(0, &mover);
    CHECK(camera.worldLights[0] == &building);
    CHECK(camera.worldLights[1] == nullptr);
    CHECK(slot_owner(0) == &building);
    CHECK_FALSE(is_e_slot(1, nullptr));
    CHECK(camera.updates == 1);
}

TEST_CASE("WorldLightTags/full pool and absent removal preserve live owners") {
    Tags cleanup;
    CameraSlots camera;
    Light lights[MAX_LIGHTS_IN_WORLD + 1];
    for (int i = 0; i < MAX_LIGHTS_IN_WORLD; ++i) {
        REQUIRE(camera.addWorldLight(&lights[i]) == i);
        tag_slot(i, Mech, &lights[i]);
    }
    CHECK(camera.addWorldLight(&lights[MAX_LIGHTS_IN_WORLD]) == -1);
    camera.removeWorldLight(UINT32_MAX, &lights[MAX_LIGHTS_IN_WORLD]);
    for (int i = 0; i < MAX_LIGHTS_IN_WORLD; ++i)
        CHECK(slot_owner(i) == &lights[i]);
    camera.removeWorldLight(UINT32_MAX, &lights[2]);
    CHECK_FALSE(is_e_slot(2, nullptr));
    CHECK(slot_owner(1) == &lights[1]);
}
