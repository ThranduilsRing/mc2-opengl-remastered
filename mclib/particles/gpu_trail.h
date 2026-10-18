//==========================================================================//
// File:    gpu_trail.h                                                     //
// Contents: GpuTrailEmitter — segment-stamp GPU particles between two     //
//           world-space points. FX-GPU-1 B2 Phase 2 (Tasks P2.1/P2.2).   //
//                                                                          //
// Callers call GpuTrailEmitter::Spawn() once per frame per projectile,    //
// passing the previous and current world-space positions. The emitter     //
// distributes trail particles along the segment and optionally emits a    //
// head sprite at the current position.                                    //
//                                                                          //
// No new shader program: we extend particle_billboard.{vert,frag} in P3.  //
// No GpuParticle schema bump: kind is carried in the per-group key only.  //
// No new texture assets: reuses MLR handle 41 (smoke).                    //
//==========================================================================//

#pragma once

#include <cstdint>

#include "../math/glm_config.h"

namespace mc2 { namespace particles {

enum class GpuTrailKind : uint8_t {
    None         = 0,
    MissileSmoke = 1,
    PpcBolt      = 2,   // P3 adds tuning + textures; declared here for forward compat
};

struct GpuTrailTuning {
    float    head_color[4];           // RGBA
    float    trail_color[4];          // RGBA
    float    head_size;               // world units; 0 = no head sprite
    float    trail_particle_size;     // world units per particle
    float    trail_lifetime_s;
    float    trail_density_per_meter;
    uint8_t  blend_mode;              // 0 = alpha, 1 = additive (match existing Batcher convention)
    uint16_t texture_id;              // MLR pool index (Batcher resolves to gos handle at flush)
};

class GpuTrailEmitter {
public:
    // Hard cap on trail particles per segment. Protects against fast projectiles
    // and frame hitches. Tune from first smoke pass.
    static constexpr int MAX_PARTICLES_PER_SEGMENT = 32;

    // Spawn trail particles along the segment prev->cur, plus optional head sprite
    // at cur. Caller owns prev/cur (do not store internally).
    static void Spawn(GpuTrailKind kind,
                      const glm::vec3& prev_world,
                      const glm::vec3& cur_world,
                      float deltaT);

private:
    static const GpuTrailTuning& tuning_for(GpuTrailKind k);
};

}} // namespace mc2::particles
