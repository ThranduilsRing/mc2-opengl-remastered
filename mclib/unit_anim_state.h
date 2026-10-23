#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include "math/glm_config.h"
#include "mech_clip_state.h"

namespace mc2::sim {
struct ImportedAnimState {
    std::string activeClip, pendingClip;
    int pendingStreak = 0, turnStreak = 0;
    bool havePrev = false;
    float prevPx = 0, prevPy = 0, prevPz = 0, prevHeading = 0;
    float clipTimeSec = 0, durationSec = 1;
    uint32_t lastFrame = 0xffffffffu;
    // Clip-selector memory (jump start/land owed, last gesture, playing mode): the selector runs in the fixed tick, so it is sim state.
    mc2clip::Phase phase;
    mc2clip::Mode mode = mc2clip::Mode::Loop;
    bool locomotion = false;
    template<class Archive> void serialize(Archive& a) {
        std::vector<uint8_t> active(activeClip.begin(), activeClip.end()), pending(pendingClip.begin(), pendingClip.end());
        a.sequence(active, 256); a.sequence(pending, 256);
        if (a.reading()) {
            activeClip.assign(active.begin(), active.end()); pendingClip.assign(pending.begin(), pending.end());
        }
        a(pendingStreak, turnStreak, havePrev,
            prevPx, prevPy, prevPz, prevHeading, clipTimeSec, durationSec, lastFrame);
        if (a.sectionVersion < 3) return;   // R2AN v2 records end here; the selector memory keeps its defaults
        int modeInt = static_cast<int>(mode);
        a(phase.prevGesture, phase.jumpStarted, phase.landing, modeInt, locomotion);
        if (a.reading()) {
            if (modeInt != 0 && modeInt != 1) { a.reject("clip_mode"); return; }
            mode = modeInt == 1 ? mc2clip::Mode::Once : mc2clip::Mode::Loop;
        }
    }
};
// Advanced only by the fixed tick; copied into immutable render snapshots.
struct UnitAnimState {
    float mechFrameRate{};
    bool leftArmOff{};
    bool rightArmOff{};
    bool fallen{};
    bool forceStop{};
    bool atTransitionToNextGesture{};
    bool inReverse{};
    bool inJump{};
    bool jumpSetup{};
    bool jumpAirborne{};
    bool oncePerFrame{};
    bool lockRotation{};
    float velocity{};
    long currentStateGoal{};
    long currentGestureId{};
    long transitionState{};
    long oldStateGoal{};
    long hitGestureId{};
    float currentFrame{};
    uint32_t lastAnimAdvanceFrame{};
    float idleTime{};
    glm::vec3 jumpDestination{};
    glm::vec3 jumpVelocity{};
    std::vector<long> nodeUsed{};
    std::vector<float> nodeRecycle{};
    bool inCombatMode{};
    float torsoRotation{};
    float turretRotation{};
    float pitch{};
    float roll{};
    float velocityMagnitude{};
    long gvAnimationState{};
    float gvFrameRate{};
    bool isReversed{};
    bool isLooping{};
    bool setFirstFrame{};
    bool canTransition{};
    long bdAnimationState = 0;
    float bdFrameRate = 0;
    float withdrawSeconds = 0;
    long smokeLevel = 0;
    bool wakeRequested = false;
    long activityEffect = -1;
    bool activityLoop = false;
    uint32_t ejectionSerial = 0;
    uint32_t destructionSerial = 0;
    ImportedAnimState imported;
    template<class Archive> void serialize(Archive& a) {
        a(mechFrameRate, leftArmOff, rightArmOff, fallen, forceStop, atTransitionToNextGesture, inReverse, inJump, jumpSetup, jumpAirborne, oncePerFrame, lockRotation, velocity, currentStateGoal, currentGestureId, transitionState, oldStateGoal, hitGestureId, currentFrame, lastAnimAdvanceFrame, idleTime, jumpDestination.x, jumpDestination.y, jumpDestination.z, jumpVelocity.x, jumpVelocity.y, jumpVelocity.z, nodeUsed, nodeRecycle, inCombatMode, torsoRotation, turretRotation, pitch, roll, velocityMagnitude, gvAnimationState, gvFrameRate, isReversed, isLooping, setFirstFrame, canTransition, bdAnimationState, bdFrameRate, withdrawSeconds, smokeLevel, wakeRequested, activityEffect, activityLoop, ejectionSerial, destructionSerial, imported);
    }
};
struct SimPose {
    glm::vec3 root{};
    float heading = 0;
    std::vector<glm::vec3> nodes;
    std::vector<glm::vec3> shapeNodes;
    glm::vec3 hitLeft{}, hitRight{}, cockpit{};
    bool ready = false;
};
}
