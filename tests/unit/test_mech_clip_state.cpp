// tests/unit/test_mech_clip_state.cpp
//
// BT2018-FIRST-CLASS P9: the imported-mech clip state machine (mclib/mech_clip_state.h).
// Pure: gesture ids + measured speed in, clip choice out. Locks the semantics the 1C
// selector got wrong (get-up played the knockdown, hits/falls looped, limps walked, the
// whole jump was the airborne idle) and the stride-rate rule that removes foot sliding.
#include "doctest.h"
#include "mech_clip_state.h"
#include "mech_grounding.h"
#include "mw5_contact_passes.h"
#include <chrono>
#include "foot_grounding.h"
#include "mech_model_delta.h"
#include "unit_anim_state.h"
#include "mech_skel_import.h"
#include "save_archive.h"
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <filesystem>
#include <map>
#include "mc2fmt.h"
#include "mech_import_scale.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <regex>
#include <sstream>

namespace imported_scale_test {
struct ActorAnimState {
    std::vector<mc2skel::GpuBone> lastGlobals;
    float gpuLift = 0;
};
struct ImportedAnimEntry {
    const void* multi = nullptr;
    std::map<std::string, std::string> nodeManifest;
    std::map<std::string, aiVector3D> socketOffsets;
    std::vector<std::string> names;
    std::map<const void*, ActorAnimState> actors;
    ActorAnimState cpuState;
    aiMatrix4x4 AS1, simAS1;
    float restLift = 0, simRestLift = 0;
};
static std::vector<ImportedAnimEntry> g_importedAnims;
namespace mc2mechanim {
bool GetImportedNodeWorld(const void*, const void*, const char*, const float*, float*, bool = false);
}
#include "imported_node.inc"

struct Appearance {
    struct { bool ready = true; } pose;
    decltype(pose)* simPose = &pose;
    mc2::sim::UnitAnimState anim;
    glm::vec3 socket{};
    void settleSimPose() {}
    long getLowestWeaponNode() { return 0; }
    glm::vec3 getWeaponNodePosition(long) { return socket; }
    auto simulationAnimation() { return &anim; }
};
struct Terrain { float getTerrainElevation(glm::vec3) { return 7; } };
struct Mover {
    Appearance* appearance;
    Terrain* land;
    glm::vec3 position{12, 23, 7};
    bool isMech() { return true; }
    glm::vec3 getLOSPosition();
};
#include "imported_los.inc"
}

TEST_CASE("clip state: imported calibration preserves simulation sockets and LOS") {
    using namespace imported_scale_test;
    const char* path = std::getenv("BT2018_TEST_CHASSIS");
    if (!path) { WARN("set BT2018_TEST_CHASSIS for the imported chassis oracle"); return; }
    Assimp::Importer importer;
    const auto* scene = importer.ReadFile(path, 0);
    REQUIRE(scene != nullptr);
    const auto plan = mc2skel::BuildPlan(scene, {"j_RHand"});
    REQUIRE(plan != nullptr);
    g_importedAnims.assign(1, {});
    auto& e = g_importedAnims.front();
    e.multi = &e;
    e.names = {"j_RHand"};
    e.nodeManifest["weapon_rightarm"] = "j_RHand";
    e.socketOffsets["weapon_rightarm"] = {0.2f, 0.3f, 0.4f};
    e.simAS1.a1 = e.simAS1.b2 = e.simAS1.c3 = 10;
    e.simAS1.b4 = 2;
    e.simRestLift = 3;
    const float root[12] = {0, 0, 1, -12, 0, 1, 0, 7, -1, 0, 0, 23};
    Appearance appearance;
    Terrain terrain;
    Mover mover{&appearance, &terrain};
    for (float frame : {0.0f, 7.0f, 19.0f}) {
        REQUIRE(mc2skel::EvaluateClipGpuBones(*plan, "atlas_moveCoreWalkFwd", frame, e.cpuState.lastGlobals));
        float baseline[3], scaled[3], rendered[3];
        e.AS1 = e.simAS1;
        e.restLift = e.simRestLift;
        REQUIRE(imported_scale_test::mc2mechanim::GetImportedNodeWorld(nullptr, &e, "weapon_rightarm", root, baseline));
        appearance.socket = {baseline[0], baseline[1], baseline[2]};
        const auto los = mover.getLOSPosition();
        for (float scale : {0.6f, 1.8f}) {
            e.AS1 = e.simAS1;
            e.AS1.a1 *= scale; e.AS1.b2 *= scale; e.AS1.c3 *= scale; e.AS1.b4 *= scale;
            e.restLift = e.simRestLift * scale;
            REQUIRE(imported_scale_test::mc2mechanim::GetImportedNodeWorld(nullptr, &e, "weapon_rightarm", root, scaled));
            REQUIRE(imported_scale_test::mc2mechanim::GetImportedNodeWorld(nullptr, &e, "weapon_rightarm", root, rendered, true));
            for (int k = 0; k < 3; ++k) CHECK(scaled[k] == baseline[k]);
            CHECK(rendered[2] != baseline[2]);
            appearance.socket = {scaled[0], scaled[1], scaled[2]};
            CHECK(mover.getLOSPosition() == los);
        }
    }
    g_importedAnims.clear();
}

using namespace mc2clip;

TEST_CASE("clip state: repair bay stock gesture sequence powers down imported chassis") {
    // mstates.h Walk -> Park is 2, 5, 0; Park -> Stand is 1, 2.
    for (bool authoredShutdown : {false, true}) {
        Phase ph;
        const auto has = [=](std::string_view name) {
            return name == "atlas_moveCoreIdle" || (authoredShutdown &&
                (name == "atlas_shutdownPwroff" || name == "atlas_shutdownIdle" ||
                 name == "atlas_shutdownPwron"));
        };
        for (int gesture : {2, 5, 0, 1, 2, 13}) {
            const auto choice = select(gesture, 0, 0, 0, false, false, "atlas_moveCoreWalkFwd", ph);
            const auto clip = resolveClip(choice.clip, "atlas_moveCoreWalkFwd", has);
            CHECK_FALSE(choice.locomotion);
            CHECK(choice.immediate);
            CHECK(clip == (authoredShutdown && gesture == 5 ? "atlas_shutdownPwroff" :
                           authoredShutdown && gesture == 0 ? "atlas_shutdownIdle" :
                           authoredShutdown && gesture == 1 ? "atlas_shutdownPwron" : "atlas_moveCoreIdle"));
        }
    }
}

TEST_CASE("clip state: distance matching inverts nonlinear travel and wraps cycles") {
    const DistanceCurve c{2, {0, 1, 4}};
    REQUIRE(c.valid());
    CHECK(c.advance(0, .5f) == doctest::Approx(.5f));
    CHECK(c.advance(.5f, 2) == doctest::Approx(1.5f));
    CHECK(c.advance(1.5f, 10) == doctest::Approx(.5f));
    CHECK(c.advance(.75f, 0) == .75f);
    CHECK_FALSE(DistanceCurve{2, {0, 1, 1}}.valid());
    CHECK_FALSE(DistanceCurve{0, {0, 1}}.valid());
}

