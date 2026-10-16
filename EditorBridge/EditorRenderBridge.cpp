// EditorBridge/EditorRenderBridge.cpp
//
// CARVE-OUT: this is the ONLY file in EditorBridge/ permitted to include
// both game-side and engine-side headers.
// See: scripts/check-include-firewall.allowlist
//
// EditorBridge v0.
// Spec: docs/superpowers/specs/mission-editor-render-bridge-v0-spec.md

#include "EditorRenderBridge.h"

#include <cstdlib>
#include <cstring>   // std::strcmp

// Engine side
#include "../RenderWorld/RenderWorld.h"
#include "../RenderWorld/ScreenPick.h"   // ScreenPickContext, screenPickCompute

// Game side
#include "../mclib/terrain.h"   // Terrain, TerrainPtr, land
#include "../mclib/camera.h"    // Camera, CameraPtr (via dcamera.h), eye

// GameOS (gos_VERTEX, gos_DrawLines, gos_SetRenderState, gos_GetViewport, Environment)
#include "../GameOS/include/gameos.hpp"

// GPU-internal headers. This carve-out TU is the
// ONLY editor-facing seam allowed to include these; editor/ TUs route through
// the lifecycle passthroughs below instead of including them directly.
#include "../GameOS/gameos/gos_static_prop_batcher.h"   // GpuStaticPropBatcher, batcher_getTypeCount, TG_TypeMultiShape
#include "../GameOS/gameos/gos_static_prop_registry.h"  // GpuStaticPropRegistry
#include "../GameOS/gameos/gos_mech_batcher.h"          // GpuMechBatcher
#include "../GameOS/gameos/gpu_cull_compute.h"           // gpu_cull::compute_*
#include "../GameOS/gameos/gpu_cull_substrate.h"          // gpu_cull::substrate_shutdown
#include "../GameOS/gameos/gpu_cull_readback.h"           // gpu_cull::readback_shutdown
#include "Gates/gate.h"

// ---- game-side globals wired by the editor at startup ----
extern TerrainPtr land;   // defined in mclib/terrain.cpp
extern CameraPtr  eye;    // defined in mclib/camera.cpp

