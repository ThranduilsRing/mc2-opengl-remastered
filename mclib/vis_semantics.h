//---------------------------------------------------------------------------
// vis_semantics.h — VISIBILITY-SEMANTICS-1
//
// ONE flag ("visible") was doing FOUR jobs. Every lane that tuned the
// predicate for one job broke another. This header separates them.
//
//   (a) MESH DRAWN        — is the unit's solid mesh rasterized this frame?
//   (b) KNOWN TO PLAYER   — does the player hold contact (mesh OR sensor blip)?
//   (c) EMITS LIGHT       — pool light + searchlight cone + additive cone glow.
//   (d) SELF LIT          — does the unit receive night object-light?
//
// MEASURED FACTS (see .claude/VISIBILITY-SEMANTICS-1-STATUS.md — do not
// re-derive):
//
//  * `Appearance::visible` carries NO information for movers.
//    code/mech.cpp:6638 sets it true unconditionally in BattleMech::update()
//    for EVERY mech every frame, and code/mech.cpp:6816-6895 /
//    code/gvehicl.cpp:3784-3833 set it true again on EVERY contact branch,
//    sensor-blip branches included. The only site that ever passes a computed
//    value is code/objmgr.cpp:4918 in updateAppearancesOnly(), which is not
//    the mainline. Treating it as fog-of-war is what produced the ghost-light
//    class of bugs.
//
//  * The mesh is NOT hidden by a boolean. It is hidden by FADING
//    `alphaValue` to 0 (code/mech.cpp:6708-6744, code/gvehicl.cpp). The sensor
//    blip shape is drawn ALONGSIDE it at (0xff - alpha), not instead of it
//    (mech3d.cpp:3641, gvactor.cpp:2432). So the true mesh-drawn predicate is
//    `meshAlpha != 0`, and it covers the fade-in/fade-out transitions for
//    free.
//
//  * sensorLevel is NOT a uniform scale across classes:
//       mechs    — 0 visual-contact enemy | 1..4 blip-only enemy |
//                  5 own/allied (set in BattleMech::update, but ONLY when
//                  isOnGui; BattleMech::render then sets it back to 0).
//       vehicles — 0 own/allied AND visual-contact enemy | 1..4 blip-only.
//                  code/gvehicl.cpp NEVER passes 5.
//    Any predicate of the form `sensorLevel > 4` therefore excludes every
//    vehicle in the game and every home mech that render() has just reset to
//    0. That is a live defect, not a hypothetical one.
//
// RULES this module encodes (from the user):
//   - Player-owned and allied units are ALWAYS drawn, lit and emitting.
//   - Enemy units must not leak position via emission when blip-only.
//   - Buildings / static lamps are exempt from mover rules entirely; nothing
//     in this header is consulted by mclib/bdactor.cpp.
//
// GATE: MC2_VIS_SEMANTICS, default ON (correctness fix). =0 restores the
// pre-lane sensorLevel predicates. SUPERSEDES MC2_LIGHT_MOVER_VIS_GATE and
// MC2_LIGHT_CONE_VIS_GATE, which are PARKED default-OFF (89f0ed4f) and are
// deliberately NOT ANDed in — consulting them would leave this module dead on
// arrival. Setting either legacy gate to 1 arms its own job independently;
// setting either to 0 does NOT disable this module. MC2_FORCE_ALL_VISIBLE=1
// (a diagnostic, default ON only under the smoke runner) disarms everything.
//---------------------------------------------------------------------------
#ifndef VIS_SEMANTICS_H
#define VIS_SEMANTICS_H

namespace mc2_vis {

// homeTeamRelationship, as fed by BattleMech::update / GroundVehicle::update
// through Appearance::setObjectParameters.
enum Relation
{
    REL_OWN     = 0,   // same team AND same commander as the local player
    REL_ALLIED  = 1,   // same team, different commander
    REL_FOREIGN = 2    // everything else (enemy, neutral, unaligned)
};

// Everything the four predicates need. Filled at the call site from state the
// appearance already owns; nothing new is plumbed through the game layer.
struct MoverVis
{
    long     sensorLevel;    // class-dependent, see header comment
    unsigned meshAlpha;      // 0..255 from TG_MultiShape::GetAlphaValue()
    long     status;         // OBJECT_STATUS_*
    long     relation;       // homeTeamRelationship (Relation above)
    bool     legacyVisible;  // the old `visible` flag — recorded, not trusted
    bool     inEditor;

