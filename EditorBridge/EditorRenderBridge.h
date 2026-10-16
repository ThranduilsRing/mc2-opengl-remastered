// EditorBridge/EditorRenderBridge.h
//
// EditorBridge v0: stable API seam for the mission editor.
// Editor code calls ONLY this namespace -- never RenderWorld/GameAdapters/
// gameplay_pick/gos_postprocess directly.
//
// Initialization: the mission editor MUST call EditorBridge::init() before
// using any other function. mc2 game startup does NOT auto-call init().
// If init() is never called, all functions are no-ops (s_enabled=false).
//
// Firewall: this header includes engine types only.
//   - RenderCore/Handle.h              (RenderObjectHandle)
//   - RenderWorld/VisibilityRequest.h  (VisibilityRequest, VisibilityResult)
// No game headers. No GL. No GameOS. No Stuff/.

#pragma once

#include <cstdint>
#include "../RenderCore/Handle.h"
#include "../RenderWorld/VisibilityRequest.h"

namespace EditorBridge {

// ---- Lifecycle ----

// Call once at editor startup. Reads MC2_EDITOR_MODE (process-lifetime cached).
// All other functions are no-ops until this has been called with MC2_EDITOR_MODE=1.
void init();

// Call once at editor shutdown.
void shutdown();

// True iff MC2_EDITOR_MODE=1 was set at init() time.
bool isEnabled();

// ---- Types ----

struct EditorPickResult {
    enum class Kind : uint8_t {
        Miss       = 0,  // no hit (background, env-OFF, out-of-bounds)
        StaticProp = 1,  // building / tree / prop -- handle valid
        Mech       = 2,  // unit -- handle valid
        Terrain    = 3,  // terrain surface -- tile fields valid; handle invalid
    };

    Kind kind = Kind::Miss;

    // Valid when kind == StaticProp or kind == Mech.
    RenderCore::RenderObjectHandle handle =
        RenderCore::RenderObjectHandle::invalid();

    // Valid when kind == Terrain (-1 otherwise).
    int32_t terrainTileRow   = -1;
    int32_t terrainTileCol   = -1;
    int32_t terrainType      = -1;   // short->int from Terrain::getTerrainType; -1 if unavailable
    float   terrainElevation = 0.f;  // world-space Z from Terrain::getTerrainElevation

    // World-space surface point (all kinds; Miss = all zero).
    float worldX = 0.f;
    float worldY = 0.f;
    float worldZ = 0.f;
};

// AABB in world space. Caller provides -- v0 does not query from handle.
struct EditorAabb {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

struct SelectionBoundsStyle {
    uint32_t colorRGBA   = 0xFFFFFF80u;  // RGBA: R=bits31-24 .. A=bits7-0
    float    lineWidthPx = 2.0f;         // ignored in v0 (gos_DrawLines has no width)
};

struct TerrainTileOverlayDesc {
    int32_t  tileRow   = -1;    // -1 = invalid (Task 7 bounds check guards this)
    int32_t  tileCol   = -1;
    uint32_t colorRGBA = 0u;    // e.g. 0xFF000040 (red 25% alpha) for erase preview
};

// ---- API ----

// Pick at screen position (Win32 convention: origin top-left, Y grows down).
//
// Env behavior:
//   MC2_EDITOR_MODE=0          -> Kind::Miss immediately; no side effects
//   MC2_OBJECT_ID_BUFFER=0     -> Path A (StaticProp/Mech) skipped;
//                                 terrain (Path B) still works; not an error
//   both flags off, terrain miss -> Kind::Miss
EditorPickResult pickAt(int screenX, int screenY);

// Object counts for editor UI panels. Thin wrapper over RenderWorld::queryVisibility.
// Returns zeroed result if MC2_EDITOR_MODE=0.
RenderWorld::VisibilityResult queryVisibility(RenderWorld::VisibilityRequest req = {});

// Draw 12-edge AABB wireframe outline around a selected object.
// Caller provides world-space bounds. No-op if bounds degenerate or env-OFF.
//
// Overlay draw discipline (v0): overlay state is SET on entry and RESTORED
// to hardcoded defaults on exit. Does NOT preserve arbitrary prior render state.
// Must be called at a known overlay/debug phase of the frame.
void drawSelectionBounds(const EditorAabb& bounds, SelectionBoundsStyle style = {});

// Draw 4-edge wireframe outline of one terrain tile (brush preview).
// No-op if tile out of bounds or env-OFF.
//
// Overlay draw discipline: same as drawSelectionBounds.
void drawTerrainTileOutline(const TerrainTileOverlayDesc& desc);

// Mission render-resource lifecycle.
//
// These passthroughs let editor TUs orchestrate the GPU static-prop/mech
// batchers, the static-prop registry, and the gpu_cull compute pass WITHOUT
// including the GPU-internal headers (gos_static_prop_batcher.h,
// gos_static_prop_registry.h, gos_mech_batcher.h, gpu_cull_compute.h). The
// bridge owns those includes (carve-out TU) so editor/ stays firewalled.
//
// All return plain int/bool/void -- no GPU/Stuff structs cross this header.
// These are NOT gated on MC2_EDITOR_MODE (s_enabled): the editor must drive
// the batcher lifecycle regardless of the pick-bridge enable flag, mirroring
// the game runtime which calls these engine-side unconditionally.

// Mission load: GpuStaticPropBatcher::onMapLoad() + GpuMechBatcher::onMapLoad()
// + GpuStaticPropRegistry::init(). Call once per map load, before geometry
// registration. Mirrors code/mission.cpp:1693-1695.
void beginMissionRenderResources();

// Mission unload teardown (6-step, locked order). Mirrors mission.cpp:3272-3283:
// gpu_cull readback/compute/substrate shutdown, batcher onMapUnload x2,
// registry destroy. Call before EditorObjectMgr::clear().
void endMissionRenderResources();

// Finalize batcher geometry (VBO/IBO upload) for both batchers, then build the
// gpu_cull indirect buffer if the compute path is enabled. Mirrors
// mission.cpp:3136-3143. Call after all types are registered.
void finalizeMissionGeometry();

// Per-frame: GpuStaticPropRegistry::frameBegin(). Clears the per-frame live list.
// Call once per editor frame before land->render(). Mirrors gamecam.cpp:198.
void staticPropFrameBegin();

// Per-frame: GpuMechBatcher::finalizePending(). Absorbs mech types registered
// after finalizeGeometry() (late editor placement). No-op when none pending.
void mechFinalizePending();

// gpu_cull compute pass init (gpu_cull::compute_init()). Substrate/readback init
// are driven separately by the editor (those headers are not firewalled).
void cullComputeInit();

// Register one static-prop TG_TypeMultiShape with GpuStaticPropBatcher. The
// shape pointer is opaque to the editor (passed as void*); the bridge casts it
// back to TG_TypeMultiShape* internally. No-op if multiShape is null.
void registerStaticPropShape(void* multiShape);

// True iff MC2_STATIC_PROP_MISSION_LOAD_REG=1 (GpuStaticPropRegistry gate).
// Gates the editor's mission-load bulk static-prop registration walk.
bool staticPropMissionLoadRegEnabled();

} // namespace EditorBridge
