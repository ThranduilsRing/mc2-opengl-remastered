//==========================================================================//
// File:    spawn_point.cpp                                                  //
// Contents: mc2::particles::SpawnPoint implementation.                      //
//           Plan v6 §5.4 B1 Stage 2' C4.                                    //
//                                                                           //
// Spawn-event semantics derived from mclib/gosfx/pointcloud.cpp +           //
// mclib/gosfx/particlecloud.cpp:                                            //
//                                                                           //
//   - PointCloud is a multi-particle Effect. Starting population at         //
//     Effect::Start time is sampled from spec->m_startingPopulation         //
//     at age=0 / parentSeed (particlecloud.cpp:351); birth-rate-driven      //
//     ongoing spawns (m_particlesPerSecond) are RUNTIME spawns, not the     //
//     responsibility of this entry point — they are folded into the GPU    //
//     animation pass per the v6 plan (filed as B2 polish debt; v1 of the   //
//     GPU path emits the starting population only).                         //
//   - Each starting particle is built by ParticleCloud::CreateNewParticle   //
//     (particlecloud.cpp:523): random child seed in [m_minimumChildSeed,   //
//     m_maximumChildSeed], lifetime from m_pLifeSpan at (parent_age=0,     //
//     child_seed), random emitter-volume offset scaled by                   //
//     m_emitterSize{X,Y,Z}, random initial velocity from m_startingSpeed / //
//     m_minimum/maximumDeviation.                                           //
//   - Color is sampled per-particle from m_pRed/Green/Blue/Alpha at        //
//     (age=0, child_seed) — same SeededCurve idiom as Card but on the      //
//     PointCloud spec (pointcloud.cpp:439-442 in legacy AnimateParticle).  //
//   - Legacy Draw() fires DrawEffect once per cloud, not per particle      //
//     (pointcloud.cpp:476 inside the m_activeParticleCount > 0 block).     //
//     FX_TRACE_DRAW therefore fires ONCE per SpawnPoint call (per-Effect), //
//     matching the per-cloud cardinality of legacy Effect::Draw.           //
//                                                                           //
// In the new path the GPU shader owns animation (drag, ether-velocity,    //
// acceleration, color curves, age advance, billboard alignment,            //
// projection). The CPU emits the starting population's t=0 state in one   //
// burst; the shader extrapolates each particle's trajectory from the      //
// spawn timestamp + initial velocity baked into its GpuParticle record.   //
//                                                                           //
// CPU projection invariant: this file MUST NOT include or reference any   //
// of the forbidden projection wrappers — see                               //
// scripts/check-particles-no-cpu-projection.sh for the authoritative list. //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/pointcloud.hpp"
#include "spawn_point.h"
#include "spawn.h"   // resolveSampleAge (VFX-AGE-SAMPLE-1)
#include "batcher.h"
#include "fx_trace/fx_trace.h"

#include <stuff/linearmatrix.hpp>
#include <stuff/point3d.hpp>
#include <stuff/random.hpp>
#include "math/stuff_glm_bridge.h"

