#include "debug_renderer.h"

#include <cstdlib>   // getenv
#include <cstdio>    // fprintf, stderr
#include <vector>
#include <cmath>     // sinf, cosf

#include "imgui.h"

#include "debug_line_project.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

// Scene view-projection of the frame being drawn (defined in gameos_graphics.cpp).
extern const float* gos_GetTerrainMVPMat4();

namespace DebugRenderer {

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct DbgVert { float x, y, z; uint32_t rgba; };

// Segments kept per frame; a frame that queues more is truncated with one log line.
static constexpr size_t kVertCapacity = 65536;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool              s_enabled       = mc2::gate::get<"MC2_DEBUG_RENDERER">();
static bool              s_testCanary    = mc2::gate::get<"MC2_DEBUG_RENDERER_TEST">();
static bool              s_capWarnedOnce = false;

// Primitives collect in s_verts during the scene; flushWorldPrims freezes them with the frame's matrix and the
// overlay pass draws the frozen set once the UI frame is open.
static std::vector<DbgVert> s_verts;
static std::vector<DbgVert> s_frozen;
static float                s_frozenMvp[16];

static DbgVert makeVert(float x, float y, float z, uint32_t rgba) {
    return DbgVert{x, y, z, rgba};
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void drawLineWorld(Vec3 a, Vec3 b, uint32_t rgba) {
    if (!s_enabled) return;
    s_verts.push_back(makeVert(a.x, a.y, a.z, rgba));
    s_verts.push_back(makeVert(b.x, b.y, b.z, rgba));
}

void drawAabbWorld(Vec3 mn, Vec3 mx, uint32_t rgba) {
    if (!s_enabled) return;
    // 8 corners: bit0=X, bit1=Y, bit2=Z
    float cx[8], cy[8], cz[8];
    for (int i = 0; i < 8; ++i) {
        cx[i] = (i & 1) ? mx.x : mn.x;
        cy[i] = (i & 2) ? mx.y : mn.y;
        cz[i] = (i & 4) ? mx.z : mn.z;
    }
    static constexpr int kEdges[12][2] = {
        {0,1}, {0,2}, {0,4},
        {1,3}, {1,5},
        {2,3}, {2,6},
        {3,7},
        {4,5}, {4,6},
        {5,7},
        {6,7},
    };
    for (const auto& e : kEdges) {
        int a = e[0], b = e[1];
        s_verts.push_back(makeVert(cx[a], cy[a], cz[a], rgba));
        s_verts.push_back(makeVert(cx[b], cy[b], cz[b], rgba));
    }
}

void drawRingWorld(Vec3 center, float radius, int segments, uint32_t rgba) {
    if (!s_enabled) return;
    if (segments < 3)   segments = 3;
    if (segments > 256) segments = 256;
    const float step = 6.2831853f / (float)segments;  // 2*PI
    for (int i = 0; i < segments; ++i) {
        float a0 = step * i;
        float a1 = step * (i + 1);
        s_verts.push_back(makeVert(
            center.x + radius * cosf(a0),
            center.y,
            center.z + radius * sinf(a0), rgba));
        s_verts.push_back(makeVert(
            center.x + radius * cosf(a1),
            center.y,
            center.z + radius * sinf(a1), rgba));
    }
}

void drawRingWorld(Vec3 center, Vec3 normal, float radius, int segments, uint32_t rgba) {
    if (!s_enabled) return;
    if (segments < 3)   segments = 3;
    if (segments > 256) segments = 256;

    // Normalize; fall back to XZ-plane ring on degenerate normal.
    float nx = normal.x, ny = normal.y, nz = normal.z;
    const float len = sqrtf(nx*nx + ny*ny + nz*nz);
    if (len < 1e-6f) {
        drawRingWorld(center, radius, segments, rgba);
        return;
    }
    nx /= len; ny /= len; nz /= len;

    // Build orthonormal tangent in the ring plane.
    // Switch reference axis when normal is near-parallel to Y-up (|ny| > 0.9)
    // to avoid the degenerate cross product.
    float tx, ty, tz;
    if (fabsf(ny) < 0.9f) {
        // cross(normal, Y_up=(0,1,0)) = (-nz, 0, nx)
        tx = -nz; ty = 0.0f; tz = nx;
    } else {
        // cross(normal, X_right=(1,0,0)) = (0, nz, -ny)
        tx = 0.0f; ty = nz; tz = -ny;
    }
    const float tlen = sqrtf(tx*tx + ty*ty + tz*tz);
    tx /= tlen; ty /= tlen; tz /= tlen;

    // bitangent = cross(normal, tangent) -- unit length since both are unit + orthogonal
    const float bx = ny*tz - nz*ty;
    const float by = nz*tx - nx*tz;
    const float bz = nx*ty - ny*tx;

    const float step = 6.2831853f / (float)segments;
    for (int i = 0; i < segments; ++i) {
        const float c0 = cosf(step *  i)      * radius;
        const float s0 = sinf(step *  i)      * radius;
        const float c1 = cosf(step * (i + 1)) * radius;
        const float s1 = sinf(step * (i + 1)) * radius;
        s_verts.push_back(makeVert(center.x + c0*tx + s0*bx,
                                   center.y + c0*ty + s0*by,
                                   center.z + c0*tz + s0*bz, rgba));
        s_verts.push_back(makeVert(center.x + c1*tx + s1*bx,
                                   center.y + c1*ty + s1*by,
                                   center.z + c1*tz + s1*bz, rgba));
    }
}

void drawCrossWorld(Vec3 center, float halfSize, uint32_t rgba) {
    if (!s_enabled) return;
    drawLineWorld({center.x - halfSize, center.y,            center.z           },
                  {center.x + halfSize, center.y,            center.z           }, rgba);
    drawLineWorld({center.x,            center.y - halfSize, center.z           },
                  {center.x,            center.y + halfSize, center.z           }, rgba);
    drawLineWorld({center.x,            center.y,            center.z - halfSize},
                  {center.x,            center.y,            center.z + halfSize}, rgba);
}

void drawSphereApproxWorld(Vec3 center, float radius, int rings, int segments, uint32_t rgba) {
    if (!s_enabled) return;
    if (rings < 1)  rings = 1;
    if (rings > 16) rings = 16;

    // 3 great-circle rings, one per axis plane
    drawRingWorld(center, {0,1,0}, radius, segments, rgba);  // XZ plane (Y normal)
    drawRingWorld(center, {0,0,1}, radius, segments, rgba);  // XY plane (Z normal)
    drawRingWorld(center, {1,0,0}, radius, segments, rgba);  // YZ plane (X normal)

    // Extra latitude rings between poles (rings=1 = great circles only)
    const float pi = 3.14159265f;
    for (int i = 1; i < rings; ++i) {
        const float lat = pi * (float)i / (float)rings;
        const float yr  = center.y + radius * cosf(lat);
        const float r   = radius * sinf(lat);
        drawRingWorld({center.x, yr, center.z}, r, segments, rgba);
    }
}

void drawTextWorld(Vec3 pos, const char* text, uint32_t rgba) {
    (void)pos; (void)text; (void)rgba;   // reserved: no world-space text yet
}

void flushWorldPrims() {
    if (!s_enabled) return;

    // Test canary: enqueue BEFORE empty check so MC2_DEBUG_RENDERER_TEST=1
    // produces output even when no other callers have queued verts.
    if (s_testCanary) {
        // World axis tripod at origin: X=red, Y=green, Z=blue, 50-unit arms
        drawLineWorld({0,0,0}, {50,0,0},  0xFF0000FFu);  // X red
        drawLineWorld({0,0,0}, {0,50,0},  0x00FF00FFu);  // Y green
        drawLineWorld({0,0,0}, {0,0,50},  0x0000FFFFu);  // Z blue
        // White ring at (0, 50, 0) radius 30, 64 segments
        drawRingWorld({0, 50, 0}, 30.0f, 64, 0xFFFFFFFFu);
    }

    if (s_verts.empty()) return;

    const float* vp = gos_GetTerrainMVPMat4();
    if (!vp) {
        s_verts.clear();
        return;
    }

    if (s_verts.size() > kVertCapacity) {
        if (!s_capWarnedOnce) {
            mc2::fmt::print_err("[DEBUGDRAW v1] event=vert_cap_exceeded cap={:d} requested={:d}\n", mc2::fmt::as_i32((int)kVertCapacity), mc2::fmt::as_i32((int)s_verts.size()));
            s_capWarnedOnce = true;
        }
        s_verts.resize(kVertCapacity);
    }
    if (s_verts.size() % 2 != 0) s_verts.pop_back();   // segments come in pairs

    s_frozen.swap(s_verts);
    s_verts.clear();
    for (int i = 0; i < 16; ++i) s_frozenMvp[i] = vp[i];
}

void drawFrozenPrimsToOverlay() {
    if (s_frozen.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (!dl || !(size.x > 0.0f) || !(size.y > 0.0f)) { s_frozen.clear(); return; }
    static bool s_firstDrawLogged = false;
    size_t drawn = 0;
    for (size_t i = 0; i + 1 < s_frozen.size(); i += 2) {
        const DbgVert& a = s_frozen[i];
        const DbgVert& b = s_frozen[i + 1];
        const float pa[3] = {a.x, a.y, a.z};
        const float pb[3] = {b.x, b.y, b.z};
        ScreenSegment seg;
        if (!projectSegment(s_frozenMvp, pa, pb, size.x, size.y, &seg)) continue;
        // Colour packs r,g,b,a from the top byte down.
        const uint32_t c = a.rgba;
        const ImU32 col = IM_COL32((c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        dl->AddLine(ImVec2(seg.x0, seg.y0), ImVec2(seg.x1, seg.y1), col, 1.0f);
        ++drawn;
        if (!s_firstDrawLogged) {
            s_firstDrawLogged = true;
            mc2::fmt::print_err("[DEBUGDRAW v1] event=first_draw queued={:d} display={:.0f}x{:.0f} seg0=({:.1f},{:.1f})-({:.1f},{:.1f})\n", mc2::fmt::as_i32((int)(s_frozen.size() / 2)), size.x, size.y, seg.x0, seg.y0, seg.x1, seg.y1);
        }
    }
    static bool s_cullLogged = false;
    if (drawn == 0 && !s_cullLogged) {
        s_cullLogged = true;
        mc2::fmt::print_err("[DEBUGDRAW v1] event=all_culled queued={:d}\n", mc2::fmt::as_i32((int)(s_frozen.size() / 2)));
    }
    s_frozen.clear();
}

void flushScreenPrims() {
    // reserved for future crisp HUD/debug-text pass
}

} // namespace DebugRenderer
