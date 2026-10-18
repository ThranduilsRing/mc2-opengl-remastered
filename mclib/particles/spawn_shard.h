//==========================================================================//
// File:    spawn_shard.h                                                    //
// Contents: mc2::particles::SpawnShard — CPU spawn-event entry point for   //
//           the legacy gosFX::ShardCloud primitive class.                   //
//                                                                           //
//           Per integrated plan §5.4 B1 Stage 2' C5. Replaces the legacy   //
//           gosFX::ShardCloud Execute()+Draw() per-frame chain with a       //
//           single spawn-event Emit() of the cloud's starting population    //
//           into the GPU particle batcher; the GPU billboard pass owns      //
//           animation (advection, color curves, age advance, projection).   //
//                                                                           //
//           CPU does NOT project. The authoritative list of forbidden       //
//           projection wrappers lives in                                    //
//           scripts/check-particles-no-cpu-projection.sh.                   //
//                                                                           //
//           NOT YET WIRED into producers — wiring lands at C7. C5 only     //
//           ships the function so it builds clean and is ready to call.    //
//===========================================================================//

#pragma once

#include "spec.h"

#include "../math/glm_config.h"
namespace gosFX { class ShardCloud__Specification; }

namespace mc2 {
namespace particles {

// Spawn the starting population of a ShardCloud from its spec.
//
// ShardCloud is a SpinningCloud subclass (which extends ParticleCloud), so
// the starting-population semantics MATCH PointCloud / SpawnPoint: N
// particles spawn at Effect::Start, where N is sampled from
// spec->m_startingPopulation at age=0 / parentSeed. Each particle gets a
// random per-particle seed in [m_minimumChildSeed, m_maximumChildSeed], a
// random emitter-volume offset (m_emitterSize{X,Y,Z}), and an initial
// velocity (m_startingSpeed / m_minimum/maximumDeviation). The shard-
// specific additions are m_size (per-particle world-space radius, sampled
// once at age=0 / child_seed) and m_pScale (a multiplier on radius; see
// SpinningCloud::AnimateParticle). The two collapse to GpuParticle.size
// at spawn time; the legacy per-frame size update is folded into the GPU
// shader's age-extrapolation path (filed as B2 polish debt).
//
// The Shard-specific rotation/angularity attributes (m_localRotation,
// m_angle = sin(spec->m_angularity)) describe a TRIANGLE billboard whose
// per-vertex offsets depend on the camera direction (shardcloud.cpp:354-
// 411). The current GpuParticle schema does not carry rotation or a per-
// vertex angularity factor, so the GPU billboard pass renders shards as
// camera-facing quads at the spawn-time size — visually equivalent at the
// per-particle footprint scale, and the rotation/triangle-shape fidelity
// is filed as B2 polish debt (will require a GpuParticle schema extension
// plus a shader path that emits 3 verts instead of 6).
//
// Inputs:
//   spec           - ShardCloud spec (curves for population / emitter
//                    volume / velocity / color / lifetime / size / scale).
//                    Must be non-null; C7 wiring resolves via
//                    SpecLibrary::Find + RTTI.
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
//     Effect, matching legacy gosFX::Effect::Draw key schema at
//     mclib/gosfx/effect.cpp:697 — Draw fires once per cloud regardless
//     of particle count, so SpawnShard matches that cardinality; the
//     legacy ShardCloud::Draw at shardcloud.cpp:659 calls DrawEffect
//     exactly once per cloud).
//   - No-op when Batcher::is_enabled() is false (cheap early-out AFTER
//     the FX_TRACE_DRAW count, which is the per-spec invocation oracle).
//
// Pre-conditions:
//   spec != nullptr. parentToWorld may be null only if caller knows the
//   producer always supplies one (gosFX always does).
void SpawnShard(const gosFX::ShardCloud__Specification* spec,
                const glm::mat4*            parentToWorld,
                float                                    spawnSeed,
                float                              callerAge = 0.5f);

}  // namespace particles
}  // namespace mc2
