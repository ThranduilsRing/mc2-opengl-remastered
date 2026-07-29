//-------------------------------------------------------------------------------
// LIGHT-CONE-MODELS-1 — authored light-cone geometry as light-source ground truth.
//
// Stock mech/vehicle/building TGL models carry "SpotLight_*" child shapes:
// translucent cone volumes authored in MAX (headlights, searchlights, tower
// spotlights). Legacy MC2 rendered them additively at night (MC2_ISSPOTLGT
// vertex batch, gos_Alpha_OneOne) and the (E) SpotLight_->real-illumination
// arc later registered a TG_LIGHT_POINT per cone node — but BOTH GPU batchers
// (gos_mech_batcher.cpp:1301, gos_static_prop_batcher.cpp:5006) suppress the
// cone geometry, so on the modern default path the cones are invisible, and
// the pool light sits at the node ORIGIN (or a heuristic forward bias,
// MOVER-LIGHT-FORWARD-1) rather than where the authored beam actually lands.
//
// This module supplies:
//  (A) GetNodeAim — the authored cone's aim direction + length, derived from
//      the cone's own type geometry (centroid of node-local vertices) and the
//      node's live shapeToWorld. Callers move the pool light to the beam's
//      far end (spill lands AHEAD of the lamp, geometrically correct),
//      replacing the facing-heuristic where a cone exists.
//  (C) The same call gives the spill anchor (far end); callers ground it on
//      terrain.
//  (B) QueueConeGlow / FlushFrame — night-gated additive re-render of the
//      authored cone mesh through the still-alive legacy MC2_ISSPOTLGT
//      consumer in MC_TextureManager::renderLists (OneOne additive, ZWrite
//      off, fog off). Reservation (addTriangle) happens at queue time in the
//      update phase — same phase legacy MultiTransformShape reserved from —
//      and vertex fill is deferred to FlushFrame() at the top of
//      renderLists(), after every legacy addVertices for the frame.
//
// Gates (registered in scripts/check-env-registry.sh + docs/tier1_env_vars.md):
//   MC2_LIGHT_CONE_NODES      default ON  (=0: legacy heuristic pool centers)
//   MC2_LIGHT_CONE_GLOW       default ON  (=0: cones stay invisible, batch-6 look)
//   MC2_LIGHT_CONE_GLOW_ALPHA default 40  (0..255 peak vertex alpha, pre-nightFactor;
//                                          255 = verify-by-exaggeration)
//   MC2_LIGHT_CONE_SPILL      default 1.0 (0..3 scale on authored cone length
//                                          for the pool spill anchor)
//
// North star: "occupied glow, never daytime-bright, err dimmer."

#pragma once

#include <stuff/stuff.hpp>

class TG_MultiShape;

