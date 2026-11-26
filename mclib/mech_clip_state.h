#ifndef MECH_CLIP_STATE_H
#define MECH_CLIP_STATE_H
// BT2018-FIRST-CLASS P9: imported-mech clip state machine (pure, doctest-covered by
// tests/unit/test_mech_clip_state.cpp).
//
// Maps the stock gesture state (mech3d.h Gesture* ids, already driven by the simulation)
// plus the actor's measured ground speed to the BT2018 clip the RENDER pose plays. It is a
// one-way read of simulation state: nothing here is fed back into movement, firing or
// collision, so render poses can never steer gameplay (the R1 fixed-tick rule).
//
// Semantics fixed versus the 1C selector:
//   * get-up (Rollover/GetUp) plays atlas_downedGetup, not the knockdown;
//   * falls, hits, get-ups, deaths and jump start/land are ONE-SHOT: they play once and
//     hold their last frame instead of looping;
//   * prone holds atlas_downedIdle (alive) or atlas_deathIdle (destroyed);
//   * the destroyed fall plays atlas_deathKnockdown;
//   * limps use the directional limp clips;
//   * a jump is start (once) -> airborne idle (loop) -> land (once) on leaving the gesture;
//   * locomotion clips play at the rate that matches the measured stride to the actor's
//     actual speed, so planted feet stop sliding.

#include <algorithm>
#include <cstring>
#include <cmath>
#include <string_view>
#include <vector>
#include <map>
#include <string>