namespace EditorBridge {

// ---- internal state ----

static bool s_enabled = false;

// ---- helpers ----

// Convert spec RGBA (R=bits31-24, G=23-16, B=15-8, A=7-0) to gos_VERTEX::argb
// (A=bits31-24, R=23-16, G=15-8, B=7-0).
static uint32_t rgbaToArgb(uint32_t rgba) {
    const uint8_t r = (rgba >> 24) & 0xFFu;
    const uint8_t g = (rgba >> 16) & 0xFFu;
    const uint8_t b = (rgba >>  8) & 0xFFu;
    const uint8_t a = (rgba       ) & 0xFFu;
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

static gos_VERTEX makeVertex(float sx, float sy, uint32_t argb) {
    gos_VERTEX v{};
    v.x    = sx;
    v.y    = sy;
    v.z    = 0.0f;
    v.rhw  = 1.0f;
    v.argb = argb;
    v.frgb = 0;
    v.u    = 0.f;
    v.v    = 0.f;
    return v;
}

// Draw one line segment between two screen-space points.
// gos_DrawLines(pts, NumVertices) -- second arg is vertex count, not line count.
// 2 vertices = 1 line. Confirmed from gameos.hpp signature.
static void drawLine(float x0, float y0, float x1, float y1, uint32_t argb) {
    gos_VERTEX pts[2];
    pts[0] = makeVertex(x0, y0, argb);
    pts[1] = makeVertex(x1, y1, argb);
    gos_DrawLines(pts, 2);   // 2 vertices = 1 line segment
}

// Local runtime wrapper for screen->FBO-pixel coord transform.
// Owns gos_GetViewport -- must NOT be in RenderWorld (GameOS is not a RenderWorld dep).
// Math delegated to RenderWorld::screenPickCompute (pure, no GameOS).
static bool editorScreenToFboPixel(int screenX, int screenY,
                                    RenderWorld::ScreenPickContext* out) {
    if (!out) return false;
    out->mouseX = screenX;
    out->mouseY = screenY;
    gos_GetViewport(&out->vMulX, &out->vMulY, &out->vAddX, &out->vAddY);
    out->drawableWidth  = Environment.drawableWidth;
    out->drawableHeight = Environment.drawableHeight;
    if (out->vMulX <= 0.0f || out->vMulY <= 0.0f) return false;
    RenderWorld::screenPickCompute(out);
    return true;
}

// Push overlay render state (no Z test, alpha blend, HUD layer).
// v0 discipline: does NOT preserve prior state.
static void pushOverlayState() {
    gos_SetRenderState(gos_State_ZCompare,  0);
    gos_SetRenderState(gos_State_ZWrite,    0);
    gos_SetRenderState(gos_State_Texture,   0);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
    gos_SetRenderState(gos_State_IsHUD,     1);
}

static void popOverlayState() {
    gos_SetRenderState(gos_State_IsHUD,     0);
    gos_SetRenderState(gos_State_ZCompare,  1);
    gos_SetRenderState(gos_State_ZWrite,    1);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
}

// Project a world-space point to screen space.
// Uses Camera::projectForScreenXY (non-deprecated intent-specific wrapper).
// Returns false if the point is behind the camera.
static bool projectToScreen(const Stuff::Vector3D& worldPt,
                            float* outSx, float* outSy) {
    if (!eye || !outSx || !outSy) return false;
    Stuff::Vector3D pt = worldPt;   // mutable copy: projectForScreenXY takes non-const ref
    Stuff::Vector4D screen{};
    if (!eye->projectForScreenXY(pt, screen)) return false;
    *outSx = screen.x;
    *outSy = screen.y;
    return true;
}

// ---- lifecycle ----

void init() {
    s_enabled = mc2::gate::get<"MC2_EDITOR_MODE">();
}

void shutdown() {
    s_enabled = false;
}

bool isEnabled() { return s_enabled; }

// ---- stub implementations (replaced in Tasks 5-7) ----

EditorPickResult pickAt(int screenX, int screenY) {
    EditorPickResult result{};
    if (!s_enabled) return result;

    // ---- Path A: GPU object-ID readback (StaticProp + Mech) ----
    // Requires MC2_OBJECT_ID_BUFFER=1. Silently skipped if off -- not an error.
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        RenderWorld::ScreenPickContext spCtx;
        if (editorScreenToFboPixel(screenX, screenY, &spCtx)) {
            if (screenX >= 0 && screenY >= 0
                && screenX < static_cast<int>(spCtx.vMulX)
                && screenY < static_cast<int>(spCtx.vMulY))
            {
                const RenderWorld::LookupResult lr =
                    RenderWorld::lookupAtPixel(spCtx.glX, spCtx.glY);
                if (lr.isValid) {
                    result.handle = lr.handle;
                    // Explicit switch: unknown kinds do NOT silently become StaticProp.
                    switch (lr.kind) {
                    case RenderWorld::RenderObjectKind::StaticProp:
                        result.kind = EditorPickResult::Kind::StaticProp;
                        return result;
                    case RenderWorld::RenderObjectKind::Mech:
                        result.kind = EditorPickResult::Kind::Mech;
                        return result;
                    default:
                        // Terrain (reserved, no writer), Vfx (prohibited writers),
                        // or any future kind: fall through to terrain raycast.
                        break;
                    }
                }
            }
        }
    }

    // ---- Path B: CPU terrain raycast ----
    // Always fires when MC2_EDITOR_MODE=1, regardless of object-ID state.
    // This is the canonical ground-click path per M3 arc decision.
    if (!land || !eye) return result;

    Stuff::Vector2DOf<long> screenPt;
    screenPt.x = static_cast<long>(screenX);
    screenPt.y = static_cast<long>(screenY);
    Stuff::Vector3D worldPt{};
    // inverseProject returns 0 on tile-found success, non-zero on off-map miss.
    const unsigned long hitFlags = eye->inverseProject(screenPt, worldPt);
    if (hitFlags != 0) return result;  // missed terrain (off-map or camera not ready)

    int tileR = -1, tileC = -1;
    land->worldToTile(worldPt, tileR, tileC);

    // Guard against camera-not-ready path: inverseProject returns 0 for both
    // "tile found" (good) and "turn < 4, camera not ready" (bad, worldPt zeroed).
    // worldToTile initializes tileR/tileC to -1; if they're still -1 after the call,
    // the tile was not found -- return Miss rather than a bogus origin hit.
    if (tileR < 0 || tileC < 0) return result;

    result.kind             = EditorPickResult::Kind::Terrain;
    result.terrainTileRow   = tileR;
    result.terrainTileCol   = tileC;
    result.terrainType      = static_cast<int32_t>(land->getTerrainType(worldPt));
    result.terrainElevation = land->getTerrainElevation(worldPt);
    result.worldX           = worldPt.x;
    result.worldY           = worldPt.y;
    result.worldZ           = worldPt.z;
    return result;
}

RenderWorld::VisibilityResult queryVisibility(RenderWorld::VisibilityRequest req) {
    if (!s_enabled) return {};
    return RenderWorld::queryVisibility(req);
}

void drawSelectionBounds(const EditorAabb& b, SelectionBoundsStyle style) {
    if (!s_enabled || !eye) return;
    // Degenerate AABB guard.
    if (b.minX >= b.maxX || b.minY >= b.maxY || b.minZ >= b.maxZ) return;

    // 8 world-space corners: bottom face (z=minZ) + top face (z=maxZ).
    const Stuff::Vector3D corners[8] = {
        Stuff::Vector3D(b.minX, b.minY, b.minZ), Stuff::Vector3D(b.maxX, b.minY, b.minZ),
        Stuff::Vector3D(b.maxX, b.maxY, b.minZ), Stuff::Vector3D(b.minX, b.maxY, b.minZ),
        Stuff::Vector3D(b.minX, b.minY, b.maxZ), Stuff::Vector3D(b.maxX, b.minY, b.maxZ),
        Stuff::Vector3D(b.maxX, b.maxY, b.maxZ), Stuff::Vector3D(b.minX, b.maxY, b.maxZ),
    };

    // Project all 8 corners. If any are behind the camera, skip the entire
    // wireframe -- a partial draw produces degenerate near-clipped edges,
    // which looks worse than no overlay at all.
    float sx[8], sy[8];
    for (int i = 0; i < 8; ++i) {
        if (!projectToScreen(corners[i], &sx[i], &sy[i])) return;
    }

    // 12 edges: 4 bottom, 4 top, 4 vertical pillars.
    constexpr int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   // bottom face
        {4,5},{5,6},{6,7},{7,4},   // top face
        {0,4},{1,5},{2,6},{3,7},   // pillars
    };

