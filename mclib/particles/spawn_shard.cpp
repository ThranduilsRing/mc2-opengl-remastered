//==========================================================================//
// File:    spawn_shard.cpp                                                  //
// Contents: mc2::particles::SpawnShard implementation.                      //
//           Plan v6 §5.4 B1 Stage 2' C5.                                    //
//                                                                           //
// Spawn-event semantics derived from mclib/gosfx/shardcloud.cpp +           //
// mclib/gosfx/spinningcloud.cpp + mclib/gosfx/particlecloud.cpp:            //
//                                                                           //
//   - ShardCloud is a multi-particle Effect. It derives from SpinningCloud  //
//     -> ParticleCloud, so starting population at Effect::Start is sampled  //
//     from spec->m_startingPopulation at age=0 / parentSeed (particle-      //
//     cloud.cpp:351); birth-rate-driven ongoing spawns                      //
//     (m_particlesPerSecond) are RUNTIME spawns, not the responsibility of  //
//     this entry point — folded into the GPU animation pass per the v6     //
//     plan (filed as B2 polish debt; v1 GPU path emits starting             //
//     population only).                                                     //
//   - Each starting particle is built by the CreateNewParticle chain        //
//     (ShardCloud::CreateNewParticle at shardcloud.cpp:260 ->               //
//     SpinningCloud::CreateNewParticle at spinningcloud.cpp:338 ->          //
//     ParticleCloud::CreateNewParticle at particlecloud.cpp:523):           //
//       - random child seed in [m_minimumChildSeed, m_maximumChildSeed],   //
//         lifetime from m_pLifeSpan at (parent_age=0, child_seed)           //
//         (particlecloud.cpp:540-551)                                       //
//       - random emitter-volume offset scaled by                            //
//         m_emitterSize{X,Y,Z} (particlecloud.cpp:558-570)                  //
//       - random initial velocity from m_startingSpeed /                    //
//         m_minimum/maximumDeviation (particlecloud.cpp:577-591)            //
//       - per-particle radius = m_size at (m_age, m_seed)                  //
//         (shardcloud.cpp:285)                                              //
//       - per-particle scale = m_pScale at (age, seed)                     //
//         (spinningcloud.cpp:578)                                           //
//   - Color is sampled per-particle from m_pRed/Green/Blue/Alpha at         //
//     (age=0, child_seed) — same SeededCurve idiom as PointCloud           //
//     (shardcloud.cpp:249-252 in legacy AnimateParticle; SpawnShard         //
//     samples at age=0 to match the spawn-frame visual).                    //
//   - Legacy Draw() fires DrawEffect once per cloud, not per particle      //
//     (shardcloud.cpp:659 inside the m_activeParticleCount block).         //
//     FX_TRACE_DRAW therefore fires ONCE per SpawnShard call (per-          //
//     Effect), matching the per-cloud cardinality of legacy Effect::Draw.  //
//                                                                           //
// Schema-fidelity note (B2 polish debt): the legacy ShardCloud renders     //
// each particle as a TRIANGLE whose vertex offsets depend on camera        //
// direction + per-particle m_angle = sin(m_angularity) (shardcloud.cpp:    //
// 387-410 etc.) and m_localRotation. The current 64-byte GpuParticle       //
// schema (spec.h) carries position / color / velocity / lifetime / age /   //
// size / atlasIndex only — no rotation, no angularity. The GPU billboard   //
// pass renders each spawn as a camera-facing quad at GpuParticle.size, so  //
// per-particle footprint area matches the legacy m_radius * m_scale        //
// product but the triangle-shape and rotation fidelity is deferred until   //
// the schema extension lands. Tracked as B2 polish debt; visual review     //
// during C7 wiring will confirm whether stock missions need the extension  //
// before the legacy retirement (B3).                                        //
//                                                                           //
// In the new path the GPU shader owns animation (drag, ether-velocity,    //
// acceleration, color curves, age advance, billboard alignment,            //
// projection). The CPU emits the starting population's t=0 state in one   //
// burst; the shader extrapolates each particle's trajectory from the      //
// spawn timestamp + initial velocity baked into its GpuParticle record.   //
//                                                                           //
// CPU projection invariant: this file MUST NOT include or reference any   //
// of the forbidden projection wrappers — the authoritative list lives     //
// in scripts/check-particles-no-cpu-projection.sh.                         //
//===========================================================================//

#include "gosfx/gosfxheaders.hpp"
#include "gosfx/shardcloud.hpp"
#include "spawn_shard.h"
#include "spawn.h"   // resolveSampleAge (VFX-AGE-SAMPLE-1)
#include "batcher.h"
#include "fx_trace/fx_trace.h"

