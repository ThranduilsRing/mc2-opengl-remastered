//==========================================================================//
// File:    spawn_point.h                                                    //
// Contents: mc2::particles::SpawnPoint — CPU spawn-event entry point for   //
//           the legacy gosFX::PointCloud primitive class.                   //
//                                                                           //
//           Per integrated plan §5.4 B1 Stage 2' C4. Replaces the legacy   //
//           gosFX::PointCloud Execute()+Draw() per-frame chain with a       //
//           single spawn-event Emit() of the cloud's starting population    //
//           into the GPU particle batcher; the GPU billboard pass owns      //
//           animation (advection, color curves) and projection.             //
//                                                                           //
//           CPU does NOT project. Forbidden projection wrappers are         //
//           enumerated by scripts/check-particles-no-cpu-projection.sh;     //
//           see that script for the authoritative list.                     //
//                                                                           //
//           NOT YET WIRED into producers — wiring lands at C7. C4 only     //
//           ships the function so it builds clean and is ready to call.    //
//===========================================================================//

#pragma once

#include "spec.h"

#include "../math/glm_config.h"
namespace gosFX { class PointCloud__Specification; }

namespace mc2 {
namespace particles {

// Spawn the starting population of a PointCloud from its spec.
//
// Unlike SpawnCard (which is a Singleton — exactly one particle per call),
// a PointCloud spawns N particles per Effect::Start, where N is sampled
// from spec->m_startingPopulation at age=0 / parentSeed. Each particle
// gets a random per-particle seed (within m_minimumChildSeed..m_maximum-
// ChildSeed), a random emitter-volume offset (m_emitterSize{X,Y,Z}), and
// an initial velocity from m_startingSpeed / m_minimumDeviation /
// m_maximumDeviation. The GPU shader extrapolates age + advection from
// the baked spawn timestamp + velocity per the Stage 2' GpuParticle
// schema (see spec.h).
//
// Inputs:
//   spec           - PointCloud spec (curves for population / emitter
//                    volume / velocity / color / lifetime). Must be
//                    non-null; C7 wiring resolves via SpecLibrary::Find +
//                    RTTI.
//   parentToWorld  - Effect parent transform. Each particle's local-space
//                    emitter-volume offset is transformed to world for the
//                    GPU spawn position. May be null for world-space
//                    spawn callers (treated as identity).
//   spawnSeed      - per-Effect random seed (0..1). Used to sample the
//                    spec's SeededCurves at age=0 for population / emitter
//                    size / child-seed range. Producers that don't have a
//                    seed should pass 0.5f (median).
//
// Side effects:
//   - Pushes one GpuParticle record per starting-population particle into
//     Batcher::Instance() when the GPU particle path is enabled.
//   - Increments FX_TRACE_DRAW(spec->m_name) ONCE per Spawn call (per-
//     Effect, matching legacy gosFX::Effect::Draw key schema at
//     mclib/gosfx/effect.cpp:697 — Draw fires once per cloud regardless
//     of particle count, so SpawnPoint matches that cardinality).
//   - No-op when Batcher::is_enabled() is false (cheap early-out AFTER
//     the FX_TRACE_DRAW count, which is the per-spec invocation oracle).
//
// Pre-conditions:
//   spec != nullptr. parentToWorld may be null only if caller knows the
//   producer always supplies one (gosFX always does).
void SpawnPoint(const gosFX::PointCloud__Specification* spec,
                const glm::mat4*            parentToWorld,
                float                                    spawnSeed,
                float                              callerAge = 0.5f);

}  // namespace particles
}  // namespace mc2
