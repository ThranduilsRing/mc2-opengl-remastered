// GameOS/gameos/gos_particle_bridge.h
//
// Bridge entry point for the GPU particle batcher. The CPU producer-side
// API lives in mclib/particles/ (no GL dependency); this bridge owns the
// SSBO + VAO + shader program + draw call.
//
// Plan v5 §5.4 B1 Stage 1' Commit 1 — stub registered, no GL work yet.
// Stage 1' Commit 4 wires the actual flush.
// FX-GPU-1 Phase 2: per-group UV sub-rect support (GroupInfo array).

#pragma once

namespace mc2 { namespace particles { struct GpuParticle; } }
namespace mc2 { namespace particles { struct GroupInfo; } }

// C linkage so mclib/particles/batcher.cpp can forward-declare without
// pulling C++ name-mangling expectations from GameOS into mclib.
//
// groups / numGroups: flat array of GroupInfo records (one per BeginGroup
//   call this frame). Each entry describes a contiguous sub-range of
//   `records[]` (via .start / .count), the gos texture handle (.handle),
//   and the UV sub-rect (.u0 .v0 .us .vs) to use for that draw call.
//   numGroups==0 is treated as a single implicit full-rect group for
//   back-compat (should not happen in practice after Phase 2).
extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count,
                                          const mc2::particles::GroupInfo*   groups,
                                          unsigned int                       numGroups);

/* TUBE-DEFERRED-FLUSH-1: deferred ribbon draw path.
 *
 * gos_tube_ribbon_enqueue — called from gosFX::Tube::Draw DURING the
 *   effect-render phase (before renderLists).  Deep-copies the mesh data
 *   (positions already in MC2 WORLD space; caller pre-multiplies by
 *   local_to_world before enqueuing) into an internal per-frame queue.
 *   No GPU work. Returns void. Texture argument is an MLR pool index, resolved at drain.
 *   mlrHandle==0 = untextured (drawn white). blendMode low byte: 0 alpha,
 *   1 SRC_ALPHA/ONE, 2 ONE/ONE, 3 opaque (depth write), 4 ONE/SRC_ALPHA,
 *   5 ONE/ONE_MINUS_SRC_ALPHA; bit 8 = clamp UVs.
 *
 * gos_tube_ribbon_flush_deferred — called ONCE from code/gamecam.cpp
 *   immediately after Batcher::Instance().Flush() (post-renderLists).
 *   Drains the queue: sets up shared GL state once, loops records,
 *   issues one glDrawElements per record, restores state, clears queue.
 */
extern "C" void gos_tube_ribbon_enqueue(const float*          positions,
                                        const float*          colors,
                                        const float*          uvs,
                                        unsigned int          numVerts,
                                        const unsigned short* indices,
                                        unsigned int          numIndices,
                                        unsigned int          gosHandle,
                                        int                   blendMode);

extern "C" void gos_tube_ribbon_flush_deferred(void);

/* B2: active camera bridge — temporary stop-gap until RenderFrameContext lands.
 * Set by GameCamera::render() immediately before particle flush; cleared after.
 * If never set this frame, accessors return last-known basis (identity at boot).
 */
extern "C" void gos_SetActiveCamera(const float right_xyz[3], const float up_xyz[3]);
extern "C" void gos_GetCameraRight(float out_xyz[3]);
extern "C" void gos_GetCameraUp(float out_xyz[3]);
extern "C" void gos_ClearActiveCamera(void);

/* VFX-DEBUG-VIEWS-1 / VFX-TUNING-UI-1: read-only debug-mode + runtime
 * intensity scales for the Graphics Options "VFX Tuning" section. All scales
 * default 1.0 (byte-identical no-op), clamped 0..8; debug mode 0..5
 * (5=Age heatmap, added by VFX-SHADER-AGE-FADE-PARITY-1). Look-only:
 * no emission/lifetime/sorting/timing effect. */
extern "C" int   gos_vfx_getDebugMode(void);
extern "C" void  gos_vfx_setDebugMode(int mode);
extern "C" float gos_vfx_getBrightness(void);
extern "C" float gos_vfx_getAdditiveBrightness(void);
extern "C" float gos_vfx_getAlphaScale(void);
extern "C" void  gos_vfx_setBrightness(float v);
extern "C" void  gos_vfx_setAdditiveBrightness(float v);
extern "C" void  gos_vfx_setAlphaScale(float v);
// VFX-SHADER-AGE-FADE-PARITY-1: age-driven soft death fade (0.0=OFF, 1.0=full).
// Oracle-only (p.lifetime sentinel). Default 0.0 = byte-identical.
extern "C" float gos_vfx_getAgeFade(void);
extern "C" void  gos_vfx_setAgeFade(float v);
// VFX-SOFT-PARTICLES-MVP-1: depth-fade enable + world-unit fade band.
extern "C" int   gos_vfx_getSoftEnabled(void);
extern "C" void  gos_vfx_setSoftEnabled(int e);
extern "C" float gos_vfx_getSoftDistance(void);
extern "C" void  gos_vfx_setSoftDistance(float v);
// VFX-LIT-PARTICLES-MVP-1: scene-lighting enable + strength (0..1).
extern "C" int   gos_vfx_getLitEnabled(void);
extern "C" void  gos_vfx_setLitEnabled(int e);
extern "C" float gos_vfx_getLitStrength(void);
extern "C" void  gos_vfx_setLitStrength(float v);
