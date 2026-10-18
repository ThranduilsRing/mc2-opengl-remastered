//==========================================================================//
// File:    spawn_tube.cpp                                                   //
// Contents: mc2::particles::SpawnTube implementation.                       //
//           Plan v6 §5.4 B1 Stage 2' C6.                                    //
//                                                                           //
// Spawn-event semantics derived from mclib/gosfx/tube.cpp +                 //
// mclib/gosfx/tube.hpp:                                                     //
//                                                                           //
//   - Tube is a SWEPT-MESH Effect, not a particle cloud. The legacy path    //
//     emits a new Profile (cross-section) per spec->m_profilesPerSecond     //
//     tick (tube.cpp:789-816); consecutive profiles connect into a          //
//     continuous indexed triangle mesh (m_mesh at tube.hpp:174). Profile    //
//     shape is one of {Ribbon, AlignedRibbon, Triangle, Square, Cross,      //
//     Pentagon, Hexagon, VerticalRibbon} per spec->m_profileType            //
//     (tube.hpp:94-103).                                                    //
//                                                                           //
//   - Legacy Start (tube.cpp:725-744) primes m_birthAccumulator = 1.0f so   //
//     the first Execute() tick creates two initial profiles. Profiles are   //
//     continuously birthed by Execute; Draw fires DrawEffect ONCE per Tube  //
//     (tube.cpp:1148-1286) only when m_activeProfileCount > 1.              //
//                                                                           //
//   - The 64-byte GpuParticle schema (mclib/particles/spec.h) is a single   //
//     camera-facing billboard quad. It cannot represent a swept profile     //
//     mesh in any reasonable mapping. See the spawn_tube.h header comment   //
//     for the full LARGE B2 polish debt rationale.                          //
//                                                                           //
//   - C6 strategy (per v6 plan "map to nearest representable attribute,    //
//     log gap, file as B2 debt"): emit ONE marker GpuParticle at the       //
//     parent transform origin at Effect::Start, sized to the bounding      //
//     footprint of the tube's profile template (max of m_emitterSize{X,Y,  //
//     Z} and m_pUSize / m_pVSize sampled at age=0/seed). This preserves    //
//     the FX_TRACE_DRAW per-Effect cardinality, reserves a uniform call    //
//     shape across all five C3-C6 spawn entry points for C7 wiring, and    //
//     provides a low-fidelity footprint marker so the GPU pass has         //
//     something at the spawn location. Continuous swept-profile fidelity   //
//     requires a separate primitive type and is filed as B2 polish debt.   //
//                                                                           //
//   - Color sampled at parent_age=0 / spawnSeed from m_pRed/Green/Blue/    //
//     Alpha (matches the per-profile sampling at tube.cpp:1012-1014, taken //
//     at age=0 since the marker represents the spawn-frame state).        //
//                                                                           //
//   - Lifetime sourced from the per-profile m_pLifeSpan (tube.hpp:63 +    //
//     tube.cpp:1002) sampled at (parent_age=0, spawnSeed). Tube has no    //
//     Effect-base m_lifeSpan override; the per-profile span is the        //
//     authoritative time-scale for the marker quad.                       //
//                                                                           //
// CPU projection invariant: this file MUST NOT include or reference any   //
// of the forbidden projection wrappers - the authoritative list lives    //
// in scripts/check-particles-no-cpu-projection.sh.                         //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/tube.hpp"
#include "spawn_tube.h"
#include "spawn.h"   // resolveSampleAge (VFX-AGE-SAMPLE-1)
#include "batcher.h"
#include "fx_trace/fx_trace.h"

#include "../math/mc2_xform.h"

