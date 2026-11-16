#include "doctest.h"
#include "../../mclib/sim_overlap.h"
#include "../../mclib/spotlight_diag.h"
#include <cstdint>
#include <memory>
#include <string>

namespace {
// Constructed before the lazy registry: its destructor exercises late camera teardown.
struct LateCameraReset {
    ~LateCameraReset() { mc2_spotlight_diag::reset(this); mc2_spotlight_diag::reset(nullptr); }
} lateCameraReset;
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
using CameraPtr = CameraSlots*;
CameraPtr eye = nullptr;
bool appearanceTypeList = true;
struct PreviewAppearance {
    CameraPtr* observed;
    TG_LightPtr light;
    long slot;
    ~PreviewAppearance() {
        *observed = eye;
        eye->removeWorldLight(static_cast<DWORD>(slot), light);
    }
};
struct SimpleCamera : CameraSlots {
    std::unique_ptr<PreviewAppearance> pObject;
    std::string modelCredit_ = "previous model";
    void releaseObject();
};
// File statics the viewer's animation preview added to simplecamera.cpp (read by releaseObject).
SimpleCamera* animationCamera = nullptr;
struct PreviewAnimationStub {} animationPreview;
#include "camera_release_object.inc"
using namespace mc2_spotlight_diag;
struct Tags {
    Tags() { reset(); }
    ~Tags() { reset(); }
};
}

TEST_CASE("WorldLightTags/slot reuse discards the previous appearance's mover tag") {
    Tags cleanup;
    CameraSlots camera;
    Light oldMover, building;
    tag_slot(&camera, 0, Mech, &oldMover);
    REQUIRE(camera.addWorldLight(&building) == 0);
    CHECK_FALSE(is_e_slot(&camera, 0));
    CHECK(slot_owner(&camera, 0) == nullptr);
    tag_slot(&camera, 0, Bldg, &building);
    const auto src = is_e_slot(&camera, 0);
    REQUIRE(src);
    CHECK(src == Bldg);
    camera.removeWorldLight(0, &building);
    CHECK_FALSE(is_e_slot(&camera, 0));
    CHECK(camera.updates == 1);
}

TEST_CASE("WorldLightTags/removal follows the light when the saved slot is stale") {
    Tags cleanup;
    CameraSlots camera;
    Light building, mover;
    REQUIRE(camera.addWorldLight(&building) == 0);
    REQUIRE(camera.addWorldLight(&mover) == 1);
    tag_slot(&camera, 0, Bldg, &building);
    tag_slot(&camera, 1, Gv, &mover);
    camera.removeWorldLight(0, &mover);
    CHECK(camera.worldLights[0] == &building);
    CHECK(camera.worldLights[1] == nullptr);
    CHECK(slot_owner(&camera, 0) == &building);
    CHECK_FALSE(is_e_slot(&camera, 1));
    CHECK(camera.updates == 1);
}

TEST_CASE("WorldLightTags/full pool and absent removal preserve live owners") {
    Tags cleanup;
    CameraSlots camera;
    Light lights[MAX_LIGHTS_IN_WORLD + 1];
    for (int i = 0; i < MAX_LIGHTS_IN_WORLD; ++i) {
        REQUIRE(camera.addWorldLight(&lights[i]) == i);
        tag_slot(&camera, i, Mech, &lights[i]);
    }
    CHECK(camera.addWorldLight(&lights[MAX_LIGHTS_IN_WORLD]) == -1);
    camera.removeWorldLight(UINT32_MAX, &lights[MAX_LIGHTS_IN_WORLD]);
    for (int i = 0; i < MAX_LIGHTS_IN_WORLD; ++i)
        CHECK(slot_owner(&camera, i) == &lights[i]);
    camera.removeWorldLight(UINT32_MAX, &lights[2]);
    CHECK_FALSE(is_e_slot(&camera, 2));
    CHECK(slot_owner(&camera, 1) == &lights[1]);
}

TEST_CASE("WorldLightTags/preview teardown uses its owner camera and restores the active camera") {
    Tags cleanup;
    CameraSlots mainCamera;
    SimpleCamera preview;
    Light lamp, missionLamp;
    REQUIRE(mainCamera.addWorldLight(&missionLamp) == 0);
    tag_slot(&mainCamera, 0, Gv, &missionLamp);
    const long slot = preview.addWorldLight(&lamp);
    tag_slot(&preview, slot, Mech, &preview);
    CHECK(slot_owner(&mainCamera, 0) == &missionLamp);
    CameraPtr observed = nullptr;
    preview.pObject = std::make_unique<PreviewAppearance>();
    preview.pObject->observed = &observed;
    preview.pObject->light = &lamp;
    preview.pObject->slot = slot;
    eye = &mainCamera;
    preview.releaseObject();
    CHECK(preview.modelCredit_.empty());
    CHECK(observed == &preview);
    CHECK(eye == &mainCamera);
    CHECK(preview.worldLights[slot] == nullptr);
    CHECK_FALSE(is_e_slot(&preview, slot));
    CHECK(mainCamera.worldLights[0] == &missionLamp);
    CHECK(slot_owner(&mainCamera, 0) == &missionLamp);
    const auto src = is_e_slot(&mainCamera, 0);
    REQUIRE(src);
    CHECK(src == Gv);
    REQUIRE(preview.addWorldLight(&lamp) == 0);
    tag_slot(&preview, 0, Mech, &lamp);
    reset(&preview);
    CHECK_FALSE(is_e_slot(&preview, 0));
    CHECK(slot_owner(&mainCamera, 0) == &missionLamp);
    tag_slot(&preview, 0, Mech, &lamp);
    preview.removeWorldLight(0, &lamp);
    CHECK(slot_owner(&mainCamera, 0) == &missionLamp);
    reset(&preview);
    CHECK(slot_owner(&mainCamera, 0) == &missionLamp);
    eye = nullptr;
}
