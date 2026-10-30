//==========================================================================//
// File:    batcher.h                                                        //
// Contents: GPU particle SSBO batcher (producer-facing API).                //
//           Plan v5 §5.4 B1 Stage 1' Commit 1.                              //
//           FX-GPU-1 Phase 2: per-group UV sub-rect + animated atlas frames. //
//                                                                           //
// This is the producer-facing scaffold: Emit() pushes a GpuParticle record  //
// into per-frame CPU staging; Flush() forwards the staged buffer to the     //
// GameOS GL bridge (gos_particle_bridge_flush). The bridge owns the actual  //
// SSBO upload, VAO, sampler save/restore, depth-state, blend-state, and the //
// glDrawArrays expansion to billboard quads.                                //
//                                                                           //
// Default ON. =0 suppresses Card/CardCloud/PointCloud/ShardCloud submission;    //
// their CPU/MLR renderer is retired.                                       //
//                                                                           //
// One-way dependency: this header has NO GameOS / GL dependency; the .cpp   //
// calls a single C-style bridge function. Producers (later: per-primitive   //
// Spawn impls in Stage 2') include this header only.                        //
//===========================================================================//

#pragma once

#include "spec.h"
#include <cstdint>
#include <memory>

namespace mc2 {
namespace particles {

// Per-texture-group metadata produced by BeginGroup / closed by Flush.
// The bridge receives a flat array of these records alongside the particle
// staging buffer; each entry describes one contiguous sub-range of the
// staging buffer that shares a single texture and UV sub-rect.
//
// UV rect semantics: (u0,v0) is the lower-left atlas corner; (us,vs) is the
// width and height of the sub-rect in [0,1] UV space. For the full-texture
// case, u0=v0=0, us=vs=1. For an atlas sub-rect, u0/v0 are the frame origin
// and us/vs are the frame size as returned by spec m_UOffset/m_VOffset/
// m_USize/m_VSize.
struct GroupInfo {
    uint32_t handle;       // MLR pool index at spawn; resolved to gos_TextureHandle by ResolveTextures()
    float    u0;           // UV sub-rect origin X (0..1)
    float    v0;           // UV sub-rect origin Y (0..1)
    float    us;           // UV sub-rect width    (0..1) — per-tile width for animated atlases
    float    vs;           // UV sub-rect height   (0..1) — per-tile height for animated atlases
    unsigned start;        // index of first record in staging buffer
    unsigned count;        // number of records in this group
    int      blendMode;    // 0 = standard alpha (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
                           // 1 = additive       (GL_SRC_ALPHA, GL_ONE)
    uint32_t atlasColumns; // VFX-FLIPBOOK-ASSET-TABLE-1: atlas grid column count.
                           // 0 or 1 = non-animated (shader ignores GpuParticle.atlasIndex).
                           // >1 = animated atlas; shader uses atlasIndex as frame index:
                           //   col = atlasIndex % atlasColumns; row = atlasIndex / atlasColumns.
};

class Batcher {
 public:
    // Per-frame budget. Stage 1' uses a fixed budget; Stage 2' may grow it
    // based on per-mission Stage 0' coverage data.
    explicit Batcher(unsigned int perFrameBudget);
    ~Batcher();

    // Cached at first call (per debug_instrumentation_rule.md idiom).
    // Returns true iff MC2_GPU_PARTICLES=1.
    static bool is_enabled();

    // Returns true iff MC2_GPU_PARTICLES_LOG=1.
    // Gates verbose per-spec/per-handle SPAWN_PROBE and RESOLVE_PROBE output.
    // Normal runs (is_enabled=1, is_log_enabled=0) produce only first-flush
    // banners and error/overflow one-shots.
    static bool is_log_enabled();