TEST_CASE("clip state: stopped locomotion gestures choose idle immediately") {
    for (int gesture : {4, 7, 9, 11, 12}) {
        Phase ph;
        const auto c = select(gesture, 0, 0, 0, false, false, "atlas_moveCoreWalkFwd", ph);
        CHECK(std::string(c.clip) == "atlas_moveCoreIdle");
        CHECK_FALSE(c.locomotion);
        CHECK(c.immediate);
    }
    CHECK(resolveClip("atlas_shutdownPwroff", "atlas_moveCoreWalkFwd",
        [](std::string_view c) { return c == "atlas_moveCoreIdle"; }) == "atlas_moveCoreIdle");
}

TEST_CASE("clip grounding: package roles override naming and preserve legacy packs") {
    const std::map<std::string, std::string> roles{{"mw5_idle", "feet"}, {"mw5_walk", "feet"},
        {"mw5_death", "body"}, {"atlas_moveCoreIdle", "none"}};
    CHECK(groundingRole(roles, "mw5_idle") == "feet");
    CHECK(groundingRole(roles, "mw5_walk") == "feet");
    CHECK(groundingRole(roles, "mw5_death") == "body");
    CHECK(groundingRole(roles, "atlas_moveCoreIdle") == "none");
    CHECK(groundingRole(roles, "atlas_moveCoreWalkFwd") == "none");
    CHECK(groundingRole({}, "atlas_moveCoreWalkFwd") == "feet");
    CHECK(groundingRole({}, "atlas_deathIdle") == "body");
    CHECK(groundingRole({}, "atlas_moveJumpUpIdle") == "none");
}

TEST_CASE("clip grounding: a walk on a rig the leg solver cannot resolve follows the pose (MW5 in-game float)") {
    CHECK(liftFollowsPose("feet", false));    // MW5 Mad Cat: no j_L/R Thigh-Calf-Foot bones, solver is a no-op
    CHECK_FALSE(liftFollowsPose("feet", true));   // BT rig: the solver grounds each foot, rest lift stays
    CHECK(liftFollowsPose("body", true));
    CHECK_FALSE(liftFollowsPose("none", false));
}

// Trigger: interrupted/restored fades in assimp_importer.cpp and mech_skel_import.cpp.
TEST_CASE("clip state: interrupted local fade and missing outgoing restore publish immediately") {
    aiScene scene;
    scene.mRootNode = new aiNode("root");
    scene.mRootNode->mNumChildren = 1;
    scene.mRootNode->mChildren = new aiNode*[1]{new aiNode("tip")};
    auto* tip = scene.mRootNode->mChildren[0];
    tip->mParent = scene.mRootNode; tip->mTransformation.a4 = 2;
    scene.mNumAnimations = 3;
    scene.mAnimations = new aiAnimation*[3];
    for (unsigned i = 0; i < 3; ++i) {
        auto* clip = scene.mAnimations[i] = new aiAnimation;
        clip->mName = i == 0 ? "A" : i == 1 ? "B" : "C";
        clip->mDuration = 1; clip->mTicksPerSecond = 1;
        clip->mNumChannels = 1; clip->mChannels = new aiNodeAnim*[1]{new aiNodeAnim};
        auto* ch = clip->mChannels[0]; ch->mNodeName = "root";
        ch->mNumRotationKeys = 1;
        ch->mRotationKeys = new aiQuatKey[1]{{0, aiQuaternion(aiVector3D(0, 1, 0), i * 0.8f)}};
    }
    const auto plan = mc2skel::BuildPlan(&scene, {"root", "tip"});
    mc2::sim::ImportedAnimState state;
    state.activeClip = "B"; state.previousClip = "A"; state.fadeTimeSec = 0.075f;
    const mc2skel::LocalRotation twist{0, {0, 1, 0}, 20};
    std::vector<mc2skel::GpuBone> displayed, pose;
    REQUIRE(mc2skel::EvaluateActorClipGpuBones(*plan, state, displayed, &twist));
    SUBCASE("interrupt at 50 percent, save/load, then interrupt again") {
        mc2skel::BeginActorClipFade(*plan, state); state.activeClip = "C";
        REQUIRE_FALSE(state.fadeSourceLocals.empty());
        for (unsigned version : {2u, 3u, 4u}) {
            mc2::save::Archive legacyWrite; legacyWrite.sectionVersion = version; legacyWrite(state);
            auto legacy = state;
            mc2::save::Archive legacyRead(legacyWrite.bytes()); legacyRead.sectionVersion = version;
            legacyRead(legacy); REQUIRE(legacyRead.finish()); CHECK(legacy.fadeSourceLocals.empty());
        }
        for (int interruption = 0; interruption < 2; ++interruption) {
            mc2::save::Archive write; write(state); REQUIRE(write.good());
            mc2::sim::ImportedAnimState restored;
            mc2::save::Archive read(write.bytes()); read(restored); REQUIRE(read.finish());
            CHECK(restored.fadeSourceLocals == state.fadeSourceLocals);
            REQUIRE(mc2skel::EvaluateActorClipGpuBones(*plan, restored, pose, &twist));
            for (size_t b = 0; b < pose.size(); ++b)
                for (int k = 0; k < 16; ++k)
                    CHECK(pose[b].m[k] == doctest::Approx(displayed[b].m[k]).epsilon(1e-5));
            state = restored; state.fadeTimeSec = 0.075f;
            REQUIRE(mc2skel::EvaluateActorClipGpuBones(*plan, state, displayed, &twist));
            mc2skel::BeginActorClipFade(*plan, state); state.activeClip = "A";
        }
    }
    SUBCASE("missing outgoing clip on first paused evaluation") {
        state.previousClip = "missing";
        mc2::save::Archive write; write(state);
        mc2::save::Archive read(write.bytes()); read(state); REQUIRE(read.finish());
        REQUIRE(mc2skel::EvaluateActorClipGpuBones(*plan, state, pose, &twist));
        CHECK(state.previousClip.empty()); CHECK(state.fadeSourceLocals.empty());
        REQUIRE(mc2skel::EvaluateClipGpuBones(*plan, "B", 0, displayed, nullptr, nullptr, &twist));
        for (size_t b = 0; b < pose.size(); ++b)
            for (int k = 0; k < 16; ++k) CHECK(pose[b].m[k] == displayed[b].m[k]);
    }
}

// Trigger: imported preview pose publication in mclib/assimp_importer.cpp.
TEST_CASE("preview publication: CPU geometry changes across ticks; GPU stays on palette path") {
    glm::vec4 published(0), vertex(1, 2, 3, 1);
    glm::mat4 sampled(1);
    int cpu = 0, gpu = 0;
    const auto tick = [&](bool preview, bool gpuMode, float x) {
        sampled[3].x = x;
        publishPreviewPose(preview, gpuMode, [&] { ++gpu; }, [&] { published = sampled * vertex; ++cpu; });
    };
    tick(true, false, 1); const auto first = published;
    tick(true, false, 2); CHECK(published.x == first.x + 1); CHECK(cpu == 2); CHECK(gpu == 0);
    tick(true, true, 3); tick(false, false, 4);
    CHECK(published.x == first.x + 1); CHECK(cpu == 2); CHECK(gpu == 1);
}

