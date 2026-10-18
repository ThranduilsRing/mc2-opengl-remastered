//==========================================================================//
// File:    spawn_tube.h                                                     //
// Contents: mc2::particles::SpawnTube - CPU spawn-event entry point for    //
//           the legacy gosFX::Tube primitive class.                         //
//                                                                           //
//           Per integrated plan v6 §5.4 B1 Stage 2' C6. Replaces the        //
//           legacy gosFX::Tube Execute()+Draw() per-frame chain with a      //
//           single spawn-event Emit() into the GPU particle batcher.        //
//                                                                           //
//           CPU does NOT project. The authoritative list of forbidden       //
//           projection wrappers lives in                                    //
//           scripts/check-particles-no-cpu-projection.sh.                   //
//                                                                           //
//           NOT YET WIRED into producers - wiring lands at C7. C6 only      //
//           ships the function so it builds clean and is ready to call.    //
//===========================================================================//
//
// Schema-fidelity note (LARGE B2 polish debt - bigger than any prior C):
//
//   Tube is a SWEPT-MESH primitive, not a particle cloud. Each Effect emits
//   PROFILES (cross-sections) over time at the head of the tube as the
//   parent transform moves through the world; consecutive profiles connect
//   into a continuous indexed triangle mesh (m_mesh,
//   MidLevelRenderer::MLRIndexedTriangleCloud at tube.hpp:174). The profile
//   shape is one of {Ribbon, AlignedRibbon, Triangle, Square, Cross,
//   Pentagon, Hexagon, VerticalRibbon} (tube.hpp:94-103).
//
//   The 64-byte GpuParticle schema (mclib/particles/spec.h) carries only
//   position / color / velocity / lifetime / age / size / atlasIndex - it
//   describes a SINGLE camera-facing billboard quad. There is no way to
//   represent a continuous swept profile mesh in this schema. Even the
//   degraded "row of billboards along the parent path" mapping requires
//   per-frame spawn-event invocations (continuous over the tube's life),
//   which is incompatible with the C3-C5 spawn-event-at-Start lifecycle.
//
//   Per the v6 plan's "map to nearest representable attribute, log the
//   gap, file as B2 polish debt" rule: C6 emits a SINGLE marker
//   GpuParticle at Effect::Start representing the tube's head position
//   with size = max(m_emitterSize, m_pUSize, m_pVSize) as the bounding
//   footprint. This preserves the FX_TRACE_DRAW count cardinality (one
//   per-Effect draw, matching legacy tube.cpp:1148 Draw cardinality) and
//   reserves a slot in the GPU batcher so producer wiring at C7 has
//   uniform call-shape across all five primitives.
//
//   Full Tube fidelity requires its own primitive type (swept-mesh on
//   GPU, distinct from the billboard quad pass). Filed as B2 polish debt
//   alongside the angular/rotation gaps from SpawnShard. See the C6 plan
//   ticket for the proposed extension shape.
//

#pragma once

#include "spec.h"

#include "../math/glm_config.h"
namespace gosFX { class Tube__Specification; }

namespace mc2 {
namespace particles {

// Spawn-event entry point for a Tube effect.
//
// Tube is a swept-mesh primitive (see header comment above for why the
// current GpuParticle schema cannot represent it faithfully). C6 emits
// ONE marker GpuParticle per spawn to preserve FX_TRACE_DRAW cardinality
// and producer-call uniformity; the continuous profile sweep + mesh
// draw is filed as B2 polish debt.
//
// Inputs:
//   spec           - Tube spec (curves for profilesPerSecond / lifespan /
//                    emitter size / displacement / scale / color / UV).
//                    Must be non-null; C7 wiring resolves via
//                    SpecLibrary::Find + RTTI.
//   parentToWorld  - Effect parent transform. World-space spawn position
//                    is its translation. May be null (treated as identity).
//   spawnSeed      - per-Effect random seed (0..1). Used to sample the
//                    spec's SeededCurves at age=0 for color / lifetime /
//                    size proxies. Producers that don't have a seed
//                    should pass 0.5f (median).
//
// Side effects:
//   - Pushes ONE GpuParticle marker into Batcher::Instance() when the
//     GPU particle path is enabled.
//   - Increments FX_TRACE_DRAW(spec->m_name) ONCE per Spawn call,
//     matching the legacy per-Effect Draw cardinality at
//     mclib/gosfx/tube.cpp:1148 (Draw calls DrawEffect exactly once per
//     Tube when m_activeProfileCount > 1).
//   - No-op (after FX_TRACE_DRAW) when Batcher::is_enabled() is false.
//
// Pre-conditions:
//   spec != nullptr. parentToWorld may be null only if caller knows the
//   producer always supplies one (gosFX always does).
void SpawnTube(const gosFX::Tube__Specification* spec,
               const glm::mat4*      parentToWorld,
               float                              spawnSeed,
               float                              callerAge = 0.5f);

}  // namespace particles
}  // namespace mc2
