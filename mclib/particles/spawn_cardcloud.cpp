//==========================================================================//
// File:    spawn_cardcloud.cpp                                              //
// Contents: mc2::particles::SpawnCardCloud implementation.                  //
//           Plan v6 §5.4 B1 Stage 2' C11.                                   //
//                                                                           //
// Spawn-event semantics derived from mclib/gosfx/cardcloud.cpp +            //
// mclib/gosfx/spinningcloud.cpp + mclib/gosfx/particlecloud.cpp:            //
//                                                                           //
//   - CardCloud is a multi-particle Effect. It derives from SpinningCloud   //
//     -> ParticleCloud, so starting population at Effect::Start is sampled  //
//     from spec->m_startingPopulation at age=0 / parentSeed (particle-      //
//     cloud.cpp:351). Birth-rate-driven ongoing spawns                      //
//     (m_particlesPerSecond) are RUNTIME spawns folded into the GPU         //
//     animation pass per the v6 plan (B2 polish debt).                      //
//   - Per-particle attributes (seed / lifetime / emitter offset / velocity ///
//     color) follow the same ParticleCloud chain as PointCloud/ShardCloud.  //
//   - CardCloud-specific size: halfY = m_halfHeight at (age=0, child_seed); //
//     halfX = halfY * m_aspectRatio at (age=0, child_seed); bounding radius //
//     = sqrt(halfX^2 + halfY^2) (cardcloud.cpp:375-383). We bake the radius //
//     into GpuParticle.size — the GPU billboard pass emits a camera-facing  //
//     quad whose footprint matches the legacy bounding extent.              //
//   - Legacy Draw() fires DrawEffect once per cloud (effect.cpp:697); we    //
//     match by firing FX_TRACE_DRAW once per SpawnCardCloud call.           //
//                                                                           //
// Schema-fidelity note:                                                      //
//   - m_pIndex / m_animated / m_U/VOffset / m_U/VSize / m_width: DONE as   //
//     FX-GPU-1 Phase 2. UV sub-rect + animated first frame stored as        //
//     per-group GroupInfo metadata via BeginGroup; GpuParticle 64-byte      //
//     schema is unchanged. Per-particle frame variation remains B2 debt.    //
//   - m_localRotation (per-particle spin from SpinningCloud) is not in the  //
//     schema. Quads are camera-facing axis-aligned. B2 polish debt.         //
//                                                                           //
// CPU projection invariant: this file MUST NOT include or reference any    //
// of the forbidden projection wrappers — the authoritative list lives      //
// in scripts/check-particles-no-cpu-projection.sh.                          //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/cardcloud.hpp"
#include "spawn_cardcloud.h"
#include "spawn.h"   // resolveSampleAge (VFX-AGE-SAMPLE-1)
#include "batcher.h"
#include "fx_trace/fx_trace.h"

#include <stuff/linearmatrix.hpp>
#include <stuff/point3d.hpp>
#include <stuff/random.hpp>
#include "math/stuff_glm_bridge.h"

#include <string>
#include <unordered_set>
#include "mc2fmt.h"