TEST_CASE("preview resolver: missing binding retains the mission active clip") {
    const auto has = [](std::string_view clip) { return clip == "atlas_moveCoreIdle" || clip == "atlas_moveCoreWalkFwd"; };
    Phase phase;
    const auto walk = select(kGestureWalk, 10, 0, 0, false, false, "atlas_moveCoreIdle", phase);
    CHECK(resolveClip(walk.clip, "atlas_moveCoreIdle", has) == "atlas_moveCoreWalkFwd");
    const auto death = select(kGestureFallForward, 0, 0, 0, true, false, "atlas_moveCoreIdle", phase);
    CHECK(resolveClip(death.clip, "atlas_moveCoreIdle", has) == "atlas_moveCoreIdle");
    CHECK(resolveClip("", "atlas_moveCoreIdle", has) == "atlas_moveCoreIdle");
}

TEST_CASE("BT2018 standing: authored moving idle binds in mission and preview; MW5 keeps Stand") {
    const auto bt = [](std::string_view s) {
        return s == "atlas_moveCoreIdle" || s == "atlas_moveCoreIdleFlavor1";
    };
    Phase ph;
    for (int gesture : {2, 13}) { // stand and idle
        const auto choice = select(gesture, 0, 0, 0, false, false, "atlas_moveCoreIdle", ph);
        CHECK(standingClip(choice.clip, bt) == std::string_view("atlas_moveCoreIdleFlavor1"));
        CHECK(choice.mode == Mode::Loop);
        CHECK_FALSE(choice.locomotion);
    }
    CHECK(previewClip("atlas_moveCoreIdle", "", bt) == "atlas_moveCoreIdleFlavor1");
    CHECK(previewClip("", "", bt) == "atlas_moveCoreIdleFlavor1");
    CHECK(standingClip("atlas_shutdownIdle", bt) == std::string_view("atlas_shutdownIdle"));
    CHECK(standingClip("atlas_moveCoreWalkFwd", bt) == std::string_view("atlas_moveCoreWalkFwd"));
    const auto mw5 = [](std::string_view s) { return s == "atlas_moveCoreIdle"; };
    CHECK(standingClip("atlas_moveCoreIdle", mw5) == std::string_view("atlas_moveCoreIdle"));
    CHECK(previewClip("atlas_moveCoreIdle", "", mw5) == "atlas_moveCoreIdle");
}

TEST_CASE("preview showcase: manifest, default walk, idle, first clip fallback (never the park gesture's shutdown pose)") {
    const auto has = [](std::string_view s) { return s == "show" || s == "atlas_shutdownIdle" || s == "atlas_moveCoreWalkFwd" || s == "atlas_moveCoreIdle" || s == "first"; };
    CHECK(previewClip("show", "first", has) == "show");
    CHECK(previewClip("", "first", has) == "atlas_moveCoreIdle");   // pack without clips.showcase: upright idle, not the forward-pitched walk
    CHECK(previewClip("unknown", "first", has) == "atlas_moveCoreIdle");
    CHECK(previewClip("", "first", [](auto s) { return s == "atlas_moveCoreWalkFwd" || s == "atlas_shutdownIdle"; }) == "atlas_moveCoreWalkFwd");
    CHECK(previewClip("", "first", [](auto s) { return s == "first"; }) == "first");
}

TEST_CASE("imported palette: restored clip with no presentation storage") {
    struct Entry : mc2::sim::ImportedAnimState { std::vector<float> modelDelta; };
    mc2::sim::ImportedAnimState saved;
    saved.activeClip = "atlas_moveCoreWalkFwd";
    saved.clipTimeSec = 0.5f;
    Entry actor;
    static_cast<mc2::sim::ImportedAnimState&>(actor) = saved;
    REQUIRE(actor.modelDelta.empty());
    Entry draw = actor; // CopyImportedActorPose rebuilds the draw palette.
    mc2mechanim::sizeModelDelta(draw.modelDelta, 3);
    REQUIRE(draw.modelDelta.size() == 48);
    draw.modelDelta.at(47) = 7.0f; // last matrix is writable after restore
    mc2mechanim::sizeModelDelta(draw.modelDelta, 3);
    CHECK(draw.modelDelta.at(47) == 7.0f);
    CHECK(draw.activeClip == saved.activeClip);
    CHECK(draw.clipTimeSec == saved.clipTimeSec);
    mc2mechanim::sizeModelDelta(draw.modelDelta, 4);
    CHECK(draw.modelDelta.at(63) == 0.0f);
    mc2mechanim::sizeModelDelta(draw.modelDelta, 1);
    CHECK(draw.modelDelta.size() == 16);
    mc2mechanim::sizeModelDelta(draw.modelDelta, 0);
    CHECK(draw.modelDelta.empty());
}

TEST_CASE("imported grounding: soles contact, reverse knees and torso motion survive") {
    mc2ground::Rig rig;
    std::vector<glm::mat4> pose(7, glm::mat4(1));
    for (int side = 0; side < 2; ++side) {
        auto& leg = rig.legs[side];
        leg.thigh = side * 3; leg.calf = side * 3 + 1; leg.foot = side * 3 + 2;
        const float x = side ? 0.3f : -0.3f;
        pose[leg.thigh][3] = glm::vec4(x, 2, 0, 1);
        pose[leg.calf][3] = glm::vec4(x, 1, -0.6f, 1);
        pose[leg.foot] = mc2ground::columnRotation(glm::angleAxis(0.3f, glm::vec3(1, 0, 0)));
        pose[leg.foot][3] = glm::vec4(x, side ? 0.4f : -0.2f, 0, 1);
        for (float px : {-0.15f, 0.15f})
            for (float pz : {-0.3f, 0.3f}) leg.sole.push_back({leg.foot, {px, -0.1f, pz}});
    }
    pose[6] = mc2ground::columnRotation(glm::angleAxis(0.14f, glm::vec3(1, 0, 0)));
    const auto original = pose;
    mc2ground::solve(rig, pose);
    CHECK(pose[6] == original[6]); // authored torso lean, no whole-mech lift or pitch
    for (const auto& leg : rig.legs) {
        float minY = 100;
        for (const auto& p : leg.sole)
            minY = std::min(minY, (pose[p.bone] * glm::vec4(p.local, 1)).y);
        CHECK(minY >= -1e-5f);
        if (leg.foot == 2) CHECK(minY == doctest::Approx(0).epsilon(1e-5));
        for (auto pair : {std::pair{leg.thigh, leg.calf}, std::pair{leg.calf, leg.foot}}) {
            const float before = glm::length(glm::vec3(original[pair.first][3] - original[pair.second][3]));
            const float after = glm::length(glm::vec3(pose[pair.first][3] - pose[pair.second][3]));
            CHECK(after == doctest::Approx(before).epsilon(1e-5));
            const auto localEnd = glm::inverse(original[pair.first]) * original[pair.second][3];
            CHECK(glm::length(pose[pair.first] * localEnd - pose[pair.second][3]) < 1e-5f);
        }
        CHECK(pose[leg.calf][3].z < 0); // retain the source bend plane
    }
    std::array<glm::vec3, 2> targets{glm::vec3(pose[2][3]), glm::vec3(pose[5][3])};
    targets[0] += glm::vec3(.05f, .15f, .05f);
    auto locked = original;
    mc2ground::solve(rig, locked, nullptr, &targets);
    CHECK(glm::length(glm::vec3(locked[2][3]) - targets[0]) < 1e-5f);
    auto repeated = original;
    mc2ground::solve(rig, repeated, nullptr, &targets);
    CHECK(repeated == locked); // no dependency on an earlier frame's solve
    const std::array<glm::quat, 2> rotations{mc2ground::columnQuaternion(locked[2]),
                                           mc2ground::columnQuaternion(locked[5])};
    const float lift[2] = {.1f, 0};
    repeated = original;
    mc2ground::solve(rig, repeated, lift, &targets, &rotations);
    CHECK(glm::length(glm::vec3(repeated[2][3]) - targets[0] - glm::vec3(0, .1f, 0)) < 1e-5f);
    targets[0].x += 100;
    mc2ground::solve(rig, repeated, nullptr, &targets);
    const float reach = glm::length(glm::vec3(original[1][3] - original[0][3])) +
                        glm::length(glm::vec3(original[2][3] - original[1][3]));
    CHECK(glm::length(glm::vec3(repeated[2][3] - repeated[0][3])) <= reach);
}

