//==========================================================================//
// File:    spawn_card.cpp                                                   //
// Contents: mc2::particles::SpawnCard implementation.                       //
//           Plan v6 §5.4 B1 Stage 2' C3.                                    //
//                                                                           //
// Spawn-event semantics derived from mclib/gosfx/card.cpp:                  //
//                                                                           //
//   - Card is a Singleton effect: one Card instance == exactly one          //
//     particle for its entire lifetime (m_cardCount = 1, see card.cpp:295). //
//   - Legacy per-frame chain: Execute() samples spec->m_halfHeight /        //
//     m_aspectRatio / m_UOffset etc at the instance's m_age (card.cpp:380-  //
//     456), Draw() emits one quad through the MLR clipper (card.cpp:474).   //
//   - Lifetime sourced from the Effect base spec->m_lifeSpan (sampled at    //
//     m_seed per Effect::Start; see effect.cpp:461).                        //
//   - Color sourced from Singleton spec curves m_red/m_green/m_blue/m_alpha //
//     (see singleton.cpp:241-244 ComputeValue(m_age,m_seed)).               //
//   - Size sourced from Card spec m_halfHeight (Card::Start at card.cpp:349 //
//     uses this as the world-space half-extent; m_radius = sqrt(hx^2+hy^2)  //
//     at card.cpp:351).                                                     //
//                                                                           //
// In the new path the GPU shader owns animation (age advance, billboard     //
// alignment, projection). The CPU emits exactly ONE record at spawn time    //
// containing the initial state; the shader extrapolates age from the spawn //
// timestamp baked into the record (Stage 2' lifecycle, see plan §5.4).     //
//                                                                           //
// CPU projection invariant: this file MUST NOT include or reference any    //
// of the forbidden projection wrappers enumerated by the CI gate            //
// scripts/check-particles-no-cpu-projection.sh.                             //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/card.hpp"
#include "spawn_card.h"
#include "spawn.h"   // resolveSampleAge (VFX-AGE-SAMPLE-1)
#include "batcher.h"
#include "fx_trace/fx_trace.h"

#include "../math/mc2_xform.h"

