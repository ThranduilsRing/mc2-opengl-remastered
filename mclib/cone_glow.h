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
void QueueConeGlow(TG_MultiShape* shape, long nodeId,
                   unsigned long argb, float nightFactor);

// Submit all queued cone triangles into the MC2_ISSPOTLGT vertex batch.
// Called once per frame from MC_TextureManager::renderLists() (top).
void FlushFrame();

} // namespace mc2_cone_glow