TEST_CASE("imported grounding: a toe-down swing foot keeps its sole height and knee side") {
    mc2ground::Rig rig;
    std::vector<glm::mat4> pose(6, glm::mat4(1));
    const glm::vec3 knees[2] = {{-0.3f, 1, -0.6f}, {0.3f, 1.2f, -0.7f}};
    const glm::vec3 ankles[2] = {{-0.3f, 0.2f, 0}, {0.3f, 0.9f, 0.3f}};
    for (int side = 0; side < 2; ++side) {
        auto& leg = rig.legs[side];
        leg.thigh = side * 3; leg.calf = side * 3 + 1; leg.foot = side * 3 + 2;
        pose[leg.thigh][3] = glm::vec4(side ? 0.3f : -0.3f, 2, 0, 1);
        pose[leg.calf][3] = glm::vec4(knees[side], 1);
        if (side) pose[leg.foot] = mc2ground::columnRotation(glm::angleAxis(1.2f, glm::vec3(1, 0, 0)));
        pose[leg.foot][3] = glm::vec4(ankles[side], 1);
        for (float px : {-0.15f, 0.15f})
            for (float pz : {-0.3f, 0.3f}) leg.sole.push_back({leg.foot, {px, -0.1f, pz}});
    }
    auto sole = [&](const auto& leg, const std::vector<glm::mat4>& p) {
        float y = 100;
        for (const auto& s : leg.sole) y = std::min(y, (p[s.bone] * glm::vec4(s.local, 1)).y);
        return y;
    };
    auto plane = [](const std::vector<glm::mat4>& p, const mc2ground::Leg& leg) {
        const glm::vec3 h(p[leg.thigh][3]), k(p[leg.calf][3]), a(p[leg.foot][3]);
        return glm::cross(k - h, a - k);
    };
    const auto original = pose;
    const float authoredSwing = sole(rig.legs[1], original) - sole(rig.legs[0], original);
    mc2ground::solve(rig, pose);
    CHECK(sole(rig.legs[0], pose) == doctest::Approx(0).epsilon(1e-4));
    CHECK(sole(rig.legs[1], pose) == doctest::Approx(authoredSwing).epsilon(1e-4));
    for (const auto& leg : rig.legs) CHECK(glm::dot(plane(original, leg), plane(pose, leg)) > 0);
}

TEST_CASE("imported grounding: each leg rides above the shared floor as far as its foot's surface and reach allow") {
    mc2ground::Rig rig;
    std::vector<glm::mat4> pose(7, glm::mat4(1));
    for (int side = 0; side < 2; ++side) {
        auto& leg = rig.legs[side];
        leg.thigh = side * 3; leg.calf = side * 3 + 1; leg.foot = side * 3 + 2;
        const float x = side ? 0.3f : -0.3f;
        pose[leg.thigh][3] = glm::vec4(x, 2, 0, 1);
        pose[leg.calf][3] = glm::vec4(x, 1, -0.6f, 1);
        pose[leg.foot] = mc2ground::columnRotation(glm::angleAxis(0.3f, glm::vec3(1, 0, 0)));
        pose[leg.foot][3] = glm::vec4(x, 0.2f, 0, 1);
        for (float px : {-0.15f, 0.15f})
            for (float pz : {-0.3f, 0.3f}) leg.sole.push_back({leg.foot, {px, -0.1f, pz}});
    }
    const auto original = pose;
    auto sole = [&](int side, const std::vector<glm::mat4>& p) {
        float y = 100;
        for (const auto& s : rig.legs[side].sole) y = std::min(y, (p[s.bone] * glm::vec4(s.local, 1)).y);
        return y;
    };
    const float reach = (glm::length(glm::vec3(original[1][3] - original[0][3])) + glm::length(glm::vec3(original[2][3] - original[1][3]))) * 0.99999f;
    mc2ground::solve(rig, pose, nullptr);
    const auto shared = sole(1, pose);
    const auto kneeSide = [](glm::vec3 hip, glm::vec3 knee, glm::vec3 ankle) {
        return glm::dot(knee - hip, glm::cross(glm::normalize(ankle - hip), glm::vec3(1, 0, 0)));
    };
    SUBCASE("a leg lifts by its own amount; the other stays on the shared floor") {
        const float legLift[2] = {-0.2f, 0.5f};
        mc2ground::solve(rig, pose, legLift);
        CHECK(sole(0, pose) == doctest::Approx(shared - 0.2f).epsilon(1e-4));
        CHECK(sole(1, pose) == doctest::Approx(shared + 0.5f).epsilon(1e-4));
        CHECK(glm::length(glm::vec3(pose[3][3] - pose[4][3])) == doctest::Approx(glm::length(glm::vec3(original[3][3] - original[4][3]))).epsilon(1e-5));
        CHECK(kneeSide(original[3][3], original[4][3], original[5][3]) * kneeSide(pose[3][3], pose[4][3], pose[5][3]) > 0); // authored knee side
    }
    SUBCASE("an unreachable lift stops at full extension instead of dropping the authored pose") {
        const float legLift[2] = {0.0f, 50.0f};
        mc2ground::solve(rig, pose, legLift);
        CHECK(sole(0, pose) == doctest::Approx(shared).epsilon(1e-4));
        CHECK(sole(1, pose) > shared + 0.5f);
        CHECK(glm::length(glm::vec3(pose[5][3] - pose[3][3])) == doctest::Approx(reach).epsilon(1e-4));
    }
}

