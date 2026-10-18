//==========================================================================//
// File:    spawn_cardcloud.h                                                //
// Contents: mc2::particles::SpawnCardCloud — CPU spawn-event entry point   //
//           for the legacy gosFX::CardCloud primitive class.                //
//                                                                           //
//           Per integrated plan v6 §5.4 B1 Stage 2' C11. Replaces the      //
//           legacy gosFX::CardCloud Execute()+Draw() per-frame chain with  //
//           a single spawn-event Emit() of the cloud's starting population //
//           into the GPU particle batcher; the GPU billboard pass owns     //
//           animation (advection, color curves, age advance, projection).  //
//                                                                           //
//           CPU does NOT project. The authoritative list of forbidden      //
//           projection wrappers lives in                                    //
//           scripts/check-particles-no-cpu-projection.sh.                   //
//===========================================================================//

#pragma once

#include "spec.h"

#include "../math/glm_config.h"
namespace gosFX { class CardCloud__Specification; }

namespace mc2 {
namespace particles {

// Spawn the starting population of a CardCloud from its spec.
//
// CardCloud is a SpinningCloud subclass (which extends ParticleCloud), so
// the starting-population semantics MATCH PointCloud / ShardCloud: N
// particles spawn at Effect::Start, where N is sampled from
// spec->m_startingPopulation at age=0 / parentSeed. Each particle gets a
// random per-particle seed in [m_minimumChildSeed, m_maximumChildSeed], a
// random emitter-volume offset (m_emitterSize{X,Y,Z}), and an initial
// velocity (m_startingSpeed / m_minimum/maximumDeviation).
//
// CardCloud-specific size derivation (cardcloud.cpp:375-378 in legacy
// CreateNewParticle): per-particle half-height is sampled from
// spec->m_halfHeight at (age=0, child_seed); per-particle half-width is
// halfY * spec->m_aspectRatio.ComputeValue(age=0, child_seed). The legacy
// per-particle bounding radius is sqrt(halfX^2 + halfY^2). The current
// 64-byte GpuParticle schema carries a single scalar `size`; we collapse
// the per-axis half-extents to that bounding radius (matches the legacy
// m_radius set at cardcloud.cpp:379-383, which is what the renderer's
// camera-facing footprint uses).
//
// CardCloud-specific deferred attributes (B2 polish debt):
//   - m_pIndex / m_animated / m_width / m_U/VOffset / m_U/VSize — these
//     describe per-particle UV-atlas animation (animated card primitive
//     selects a sub-rect of the atlas per frame). The current GpuParticle
//     schema carries only `atlasIndex` (single uint), no UV sub-rect; the
//     billboard pass renders the full atlas page. Tracked as B2 schema-
//     extension debt; static (non-animated) CardClouds render at correct
//     world-space footprint and color, which is the visual majority of
//     dust / smoke / explosion clouds in stock missions.
//   - Spin (m_localRotation from SpinningCloud) is not in the schema; the
//     billboard pass renders camera-facing axis-aligned quads. Same B2
//     bucket as ShardCloud's rotation deferral.
//
// Inputs:
//   spec           - CardCloud spec (curves for population / emitter
//                    volume / velocity / color / lifetime / halfHeight /
//                    aspectRatio). Must be non-null.
//   parentToWorld  - Effect parent transform. Each particle's local-space
//                    emitter-volume offset is transformed to world for the
//                    GPU spawn position. May be null (treated as identity).
//   spawnSeed      - per-Effect random seed (0..1). Used to sample the
//                    spec's SeededCurves at age=0 for population /
//                    emitter size / child-seed range. Producers that
//                    don't have a seed should pass 0.5f (median).
//
// Side effects:
//   - Pushes one GpuParticle record per starting-population particle into
//     Batcher::Instance() when the GPU particle path is enabled.
//   - Increments FX_TRACE_DRAW(spec->m_name) ONCE per Spawn call (per-
//     Effect, matching legacy gosFX::Effect::Draw key schema).
//   - No-op when Batcher::is_enabled() is false (cheap early-out AFTER
//     the FX_TRACE_DRAW count, which is the per-spec invocation oracle).
//
// Pre-conditions:
//   spec != nullptr.
void SpawnCardCloud(const gosFX::CardCloud__Specification* spec,
                    const glm::mat4*           parentToWorld,
                    float                                   spawnSeed,
                    float                              callerAge = 0.5f);

}  // namespace particles
}  // namespace mc2
