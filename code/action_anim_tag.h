#ifndef ACTION_ANIM_TAG_H
#define ACTION_ANIM_TAG_H
// ACTION-ANIMATION-BINDINGS-1 (Phase-2 slice 5 of the UNIT-PROFILE arc).
//
// A SEMANTIC animation-tag layer that resolves to legacy integer gesture ids,
// with a pure-data fallback chain. This lets a future PresentationAdapter emit
// intent ("FireWeapon", "JumpLand") instead of hardcoding `setGestureGoal(6)`
// in a `if (isBattleMech)` branch. This slice ESTABLISHES + VALIDATES the layer;
// it does NOT rewire any live gesture trigger. `setGestureGoal(int)` remains the
// final legacy sink; FK/skinning/`setAnimation` are untouched below the line.
//
// PURE + header-only (plain enums / a predicate callback / int gesture ids), so
// it compiles into the offline `mc2_tests` target and drags NO engine/GL/IO code.
//
// -------------------------------------------------------------------------
// THE LEGACY GESTURE-ID SPACE (authoritative; from mclib/mech3d.h:63-88, and
// echoed verbatim by the MechAnimationNames[] comments at mech3d.cpp:274-303).
// These `#define`s are the ints every action already funnels into via
// setGestureGoal / currentGestureId. We MIRROR the numbers here (this header
// must stay engine-free, so it cannot #include mech3d.h) and a static_assert in
// the test file locks them against drift:
//   GesturePark            0    GestureLimpLeft        11
//   GestureStandToPark     1    GestureLimpRight       12
//   GestureStand           2    GestureIdle            13
//   GestureStandToWalk     3    GestureFallBackward    14
//   GestureWalk            4    GestureFallForward     15
//   GestureParkToStand     5    GestureHitFront        16
//   GestureWalkToRun       6    GestureHitBack         17
//   GestureRun             7    GestureHitLeft         18
//   GestureRunToWalk       8    GestureHitRight        19
//   GestureReverse         9    GestureJump            20
//   GestureStandToReverse 10    GestureRollover        21
//                               GestureGetUp           22
//                               GestureFallenForward   23
//                               GestureFallenBackward  24
// MaxGestures = 25. NOTE: fire is NOT skeletal (weapon-node FX, mech3d.cpp:826),
// and there is no stock death gesture — "death" in the stock machine is a forced
// FallForward/FallBackward (mech3d.cpp:5030 setGestureGoal(7)). We map FireWeapon
// to Idle (the mech keeps its locomotion pose while the weapon-node FX plays) and
// Death to the fall gestures, so the resolver never invents an id the runtime
// can't consume.
#include <vector>
#include <cstddef>

// Mirror of the legacy gesture ids (mech3d.h). Named here so the mapping table
// reads intelligibly; the test file static_asserts each against mech3d.h.
enum LegacyGestureId {
    kGesturePark            = 0,
    kGestureStand           = 2,
    kGestureWalk            = 4,
    kGestureRun             = 7,
    kGestureReverse         = 9,
    kGestureIdle            = 13,
    kGestureFallBackward    = 14,
    kGestureFallForward     = 15,
    kGestureHitFront        = 16,
    kGestureJump            = 20,
    kGestureGetUp           = 22
};

// Sentinel: the resolver returns this when a tag's fallback chain dead-ends (no
// link in the chain is available). Negative so it can never collide with a real
// gesture id (0..24); callers treat it as "no animation — keep current pose".
static const int kNoGesture = -1;

// -------------------------------------------------------------------------
// SEMANTIC ACTION TAGS. What a PresentationAdapter emits; the resolver turns each
// into a concrete legacy gesture id (via its fallback chain). Deliberately small
// and action-shaped (not a 1:1 rename of the 25 legacy slots): the point is that
// a modder/adapter names an INTENT, not a slot index.
enum class ActionAnimTag {
    Idle,        // at-rest locomotion pose
    Move,        // generic locomotion (walk-class)
    Turn,        // turn-in-place (no stock GestureTurn -> falls back to Move/Idle)
    FireWeapon,  // weapon discharge (NOT skeletal in stock -> Idle pose + node FX)
    JumpStart,   // launch
    JumpLoop,    // airborne
    JumpLand,    // touchdown
    Death,       // destroyed (stock = forced fall)
    HitReact,    // took a hit (front-hit representative)
    Startup,     // power-on (park -> stand)
    Shutdown     // power-off (stand -> park)
};

// A fallback chain: an ordered list of candidate legacy gesture ids for one tag.
// The resolver walks it head-to-tail and returns the FIRST that a caller-supplied
// availability predicate accepts. Chains are PURE DATA — no engine state.
struct AnimFallbackChain {
    ActionAnimTag    tag;
    std::vector<int> chain;   // ordered legacy gesture ids, most-specific first
};