TEST_CASE("foot grounding: contacts follow the surface within reach; a resting body moves only for its most demanding contact") {
    using namespace mc2foot;
    CHECK(follow(3.0f, 2.0f) == 2.0f);
    CHECK(follow(-3.0f, 2.0f) == -2.0f);
    CHECK(follow(1.0f, 2.0f) == 1.0f);
    CHECK(reach(0.0f) == kMinReach);
    CHECK(reach(7.0f) == 7.0f);
    const float zero[3] = {};
    CHECK(restLift(zero, zero) == 0.0f);            // surfaces agree: byte-identical lift
    const float under[1] = {-4.0f}, resting[1] = {0.0f};
    CHECK(restLift(under, resting) == -4.0f);       // root over a bump: the body lowers onto the ground under its low point
    const float offset[3] = {-4.0f, 3.0f, 0.5f}, height[3] = {0.0f, 1.0f, 2.5f};
    CHECK(restLift(offset, height) == 2.0f);        // only the bump under the point 1 above the resting one penetrates (3 - 1)
    const float high[2] = {0.0f, 3.0f}, higher[2] = {0.0f, 9.0f};
    CHECK(restLift(high, higher) == 0.0f);          // a high contact over a bump stays clear: no lift
    CHECK(restLift({}, {}) == 0.0f);
}

TEST_CASE("imported grounding: a stock rig without imported soles stays byte-identical") {
    std::vector<glm::mat4> pose{glm::mat4(2)};
    const auto original = pose;
    mc2ground::solve({}, pose);
    CHECK(pose == original);
}

namespace {
Choice pick(int gesture, Phase& ph, float speed = 0.0f, bool destroyed = false, bool finished = false,
            const char* active = "atlas_moveCoreIdle") {
    return select(gesture, speed, 0.0f, 0, destroyed, finished, active, ph);
}
}

TEST_CASE("clip state: get-up plays the get-up clip once, not the knockdown") {
    Phase ph;
    const Choice c = pick(kGestureGetUp, ph);
    CHECK(std::string(c.clip) == "atlas_downedGetup");
    CHECK(c.mode == Mode::Once);
    CHECK(c.immediate);
    CHECK(std::string(pick(kGestureRollover, ph).clip) == "atlas_downedGetup");
}

TEST_CASE("clip state: falls, hits and deaths are one-shot; prone holds the downed or death idle") {
    Phase ph;
    CHECK(pick(kGestureFallForward, ph).mode == Mode::Once);
    CHECK(std::string(pick(kGestureFallForward, ph).clip) == "atlas_hitReactKnockdownInpl");
    CHECK(std::string(pick(kGestureFallBackward, ph, 0, true).clip) == "atlas_deathKnockdown");
    CHECK(pick(kGestureHitLeft, ph).mode == Mode::Once);
    CHECK(std::string(pick(kGestureHitLeft, ph).clip) == "atlas_hitReactLgtLeft");
    CHECK(std::string(pick(kGestureFallenForward, ph).clip) == "atlas_downedIdle");
    CHECK(std::string(pick(kGestureFallenBackward, ph, 0, true).clip) == "atlas_deathIdle");
}

TEST_CASE("clip state: limps use the directional limp clips with stride matching") {
    Phase ph;
    Choice l = pick(kGestureLimpLeft, ph, 6.0f);
    CHECK(std::string(l.clip) == "atlas_moveCoreWalkLimpFwdLeft");
    CHECK(l.locomotion);
    CHECK(std::string(pick(kGestureLimpRight, ph, 6.0f).clip) == "atlas_moveCoreWalkLimpFwdRight");
}

TEST_CASE("clip state: a jump is start -> airborne idle -> land") {
    Phase ph;
    pick(kGestureWalk, ph, 10.0f);
    Choice c = pick(kGestureJump, ph);
    CHECK(std::string(c.clip) == "atlas_moveJumpUpStart");
    CHECK(c.mode == Mode::Once);
    c = pick(kGestureJump, ph, 0, false, false, "atlas_moveJumpUpStart");     // start still playing
    CHECK(std::string(c.clip) == "atlas_moveJumpUpStart");
    c = pick(kGestureJump, ph, 0, false, true, "atlas_moveJumpUpStart");      // start finished
    CHECK(std::string(c.clip) == "atlas_moveJumpUpIdle");
    CHECK(c.mode == Mode::Loop);
    c = pick(2 /*Stand*/, ph, 0, false, false, "atlas_moveJumpUpIdle");        // touched down
    CHECK(std::string(c.clip) == "atlas_moveJumpDownLand");
    CHECK(c.mode == Mode::Once);
    c = pick(2, ph, 0, false, false, "atlas_moveJumpDownLand");                // landing plays out
    CHECK(std::string(c.clip) == "atlas_moveJumpDownLand");
    c = pick(2, ph, 0, false, true, "atlas_moveJumpDownLand");                 // then locomotion
    CHECK(std::string(c.clip) == "atlas_moveCoreIdle");
}

TEST_CASE("clip state: locomotion picks walk/run/reverse and turn-in-place") {
    Phase ph;
    CHECK(std::string(pick(kGestureWalk, ph, 10.0f).clip) == "atlas_moveCoreWalkFwd");
    CHECK(std::string(pick(kGestureRun, ph, 20.0f).clip) == "atlas_moveCoreRunFwd");
    CHECK(std::string(pick(kGestureReverse, ph, 8.0f).clip) == "atlas_moveCoreWalkBwd");
    CHECK_FALSE(pick(kGestureWalk, ph, 10.0f).immediate);
    CHECK(std::string(select(2, 0.0f, 30.0f, 3, false, false, "atlas_moveCoreIdle", ph).clip) == "atlas_moveCoreTurnLeftIdle");
}