    const uint32_t argb = rgbaToArgb(style.colorRGBA);
    pushOverlayState();
    for (int e = 0; e < 12; ++e) {
        drawLine(sx[edges[e][0]], sy[edges[e][0]],
                 sx[edges[e][1]], sy[edges[e][1]], argb);
    }
    popOverlayState();
}

void drawTerrainTileOutline(const TerrainTileOverlayDesc& desc) {
    if (!s_enabled || !land || !eye) return;
    const int tileR = desc.tileRow;
    const int tileC = desc.tileCol;

    // Internal bounds check -- EditorBridge is a public API and must not
    // allow caller-supplied tile indices to run off the static coord arrays.
    //
    // Terrain::realVerticesMapSide is a static long holding the VERTEX/BOUNDARY
    // count on each side of the map (confirmed: terrain.h line 132 comment says
    // "Number of vertices on each side of map").  The inline helpers in terrain.h
    // treat tile indices [0, realVerticesMapSide-1) as valid tiles (line 397:
    // tileC >= realVerticesMapSide is the OOB check).  The outline needs both
    // tileR and tileR+1 to be valid boundary indices, so the guard is
    // tileR+1 < realVerticesMapSide  (equivalently tileR < realVerticesMapSide-1).
    const int coordLimit = static_cast<int>(Terrain::realVerticesMapSide);
    if (tileR < 0 || tileC < 0) return;
    if (tileR + 1 >= coordLimit) return;
    if (tileC + 1 >= coordLimit) return;

    // Tile world-space extent from Terrain's public static coord arrays.
    // tileColToWorldCoord[tileC]   = left X boundary of tile
    // tileColToWorldCoord[tileC+1] = right X boundary
    // tileRowToWorldCoord[tileR]   = near Y boundary
    // tileRowToWorldCoord[tileR+1] = far Y boundary
    const float x0 = Terrain::tileColToWorldCoord[tileC];
    const float x1 = Terrain::tileColToWorldCoord[tileC + 1];
    const float y0 = Terrain::tileRowToWorldCoord[tileR];
    const float y1 = Terrain::tileRowToWorldCoord[tileR + 1];

    // Sample elevation at the four world-space corners (CPU heightmap read).
    auto elevAt = [&](float x, float y) -> float {
        Stuff::Vector3D pos(x, y, 0.f);
        return land->getTerrainElevation(pos);
    };

    const Stuff::Vector3D corners[4] = {
        Stuff::Vector3D(x0, y0, elevAt(x0, y0)),  // NW
        Stuff::Vector3D(x1, y0, elevAt(x1, y0)),  // NE
        Stuff::Vector3D(x1, y1, elevAt(x1, y1)),  // SE
        Stuff::Vector3D(x0, y1, elevAt(x0, y1)),  // SW
    };

    // Project all 4 corners. Same policy as drawSelectionBounds: if any corner
    // is behind the camera, skip the entire outline rather than drawing a
    // partially clipped polygon.
    float sx[4], sy[4];
    for (int i = 0; i < 4; ++i) {
        if (!projectToScreen(corners[i], &sx[i], &sy[i])) return;
    }

    const uint32_t argb = rgbaToArgb(desc.colorRGBA);
    pushOverlayState();
    drawLine(sx[0], sy[0], sx[1], sy[1], argb);  // N edge
    drawLine(sx[1], sy[1], sx[2], sy[2], argb);  // E edge
    drawLine(sx[2], sy[2], sx[3], sy[3], argb);  // S edge
    drawLine(sx[3], sy[3], sx[0], sy[0], argb);  // W edge
    popOverlayState();
}

