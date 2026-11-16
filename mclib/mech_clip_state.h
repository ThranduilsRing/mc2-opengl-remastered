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
#include <string_view>

namespace mc2clip {

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

inline constexpr int kGestureWalk = 4, kGestureRun = 7, kGestureReverse = 9, kGestureLimpLeft = 11,
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

template<class HasClip>
std::string_view previewClip(std::string_view showcase, std::string_view gesture,
                             std::string_view first, HasClip hasClip) {
    for (auto name : {showcase, gesture, std::string_view("atlas_moveCoreIdle"), first})
        if (!name.empty() && hasClip(name)) return name;
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
    if (gesture != kGestureJump) {
        if (prev == kGestureJump && ph.jumpStarted) ph.landing = true;
        ph.jumpStarted = false;
    }
    switch (gesture) {
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
        if (!inLand || !activeClipFinished) return once("atlas_moveJumpDownLand");
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

// Playback rate for a locomotion clip: the clip's measured planted-foot speed (world units/s
// at rate 1) against the actor's actual ground speed. Clamped so a stop or a teleport never
// freezes or spins the legs; a clip without a measured stride plays at 1.
inline float strideRate(float actualSpeed, float clipStrideSpeed) {
    if (clipStrideSpeed <= 1e-3f) return 1.0f;
    return std::clamp(actualSpeed / clipStrideSpeed, 0.35f, 2.0f);
}

// Residual planted-foot drift (world units/s) left after rate matching.
inline float footDrift(float actualSpeed, float clipStrideSpeed) {
    const float r = strideRate(actualSpeed, clipStrideSpeed);
    const float d = actualSpeed - r * clipStrideSpeed;
    return d < 0.0f ? -d : d;
}

} // namespace mc2clip

#endif // MECH_CLIP_STATE_H