// The DEFAULT tag -> fallback-chain table. Each chain ends in a broadly-available
// anchor (Idle / a fall gesture) so a well-formed stock skeleton always resolves.
// Rationale for the non-obvious chains:
//   * Move/Turn: no stock GestureTurn, so Turn degrades to Move then Idle. Move
//     degrades Walk -> Run -> Idle (any locomotion clip beats a frozen pose).
//   * Jump trio: Land -> Loop -> Move -> Idle; Loop -> (Jump) -> Move -> Idle;
//     Start -> Jump. Jump is INTERLOCKED in the engine (gesture + physics + FX);
//     this table is READ-ONLY proof data — it does NOT rewire the live jump path.
//   * FireWeapon: Idle only (fire is weapon-node FX, not a skeletal clip).
//   * Death: FallForward -> FallBackward -> Idle (stock has no death clip; death
//     forces a fall gesture, mech3d.cpp:5030).
//   * HitReact: HitFront -> Idle. Startup: ParkToStand-equivalent via Stand ->
//     Idle. Shutdown: Park -> Stand.
inline const std::vector<AnimFallbackChain>& defaultAnimFallbackChains() {
    static const std::vector<AnimFallbackChain> kTable = {
        { ActionAnimTag::Idle,       { kGestureIdle } },
        { ActionAnimTag::Move,       { kGestureWalk, kGestureRun, kGestureIdle } },
        { ActionAnimTag::Turn,       { kGestureWalk, kGestureIdle } },
        { ActionAnimTag::FireWeapon, { kGestureIdle } },
        { ActionAnimTag::JumpStart,  { kGestureJump, kGestureWalk, kGestureIdle } },
        { ActionAnimTag::JumpLoop,   { kGestureJump, kGestureWalk, kGestureIdle } },
        { ActionAnimTag::JumpLand,   { kGestureJump, kGestureWalk, kGestureIdle } },
        { ActionAnimTag::Death,      { kGestureFallForward, kGestureFallBackward, kGestureIdle } },
        { ActionAnimTag::HitReact,   { kGestureHitFront, kGestureIdle } },
        { ActionAnimTag::Startup,    { kGestureStand, kGestureIdle } },
        { ActionAnimTag::Shutdown,   { kGesturePark, kGestureStand } },
    };
    return kTable;
}

// Look up the fallback chain for a tag in a given table. Returns nullptr if the
// tag is absent (a malformed table); the default table covers every tag.
inline const AnimFallbackChain*
findChain(const std::vector<AnimFallbackChain>& table, ActionAnimTag tag) {
    for (const AnimFallbackChain& c : table) {
        if (c.tag == tag) return &c;
    }
    return nullptr;
}

// -------------------------------------------------------------------------
// THE RESOLVER. Walks `tag`'s fallback chain and returns the first gesture id the
// `available` predicate accepts, or kNoGesture if the chain dead-ends (or the tag
// is absent from the table). `available(gestureId)` answers "does the loaded
// skeleton actually have a clip for this gesture?" — the engine glue supplies it
// (e.g. from Mech3DAppearanceType's per-gesture animation presence). The header
// stays pure: it never touches a shape.
//
// A future PresentationAdapter calls resolveAnimTag(tag, avail) and hands the int
// result straight to the existing setGestureGoal(int) — zero change below the
// line. If it returns kNoGesture the adapter keeps the current pose (never passes
// a sentinel to setGestureGoal).
template <typename AvailablePred>
inline int resolveAnimTag(ActionAnimTag tag,
                          AvailablePred available,
                          const std::vector<AnimFallbackChain>& table
                              = defaultAnimFallbackChains()) {
    const AnimFallbackChain* c = findChain(table, tag);
    if (!c) return kNoGesture;
    for (int gestureId : c->chain) {
        if (available(gestureId)) return gestureId;
    }
    return kNoGesture;   // chain dead-ended: nothing available
}

// Convenience overload: "all gestures present" (the fully-rigged case). Always
// returns the chain HEAD when the tag exists, so callers can query the canonical
// (most-specific) gesture id for a tag without an engine predicate. Useful for
// the read-only proof: the head of a tag's chain IS the id the legacy path picks.
inline int resolveAnimTagPrimary(ActionAnimTag tag,
                                 const std::vector<AnimFallbackChain>& table
                                     = defaultAnimFallbackChains()) {
    return resolveAnimTag(tag, [](int) { return true; }, table);
}

// -------------------------------------------------------------------------
// VALIDATOR SUPPORT (UPV_ANIMATION_FALLBACK_MISSING, now LIVE — see
// code/unit_profile_validator.h). "Available" here is the same predicate: given
// the set of gestures a unit's skeleton actually provides, does this tag's chain
// resolve to SOMETHING? A tag whose entire chain is unavailable is the modder
// failure "action exists but no animation fallback delivers it" (e.g. a skeleton
// missing JumpLand/JumpLoop/Move/Idle entirely).
template <typename AvailablePred>
inline bool animTagResolves(ActionAnimTag tag,
                            AvailablePred available,
                            const std::vector<AnimFallbackChain>& table
                                = defaultAnimFallbackChains()) {
    return resolveAnimTag(tag, available, table) != kNoGesture;
}

#endif // ACTION_ANIM_TAG_H