namespace mc2 {
namespace particles {

void SpawnTube(const gosFX::Tube__Specification* spec,
               const glm::mat4*      parentToWorld,
               float                              spawnSeed,
               float                              callerAge)
{
    if (!spec) {
        return;
    }

    // FX_TRACE_DRAW key schema matches the legacy gosFX::Effect::Draw entry
    // (effect.cpp:697) - ONE event per spawn. Legacy Tube::Draw at
    // tube.cpp:1148 calls DrawEffect exactly once per cloud (only when
    // m_activeProfileCount > 1, but for trace-count purposes we match the
    // per-Effect cardinality regardless - same convention as SpawnShard /
    // SpawnPoint where the trace fires even when the spawn population is
    // empty).
    FX_TRACE_DRAW(spec->m_name);

    // Cheap early-out before sampling curves: when MC2_GPU_PARTICLES=0
    // the new path is dormant and we still want the FX_TRACE_DRAW count
    // (it is the per-spec invocation oracle, not the GPU activation flag).
    if (!Batcher::is_enabled()) {
        return;
    }

    // Const_cast because the legacy curve ComputeValue() methods are not
    // const-correct (FCurve internals mutate cache state). SpecLibrary
    // owns the spec lifetime, so the cast is safe for read-only sampling.
    gosFX::Tube__Specification* mut_spec =
        const_cast<gosFX::Tube__Specification*>(spec);

    // Sample at parent_age=0.5 (mid-life / peak-visibility). See spawn_card.cpp
    // for the rationale: GPU shader doesn't advance age so age=0 bakes fade-in
    // invisibility into the marker particle.
    const Stuff::Scalar parent_age  = resolveSampleAge(callerAge);
    const Stuff::Scalar parent_seed = spawnSeed;

    // World-space spawn position is the parent transform translation
    // (tube.cpp:769 - local_to_world = m_localToParent * parentToWorld;
    // the head profile is created at this matrix's origin at Start). For
    // the marker we use the translation directly - same idiom as
    // SpawnCard / SpawnShard.
    glm::vec3 worldPos(0.0f, 0.0f, 0.0f);
    if (parentToWorld) {
        worldPos = mc2::math::getTranslation(*parentToWorld);
    }

    // Store raw MLR pool index; resolved to GOS handle at flush time.
    uint32_t mlrTexHandle = 0u;
    {
        const unsigned h = mut_spec->m_state.GetTextureHandle();
        mlrTexHandle = static_cast<uint32_t>(h);
    }

    // UV sub-rect from Tube spec (m_pUOffset / m_pVOffset / m_pUSize / m_pVSize
    // at tube.hpp:81-85). Sampled at (parent_age, parent_seed) for the marker.
    const float uSize = mut_spec->m_pUSize  .ComputeValue(parent_age, parent_seed);
    const float vSize = mut_spec->m_pVSize  .ComputeValue(parent_age, parent_seed);
    const float u0    = mut_spec->m_pUOffset.ComputeValue(parent_age, parent_seed);
    const float v0    = mut_spec->m_pVOffset.ComputeValue(parent_age, parent_seed);

    // Blend mode from MLRState alpha mode (same pattern as spawn_cardcloud.cpp).
    int blendMode = 0;
    {
        const MidLevelRenderer::MLRState::AlphaMode alphaMode =
            mut_spec->m_state.GetAlphaMode();
        if (alphaMode == MidLevelRenderer::MLRState::OneOneMode ||
            alphaMode == MidLevelRenderer::MLRState::AlphaOneMode) {
            blendMode = 1;
        }
    }

    // Register a group with the batcher so the bridge knows which texture,
    // UV rect, and blend mode to use for this tube marker's particle.
    Batcher::Instance().BeginGroup(mlrTexHandle, u0, v0,
                                   (uSize > 0.0f ? uSize : 1.0f),
                                   (vSize > 0.0f ? vSize : 1.0f),
                                   blendMode);

    // Color at age=0 / spawnSeed. Matches the per-profile color sampling
    // at tube.cpp:1012-1014.
    const Stuff::Scalar r = mut_spec->m_pRed  .ComputeValue(parent_age, parent_seed);
    const Stuff::Scalar g = mut_spec->m_pGreen.ComputeValue(parent_age, parent_seed);
    const Stuff::Scalar b = mut_spec->m_pBlue .ComputeValue(parent_age, parent_seed);
    const Stuff::Scalar a = mut_spec->m_pAlpha.ComputeValue(parent_age, parent_seed);

    // Per-profile lifetime sourced from Tube's m_pLifeSpan (tube.hpp:63;
    // sampled at tube.cpp:1002 with per-profile seed). For the marker we
    // sample at (parent_age=0, spawnSeed) - matches the spawn-frame head
    // profile's lifetime. Min-clamp to a positive value so the GPU shader
    // doesn't divide by zero when computing normalized age.
    Stuff::Scalar lifetime =
        mut_spec->m_pLifeSpan.ComputeValue(parent_age, parent_seed);
    if (lifetime <= 0.0f) {
        lifetime = 1.0f;
    }

    // Footprint marker size: bounding radius of the tube profile template
    // at spawn time. Components:
    //   - m_emitterSize{X,Y,Z}: the per-profile spawn-volume scale
    //     (tube.cpp - the emitter envelope along each axis).
    //   - m_pUSize / m_pVSize: the profile-template UV size curves
    //     (tube.hpp:84-85) - effectively the cross-section dimension in
    //     the tube's local frame at spawn.
    // We take the max of these projections so the marker quad bounds the
    // expected tube footprint at the spawn location. This is a low-
    // fidelity proxy; the full swept mesh is B2 polish debt (see header).
    const Stuff::Scalar ex =
        mut_spec->m_emitterSizeX.ComputeValue(parent_age, parent_seed);
    const Stuff::Scalar ey =
        mut_spec->m_emitterSizeY.ComputeValue(parent_age, parent_seed);
    const Stuff::Scalar ez =
        mut_spec->m_emitterSizeZ.ComputeValue(parent_age, parent_seed);
    const Stuff::Scalar usz =
        mut_spec->m_pUSize.ComputeValue(parent_age, parent_seed);
    const Stuff::Scalar vsz =
        mut_spec->m_pVSize.ComputeValue(parent_age, parent_seed);

    Stuff::Scalar size = ex;
    if (ey  > size) size = ey;
    if (ez  > size) size = ez;
    if (usz > size) size = usz;
    if (vsz > size) size = vsz;
    if (size < 0.0f) size = 0.0f;

    GpuParticle p = {};
    p.position[0] = worldPos.x;
    p.position[1] = worldPos.y;
    p.position[2] = worldPos.z;
    p.color[0]    = r;
    p.color[1]    = g;
    p.color[2]    = b;
    p.color[3]    = a;
    // Tube head is anchored to the parent transform; the legacy Execute
    // path advects new profiles by moving the parent, not by per-particle
    // velocity. Leave velocity[] at zero for the marker.
    p.lifetime    = (float)lifetime;
    p.age         = 0.0f;
    p.size        = (float)size;
    // atlasIndex carries the raw MLR pool index; Batcher::ResolveTextures()
    // converts it to a gos_TextureHandle after renderLists() / LoadImages().
    p.atlasIndex  = mlrTexHandle;

    Batcher::Instance().Emit(p);
}

}  // namespace particles
}  // namespace mc2