namespace mc2 {
namespace particles {

void SpawnCard(const gosFX::Card__Specification* spec,
               const glm::mat4*      parentToWorld,
               float                              spawnSeed,
               float                              callerAge)
{
    if (!spec) {
        return;
    }

    // FX_TRACE_DRAW key schema matches the legacy gosFX::Effect::Draw entry
    // (effect.cpp:697) — ONE event per spawn (Card is one-particle so the
    // per-Effect-Draw and per-particle keys coincide).
    FX_TRACE_DRAW(spec->m_name);

    // Cheap early-out before sampling curves: when MC2_GPU_PARTICLES=0
    // the new path is dormant and we still want the FX_TRACE_DRAW count
    // (it is the per-spec invocation oracle, not the GPU activation flag).
    if (!Batcher::is_enabled()) {
        return;
    }

    // Sample curves at age=0.5 (mid-life / peak-visibility). The Stage 2'
    // design intent is for the GPU shader to advance age per-frame from the
    // baked spawn timestamp, but the shader currently does not — every
    // particle renders as a fixed snapshot. Sampling at age=0 means
    // fade-in envelopes (alpha rises from 0, halfHeight grows from 0) bake
    // an invisible/degenerate particle. age=0.5 picks the typical peak of
    // canonical 0->1 normalized envelopes. Stage 2' polish: shader-side
    // age advancement + curve evaluation removes the need for this constant.
    const Stuff::Scalar age = resolveSampleAge(callerAge);
    const Stuff::Scalar seed = spawnSeed;

    // Const_cast because the legacy curve ComputeValue() methods are not
    // const-correct (FCurve internals mutate cache state). SpecLibrary
    // owns the spec lifetime, so the cast is safe for read-only sampling.
    gosFX::Card__Specification* mut_spec =
        const_cast<gosFX::Card__Specification*>(spec);

    const Stuff::Scalar halfY = mut_spec->m_halfHeight.ComputeValue(age, seed);
    const Stuff::Scalar halfX =
        halfY * mut_spec->m_aspectRatio.ComputeValue(age, seed);
    // Match Card::Start radius idiom (card.cpp:351). For a square-ish card
    // m_radius is the diagonal half-extent; the GPU billboard pass uses the
    // single 'size' field, so we collapse the X/Y pair to the bounding
    // radius (worst-case footprint preserves visibility parity).
    const Stuff::Scalar radius = Stuff::Sqrt(halfX * halfX + halfY * halfY);

    const Stuff::Scalar r = mut_spec->m_red  .ComputeValue(age, seed);
    const Stuff::Scalar g = mut_spec->m_green.ComputeValue(age, seed);
    const Stuff::Scalar b = mut_spec->m_blue .ComputeValue(age, seed);
    const Stuff::Scalar a = mut_spec->m_alpha.ComputeValue(age, seed);

    // Effect base spec lifetime (effect.cpp:461 uses ComputeValue(m_seed,
    // 0.0f) — the lifeSpan curve takes seed as its primary axis).
    const Stuff::Scalar lifetime =
        mut_spec->m_lifeSpan.ComputeValue(seed, 0.0f);

    // World-space spawn position is parent transform * local origin. Card
    // local origin is (0,0,0) in effect space (card.cpp:442-456 builds
    // vertices around the origin). Same idiom as card.cpp:503 (Point3D
    // card_in_world(local_to_world)) — extracts the translation from the
    // affine matrix.
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

    // UV sub-rect from Card spec. m_UOffset / m_VOffset / m_USize / m_VSize
    // are constant or seeded curves on Card__Specification (card.cpp:43-66).
    // For non-animated specs these are typically 0,0,1,1 (full page).
    float uSize = mut_spec->m_USize  .ComputeValue(age, seed);
    float vSize = mut_spec->m_VSize  .ComputeValue(age, seed);
    float u0    = mut_spec->m_UOffset.ComputeValue(age, seed);
    float v0    = mut_spec->m_VOffset.ComputeValue(age, seed);

    // Animated atlas frame selection (matches card.cpp:398-413 Execute path).
    if (mut_spec->m_animated && mut_spec->m_width > 0) {
        Stuff::Scalar frameF =
            mut_spec->m_index.ComputeValue(age, seed);
        if (frameF < 0.0f) frameF = 0.0f;
        const int frame = static_cast<int>(frameF);
        const int col   = frame % static_cast<int>(mut_spec->m_width);
        const int row   = frame / static_cast<int>(mut_spec->m_width);
        u0 += col * uSize;
        v0 += row * vSize;
    }

    // Blend mode from MLRState alpha mode (same as spawn_cardcloud.cpp).
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
    // UV rect, and blend mode to use for this card's particle.
    Batcher& batcher = Batcher::Instance();
    batcher.BeginGroup(mlrTexHandle, u0, v0,
                       (uSize > 0.0f ? uSize : 1.0f),
                       (vSize > 0.0f ? vSize : 1.0f),
                       blendMode);

    GpuParticle p = {};
    p.position[0] = worldPos.x;
    p.position[1] = worldPos.y;
    p.position[2] = worldPos.z;
    p.color[0]    = r;
    p.color[1]    = g;
    p.color[2]    = b;
    p.color[3]    = a;
    // Card has no per-particle velocity (it's a billboard locked to the
    // parent transform in the legacy path). Leave velocity[] at zero.
    p.lifetime    = (lifetime > 0.0f) ? (float)lifetime : 1.0f;
    p.age         = 0.0f;
    p.size        = (float)radius;
    // atlasIndex carries the raw MLR pool index; Batcher::ResolveTextures()
    // converts it to a gos_TextureHandle after renderLists() / LoadImages().
    p.atlasIndex  = mlrTexHandle;

    batcher.Emit(p);
}

}  // namespace particles
}  // namespace mc2
