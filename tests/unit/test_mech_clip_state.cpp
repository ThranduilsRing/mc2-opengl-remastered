// tests/unit/test_mech_clip_state.cpp
//
// BT2018-FIRST-CLASS P9: the imported-mech clip state machine (mclib/mech_clip_state.h).
// Pure: gesture ids + measured speed in, clip choice out. Locks the semantics the 1C
// selector got wrong (get-up played the knockdown, hits/falls looped, limps walked, the
// whole jump was the airborne idle) and the stride-rate rule that removes foot sliding.
#include "doctest.h"
#include "mech_clip_state.h"
#include "mech_grounding.h"
#include "mech_model_delta.h"
#include "unit_anim_state.h"
#include "mech_skel_import.h"
#include "save_archive.h"
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <cstdlib>
#include <cstdio>
#include <string>

using namespace mc2clip;

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

TEST_CASE("clip state: fades use longer prone transitions and advance outgoing clips") {
    CHECK(fadeDuration("walk", "run") == 0.15f);
    for (auto prone : {"atlas_downedIdle", "atlas_downedGetup", "atlas_deathIdle", "atlas_deathKnockdown", "atlas_hitReactKnockdownInpl"}) {
        CHECK(fadeDuration("walk", prone) == 0.25f);
        CHECK(fadeDuration(prone, "walk") == 0.25f);
    }
    CHECK(advanceClipTime(0.5f, 0.1f, 2, 1, Mode::Loop) == doctest::Approx(0.7f));
    CHECK(advanceClipTime(0.9f, 0.1f, 2, 1, Mode::Loop) == doctest::Approx(0.1f));
    CHECK(advanceClipTime(0.9f, 0.1f, 2, 1, Mode::Once) == 1);
}

TEST_CASE("clip state: BT2018 planted stride bone oracle") {
    // Optional external art, supplied by the lane's installed mod; never substituted by synthetic bones.
    const char* root = std::getenv("BT2018_TEST_TGL");
    if (!root) { WARN("BT2018 art unavailable: set BT2018_TEST_TGL to run the bone oracle"); return; }
    for (const auto& asset : {std::pair{"hunchback", 0.089690f}, std::pair{"centurion", 0.089382f}}) {
        Assimp::Importer importer;
        const auto* scene = importer.ReadFile(std::string(root) + "/" + asset.first + ".glb", 0);
        REQUIRE(scene != nullptr);
        const aiAnimation* clip = nullptr;
        for (unsigned i = 0; i < scene->mNumAnimations; ++i)
            if (scene->mAnimations[i]->mName == aiString("atlas_moveCoreWalkFwd")) clip = scene->mAnimations[i];
        REQUIRE(clip != nullptr);
        const float duration = static_cast<float>(clip->mDuration / (clip->mTicksPerSecond ? clip->mTicksPerSecond : 1000));
        const auto plan = mc2skel::BuildPlan(scene, {"j_LFoot", "j_RFoot"});
        std::vector<aiVector3D> feet[2];
        std::vector<mc2skel::GpuBone> bones;
        for (int f = 0; f <= static_cast<int>(duration * 30); ++f) {
            REQUIRE(mc2skel::EvaluateClipGpuBones(*plan, "atlas_moveCoreWalkFwd", static_cast<float>(f), bones));
            for (int k = 0; k < 2; ++k) feet[k].push_back({bones[k].m[3], bones[k].m[7], bones[k].m[11]});
        }
        const auto stride = measureStride(feet, duration);
        std::printf("Stride oracle: %s planted=%.6f estimate=%.6f selected=%.6f local u/s\n",
            asset.first, stride.planted, stride.estimate, stride.speed());
        CHECK(stride.planted == doctest::Approx(asset.second).epsilon(0.001).scale(0));
        CHECK(stride.speed() == stride.planted);
        CHECK(stride.estimate > stride.planted);
    }
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
    CHECK(StrideMeasurement{1, 1.1f}.speed() == 1.1f);
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
