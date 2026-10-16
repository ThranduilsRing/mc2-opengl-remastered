#pragma once
//============================================================================
// gos_render_pass_timer — coarse per-pass GPU timers ([RENDER_PASS_TIME v1]).
//
// Headless per-pass GPU wall-time attribution for the game render frame
// (the GPU complement of MC2_HITCH_TRACE's CPU phase split). One disjoint
// GL_TIME_ELAPSED scope per coarse render pass (shadow/terrain/props/mechs/
// alpha/HUD/post), aggregated and emitted as a single stdout line every
// MC2_RENDER_PASS_TIME_EVERY frames (default 60):
//
//   [RENDER_PASS_TIME v1] frame=N n=60 gpu_total=3.41 terrainChunk=0.85 ...
//
// Values are mean ms over the window; passes that never ran in the window
// are simply absent from the line. gpu_total = GL_TIMESTAMP pair spanning
// first pass begin -> FrameEnd (covers un-scoped gaps too).
//
// Enable: MC2_RENDER_PASS_TIME=1. Default OFF = zero cost: no GL query
// objects are allocated or begun, no log lines (lazy init on first enabled
// frame; every entry point early-returns on the cached env check).
//
// Design (pattern = editor/EditorGpuTimer + gpu_cull_compute spike timer):
//  * 4-frame ring of query slots; results are availability-polled on the
//    OLDEST pending slot at FrameEnd and DROPPED (dropped= counter) rather
//    than stalled on if the GPU is still behind after a full ring lap.
//  * GL_TIME_ELAPSED cannot nest: scopes must be disjoint. Begin() while
//    another scope is open is skipped (defensive); pre-existing spike
//    timers (gpu_cull_compute.cpp / gos_terrain_water_stream.cpp) check
//    QueryActive() and yield while a pass scope is open.
//============================================================================

namespace gos_render_pass_timer {

enum Pass {
    Pass_ShadowStatic = 0,  // Shadow.StaticFullMapBuild (one-shot per mission)
    Pass_ShadowDyn,         // RenderLists.DynamicShadowPass
    Pass_Obj3d,             // Render.3DObjects (legacy TG renderLists color)
    Pass_TerrainChunk,      // gos_TerrainLodChunk_SubmitDrawCommands (default-on path)
    Pass_TerrainSolid,      // Render.TerrainSolid (legacy / indirect dispatch+draw)
    Pass_SpColor,           // Render.GpuStaticProps (cull dispatch + batcher flush)
    Pass_Mechs,             // Render.GpuMechs
    Pass_Overlays,          // Render.TerrainMask.Solid .. Render.Decals span
    Pass_Water,             // Render.Overlays + RenderLists.TerrainAlphaWaterLoops
    Pass_Blobs,             // RenderLists.ShadowBlobs
    Pass_AlphaVfx,          // RenderLists.NonTerrainAlphaLoops (incl. GPU particles)
    Pass_Hud,               // RenderLists.VfxHudSubmit
    Pass_Post,              // gosPostProcess::endScene (HZB/SSAO/shadow/godrays/
                            // shoreline/bloom/composite chain)
    Pass_Count
};

// Collection runs when MC2_RENDER_PASS_TIME=1 (cached on first call) OR the
// runtime collect flag is set (see SetCollect). All other entry points
// early-return when this is false.
bool Enabled();

// Runtime collect flag, mirroring gos_frame_pass_stats::SetCollect. The editor
// Frame Inspector flips this true while its window is open so the per-pass GPU
// ms can be read live, then false when closed. The [RENDER_PASS_TIME v1] emit
// line stays gated on the ENV alone -- the flag never produces stdout lines.
// Game build with env unset + window never opened keeps the flag false = zero
// cost (no GL query objects allocated or begun).
void SetCollect(bool on);

// Latest harvested mean per-pass GPU ms over the last emitted window, keyed by
// the Pass enum. Read-only: no GPU stall, no new query -- just exposes what the
// collector already harvested. Returns 0.0 when the pass had no sample in the
// last window (use HasSample(p) to distinguish 0ms from no-data).
double LastMs(Pass p);
bool   HasSample(Pass p);

// Results dropped (still unfinished when their ring slot came round again) in the last emitted window, both backends.
unsigned LastDropped();

// The Vulkan source (vk_render_pass_timer) hands one finished frame in: per-pass GPU ms and which passes ran. Folded into
// the same window as a GL harvest, so LastMs / HasSample / the log line read VK values unchanged.
void SubmitBackendFrame(const double ms[Pass_Count], const bool ran[Pass_Count]);
// The Vulkan source dropped one frame's results.
void SubmitBackendDropped();

// True while a GL_TIME_ELAPSED scope opened by Begin() is still open.
// Pre-existing standalone GL_TIME_ELAPSED spike timers must skip their own
// begin/end while this is true (the target cannot nest).
bool QueryActive();

void Begin(Pass p);
void End(Pass p);

// Once per frame, just before swap. Advances the ring, polls the oldest
// pending slot (never blocks), aggregates, and emits the telemetry line at
// the configured cadence.
void FrameEnd();

} // namespace gos_render_pass_timer