namespace mc2_cone_glow {

// Env gates (cached at first call).
bool nodes_enabled();       // MC2_LIGHT_CONE_NODES
bool glow_enabled();        // MC2_LIGHT_CONE_GLOW
// CONE-SPILL-GRADE-1: isolates CONE-SPILL-COUPLING-1 (c1f98b45). ON = march the
// beam ray for the true terrain intersection and suppress the pool on a miss;
// OFF = the prior far-end-then-snap-z approximation with the 200.0f clamp and
// no miss rejection. Exists so the ungated default-behaviour change shipped by
// c1f98b45 can be A/B graded on a single attributable exe.
bool spill_coupling_enabled();  // MC2_CONE_SPILL_COUPLING
// SPOTLIGHT-DIRECTION-1: sets the building spotlight's TG_Light::direction
// (which is a light-spot POSITION for POINT/SPOT, tgl.h:213) and builds its
// lightToWorld from the swizzled Stuff-space position, matching the sibling
// terrain-light site. OFF = the historical unset direction (0,0,0) + raw-space
// lightToWorld. See cone_glow.cpp for the measured evidence.
bool spot_direction_fix_enabled();  // MC2_SPOTLIGHT_DIRECTION_FIX
float glow_alpha();         // MC2_LIGHT_CONE_GLOW_ALPHA (0..255)
float spill_scale();        // MC2_LIGHT_CONE_SPILL (0..3)

// (A) Aim of the authored cone under multishape node `nodeId` (index into
// listOfShapes, i.e. the value GetNodeNameId returned at registration).
// aimWorldOut — unit vector, MC2/Stuff world frame (same frame as
//               getNodeIdPosition results).
// coneLenOut  — authored beam length in world units (shapeScalar applied).
// Returns false (outputs untouched) when the node has no usable cone
// geometry — caller falls back to its existing heuristic.
bool GetNodeAim(TG_MultiShape* shape, long nodeId,
                Stuff::Vector3D& aimWorldOut, float& coneLenOut);

// (B) Queue the cone mesh under `nodeId` for additive glow this frame.
// argb = light color (alpha byte ignored), nightFactor 0..1 scales alpha.
// Transforms with the node's current shapeToWorld + TG_Shape camera statics,
// so call from the owner's update path AFTER TransformMultiShape ran.
// No-op when glow_enabled() is false or geometry clips fully off screen.
// ownerIsMover — GHOST-LIGHT-VISIBILITY-1. Movers additionally get the
// renderer's own staleness guard (TG_Shape::Render, tgl.cpp:3155, drops any
// leaf whose lastTurnTransformed != turn), so a culled / never-spawned /
// off-map-staged mover cannot emit a beam from a stale shapeToWorld. Static
// props pass false BY DESIGN: a building lamp's cone leaf is stripped from the
// static-prop recipe (GHOST-CONE-RECIPE-SKIP-1) and is not individually
// transformed on the GPU prop path, so the staleness stamp is meaningless for
// it — and per the user rule building/static lamp cones may always show.
void QueueConeGlow(TG_MultiShape* shape, long nodeId,
                   unsigned long argb, float nightFactor,
                   bool ownerIsMover = false);

// LIGHT-REALISM-1 (beacon emission). User: the red/green/blue beacons "emit NO
// light at all ... they're just bright unlit sprites floating on dark ground."
//
// Emits a SMALL, SOFT, colored additive ground disc under every beacon /
// runway-light card on `shape` — the light a real beacon spills on its
// immediate surroundings. Colour is taken from the beacon's own node name
// (Red/Green/Blue beaconlight), matching the classification the GPU batcher
// uses (TG_Shape::GetIsLightBillboard, node-name token "beaconlight").
//
// Deliberately NOT a registered TG_Light: beacons/runway lights number in the
// dozens per map and would exhaust the 64-slot pool-light budget, starving the
// real lamps. This rides the same additive MC2_ISSPOTLGT batch as the cone
// glow (queued here, flushed by FlushFrame), so it costs no light slots.
//
// Gated on MC2_LIGHT_REALISM (default OFF -> no-op). Call from the owner's
// update path AFTER TransformMultiShape, same contract as QueueConeGlow.
void QueueBeaconGlow(TG_MultiShape* shape, float nightFactor);

// Submit all queued cone triangles into the MC2_ISSPOTLGT vertex batch.
// Called once per frame from MC_TextureManager::renderLists() (top).
void FlushFrame();

// GHOST-LIGHT-VISIBILITY-1 (mover fairness rule). True when the player is
// actually being shown this mover's MODEL, i.e. when emitting its light/cone
// leaks nothing.
//
// sensorLevel semantics (code/mech.cpp:6698-6755, code/gvehicl.cpp:3784-3820):
//   0      enemy at CONTACT_VISUAL (or destroyed/disabled/ShowMovers) — the
//          real mesh is drawn.
//   1..4   enemy known ONLY as a sensor contact — mech3d.cpp:3641 /
//          gvactor.cpp:2432 draw a blip triangle/circle INSTEAD of the mesh.
//   >=5    not an enemy (home/allied team) — mesh drawn.
//
// NOTE the appearance-side `visible` flag is NOT a fog-of-war predicate:
// code/mech.cpp calls setVisibility(true,true) on EVERY contact branch,
// including CONTACT_SENSOR_QUALITY_1..4. Gating mover light on `visible`
// (LIGHT-POOL-VIS-GATE-1 / T1.13) therefore let a hidden enemy's searchlight
// cone advertise its position — the "poor form" positional leak this predicate
// closes. Static building lamps deliberately do NOT consult this.
bool mover_model_shown(long sensorLevel);

// MC2_LIGHT_MOVER_VIS_GATE killswitch (=0 restores the legacy leak).
bool mover_vis_gate_enabled();

// GHOST-CONE-VERIFY-4 — the cone GEOMETRY leak (distinct from the pool light
// and the additive glow, both closed by GHOST-LIGHT-VISIBILITY-1).
//
// A blip-only enemy is NOT hidden via the appearance `visible` flag — the mesh
// is submitted every frame and hidden by FADING `alphaValue` to 0
// (code/gvehicl.cpp ~3760-3820, code/mech.cpp ~6790-6860). Ordinary leaves
// honour that: TG_Shape::Render / MultiTransformShape route through
// MC2_DRAWALPHA whenever `alphaValue != 0xff`. The `isSpotlight` branch is
// tested FIRST and emits at hardcoded 0xffffffff opacity, so the searchlight
// wedge of a fully-faded (invisible) vehicle/mech kept drawing at full
// brightness — the user-visible "light cones from invisible vehicles".
//
// Vehicles never go through the GPU static-prop or mech batcher (both of which
// strip SpotLight_ leaves), so this legacy CPU leaf draw is the DEFAULT and
// ONLY path for every vehicle / helicopter / VTOL / elemental cone.
//
// Predicate is alpha-based, NOT sensorLevel-based: tgl.cpp has no sensorLevel,
// and alpha is the actual mechanism that hides the model — so this also covers
// the contact fade-in/fade-out transitions for free. Strict `== 0` keeps every
// static building lamp (alphaValue 0xff) untouched.
//
// MC2_LIGHT_CONE_VIS_GATE killswitch (=0 restores the legacy leak).
bool cone_geom_hidden_by_alpha(unsigned alphaValue);

} // namespace mc2_cone_glow
