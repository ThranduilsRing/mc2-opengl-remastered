//==========================================================================//
// File:    spawn_card.h                                                     //
// Contents: mc2::particles::SpawnCard — CPU spawn-event entry point for    //
//           the legacy gosFX::Card primitive class.                         //
//                                                                           //
//           Per integrated plan §5.4 B1 Stage 2' C3. Replaces the legacy   //
//           gosFX::Card Execute()+Draw() per-frame chain with a single     //
//           spawn-event Emit() into the GPU particle batcher; the GPU      //
//           billboard pass owns animation, projection, and rasterization.  //
//                                                                           //
//           CPU does NOT project. This function MUST NOT reference any of  //
//           the forbidden projection wrappers enumerated by the CI gate    //
//           scripts/check-particles-no-cpu-projection.sh.                  //
//                                                                           //
//           NOT YET WIRED into producers — wiring lands at C7. C3 only     //
//           ships the function so it builds clean and is ready to call.    //
//===========================================================================//

#pragma once

#include "spec.h"

#include "../math/glm_config.h"
namespace gosFX { class Card__Specification; }

namespace mc2 {
namespace particles {

// Spawn one GPU billboard from a Card spec.
//
// Inputs:
//   spec           - Card spec (curves for size / aspect / color / lifetime).
//                    Must be non-null and a Card__Specification* (caller's
//                    responsibility — adapter resolves via SpecLibrary::Find
//                    + RTTI in C7 wiring).
//   parentToWorld  - Effect parent transform. The Card's local origin (0,0,0
//                    in effect space) is transformed to world for the
//                    billboard spawn position. May be the identity for
//                    world-space spawn callers.
//   spawnSeed      - per-instance random seed (0..1). Used to sample the
//                    spec's SeededCurves (color, size) at age=0. Producers
//                    that don't have a seed should pass 0.5f (median).
//
// Side effects:
//   - Pushes one GpuParticle record into Batcher::Instance() per call when
//     the GPU particle path is enabled.
//   - Increments FX_TRACE_DRAW(spec->m_name) — matches gosFX::Effect::Draw
//     per-event keying (see mclib/gosfx/effect.cpp:697 and
//     mclib/fx_trace/fx_trace.h:54). One FX_TRACE_DRAW per spawn event,
//     NOT per emitted particle (a Card is always one particle).
//   - No-op when Batcher::is_enabled() is false (cheap early-out).
//
// Pre-conditions:
//   spec != nullptr. parentToWorld may be null only if caller knows the
//   producer always supplies one (gosFX always does).
void SpawnCard(const gosFX::Card__Specification* spec,
               const glm::mat4*      parentToWorld,
               float                              spawnSeed,
               float                              callerAge = 0.5f);

}  // namespace particles
}  // namespace mc2