namespace mc2 {
namespace particles {

void SpawnPoint(const gosFX::PointCloud__Specification* spec,
                const Stuff::LinearMatrix4D*            parentToWorld,
                float                                    spawnSeed,
                float                                    callerAge)
{
    if (!spec) {
        return;
    }

    // FX_TRACE_DRAW key schema matches the legacy gosFX::Effect::Draw entry
    // (effect.cpp:697) — ONE event per spawn (legacy PointCloud::Draw also
    // fires DrawEffect once per cloud regardless of particle count, so the
    // per-Effect cardinality matches).
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
    gosFX::PointCloud__Specification* mut_spec =
        const_cast<gosFX::PointCloud__Specification*>(spec);

    // Sample curves at parent_age=0.5 (mid-life / peak-visibility). See
    // spawn_card.cpp for the rationale.
    const Stuff::Scalar parent_age  = resolveSampleAge(callerAge);
    const Stuff::Scalar parent_seed = spawnSeed;

    Stuff::Scalar population_f =
        mut_spec->m_startingPopulation.ComputeValue(parent_age, parent_seed);
    if (population_f < 0.0f) {
        population_f = 0.0f;
    }
    // m_maxParticleCount is the hard cap (allocates m_data for that many).
    // Match the legacy birth-loop clamp at particlecloud.cpp:458.
    int population = static_cast<int>(population_f);
    if (population > mut_spec->m_maxParticleCount) {
        population = mut_spec->m_maxParticleCount;
    }
    if (population <= 0) {
        return;
    }

    // Child-seed range — sampled per-Effect at (parent_age=0, parent_seed).
    // Matches particlecloud.cpp:540-543.
    const Stuff::Scalar min_seed =
        mut_spec->m_minimumChildSeed.ComputeValue(parent_age, parent_seed);
    Stuff::Scalar seed_range =
        mut_spec->m_maximumChildSeed.ComputeValue(parent_age, parent_seed) - min_seed;
    if (seed_range < 0.0f) {
        seed_range = 0.0f;
    }

    // Translation from parent transform. Particles spawn at offsets within
    // a per-particle random emitter-volume scaled by m_emitterSize{X,Y,Z}
    // (particlecloud.cpp:558-570); the local offset is transformed through
    // parentToWorld for the GPU world-space spawn position.
    Stuff::Point3D parent_origin(0.0f, 0.0f, 0.0f);
    if (parentToWorld) {
        parent_origin = Stuff::Point3D(*parentToWorld);
    }

    // Store raw MLR pool index; resolved to GOS handle at flush time.
    uint32_t mlrTexHandle = 0u;
    {
        const unsigned h = mut_spec->m_state.GetTextureHandle();
        mlrTexHandle = static_cast<uint32_t>(h);
    }

    // PointCloud has no UV sub-rect curves. Use full-page UV (0,0,1,1).
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

    Batcher& batcher = Batcher::Instance();
    batcher.BeginGroup(mlrTexHandle, 0.0f, 0.0f, 1.0f, 1.0f, blendMode);

    for (int i = 0; i < population; ++i) {
        // Per-particle seed in [min_seed, min_seed + seed_range], clamped
        // to [0,1]. Matches particlecloud.cpp:544-546.
        Stuff::Scalar child_seed =
            Stuff::Random::GetFraction() * seed_range + min_seed;
        if (child_seed < 0.0f) child_seed = 0.0f;
        else if (child_seed > 1.0f) child_seed = 1.0f;

        // Per-particle lifetime from m_pLifeSpan at (parent_age=0,
        // child_seed). Min-clamp matches particlecloud.cpp:550 (33ms
        // floor to keep ageRate finite).
        Stuff::Scalar lifetime =
            mut_spec->m_pLifeSpan.ComputeValue(parent_age, child_seed);
        if (lifetime < 0.0333333f) {
            lifetime = 0.0333333f;
        }

        // Per-particle emitter-volume offset. Legacy uses a unit-sphere-
        // ish YawPitchRange sample (particlecloud.cpp:558-563) scaled
        // per-axis by m_emitterSize. We reuse YawPitchRange to preserve
        // the exact distribution (matters for stock-mission visual
        // parity; the v1 GPU path does not yet randomize seed
        // deterministically, so any future seed-RNG move to GPU must
        // match this distribution too).
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

        // Initial velocity. Matches particlecloud.cpp:577-591: yaw uniform
        // [0,2pi), pitch uniform within [min_dev, max_dev) shifted by
        // -pi/2, length from m_startingSpeed. We compute in local space
        // (legacy stores m_localLinearVelocity); to bake into the GPU
        // record we transform the direction by parentToWorld's rotation
        // (the matrix's translation does not affect direction vectors).
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

        // Color at age=0 / child_seed. Legacy AnimateParticle samples
        // these per-frame; for the GPU path the shader will own the
        // animation, but the t=0 sample is the spawn-record color and
        // matches the legacy first-frame visual.
        const Stuff::Scalar r = mut_spec->m_pRed  .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar g = mut_spec->m_pGreen.ComputeValue(parent_age, child_seed);
        const Stuff::Scalar b = mut_spec->m_pBlue .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar a = mut_spec->m_pAlpha.ComputeValue(parent_age, child_seed);

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
        // Point primitives have no aspect-ratio quad: a single small
        // world-space radius. The legacy MLRPointCloud rasterizes as a
        // GL_POINTS-style fixed-size sprite (size baked into the cloud
        // implementation, not per-particle). Use a small constant; the
        // GPU billboard pass uses this as the world-space radius of the
        // billboard quad. Per-spec sizing is filed as B2 polish debt.
        p.size        = 0.5f;
        // atlasIndex carries the raw MLR pool index; Batcher::ResolveTextures()
        // converts it to a gos_TextureHandle after renderLists() / LoadImages().
        p.atlasIndex  = mlrTexHandle;

        batcher.Emit(p);
    }
}

}  // namespace particles
}  // namespace mc2