// Mission render-resource lifecycle.
// Deliberately NOT gated on s_enabled: the batcher/registry/cull lifecycle must
// run whenever the editor loads a map, independent of the pick-bridge enable
// flag (which only gates pickAt/queryVisibility/overlay draws).

void beginMissionRenderResources() {
    GpuStaticPropBatcher::instance().onMapLoad();
    GpuMechBatcher::instance().onMapLoad();
    GpuStaticPropRegistry::init();
}

void endMissionRenderResources() {
    // Locked 6-step order (mission.cpp:3272-3283). Must run while actor
    // TG_MultiShape pointers are still valid (i.e. before EditorObjectMgr::clear()).
    gpu_cull::readback_shutdown();
    gpu_cull::compute_shutdown();
    gpu_cull::substrate_shutdown();
    GpuStaticPropBatcher::instance().onMapUnload();
    GpuMechBatcher::instance().onMapUnload();
    GpuStaticPropRegistry::destroy();
}

void finalizeMissionGeometry() {
    GpuStaticPropBatcher::instance().finalizeGeometry();
    GpuMechBatcher::instance().finalizeGeometry();
    if (gpu_cull::compute_isEnabled()) {
        gpu_cull::compute_buildIndirectBuffer(batcher_getTypeCount());
    }
}

void staticPropFrameBegin() {
    GpuStaticPropRegistry::frameBegin();
}

void mechFinalizePending() {
    GpuMechBatcher::instance().finalizePending();
}

void cullComputeInit() {
    gpu_cull::compute_init();
}

void registerStaticPropShape(void* multiShape) {
    if (!multiShape) return;
    GpuStaticPropBatcher::instance().registerMultiShape(
        static_cast<TG_TypeMultiShape*>(multiShape));
}

bool staticPropMissionLoadRegEnabled() {
    return GpuStaticPropRegistry::isMissionLoadRegEnabled();
}

} // namespace EditorBridge