namespace mc2clip {

struct StrideMeasurement {
    float estimate = 0, planted = 0;
    float speed() const {
        // Running stance is shorter than half a cycle: 2R/T is not its speed cap.
        // Allow contact down to a quarter cycle; reject non-contact joint motion.
        return planted >= 0.5f * estimate && planted <= 2.0f * estimate ? planted : estimate;
    }
};

// Samples are unscaled Y-up joint globals at 30 Hz, before the GPU axis conversion.
template<class Point>
StrideMeasurement measureStride(const std::vector<Point> (&feet)[2], float duration) {
    StrideMeasurement result;
    if (duration <= 0 || feet[0].size() < 3 || feet[0].size() != feet[1].size()) return result;
    float ranges[2] = {};
    for (const auto& foot : feet) {
        float lo[2] = {foot[0].x, foot[0].z}, hi[2] = {lo[0], lo[1]};
        for (const auto& p : foot) {
            lo[0] = std::min(lo[0], p.x); hi[0] = std::max(hi[0], p.x);
            lo[1] = std::min(lo[1], p.z); hi[1] = std::max(hi[1], p.z);
        }
        result.estimate += std::max(hi[0] - lo[0], hi[1] - lo[1]) / duration;
        for (int axis = 0; axis < 2; ++axis) ranges[axis] += hi[axis] - lo[axis];
    }
    float displacement[2] = {};
    int samples[2] = {};
    for (size_t f = 1; f < feet[0].size(); ++f) {
        for (int k = 0; k < 2; ++k) {
            if (feet[k][f - 1].y >= feet[1 - k][f - 1].y || feet[k][f].y >= feet[1 - k][f].y) continue;
            const float delta = ranges[0] > ranges[1] ? feet[k][f].x - feet[k][f - 1].x
                                                     : feet[k][f].z - feet[k][f - 1].z;
            const int direction = delta < 0 ? 0 : 1;
            if (delta == 0) continue;
            displacement[direction] += std::abs(delta);
            ++samples[direction];
        }
    }
    // The dominant stance direction is backwards (also handles reverse clips).
    const int backward = displacement[0] >= displacement[1] ? 0 : 1;
    if (samples[backward]) result.planted = displacement[backward] * 30.0f / samples[backward];
    return result;
}

enum class Mode : int { Loop = 0, Once = 1 };

struct Choice {
    const char* clip = "atlas_moveCoreIdle";
    Mode mode = Mode::Loop;
    bool locomotion = false;   // stride-matched playback rate
    bool immediate = false;    // switch now (events), instead of the locomotion debounce
};

// Per-actor memory the selector needs across frames (lives in the importer's ActorAnimState).
struct Phase {
    int prevGesture = -1;
    bool jumpStarted = false;   // start clip already chosen for the current jump
    bool landing = false;       // land clip owed after the jump gesture ended
};

inline constexpr int kGesturePark = 0, kGestureStandToPark = 1, kGestureParkToStand = 5,
                     kGestureWalk = 4, kGestureRun = 7, kGestureReverse = 9, kGestureLimpLeft = 11,
                     kGestureLimpRight = 12, kGestureFallBackward = 14, kGestureFallForward = 15,
                     kGestureHitFront = 16, kGestureHitBack = 17, kGestureHitLeft = 18, kGestureHitRight = 19,
                     kGestureJump = 20, kGestureRollover = 21, kGestureGetUp = 22,
                     kGestureFallenForward = 23, kGestureFallenBackward = 24;

// Clip names are string literals, so the view's data() stays valid for the Choice's lifetime.
inline Choice once(std::string_view c) { Choice ch; ch.clip = c.data(); ch.mode = Mode::Once; ch.immediate = true; return ch; }
inline Choice loop(std::string_view c, bool loco = false) { Choice ch; ch.clip = c.data(); ch.locomotion = loco; return ch; }

// Missing mission bindings keep the actor's current clip, including its clock.
template<class HasClip>
std::string_view resolveClip(std::string_view requested, std::string_view active, HasClip hasClip) {
    return hasClip(requested) ? requested : active;
}

// BT2018's base idle is a static ready pose. Its authored flavor clip moves and loops;
// MW5 packs have no such clip and retain their native Stand binding.
template<class HasClip>
std::string_view standingClip(std::string_view requested, HasClip hasClip) {
    return requested == "atlas_moveCoreIdle" && hasClip("atlas_moveCoreIdleFlavor1")
        ? "atlas_moveCoreIdleFlavor1" : requested;
}

template<class HasClip>
std::string_view previewClip(std::string_view showcase, std::string_view first, HasClip hasClip) {
    // A preview actor never leaves gesture 0 (Park = the crouched atlas_shutdownIdle), so the gesture must not pick its clip:
    // a pack without clips.showcase (the owner's v0.4 Marauder) showed the powered-down pose. Default = the idle clip: the
    // Atlas-authored walk pitches pelvis +7 and Spine2 +13 deg forward (nose-down on the Marauder); idle stays within 1 deg of bind.
    for (auto name : {showcase, std::string_view("atlas_moveCoreIdle"), std::string_view("atlas_moveCoreWalkFwd"), first})
        if (!name.empty() && hasClip(name)) return standingClip(name, hasClip);
    return {};
}

// Preview poses bypass the simulation-to-draw copy, so publish on each sampled tick.
template<class PublishGpu, class PublishCpu>
void publishPreviewPose(bool preview, bool gpuMode, PublishGpu publishGpu, PublishCpu publishCpu) {
    if (!preview) return;
    if (gpuMode) publishGpu();
    else publishCpu();
}

// activeClipFinished: the currently playing Once clip reached its end (drives jump start ->
// airborne and land -> locomotion). destroyed: the mech is dead (death clips instead of knockdowns).
inline Choice select(int gesture, float speed, float turnRate, int turnStreak, bool destroyed,
                     bool activeClipFinished, const char* activeClip, Phase& ph) {
    const int prev = ph.prevGesture;
    ph.prevGesture = gesture;
    if (destroyed && gesture != kGestureFallBackward && gesture != kGestureFallForward) {
        ph.jumpStarted = ph.landing = false;
        Choice c = loop("atlas_deathIdle"); c.immediate = true; return c;
    }
    if (gesture != kGestureJump) {
        if (prev == kGestureJump && ph.jumpStarted) ph.landing = true;
        ph.jumpStarted = false;
    }
    switch (gesture) {
        case kGesturePark: {
            ph.landing = false;
            Choice c = loop("atlas_shutdownIdle"); c.immediate = true; return c;
        }
        case kGestureStandToPark: return once("atlas_shutdownPwroff");
        case kGestureParkToStand: return once("atlas_shutdownPwron");
        case kGestureHitFront: return once("atlas_hitReactLgtFwd");
        case kGestureHitBack:  return once("atlas_hitReactLgtBwd");
        case kGestureHitLeft:  return once("atlas_hitReactLgtLeft");
        case kGestureHitRight: return once("atlas_hitReactLgtRight");
        case kGestureFallBackward:
        case kGestureFallForward:
            return once(destroyed ? "atlas_deathKnockdown" : "atlas_hitReactKnockdownInpl");
        case kGestureFallenForward:
        case kGestureFallenBackward: {
            Choice c = loop(destroyed ? "atlas_deathIdle" : "atlas_downedIdle");
            c.immediate = true;
            return c;
        }
        case kGestureRollover:
        case kGestureGetUp:
            return once("atlas_downedGetup");
        case kGestureLimpLeft:  return loop("atlas_moveCoreWalkLimpFwdLeft", true);
        case kGestureLimpRight: return loop("atlas_moveCoreWalkLimpFwdRight", true);
        case kGestureJump: {
            ph.landing = false;
            const bool inStart = activeClip && std::strcmp(activeClip, "atlas_moveJumpUpStart") == 0;
            if (!ph.jumpStarted) { ph.jumpStarted = true; return once("atlas_moveJumpUpStart"); }
            if (inStart && !activeClipFinished) return once("atlas_moveJumpUpStart");
            Choice c = loop("atlas_moveJumpUpIdle");
            c.immediate = true;
            return c;
        }
        default: break;
    }
    if (ph.landing) {
        const bool inLand = activeClip && std::strcmp(activeClip, "atlas_moveJumpDownLand") == 0;
        if (speed <= 1.0f && (!inLand || !activeClipFinished)) return once("atlas_moveJumpDownLand");
        ph.landing = false;
    }
    const float MOVE_EPS = 1.0f;   // world units/sec
    const bool standing = (gesture != kGestureWalk && gesture != kGestureRun && gesture != kGestureReverse);
    if (standing && speed < MOVE_EPS && turnStreak >= 3)
        return loop(turnRate > 0.0f ? "atlas_moveCoreTurnLeftIdle" : "atlas_moveCoreTurnRightIdle");
    switch (gesture) {
        case kGestureRun:     return loop("atlas_moveCoreRunFwd", true);
        case kGestureReverse: return loop("atlas_moveCoreWalkBwd", true);
        case kGestureWalk:    return loop("atlas_moveCoreWalkFwd", true);
        default: return speed > MOVE_EPS ? loop("atlas_moveCoreWalkFwd", true) : loop("atlas_moveCoreIdle");
    }
}

inline float onceRate(std::string_view clip, float duration, float gestureFrames, float gestureRate) {
    if (clip == "atlas_moveJumpDownLand") return 2.0f;
    if (duration <= 0.0f || gestureFrames <= 0.0f || gestureRate <= 0.0f) return 1.0f;
    return std::clamp(duration * gestureRate / gestureFrames, 0.5f, 3.0f);
}

inline bool proneClip(std::string_view clip) {
    return clip.starts_with("atlas_downed") || clip.starts_with("atlas_death") ||
           clip == "atlas_hitReactKnockdownInpl";
}

// Package roles override the legacy BT clip convention, including explicit exclusions.
inline std::string_view groundingRole(const std::map<std::string, std::string>& roles, std::string_view clip) {
    for (const auto& [name, role] : roles)
        if (name == clip) return role;
    if (!roles.empty()) return "none";
    return proneClip(clip) ? "body" : clip.starts_with("atlas_moveCore") ? "feet" : "none";
}

// Whether the draw lift follows the pose's lowest point every frame instead of the static rest lift: fallen bodies, and
// walking clips of a rig whose legs the per-leg solver cannot resolve (it then leaves the authored root bob in, which
// floats the mech by up to a stride's height).
inline bool liftFollowsPose(std::string_view role, bool legsSolved) {
    return role == "body" || (role == "feet" && !legsSolved);
}

inline float fadeDuration(std::string_view previous, std::string_view next) {
    return proneClip(previous) || proneClip(next) ? 0.25f : 0.15f;
}

inline float advanceClipTime(float clipSec, float dt, float rate, float duration, Mode mode) {
    clipSec += dt * rate;
    if (duration > 0.0f) {
        if (mode == Mode::Once) return std::min(clipSec, duration);
        while (clipSec >= duration) clipSec -= duration;
    }
    return clipSec;
}

// Playback rate for a locomotion clip: the clip's measured planted-foot speed (world units/s
// at rate 1) against the actor's actual ground speed. The bounds only guard a stop or a
// teleport: the light chassis' run speed is 2.4-2.8x their clip's stride (assassin, locust,
// cicada, spider), so a 2x ceiling left their feet gliding. A clip without a measured stride plays at 1.
inline constexpr float kStrideRateMin = 0.1f, kStrideRateMax = 4.0f;
inline float strideRate(float actualSpeed, float clipStrideSpeed) {
    if (clipStrideSpeed <= 1e-3f) return 1.0f;
    return std::clamp(actualSpeed / clipStrideSpeed, kStrideRateMin, kStrideRateMax);
}

// Residual planted-foot drift (world units/s) left after rate matching.
inline float footDrift(float actualSpeed, float clipStrideSpeed) {
    const float r = strideRate(actualSpeed, clipStrideSpeed);
    const float d = actualSpeed - r * clipStrideSpeed;
    return d < 0.0f ? -d : d;
}

// Walk/run clips have different durations; preserve the planted leg on adoption.
inline float locomotionPhase(float clipSec, float oldDuration, float newDuration) {
    return oldDuration > 0 && newDuration > 0 ? std::fmod(clipSec / oldDuration, 1.0f) * newDuration : 0.0f;
}

} // namespace mc2clip

#endif // MECH_CLIP_STATE_H