namespace mc2 {
namespace particles {

void SpawnCardCloud(const gosFX::CardCloud__Specification* spec,
                    const Stuff::LinearMatrix4D*           parentToWorld,
                    float                                   spawnSeed,
                    float                                   callerAge)
{
    if (!spec) {
        return;
    }

    // FX_TRACE_DRAW key schema matches the legacy gosFX::Effect::Draw entry
    // (effect.cpp:697) — ONE event per spawn. Legacy CardCloud::Draw fires
    // DrawEffect once per cloud regardless of particle count.
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
    gosFX::CardCloud__Specification* mut_spec =
        const_cast<gosFX::CardCloud__Specification*>(spec);

    // Sample curves at parent_age=0.5 (mid-life / peak-visibility). See
    // spawn_card.cpp for the rationale: the GPU shader does not advance
    // age, so age=0 bakes fade-in invisibility into every particle. 0.5
    // picks the typical peak of normalized 0->1 envelopes. Stage 2' polish
    // (shader-side age advancement) retires this constant.
    const Stuff::Scalar parent_age  = resolveSampleAge(callerAge);
    const Stuff::Scalar parent_seed = spawnSeed;

    Stuff::Scalar population_f =
        mut_spec->m_startingPopulation.ComputeValue(parent_age, parent_seed);
    if (population_f < 0.0f) {
        population_f = 0.0f;
    }
    int population = static_cast<int>(population_f);
    if (population > mut_spec->m_maxParticleCount) {
        population = mut_spec->m_maxParticleCount;
    }
    if (population <= 0) {
        return;
    }

    // Child-seed range — sampled per-Effect at (parent_age=0, parent_seed).
    const Stuff::Scalar min_seed =
        mut_spec->m_minimumChildSeed.ComputeValue(parent_age, parent_seed);
    Stuff::Scalar seed_range =
        mut_spec->m_maximumChildSeed.ComputeValue(parent_age, parent_seed) - min_seed;
    if (seed_range < 0.0f) {
        seed_range = 0.0f;
    }

    Stuff::Point3D parent_origin(0.0f, 0.0f, 0.0f);
    if (parentToWorld) {
        parent_origin = Stuff::Point3D(*parentToWorld);
    }

    // Store raw MLR pool index; resolved to GOS handle at flush time (post-renderLists)
    // by Batcher::ResolveTextures(). At spawn time GOSImage::GetHandle() returns 0
    // because MLRTexturePool::LoadImages() has not yet been called.
    uint32_t mlrTexHandle = 0u;
    {
        const unsigned h = mut_spec->m_state.GetTextureHandle();
        mlrTexHandle = static_cast<uint32_t>(h);
    }

    // Per-spec probe: gated behind MC2_GPU_PARTICLES_LOG=1 (first occurrence only).
    if (Batcher::is_log_enabled()) {
        static std::unordered_set<std::string> s_probed;
        const char* specNameC = static_cast<const char*>(spec->m_name);
        std::string specName(specNameC ? specNameC : "<null>");
        if (s_probed.insert(specName).second) {
            mc2::fmt::print_err("[SPAWN_PROBE] spec={} mlrHandle={:d} renderState=0x{:08x} permMask=0x{:08x} deltaMask=0x{:08x}\n", specName.c_str(), mc2::fmt::as_u32(mlrTexHandle), mc2::fmt::as_u32(mut_spec->m_state.GetRenderStateFlags()), mc2::fmt::as_u32(mut_spec->m_state.GetRenderPermissionMask()), mc2::fmt::as_u32(mut_spec->m_state.GetRenderDeltaMask()));
        }
    }

    // P2-1: Read the UV sub-rect from the spec for the first atlas frame.
    // m_UOffset / m_VOffset / m_USize / m_VSize are ConstantCurves; we
    // sample them at (parent_age, parent_seed) — the same sample point used
    // for all other per-cloud attributes. For non-animated specs these values
    // are typically 0,0,1,1 (full page); for atlas sprites they describe the
    // first frame.
    //
    // P2-2: Animated atlas frame selection.
    // If spec->m_animated, compute the first-frame column/row from
    // m_pIndex at (parent_age, child_seed=parent_seed) and m_width, then
    // offset the UV origin by (col * uSize, row * vSize). We use
    // child_seed=parent_seed as a representative sample; per-particle
    // variation is a B2 polish item (requires per-particle BeginGroup calls).
    const float uSize = mut_spec->m_USize  .ComputeValue(parent_age, parent_seed);
    const float vSize = mut_spec->m_VSize  .ComputeValue(parent_age, parent_seed);
    float u0 = mut_spec->m_UOffset.ComputeValue(parent_age, parent_seed);
    float v0 = mut_spec->m_VOffset.ComputeValue(parent_age, parent_seed);

    if (mut_spec->m_animated && mut_spec->m_width > 0) {
        // Sample the frame index curve at (parent_age, parent_seed).
        // m_pIndex is a SeededCurveOf returning a scalar frame index.
        Stuff::Scalar frameF =
            mut_spec->m_pIndex.ComputeValue(parent_age, parent_seed);
        if (frameF < 0.0f) frameF = 0.0f;
        const int frame  = static_cast<int>(frameF);
        const int col    = frame % static_cast<int>(mut_spec->m_width);
        const int row    = frame / static_cast<int>(mut_spec->m_width);
        u0 += col * uSize;
        v0 += row * vSize;
    }

    // Determine blend mode from the spec's MLRState alpha mode.
    // OneOneMode  (SRC_ONE,   DST_ONE)   = additive (sparks, laser trails)
    // AlphaOneMode (SRC_ALPHA, DST_ONE)  = additive with alpha modulation
    // All other modes (AlphaInvAlphaMode etc.) = standard alpha blend.
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
    // UV rect, and blend mode to use for this cloud's particles.
    Batcher& batcher = Batcher::Instance();
    batcher.BeginGroup(mlrTexHandle, u0, v0,
                       (uSize > 0.0f ? uSize : 1.0f),
                       (vSize > 0.0f ? vSize : 1.0f),
                       blendMode);

    for (int i = 0; i < population; ++i) {
        // Per-particle seed in [min_seed, min_seed + seed_range], clamped
        // to [0,1].
        Stuff::Scalar child_seed =
            Stuff::Random::GetFraction() * seed_range + min_seed;
        if (child_seed < 0.0f) child_seed = 0.0f;
        else if (child_seed > 1.0f) child_seed = 1.0f;

        // Per-particle lifetime; 33ms floor matches particlecloud.cpp:550.
        Stuff::Scalar lifetime =
            mut_spec->m_pLifeSpan.ComputeValue(parent_age, child_seed);
        if (lifetime < 0.0333333f) {
            lifetime = 0.0333333f;
        }

        // Per-particle emitter-volume offset (legacy unit-sphere-ish
        // YawPitchRange sample, particlecloud.cpp:558-563).
        mc2::math::YawPitchRange initial_p(
            Stuff::Random::GetFraction() * Stuff::Two_Pi,
            Stuff::Random::GetFraction() * Stuff::Pi - Stuff::Pi_Over_2,
            Stuff::Random::GetFraction());
        Stuff::Vector3D unit_pos = mc2::math::toVector3D(mc2::math::polarToVector(initial_p));
        Stuff::Point3D local_offset(
            unit_pos.x * mut_spec->m_emitterSizeX.ComputeValue(parent_age, child_seed),
            unit_pos.y * mut_spec->m_emitterSizeY.ComputeValue(parent_age, child_seed),
            unit_pos.z * mut_spec->m_emitterSizeZ.ComputeValue(parent_age, child_seed));

        Stuff::Point3D world_pos;
        if (parentToWorld) {
            world_pos.Multiply(local_offset, *parentToWorld);
        } else {
            world_pos = local_offset;
        }

        // Initial velocity (matches particlecloud.cpp:577-591).
        Stuff::Scalar pitch_min =
            mut_spec->m_minimumDeviation.ComputeValue(parent_age, child_seed);
        Stuff::Scalar pitch_range =
            mut_spec->m_maximumDeviation.ComputeValue(parent_age, child_seed) - pitch_min;
        if (pitch_range < 0.0f) {
            pitch_range = 0.0f;
        }
        pitch_min +=
            pitch_range * Stuff::Random::GetFraction() - Stuff::Pi_Over_2;
        mc2::math::YawPitchRange initial_v(
            Stuff::Random::GetFraction() * Stuff::Two_Pi,
            pitch_min,
            mut_spec->m_startingSpeed.ComputeValue(parent_age, child_seed));
        Stuff::Vector3D local_velocity = mc2::math::toVector3D(mc2::math::polarToVector(initial_v));
        Stuff::Vector3D world_velocity;
        if (parentToWorld) {
            world_velocity.Multiply(local_velocity, *parentToWorld);
        } else {
            world_velocity = local_velocity;
        }

        // Color at age=0 / child_seed.
        const Stuff::Scalar r = mut_spec->m_pRed  .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar g = mut_spec->m_pGreen.ComputeValue(parent_age, child_seed);
        const Stuff::Scalar b = mut_spec->m_pBlue .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar a = mut_spec->m_pAlpha.ComputeValue(parent_age, child_seed);

        // CardCloud-specific size: collapse legacy per-axis halfX/halfY to
        // a single bounding-radius scalar (cardcloud.cpp:375-383).
        Stuff::Scalar halfY =
            mut_spec->m_halfHeight.ComputeValue(parent_age, child_seed);
        if (halfY < 0.0f) halfY = 0.0f;
        const Stuff::Scalar aspect =
            mut_spec->m_aspectRatio.ComputeValue(parent_age, child_seed);
        Stuff::Scalar halfX = halfY * aspect;
        if (halfX < 0.0f) halfX = 0.0f;
        const Stuff::Scalar size =
            Stuff::Sqrt(halfX * halfX + halfY * halfY);

        GpuParticle p = {};
        p.position[0] = world_pos.x;
        p.position[1] = world_pos.y;
        p.position[2] = world_pos.z;
        p.color[0]    = r;
        p.color[1]    = g;
        p.color[2]    = b;
        p.color[3]    = a;
        p.velocity[0] = world_velocity.x;
        p.velocity[1] = world_velocity.y;
        p.velocity[2] = world_velocity.z;
        p.lifetime    = (float)lifetime;
        p.age         = 0.0f;
        // P2-1/P2-2: UV sub-rect and animated frame handled above via
        // BeginGroup(mlrTexHandle, u0, v0, us, vs). The GpuParticle schema
        // carries atlasIndex for texture routing; the UV rect is per-group
        // metadata in GroupInfo, not per-particle.
        // Remaining B2 polish debt: per-particle rotation (m_localRotation
        // from SpinningCloud) is not in the 64-byte schema; quads are
        // camera-facing axis-aligned.
        p.size        = (float)size;
        // atlasIndex carries the raw MLR pool index; Batcher::ResolveTextures()
        // converts it to a gos_TextureHandle after renderLists() / LoadImages().
        p.atlasIndex  = mlrTexHandle;

        batcher.Emit(p);
    }
}

}  // namespace particles
}  // namespace mc2