    MoverVis()
        : sensorLevel(0), meshAlpha(255u), status(0),
          relation(REL_FOREIGN), legacyVisible(true), inEditor(false) {}
};

// MC2_VIS_SEMANTICS killswitch (=0 -> legacy predicates).
bool enabled();

// MC2_FORCE_ALL_VISIBLE diagnostic: force every mover to read as drawn,
// known, emitting and lit. Never ship enabled in interactive play.
//
// FORCE-ALL-VISIBLE-1: default ON whenever MC2_SMOKE_MODE is set (see
// scripts/smoke_lib/runner.py), default OFF otherwise. `=0` always wins.
// Rationale: automated captures were repeatedly graded on frames in which the
// unit under test was hidden or mid-fade, producing false negatives.
bool force_all_visible();

// FORCE-ALL-VISIBLE-1: defeat all THREE hiding mechanisms at their single
// source, in Mover::update, before the appearance is pushed.
//
// The predicates above only make the *decisions* read "visible"; they cannot
// undo the state the game layer has already written. There are three distinct
// mechanisms and returning true from meshDrawn() defeats none of them:
//
//   (1) alphaValue faded to 0 — the ACTUAL hide for blip-only enemies
//       (code/mech.cpp, code/gvehicl.cpp contact branches). Ordinary leaves
//       honour it via MC2_DRAWALPHA, so a "visible" unit at alpha 0 still
//       rasterizes to nothing.
//   (2) sensorLevel — drives the blip-shape draw (1..4) and the
//       `sensorLevel == 0 || sensorLevel >= 5` emission predicates. It is
//       written only conditionally (the `isOnGui` arm), so it can retain a
//       stale value indefinitely. 5 is chosen because it is the only value
//       that satisfies BOTH that predicate and the stricter `sensorLevel > 4`
//       one (mech3d.cpp anubis pointLight), and it suppresses the blip shape.
//   (3) the legacy `visible` flag — set for completeness; it is not the real
//       mechanism (see the header comment above) but costs nothing.
//
// Templated only because BattleMech and GroundVehicle hold different
// appearance types; both expose the same three setters.
template <typename AppearanceT, typename AlphaT>
inline bool forceVisibleOverride(AppearanceT* app, AlphaT& alphaValue)
{
    if (!force_all_visible()) return false;
    if (!app) return false;
    alphaValue = (AlphaT)0xff;
    app->setAlphaValue(0xff);
    app->setSensorLevel(5);
    app->setVisibility(true, true);
    return true;
}

// --- the four separated predicates ------------------------------------------

// Not destroyed / disabled / shut down. A wreck emits nothing and is not
// self-lit, but it is still drawn and still known to the player.
bool powered(const MoverVis& v);

// Own or allied. These units are exempt from every fog-of-war rule.
bool friendly(const MoverVis& v);

// (a) is the solid mesh rasterized this frame.
bool meshDrawn(const MoverVis& v);

// (b) does the player hold contact — mesh OR sensor blip.
bool knownToPlayer(const MoverVis& v);

// (c) may this unit emit light (pool + cone + glow). Callers still AND in
// their own scene conditions (isNight, !InEditor).
bool emitsLight(const MoverVis& v);

// (d) should this unit receive night object-light / read as a lit object.
bool selfLit(const MoverVis& v);

// --- measurement -------------------------------------------------------------
// MC2_VIS_SEMANTICS_TRACE=1 emits one line per mover per sampled frame to
// stderr, tagged [VIS-SEM v1]. Sample period MC2_VIS_SEMANTICS_TRACE_EVERY
// (default 120 frames). This is how the measurement table in
// .claude/VISIBILITY-SEMANTICS-1-STATUS.md was produced.
bool trace_enabled();
void trace(const char* cls, const void* actor, const MoverVis& v);

} // namespace mc2_vis

#endif // VIS_SEMANTICS_H