#include "../math/mc2_xform.h"
#include <stuff/random.hpp>
namespace mc2 {
namespace particles {

void SpawnShard(const gosFX::ShardCloud__Specification* spec,
                const glm::mat4*            parentToWorld,
                float                                    spawnSeed,
                float                                    callerAge)
{
    if (!spec) {
        return;
    }

    // FX_TRACE_DRAW key schema matches the legacy gosFX::Effect::Draw entry
    // (effect.cpp:697) — ONE event per spawn. Legacy ShardCloud::Draw at
    // shardcloud.cpp:659 calls DrawEffect once per cloud regardless of
    // particle count, so the per-Effect cardinality matches.
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
    gosFX::ShardCloud__Specification* mut_spec =
        const_cast<gosFX::ShardCloud__Specification*>(spec);

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
    glm::vec3 parent_origin(0.0f, 0.0f, 0.0f);
    if (parentToWorld) {
        parent_origin = mc2::math::getTranslation(*parentToWorld);
    }

    // Store raw MLR pool index; resolved to GOS handle at flush time.
    uint32_t mlrTexHandle = 0u;
    {
        const unsigned h = mut_spec->m_state.GetTextureHandle();
        mlrTexHandle = static_cast<uint32_t>(h);
    }

    // ShardCloud has no UV sub-rect curves. Use full-page UV (0,0,1,1).
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
        // parity; same reasoning as SpawnPoint).
        mc2::math::YawPitchRange initial_p(
            Stuff::Random::GetFraction() * Stuff::Two_Pi,
            Stuff::Random::GetFraction() * Stuff::Pi - Stuff::Pi_Over_2,
            Stuff::Random::GetFraction());
        glm::vec3 unit_pos = (mc2::math::polarToVector(initial_p));
        glm::vec3 local_offset(
            unit_pos.x * mut_spec->m_emitterSizeX.ComputeValue(parent_age, child_seed),
            unit_pos.y * mut_spec->m_emitterSizeY.ComputeValue(parent_age, child_seed),
            unit_pos.z * mut_spec->m_emitterSizeZ.ComputeValue(parent_age, child_seed));

        glm::vec3 world_pos;
        if (parentToWorld) {
            world_pos = mc2::math::transformPoint(local_offset, *parentToWorld);
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
        glm::vec3 local_velocity = (mc2::math::polarToVector(initial_v));
        glm::vec3 world_velocity;
        if (parentToWorld) {
            world_velocity = mc2::math::transformVector(local_velocity, *parentToWorld);
        } else {
            world_velocity = local_velocity;
        }

        // Color at age=0 / child_seed. Legacy AnimateParticle samples
        // these per-frame (shardcloud.cpp:249-252); for the GPU path the
        // shader will own animation, but the t=0 sample is the spawn-
        // record color and matches the legacy first-frame visual.
        const Stuff::Scalar r = mut_spec->m_pRed  .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar g = mut_spec->m_pGreen.ComputeValue(parent_age, child_seed);
        const Stuff::Scalar b = mut_spec->m_pBlue .ComputeValue(parent_age, child_seed);
        const Stuff::Scalar a = mut_spec->m_pAlpha.ComputeValue(parent_age, child_seed);

        // Per-particle shard size = m_size at (parent_age=0, child_seed)
        // multiplied by m_pScale at (parent_age=0, child_seed). Matches
        // shardcloud.cpp:285 (m_radius from m_size) and spinningcloud.cpp:
        // 578 (m_scale from m_pScale). The legacy AnimateParticle
        // re-samples these per-frame; the GPU shader will own that
        // animation (B2 polish debt). For C5 we bake the spawn-time
        // product into GpuParticle.size.
        Stuff::Scalar radius =
            mut_spec->m_size.ComputeValue(parent_age, child_seed);
        Stuff::Scalar scale =
            mut_spec->m_pScale.ComputeValue(parent_age, child_seed);
        if (radius < 0.0f) radius = 0.0f;
        if (scale  < 0.0f) scale  = 0.0f;
        const Stuff::Scalar size = radius * scale;

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
        // Note: m_angularity (per-vertex triangle-shape factor) and
        // m_localRotation (per-particle orientation) from shardcloud.cpp
        // are NOT representable in the current GpuParticle schema. The
        // GPU billboard pass renders camera-facing quads at this size;
        // triangle-shape + rotation fidelity is filed as B2 polish debt
        // (requires a 64-byte -> larger schema extension + matching
        // shader path that emits 3 verts and applies per-particle yaw).
        p.size        = (float)size;
        // atlasIndex carries the raw MLR pool index; Batcher::ResolveTextures()
        // converts it to a gos_TextureHandle after renderLists() / LoadImages().
        p.atlasIndex  = mlrTexHandle;

        batcher.Emit(p);
    }
}

}  // namespace particles
}  // namespace mc2
