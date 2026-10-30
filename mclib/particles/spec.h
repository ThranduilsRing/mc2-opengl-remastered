//==========================================================================//
// File:    spec.h                                                           //
// Contents: GpuParticle C++ struct (std430-mirror of shaders/include/       //
//           particles.hglsl Particle).                                      //
//           Plan v5 §5.4 B1 Stage 1' Commit 2.                              //
//                                                                           //
// std430 lockstep discipline per memory/cpp_glsl_ubo_struct_lockstep.md +   //
// plan §5.4 v2 MAJOR-4 fold-in:                                             //
//   - sizeof()        static_assert (necessary)                             //
//   - alignof()       static_assert (std430 vec4 alignment requirement)     //
//   - per-field       offsetof static_assert (every field GLSL reads)       //
//                                                                           //
// Any change to this struct MUST update both the static_asserts AND the     //
// mirror at shaders/include/particles.hglsl in the SAME atomic commit.      //
// One-source schema generation is filed as B2 polish debt.                  //
//===========================================================================//

#pragma once

#include <cstddef>
#include <cstdint>

namespace mc2 {
namespace particles {

// 64-byte std430 record. Layout chosen so vec3 fields sit on 16-byte
// boundaries (std430 vec3 alignment == 16). Padding is explicit so the
// struct compiles identically MSVC / Clang / GCC.
//
// Field layout (offset / type / purpose):
//   00  vec3+pad  position   world-space spawn origin
//   16  vec4      color      straight RGBA (R,G,B,A — not premultiplied) at draw time
//   32  vec3+pad  velocity   world-space per-second delta (Stage 2' uses this;
//                            Stage 1' Card test effect leaves at zero)
//   44  uint32    kind_flags [0]=is_head, [1-3]=reserved, [4-7]=kind, [8-31]=reserved
//   48  float     lifetime   total lifetime in seconds
//   52  float     age        current age in seconds (CPU updates per frame in
//                            Stage 1'; Stage 2' shifts age advance to GPU via
//                            spawn-frame seed)
//   56  float     size       world-space radius in meters
//   60  uint      atlasIndex per-particle atlas frame index (0 = first frame).
//                            VFX-FLIPBOOK-ASSET-TABLE-1: oracle render stores the
//                            integer frame index here; shader applies tile offset
//                            (col=index%atlasColumns, row=index/atlasColumns) when
//                            GroupInfo.atlasColumns > 1. Non-oracle/non-animated
//                            producers may leave this as 0 or the raw MLR handle —
//                            the shader ignores atlasIndex when atlasColumns <= 1.
struct GpuParticle {
    float    position[3];
    float    _pad0;          // pad to vec4 alignment
    float    color[4];
    float    velocity[3];
    uint32_t kind_flags;     // [7:4]=kind (0-15), [0:0]=is_head — offset 44
    float    lifetime;
    float    age;
    float    size;
    uint32_t atlasIndex;     // per-particle atlas frame index (see comment above)
};

// Accessors for kind_flags bit fields.
// All producers emit GpuParticle p = {} so kind_flags defaults to 0.
inline uint8_t  gpu_particle_kind(const GpuParticle& p)          { return (p.kind_flags >> 4) & 0xFu; }
inline bool     gpu_particle_is_head(const GpuParticle& p)        { return (p.kind_flags & 1u) != 0; }
inline void     gpu_particle_set_kind(GpuParticle& p, uint8_t k)  { p.kind_flags = (p.kind_flags & ~0xF0u) | ((uint32_t(k) & 0xFu) << 4); }
inline void     gpu_particle_set_is_head(GpuParticle& p, bool v)  { p.kind_flags = v ? (p.kind_flags | 1u) : (p.kind_flags & ~1u); }

// --- std430 lockstep asserts (plan §5.4 v2 MAJOR-4 fold-in) ---
static_assert(sizeof(GpuParticle) == 64,
              "GpuParticle size drift — update shaders/include/particles.hglsl in the same commit");
static_assert(alignof(GpuParticle) >= 4,
              "GpuParticle alignment too low for std430 mirror");
// Per-field offset asserts. Every offset GLSL reads is pinned here. Any
// edit to GpuParticle must update BOTH this list AND particles.hglsl.
static_assert(offsetof(GpuParticle, position)   ==  0, "GpuParticle.position offset drift");
static_assert(offsetof(GpuParticle, color)      == 16, "GpuParticle.color offset drift");
static_assert(offsetof(GpuParticle, velocity)   == 32, "GpuParticle.velocity offset drift");
static_assert(offsetof(GpuParticle, kind_flags) == 44, "GpuParticle.kind_flags offset drift");
static_assert(offsetof(GpuParticle, lifetime)   == 48, "GpuParticle.lifetime offset drift");
static_assert(offsetof(GpuParticle, age)        == 52, "GpuParticle.age offset drift");
static_assert(offsetof(GpuParticle, size)       == 56, "GpuParticle.size offset drift");
static_assert(offsetof(GpuParticle, atlasIndex) == 60, "GpuParticle.atlasIndex offset drift");

}  // namespace particles
}  // namespace mc2
