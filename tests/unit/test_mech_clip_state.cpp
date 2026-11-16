// tests/unit/test_mech_clip_state.cpp
//
// BT2018-FIRST-CLASS P9: the imported-mech clip state machine (mclib/mech_clip_state.h).
// Pure: gesture ids + measured speed in, clip choice out. Locks the semantics the 1C
// selector got wrong (get-up played the knockdown, hits/falls looped, limps walked, the
// whole jump was the airborne idle) and the stride-rate rule that removes foot sliding.
#include "doctest.h"
#include "mech_clip_state.h"
#include "mech_grounding.h"
#include <string>

using namespace mc2clip;

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

TEST_CASE("preview showcase: manifest, gesture, idle, first clip fallback") {
    const auto has = [](std::string_view s) { return s == "show" || s == "walk" || s == "atlas_moveCoreIdle" || s == "first"; };
    CHECK(previewClip("show", "walk", "first", has) == "show");
    CHECK(previewClip("", "walk", "first", has) == "walk");
    CHECK(previewClip("unknown", "walk", "first", has) == "walk");
    CHECK(previewClip("unknown", "missing", "first", has) == "atlas_moveCoreIdle");
    CHECK(previewClip("", "missing", "first", [](auto s) { return s == "first"; }) == "first");
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
    CHECK(strideRate(0.0f, clipStride) == doctest::Approx(0.35f));
    CHECK(strideRate(100.0f, clipStride) == doctest::Approx(2.0f));
    CHECK(strideRate(5.0f, 0.0f) == doctest::Approx(1.0f));
}
