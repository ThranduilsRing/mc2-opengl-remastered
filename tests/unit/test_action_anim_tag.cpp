// tests/unit/test_action_anim_tag.cpp
// ACTION-ANIMATION-BINDINGS-1 (Phase-2 slice 5 of the UNIT-PROFILE arc).
//
// Pure-logic doctest coverage for code/action_anim_tag.h: the semantic
// ActionAnimTag -> legacy-gesture-id resolver with data-driven fallback chains,
// plus the LIVE UPV_ANIMATION_FALLBACK_MISSING validator check. Header-only,
// drags no engine/GL/IO. Mirrors test_unit_profile_validator.cpp.
#include "doctest.h"
#include "action_anim_tag.h"
#include "unit_profile_validator.h"
#include <set>

// -------------------------------------------------------------------------
// DRIFT LOCK: the mirrored gesture ids in action_anim_tag.h must match the
// authoritative #defines in mclib/mech3d.h (the header stays engine-free so it
// cannot #include them; this test locks the numbers). Values from mech3d.h:63-88.
// -------------------------------------------------------------------------
static_assert(kGesturePark         == 0,  "GesturePark drifted from mech3d.h");
static_assert(kGestureStand        == 2,  "GestureStand drifted from mech3d.h");
static_assert(kGestureWalk         == 4,  "GestureWalk drifted from mech3d.h");
static_assert(kGestureRun          == 7,  "GestureRun drifted from mech3d.h");
static_assert(kGestureReverse      == 9,  "GestureReverse drifted from mech3d.h");
static_assert(kGestureIdle         == 13, "GestureIdle drifted from mech3d.h");
static_assert(kGestureFallBackward == 14, "GestureFallBackward drifted from mech3d.h");
static_assert(kGestureFallForward  == 15, "GestureFallForward drifted from mech3d.h");
static_assert(kGestureHitFront     == 16, "GestureHitFront drifted from mech3d.h");
static_assert(kGestureJump         == 20, "GestureJump drifted from mech3d.h");
static_assert(kGestureGetUp        == 22, "GestureGetUp drifted from mech3d.h");
static_assert(kNoGesture < 0,             "sentinel must not collide with a real id");

// "everything present" predicate (a fully-rigged skeleton).
static bool allAvailable(int) { return true; }

// A predicate driven by an explicit available-set (a partially-rigged skeleton).
struct AvailSet {
    std::set<int> ids;
    bool operator()(int g) const { return ids.count(g) != 0; }
};

// -------------------------------------------------------------------------
// Every tag resolves on a fully-rigged skeleton, to the HEAD of its chain.
// -------------------------------------------------------------------------

TEST_CASE("resolve: every tag resolves to its primary gesture when all available") {
    CHECK(resolveAnimTag(ActionAnimTag::Idle,       allAvailable) == kGestureIdle);
    CHECK(resolveAnimTag(ActionAnimTag::Move,       allAvailable) == kGestureWalk);
    CHECK(resolveAnimTag(ActionAnimTag::Turn,       allAvailable) == kGestureWalk);
    CHECK(resolveAnimTag(ActionAnimTag::FireWeapon, allAvailable) == kGestureIdle);
    CHECK(resolveAnimTag(ActionAnimTag::JumpStart,  allAvailable) == kGestureJump);
    CHECK(resolveAnimTag(ActionAnimTag::JumpLoop,   allAvailable) == kGestureJump);
    CHECK(resolveAnimTag(ActionAnimTag::JumpLand,   allAvailable) == kGestureJump);
    CHECK(resolveAnimTag(ActionAnimTag::Death,      allAvailable) == kGestureFallForward);
    CHECK(resolveAnimTag(ActionAnimTag::HitReact,   allAvailable) == kGestureHitFront);
    CHECK(resolveAnimTag(ActionAnimTag::Startup,    allAvailable) == kGestureStand);
    CHECK(resolveAnimTag(ActionAnimTag::Shutdown,   allAvailable) == kGesturePark);
}

TEST_CASE("resolve: resolveAnimTagPrimary returns the chain head") {
    CHECK(resolveAnimTagPrimary(ActionAnimTag::JumpLand) == kGestureJump);
    CHECK(resolveAnimTagPrimary(ActionAnimTag::Death)    == kGestureFallForward);
}

// -------------------------------------------------------------------------
// Fallback WALKS when the primary (and intermediate links) are unavailable.
// -------------------------------------------------------------------------

TEST_CASE("resolve: Move degrades Walk -> Run -> Idle") {
    // Walk missing -> Run.
    AvailSet noWalk{ {kGestureRun, kGestureIdle} };
    CHECK(resolveAnimTag(ActionAnimTag::Move, noWalk) == kGestureRun);
    // Walk & Run missing -> Idle (the anchor).
    AvailSet idleOnly{ {kGestureIdle} };
    CHECK(resolveAnimTag(ActionAnimTag::Move, idleOnly) == kGestureIdle);
}