TEST_CASE("clip state: stride matching removes planted-foot drift across speeds") {
    const float clipStride = 9.5f;   // planted-foot speed of the walk clip at rate 1
    for (float speed : {4.0f, 8.0f, 9.5f, 12.0f, 16.0f}) {
        CHECK(footDrift(speed, clipStride) == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(strideRate(speed, clipStride) == doctest::Approx(speed / clipStride));
    }
    // Unmatched playback (rate 1) would slide by |speed - stride|; the clamp bounds extremes.
    CHECK(strideRate(0.0f, clipStride) == doctest::Approx(kStrideRateMin));
    CHECK(strideRate(1000.0f, clipStride) == doctest::Approx(kStrideRateMax));
    // Light-chassis run: 155 u/s ground speed on a 60 u/s clip is 2.6x and must stay planted (was clamped at 2x).
    CHECK(footDrift(155.0f, 60.0f) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(strideRate(5.0f, 0.0f) == doctest::Approx(1.0f));
}

TEST_CASE("clip state: park and power transitions have their own clips") {
    Phase ph;
    CHECK(std::string(pick(kGesturePark, ph).clip) == "atlas_shutdownIdle");
    CHECK(pick(kGesturePark, ph).mode == Mode::Loop);
    CHECK(std::string(pick(kGestureStandToPark, ph).clip) == "atlas_shutdownPwroff");
    CHECK(std::string(pick(kGestureParkToStand, ph).clip) == "atlas_shutdownPwron");
    CHECK(pick(kGestureStandToPark, ph).mode == Mode::Once);
    CHECK(pick(kGestureParkToStand, ph).mode == Mode::Once);
}

TEST_CASE("clip state: destroyed non-fall gestures hold death idle") {
    for (int gesture = 0; gesture <= 24; ++gesture) {
        if (gesture == kGestureFallForward || gesture == kGestureFallBackward) continue;
        Phase ph;
        const auto c = pick(gesture, ph, 20, true);
        CHECK(std::string(c.clip) == "atlas_deathIdle");
        CHECK(c.mode == Mode::Loop);
        CHECK(c.immediate);
    }
}

TEST_CASE("clip state: landing yields immediately above one unit per second") {
    Phase ph;
    pick(kGestureJump, ph);
    CHECK(std::string(pick(2, ph, 1).clip) == "atlas_moveJumpDownLand");
    CHECK(std::string(pick(kGestureRun, ph, 1.01f, false, false, "atlas_moveJumpDownLand").clip) == "atlas_moveCoreRunFwd");
    CHECK_FALSE(ph.landing);
}

TEST_CASE("clip state: one-shots fit stock gesture time with safe bounds") {
    for (auto clip : {"atlas_hitReactLgtFwd", "atlas_downedGetup", "atlas_hitReactKnockdownInpl", "atlas_shutdownPwron"}) {
        CHECK(onceRate(clip, 2, 30, 30) == 2);
        CHECK(onceRate(clip, 0.1f, 30, 30) == 0.5f);
        CHECK(onceRate(clip, 9, 30, 30) == 3);
        CHECK(onceRate(clip, 2, 0, 30) == 1);
        CHECK(onceRate(clip, 2, 30, 0) == 1);
    }
    CHECK(onceRate("atlas_moveJumpDownLand", 4, 0, 0) == 2);
}

TEST_CASE("clip state: sim gesture change replaces a one-shot with idle or walk") {
    // Dispatch coverage only: select ignores activeClipFinished. This does not prove
    // imported clips end on the idle pose, including pelvis roll and translation.
    // That owner 11-16 data regression needs external animation assets; the checked-in
    // BT2018 fixtures contain chassis/socket data, not sampled animation channels.
    for (int gesture : {kGestureHitFront, kGestureHitBack, kGestureHitLeft, kGestureHitRight,
                        kGestureParkToStand, kGestureGetUp, kGestureRollover}) {
        Phase phase;
        const auto shot = pick(gesture, phase);
        REQUIRE(shot.mode == Mode::Once);
        const auto idle = pick(2, phase, 0.0f, false, false, shot.clip);
        CHECK(std::string(idle.clip) == "atlas_moveCoreIdle");
        CHECK(idle.mode == Mode::Loop);
        CHECK_FALSE(idle.locomotion);
        const auto walk = pick(kGestureWalk, phase, 5.0f, false, false, shot.clip);
        CHECK(std::string(walk.clip) == "atlas_moveCoreWalkFwd");
        CHECK(walk.mode == Mode::Loop);
        CHECK(walk.locomotion);
    }
}

TEST_CASE("clip state: fades use longer prone transitions and advance outgoing clips") {
    CHECK(locomotionPhase(1.5f, 2.0f, 0.8f) == doctest::Approx(0.6f));
    CHECK(locomotionPhase(0.6f, 0.8f, 2.0f) == doctest::Approx(1.5f));
    CHECK(locomotionPhase(2.0f, 2.0f, 0.8f) == 0);
    CHECK(locomotionPhase(1.0f, 0, 0.8f) == 0);
    CHECK(fadeDuration("walk", "run") == 0.15f);
    for (auto prone : {"atlas_downedIdle", "atlas_downedGetup", "atlas_deathIdle", "atlas_deathKnockdown", "atlas_hitReactKnockdownInpl"}) {
        CHECK(fadeDuration("walk", prone) == 0.25f);
        CHECK(fadeDuration(prone, "walk") == 0.25f);
    }
    CHECK(advanceClipTime(0.5f, 0.1f, 2, 1, Mode::Loop) == doctest::Approx(0.7f));
    CHECK(advanceClipTime(0.9f, 0.1f, 2, 1, Mode::Loop) == doctest::Approx(0.1f));
    CHECK(advanceClipTime(0.9f, 0.1f, 2, 1, Mode::Once) == 1);
}

TEST_CASE("clip state: world distance follows rendered chassis scale") {
    DistanceCurve curve{1, {0, 1, 2, 3, 4}};
    for (float scale : {0.5f, 2.0f, 4.0f}) {
        CHECK(curve.advanceWorld(0, scale, scale) == doctest::Approx(0.25f));
        CHECK(curve.advanceWorld(0.75f, scale * 2, scale) == doctest::Approx(0.25f));
        CHECK(curve.advanceWorld(0.25f, 0, scale) == doctest::Approx(0.25f));
    }
}

TEST_CASE("clip state: MW5 contact allowlist keeps only measured passing pairs") {
    unsigned count = 0;
    for (const auto& row : mc2ground::contactPasses)
        for (unsigned i = 0; i < 5; ++i) count += (row.gaits >> i) & 1u;
    CHECK(std::size(mc2ground::contactPasses) == 48);
    CHECK(count == 198);
    CHECK(mc2ground::contactCurveAllowed("tgl/mw5_kitfox.glb", "atlas_moveCoreRunFwd"));
    CHECK_FALSE(mc2ground::contactCurveAllowed("tgl/mw5_kitfox.glb", "atlas_moveCoreWalkLimpFwdRight"));
    CHECK_FALSE(mc2ground::contactCurveAllowed("tgl/mw5_atlas.glb", "atlas_moveCoreRunFwd"));
    CHECK_FALSE(mc2ground::contactCurveAllowed("tgl/mw5_archer.glb", "atlas_moveCoreRunFwd"));
    CHECK_FALSE(mc2ground::contactCurveAllowed("tgl/unknown.glb", "atlas_moveCoreWalkFwd"));
}

TEST_CASE("clip state: MW5 grounded stance drift at installed gesture speeds") {
    const char* root = std::getenv("MW5_TEST_TGL");
    if (!root) { WARN("set MW5_TEST_TGL to private installed pack"); return; }
    int chassis = 0;
    std::vector<std::filesystem::directory_entry> assets;
    for (const auto& asset : std::filesystem::directory_iterator(root))
        if (asset.path().extension() == ".glb") assets.push_back(asset);
    std::stable_sort(assets.begin(), assets.end(), [](const auto& a, const auto& b) {
        return a.path().stem() == "mw5_kitfox" && b.path().stem() != "mw5_kitfox";
    });
    for (const auto& asset : assets) {
        auto package = asset.path(); package.replace_extension(".package.json");
        std::ifstream input(package); nlohmann::json pkg; input >> pkg;
        const float height = mc2mechscale::targetHeight(pkg.at("chassisId").get<std::string>());
        REQUIRE(height > 0);
        Assimp::Importer importer;
        const auto* scene = importer.ReadFile(asset.path().string(), 0);
        REQUIRE(scene != nullptr);
        float low = 1e30f, high = -1e30f;
        for (unsigned m = 0; m < scene->mNumMeshes; ++m)
            for (unsigned v = 0; v < scene->mMeshes[m]->mNumVertices; ++v) {
                const float y = scene->mMeshes[m]->mVertices[v].y;
                low = std::min(low, y); high = std::max(high, y);
            }
        const float scale = height / (high - low);
        auto ini = asset.path(); ini.replace_extension(".ini");
        std::ifstream iniInput(ini);
        const std::string text((std::istreambuf_iterator<char>(iniInput)), {});
        std::vector<std::string> names;
        std::vector<int> parents;
        const auto visit = [&](const auto& self, const aiNode* node, int parent) -> void {
            const int index = static_cast<int>(names.size());
            names.push_back(node->mName.C_Str()); parents.push_back(parent);
            for (unsigned i = 0; i < node->mNumChildren; ++i) self(self, node->mChildren[i], index);
        };
        visit(visit, scene->mRootNode, -1);
        const auto plan = mc2skel::BuildPlan(scene, names);
        const auto matrix = [](const mc2skel::GpuBone& b) {
            glm::mat4 m;
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) m[c][r] = b.m[r * 4 + c];
            return m;
        };
        std::vector<mc2skel::GpuBone> rest;
        mc2skel::EvaluateRestGpuBones(scene, names, rest);
        mc2ground::Rig rig; rig.floor = 1e30f;
        for (int side = 0; side < 2; ++side) {
            auto& leg = rig.legs[side];
            const std::string prefix = side ? "j_R" : "j_L";
            auto index = [&](const std::string& name) { return static_cast<int>(std::find(names.begin(), names.end(), name) - names.begin()); };
            leg.thigh = index(prefix + "Thigh"); leg.calf = index(prefix + "Calf"); leg.foot = index(prefix + "Foot");
            REQUIRE(leg.foot < names.size());
            leg.restFoot = matrix(rest[leg.foot]);
            for (int b = 0; b < static_cast<int>(names.size()); ++b) {
                int parent = b;
                while (parent >= 0 && parent != leg.foot) parent = parents[parent];
                if (parent == leg.foot && b != leg.foot) leg.children.push_back(b);
            }
            for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
                const auto* mesh = scene->mMeshes[m];
                if (!mesh->mNumBones) continue;
                const int b = index(mesh->mBones[0]->mName.C_Str());
                if (b != leg.foot && std::find(leg.children.begin(), leg.children.end(), b) == leg.children.end()) continue;
                for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
                    const auto p = mesh->mBones[0]->mOffsetMatrix * mesh->mVertices[v];
                    leg.sole.push_back({b, {p.x, p.y, p.z}});
                    rig.floor = std::min(rig.floor, (matrix(rest[b]) * glm::vec4(p.x, p.y, p.z, 1)).y);
                }
            }
            REQUIRE_FALSE(leg.sole.empty());
        }
        for (int gesture : {kGestureWalk, kGestureRun, kGestureReverse, kGestureLimpLeft, kGestureLimpRight}) {
            INFO(asset.path().stem().string(), " gesture=", gesture);
            std::smatch match;
            REQUIRE(std::regex_search(text, match, std::regex("\\[Gestures" + std::to_string(gesture) + "\\][\\s\\S]*?EndVel=([0-9.]+)")));
            const float speed = std::stof(match[1]);
            Phase phase;
            const auto choice = select(gesture, speed, 0, 0, false, false, "", phase);
            REQUIRE(choice.locomotion);
            std::istringstream encoded(pkg.at("distanceCurves").at(choice.clip).get<std::string>());
            DistanceCurve curve; encoded >> curve.duration;
            float d; while (encoded >> d) curve.distance.push_back(d);
            REQUIRE(curve.valid());
            std::vector<std::vector<glm::mat4>> poses;
            const auto samplePose = [&](float time) {
                std::vector<mc2skel::GpuBone> bones;
                mc2skel::EvaluateClipGpuBones(*plan, choice.clip, time * 30, bones);
                std::vector<glm::mat4> pose;
                for (const auto& bone : bones) pose.push_back(matrix(bone));
                return pose;
            };
            for (size_t f = 0; f < curve.distance.size(); ++f)
                poses.push_back(samplePose(float(f) * curve.duration / float(curve.distance.size() - 1)));
            const auto packageDistance = curve.distance;
            const bool refine = mc2ground::contactCurveAllowed(asset.path().string(), choice.clip);
            const auto plants = mc2ground::buildPlants(rig, poses, curve.distance, gesture == kGestureReverse, refine);
            if (!refine) CHECK(curve.distance == packageDistance);
            REQUIRE_FALSE(plants.feet.empty());
            float time = 0, world = 0, peak = 0, maxVelocity = 0, reachError = 0;
            double solveSeconds = 0;
            float drift[2] = {}, previousWorld[2][2] = {};
            int previousContact[2] = {-1, -1}, contactFrames = 0;
            const float dt = 1.0f / 60, moved = speed * dt;
            for (int f = 0; f < 600; ++f) {
                auto pose = samplePose(time);
                const auto targets = plants.sample(time / curve.duration);
                const auto rotations = plants.sampleRotations(time / curve.duration);
                const auto started = std::chrono::steady_clock::now();
                mc2ground::solve(rig, pose, nullptr, &targets, &rotations);
                solveSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                const int frame = std::min(int(time / curve.duration * plants.feet.size()), int(plants.feet.size()) - 1);
                for (int k = 0; k < 2; ++k) {
                    const auto ankle = glm::vec3(pose[rig.legs[k].foot][3]);
                    const float x = ankle.x * scale, z = ankle.z * scale + world;
                    const bool contact = plants.contact[frame][k] && plants.contact[(frame + 1) % plants.feet.size()][k];
                    if (contact) {
                        const glm::vec2 error(ankle.x - targets[k].x, ankle.z - targets[k].z);
                        reachError = std::max(reachError, glm::length(error) * scale / mc2mechscale::worldUnitsPerMetre);
                    }
                    if (contact && previousContact[k] == 1) {
                        const float dx = x - previousWorld[k][0], dz = z - previousWorld[k][1];
                        const float step = std::sqrt(dx * dx + dz * dz) / mc2mechscale::worldUnitsPerMetre;
                        drift[k] += step; peak = std::max(peak, drift[k]);
                        maxVelocity = std::max(maxVelocity, step / dt); ++contactFrames;
                    } else drift[k] = 0;
                    previousWorld[k][0] = x; previousWorld[k][1] = z; previousContact[k] = contact ? 1 : 0;
                }
                time = curve.advanceWorld(time, moved, scale);
                world += gesture == kGestureReverse ? -moved : moved;
            }
            std::printf("MW5_CONTACT %s %s stance_cm=%.3f peak_cm_s=%.3f contact_frames=%d speed=%.3f reach_cm=%.3f\n",
                asset.path().stem().string().c_str(), choice.clip, peak * 100, maxVelocity * 100, contactFrames, speed, reachError * 100);
            const char* reason = !contactFrames ? "NO_MEASURABLE_CONTACT" : peak < .02f ? "PASS" :
                reachError > .0001f ? "REACH_CLAMP_RESIDUAL" : "PHASE_INTERPOLATION_DRIFT";
            std::printf("MW5_LOCK %s %s solve20_ms=%.4f reason=%s\n",
                asset.path().stem().string().c_str(), choice.clip, solveSeconds * 1000 * 20 / 600, reason);
            CHECK(contactFrames > 0);
            CHECK(peak < .02f);
        }
        ++chassis;
    }
    CHECK(chassis >= 48);
}