    // MC2_VFX_ORACLE_TUBE slice 1: gosFX Tube swept-quad ribbon oracle gate
    // (default OFF). When ON, supported Tube kinds (MissileSmoke / alpha
    // ribbon, vertex_count==2) submit their already-built swept-quad mesh via
    // gos_tube_ribbon_flush instead of legacy MLR; unsupported kinds (additive
    // PPC, polygon profiles) fall through to legacy MLR unchanged.
    static bool is_oracle_tube_enabled();
    // MC2_VFX_ORACLE_SHAPE (default OFF). When ON, gosFX::Shape::Draw harvests
    // its single rigid MLRShape (model-space verts/UVs/indices + per-primitive
    // texture + alpha mode) and submits through the persistent-mesh VFX bridge
    // (gos_vfx_mesh_flush) instead of MLRClipper::DrawScalableShape. Opt-in =1;
    // any other / absent value = legacy path unchanged. Shape class ONLY.
    static bool is_oracle_shape_enabled();
    // MC2_VFX_ORACLE_SHAPE_LOG=1 — FIRST_HARVEST line + 240-call summary.
    static bool is_oracle_shape_log_enabled();

    // Open a new texture group before emitting a batch of particles that
    // share the same texture and UV sub-rect. Must be called at least once
    // before the first Emit; closing the previous group is automatic.
    //
    // Parameters:
    //   handle         MLR pool index from spec->m_state.GetTextureHandle() (resolved to GOS handle by ResolveTextures)
    //   u0, v0         atlas sub-rect origin (0..1) — base tile origin for animated; full-page origin for static
    //   us, vs         atlas sub-rect size   (0..1) — per-tile size for animated; full-page size for static
    //   atlasColumns   VFX-FLIPBOOK-ASSET-TABLE-1: atlas grid column count (default 0 = non-animated).
    //                  When >1, the billboard VS uses GpuParticle.atlasIndex as a frame index to
    //                  compute per-particle tile offsets: col=frame%columns, row=frame/columns.
    //
    // Use u0=0, v0=0, us=1, vs=1, atlasColumns=0 for full-page textures.
    // No-op when is_enabled() is false.
    // blendMode: 0 = standard alpha, 1 = additive (from MLRState AlphaMode).
    void BeginGroup(uint32_t handle, float u0, float v0, float us, float vs,
                    int blendMode = 0, uint32_t atlasColumns = 0u);

    // Push one particle record into the per-frame staging buffer. Bounds-
    // checked against perFrameBudget; on overflow the record is dropped and
    // a one-shot stderr line is emitted (overflow is a latent class to track
    // before Stage 5' default-flip). Safe to call when is_enabled() is false
    // (returns immediately without staging). Particle is appended to the
    // currently-open group (most recent BeginGroup call).
    void Emit(const GpuParticle& p, const void* source = nullptr, uint32_t corner = 0);

    // Resolve MLR pool indices in each GroupInfo to GOS texture handles.
    // Must be called AFTER renderLists() (which triggers LoadImages()) and
    // BEFORE Flush(). mclib/particles can include MLR headers; GameOS cannot.
    void ResolveTextures();

    // Upload staging to the SSBO and issue the billboard draw. Must run
    // AFTER mc2::VkSceneRenderer::renderLists(mc2::renderSubmissions()) (per gpu_direct_renderer_
    // bringup_checklist.md trap #6 — render order). The hook site is
    // code/gamecam.cpp post-renderLists. Clears staging after flush.
    // No-op when is_enabled() is false OR staged count is zero.
    void Flush();

    // Global singleton accessor. Stage 1' uses one batcher per process;
    // Stage 2' (per-type Spawn) may sharded if budget breakdown demands.
    static Batcher& Instance();

    // VFX-SPINE-0: read-only inspector getters for the per-frame budget and
    // overflow latch. Public so extern "C" accessors in this TU can read them
    // without a friend declaration. No mutation; safe at any time.
    unsigned int inspect_budget() const;
    bool         inspect_overflowReported() const;

 // Trail-spawn diagnostic counters. Incremented by GpuTrailEmitter::Spawn()
 // and printed by dump_summary() at atexit. Public so gpu_trail.cpp can
 // reach them without a friend declaration.
 static unsigned long long s_trail_spawn_total;
 static unsigned long long s_trail_head_total;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Batcher(const Batcher&)            = delete;
    Batcher& operator=(const Batcher&) = delete;
};

}  // namespace particles
}  // namespace mc2