TEST_CASE("resolve: JumpLand walks Jump -> Move -> Idle when clips missing") {
    // No Jump clip, but Walk present -> falls to the Move link (Walk).
    AvailSet noJump{ {kGestureWalk, kGestureIdle} };
    CHECK(resolveAnimTag(ActionAnimTag::JumpLand, noJump) == kGestureWalk);
    // Only Idle rigged -> lands on the anchor.
    AvailSet idleOnly{ {kGestureIdle} };
    CHECK(resolveAnimTag(ActionAnimTag::JumpLand, idleOnly) == kGestureIdle);
}

TEST_CASE("resolve: Death degrades FallForward -> FallBackward -> Idle") {
    AvailSet noFwd{ {kGestureFallBackward, kGestureIdle} };
    CHECK(resolveAnimTag(ActionAnimTag::Death, noFwd) == kGestureFallBackward);
    AvailSet idleOnly{ {kGestureIdle} };
    CHECK(resolveAnimTag(ActionAnimTag::Death, idleOnly) == kGestureIdle);
}

TEST_CASE("resolve: HitReact degrades HitFront -> Idle") {
    AvailSet idleOnly{ {kGestureIdle} };
    CHECK(resolveAnimTag(ActionAnimTag::HitReact, idleOnly) == kGestureIdle);
}

// -------------------------------------------------------------------------
// Dead-end chain returns the sentinel.
// -------------------------------------------------------------------------

TEST_CASE("resolve: a fully-unavailable chain dead-ends to kNoGesture") {
    AvailSet nothing{ {} };   // an empty / unrigged skeleton
    CHECK(resolveAnimTag(ActionAnimTag::Move,     nothing) == kNoGesture);
    CHECK(resolveAnimTag(ActionAnimTag::JumpLand, nothing) == kNoGesture);
    CHECK(resolveAnimTag(ActionAnimTag::Death,    nothing) == kNoGesture);
    CHECK(resolveAnimTag(ActionAnimTag::Idle,     nothing) == kNoGesture);
}

TEST_CASE("resolve: an absent tag in a malformed table returns kNoGesture") {
    // A table missing the requested tag -> no chain -> sentinel.
    std::vector<AnimFallbackChain> onlyIdle = {
        { ActionAnimTag::Idle, { kGestureIdle } },
    };
    CHECK(resolveAnimTag(ActionAnimTag::JumpLand, allAvailable, onlyIdle) == kNoGesture);
    CHECK(resolveAnimTag(ActionAnimTag::Idle,     allAvailable, onlyIdle) == kGestureIdle);
}

// -------------------------------------------------------------------------
// The resolver never emits an id outside the legacy 0..24 space (so setGestureGoal
// always gets a consumable id, or the sentinel the caller filters out).
// -------------------------------------------------------------------------

TEST_CASE("resolve: every resolved id is a legal legacy gesture (0..24) or the sentinel") {
    for (const AnimFallbackChain& c : defaultAnimFallbackChains()) {
        int g = resolveAnimTag(c.tag, allAvailable);
        CHECK((g == kNoGesture || (g >= 0 && g < 25)));
    }
}

// -------------------------------------------------------------------------
// LIVE validator reason code: UPV_ANIMATION_FALLBACK_MISSING.
// -------------------------------------------------------------------------

TEST_CASE("validator: fully-rigged skeleton -> ZERO animation findings (stock parity)") {
    // A stock mech has all locomotion/idle/fall/hit gestures -> every needed tag
    // resolves -> no findings.
    std::vector<NeededAnimTag> needed = {
        {ActionAnimTag::Idle}, {ActionAnimTag::Move}, {ActionAnimTag::JumpLand},
        {ActionAnimTag::Death}, {ActionAnimTag::HitReact},
    };
    CHECK(validateAnimationFallbacks(needed, allAvailable).empty());
}

TEST_CASE("validator: no needed tags -> ZERO findings") {
    std::vector<NeededAnimTag> none;
    CHECK(validateAnimationFallbacks(none, allAvailable).empty());
}

TEST_CASE("validator: a needed tag whose chain dead-ends -> ANIMATION_FALLBACK_MISSING") {
    // Skeleton has NO clips at all -> every needed tag dead-ends.
    AvailSet nothing{ {} };
    std::vector<NeededAnimTag> needed = { {ActionAnimTag::JumpLand} };
    std::vector<UnitProfileFinding> f = validateAnimationFallbacks(needed, nothing);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_ANIMATION_FALLBACK_MISSING);
    CHECK(f[0].message != nullptr);
}

TEST_CASE("validator: only the unresolvable tags trip (idle-only skeleton)") {
    // Idle rigged only: Idle/Move/JumpLand/Death all anchor on Idle (resolve);
    // but a tag whose chain never includes Idle (Shutdown: Park->Stand) dead-ends.
    AvailSet idleOnly{ {kGestureIdle} };
    std::vector<NeededAnimTag> needed = {
        {ActionAnimTag::Idle},      // resolves (Idle)
        {ActionAnimTag::Move},      // resolves (-> Idle)
        {ActionAnimTag::JumpLand},  // resolves (-> Idle)
        {ActionAnimTag::Shutdown},  // Park->Stand, neither present -> dead-end
    };
    std::vector<UnitProfileFinding> f = validateAnimationFallbacks(needed, idleOnly);
    REQUIRE(f.size() == 1);
    CHECK(f[0].code == UPV_ANIMATION_FALLBACK_MISSING);
}