TEST_CASE("clip state: BT2018 planted stride bone oracle") {
    // Optional external art, supplied by the lane's installed mod; never substituted by synthetic bones.
    const char* root = std::getenv("BT2018_TEST_TGL");
    if (!root) { WARN("BT2018 art unavailable: set BT2018_TEST_TGL to run the bone oracle"); return; }
    int chassis = 0;
    for (const auto& asset : std::filesystem::directory_iterator(root)) {
        if (asset.path().extension() != ".glb") continue;
        ++chassis;
        Assimp::Importer importer;
        const auto* scene = importer.ReadFile(asset.path().string(), 0);
        REQUIRE(scene != nullptr);
        for (const char* gait : {"atlas_moveCoreWalkFwd", "atlas_moveCoreRunFwd"}) {
        INFO(asset.path().stem().string(), " ", gait);
        const aiAnimation* clip = nullptr;
        for (unsigned i = 0; i < scene->mNumAnimations; ++i)
            if (scene->mAnimations[i]->mName == aiString(gait)) clip = scene->mAnimations[i];
        REQUIRE(clip != nullptr);
        const float duration = static_cast<float>(clip->mDuration / (clip->mTicksPerSecond ? clip->mTicksPerSecond : 1000));
        const auto plan = mc2skel::BuildPlan(scene, {"j_LFoot", "j_RFoot"});
        std::vector<aiVector3D> feet[2];
        std::vector<mc2skel::GpuBone> bones;
        for (int f = 0; f <= static_cast<int>(duration * 30); ++f) {
            REQUIRE(mc2skel::EvaluateClipGpuBones(*plan, gait, static_cast<float>(f), bones));
            for (int k = 0; k < 2; ++k) feet[k].push_back({bones[k].m[3], bones[k].m[7], bones[k].m[11]});
        }
        const auto stride = measureStride(feet, duration);
        REQUIRE(stride.planted > 0);
        // Mean contact displacement per second, independent of the range fallback.
        // Scale cancels in this ratio; exercise walk/run rates without changing sim speeds.
        for (float rate : {0.5f, 1.0f, 2.0f}) {
            const float speed = stride.planted * rate;
            const float slip = std::abs(speed - strideRate(speed, stride.speed()) * stride.planted) / speed;
            CHECK(slip < 0.05f);
        }
        std::printf("Stride oracle: %s %s planted=%.6f estimate=%.6f selected=%.6f local u/s\n",
            asset.path().stem().string().c_str(), gait, stride.planted, stride.estimate, stride.speed());
        }
    }
    CHECK(chassis >= 51);
}

TEST_CASE("clip state: digitigrade contact-joint stride guard") {
    // The lower j_Foot moves much faster than 2R/T: it is not the planted contact joint.
    std::vector<aiVector3D> feet[2] = {{{0, 0, 0}, {0, 0, -1}, {0, 0, -2}},
                                     {{0, 1, -2}, {0, 1, -1}, {0, 1, 0}}};
    const auto stride = measureStride(feet, 1);
    CHECK(stride.planted == doctest::Approx(30));
    CHECK(stride.estimate == doctest::Approx(4));
    CHECK(stride.speed() == stride.estimate);
    CHECK(StrideMeasurement{1, 0.49f}.speed() == 1);
    CHECK(StrideMeasurement{1, 0.5f}.speed() == 0.5f);
    CHECK(StrideMeasurement{1, 1.425f}.speed() == 1.425f); // Jenner run, short stance
    CHECK(StrideMeasurement{1, 2.0f}.speed() == 2.0f);
    CHECK(StrideMeasurement{1, 2.01f}.speed() == 1.0f);
}

TEST_CASE("clip state: presentation clocks follow root alpha without changing simulation") {
    mc2::sim::ImportedAnimState previous;
    previous.activeClip = "walk"; previous.durationSec = 1; previous.playRate = 2;
    previous.clipTimeSec = 0.98f;
    const float dt = 1.0f / 30;
    auto current = previous;
    current.clipTimeSec = advanceClipTime(previous.clipTimeSec, dt, current.playRate, 1, Mode::Loop);
    for (float alpha : {0.0f, 0.25f, 0.5f, 1.0f}) {
        auto draw = current;
        mc2::sim::presentImportedAnimation(previous, draw, alpha);
        CHECK(draw.clipTimeSec == doctest::Approx(advanceClipTime(previous.clipTimeSec, alpha * dt, 2, 1, Mode::Loop)));
    }
    CHECK(previous.clipTimeSec == 0.98f);
    auto draw = current;
    previous = current; // paused: identical published clocks stay frozen
    mc2::sim::presentImportedAnimation(previous, draw, 0.5f);
    CHECK(draw.clipTimeSec == current.clipTimeSec);
    previous.mode = current.mode = Mode::Once;
    previous.clipTimeSec = 0.99f; current.clipTimeSec = 1;
    previous.previousClip = current.previousClip = "outgoing";
    previous.previousClipTimeSec = 0.1f; current.previousClipTimeSec = 0.2f;
    previous.fadeTimeSec = 0.02f; current.fadeTimeSec = 0.04f;
    draw = current;
    mc2::sim::presentImportedAnimation(previous, draw, 0.5f);
    CHECK(draw.clipTimeSec == doctest::Approx(0.995f));
    CHECK(draw.previousClipTimeSec == doctest::Approx(0.15f));
    CHECK(draw.fadeTimeSec == doctest::Approx(0.03f));
    current.activeClip = "land"; current.clipTimeSec = 0;
    draw = current;
    mc2::sim::presentImportedAnimation(previous, draw, 0.5f);
    CHECK(draw.clipTimeSec == 0);
}

// Trigger: mc2foot::worldPoint (foot grounding visual-surface read). The engine matrix is row-vector: point * M.
TEST_CASE("foot grounding: worldPoint uses the row-vector shapeToWorld layout") {
    glm::mat4 m(1.0f);
    m[0][0] = 0; m[0][2] = 1; m[2][0] = -1; m[2][2] = 0;   // rotation part, glm m[column][row]
    m[0][3] = 10; m[1][3] = 2; m[2][3] = -3;               // translation in m[c][3], as buildTranslation writes it
    const float p[3] = {1, 0, 0};
    const glm::vec3 w = mc2foot::worldPoint(m, p);          // out[c] = dot((x,y,z,1), m[c])
    CHECK(w.x == doctest::Approx(10));
    CHECK(w.y == doctest::Approx(2));
    CHECK(w.z == doctest::Approx(-4));
}
