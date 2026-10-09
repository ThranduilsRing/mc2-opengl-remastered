#include "debug_renderer.h"

#include <cstdlib>   // getenv
#include <cstdio>    // fprintf, stderr
#include <cstddef>   // offsetof
#include <vector>
#include <cmath>     // sinf, cosf

#include <GL/glew.h>

#include "utils/shader_builder.h"  // glsl_program::makeProgram

// Forward-declare the scene viewproj getter (defined in gameos_graphics.cpp).
// Upload with the same transpose convention as the existing terrain MVP upload sites.
extern const float* gos_GetTerrainMVPMat4();

namespace DebugRenderer {

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct DbgVert { float x, y, z, r, g, b, a; };

static constexpr int kVertCapacity = 65536;

// ---------------------------------------------------------------------------
// Env helper (default OFF -- absent, empty or "0" = disabled)
// MUST be defined before the static bools that call it.
// ---------------------------------------------------------------------------

static bool envFlagDefaultOff(const char* name) {
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

// ---------------------------------------------------------------------------
// Static state
// ---------------------------------------------------------------------------

static bool              s_enabled       = envFlagDefaultOff("MC2_DEBUG_RENDERER");
static bool              s_testCanary    = envFlagDefaultOff("MC2_DEBUG_RENDERER_TEST");
static bool              s_initialized        = false;
static bool              s_capWarnedOnce      = false;
static bool              s_restoreWarnedOnce  = false;
static bool              s_drawBufWarnedOnce  = false;

static GLuint            s_vao = 0;
static GLuint            s_vbo = 0;
static glsl_program*     s_program = nullptr;
static GLuint            s_programId = 0;  // cached for glProgramUniformMatrix4fv
static GLint             s_mvpLoc = -1;

static std::vector<DbgVert> s_verts;

// ---------------------------------------------------------------------------
// Internal: init on first flush
// ---------------------------------------------------------------------------

static void init_once() {
    if (s_initialized) return;
    s_initialized = true;

    fprintf(stderr, "[DEBUGDRAW v1] event=init enabled=1\n");

    // Load shader (no #version in files; prefix injected here per project rule)
    s_program = glsl_program::makeProgram(
        "debug_prim",
        "shaders/debug_prim.vert",
        "shaders/debug_prim.frag",
        "#version 430\n"
    );
    if (!s_program) {
        fprintf(stderr, "[DEBUGDRAW v1] event=shader_fail -- debug draw disabled\n");
        s_enabled = false;
        return;
    }

    // Cache program ID for explicit-program uniform upload.
    // We read shp_ directly (public field, same as gos_static_prop_batcher.cpp:539 and
    // gameos_graphics.cpp:2094) instead of going through apply() -- apply() flushes dirty
    // matrix uniforms with GL_TRUE (shader_builder.cpp:781), wrong for our GL_FALSE MVP.
    s_programId = s_program->shp_;

    s_mvpLoc = glGetUniformLocation(s_programId, "uMVP");
    if (s_mvpLoc < 0) {
        fprintf(stderr, "[DEBUGDRAW v1] event=uniform_missing name=uMVP -- debug draw disabled\n");
        s_enabled = false;
        return;
    }

    // VAO + VBO
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);

    // Allocate buffer at max capacity once
    glBufferData(GL_ARRAY_BUFFER,
                 kVertCapacity * (GLsizeiptr)sizeof(DbgVert),
                 nullptr, GL_DYNAMIC_DRAW);

    // layout(location=0) aPosition: vec3 at offset 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(DbgVert),
                          (const void*)offsetof(DbgVert, x));

    // layout(location=1) aColor: vec4 at offset 12
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                          sizeof(DbgVert),
                          (const void*)offsetof(DbgVert, r));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // Drain any errors left in the GL queue by makeProgram introspection.
    // shader_builder::apply() checks glGetError() after each glUniform* call;
    // a stale error here would be mis-attributed to the next program that calls apply().
    while (glGetError() != GL_NO_ERROR) {}

    s_verts.reserve(4096);
}

// ---------------------------------------------------------------------------
// Color unpack helper
// ---------------------------------------------------------------------------

static DbgVert makeVert(float x, float y, float z, uint32_t rgba) {
    DbgVert v;
    v.x = x; v.y = y; v.z = z;
    v.r = ((rgba >> 24) & 0xFF) / 255.0f;
    v.g = ((rgba >> 16) & 0xFF) / 255.0f;
    v.b = ((rgba >>  8) & 0xFF) / 255.0f;
    v.a = ((rgba >>  0) & 0xFF) / 255.0f;
    return v;
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
    // 12 edges: explicit list, no __builtin_popcount (MSVC portability)
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

void drawTextWorld(Vec3 /*pos*/, const char* /*text*/, uint32_t /*rgba*/) {
    // Reserved: world-space text label. No-op in M1/M2.
    // Future spec: billboard projected from pos to screen, rendered in
    // flushScreenPrims (post post-process) via a font atlas (M3 TBD).
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

    if (!s_initialized) init_once();
    if (!s_enabled) { s_verts.clear(); return; }  // init_once() may disable on shader fail

    const float* vp = gos_GetTerrainMVPMat4();
    if (!vp) {
        s_verts.clear();
        return;
    }

    // Truncate to capacity
    if (s_verts.size() > static_cast<size_t>(kVertCapacity)) {
        if (!s_capWarnedOnce) {
            fprintf(stderr, "[DEBUGDRAW v1] event=vert_cap_exceeded cap=%d requested=%d\n",
                    kVertCapacity, (int)s_verts.size());
            s_capWarnedOnce = true;
        }
        s_verts.resize(kVertCapacity);
        // Ensure even count for GL_LINES (pairs)
        if (s_verts.size() % 2 != 0) s_verts.pop_back();
    }

    const GLsizeiptr byteSize = (GLsizeiptr)(s_verts.size() * sizeof(DbgVert));

    // --- Save GL state ---
    GLint  prevProg = 0, prevVAO = 0, prevVBO = 0;
    GLint  depthFuncWas = 0;
    GLint  blendSrcRGB = 0, blendDstRGB = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
    GLint  blendEqRGB = 0, blendEqAlpha = 0;
    GLboolean depthTestWas = GL_FALSE, blendWas = GL_FALSE, cullWas = GL_FALSE, depthMaskWas = GL_FALSE;
    GLfloat lineWidthWas = 1.0f;
    GLint  drawBuf0Was = GL_NONE;  // for draw-buffer preservation assertion

    glGetIntegerv(GL_CURRENT_PROGRAM,      &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevVBO);
    depthTestWas = glIsEnabled(GL_DEPTH_TEST);
    blendWas     = glIsEnabled(GL_BLEND);
    cullWas      = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK,      &depthMaskWas);
    glGetIntegerv(GL_DEPTH_FUNC,           &depthFuncWas);
    glGetIntegerv(GL_BLEND_SRC_RGB,        &blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB,        &blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,      &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA,      &blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB,   &blendEqRGB);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
    glGetFloatv(GL_LINE_WIDTH,             &lineWidthWas);
    glGetIntegerv(GL_DRAW_BUFFER0,         &drawBuf0Was);

    // --- Set known state ---
    // Use glUseProgram directly (not s_program->apply()) because apply() would
    // flush dirty matrix uniforms with GL_TRUE -- wrong for our GL_FALSE MVP.
    glUseProgram(s_programId);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE,       GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glDisable(GL_CULL_FACE);
    glLineWidth(1.0f);

    // Upload MVP. GL_FALSE: matrix from gos_GetTerrainMVPMat4 is already
    // row-major (converted at gos_SetWorldToClipGL). All terrain upload
    // sites use GL_FALSE with the same matrix (gameos_graphics.cpp:5012,
    // 5091, 5202, 5245, 7376).
    glProgramUniformMatrix4fv(s_programId, s_mvpLoc, 1, GL_FALSE, vp);

    // Orphan at full capacity for stable allocation; upload only the needed range.
    glBufferData(GL_ARRAY_BUFFER,
                 kVertCapacity * (GLsizeiptr)sizeof(DbgVert),
                 nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, byteSize, s_verts.data());

    glDrawArrays(GL_LINES, 0, (GLsizei)s_verts.size());

    // --- Restore GL state ---
    glDepthMask(depthMaskWas);
    if (depthTestWas) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blendWas)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (cullWas)      glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);
    glDepthFunc(depthFuncWas);
    glBlendFuncSeparate(blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha);
    glBlendEquationSeparate(blendEqRGB, blendEqAlpha);
    glLineWidth(lineWidthWas);
    glUseProgram(prevProg);
    glBindVertexArray(prevVAO);
    glBindBuffer(GL_ARRAY_BUFFER, prevVBO);

    // State restore assertion: verify critical bindings were fully restored.
    // Fires once on first mismatch to catch incomplete-restore regressions.
    if (!s_restoreWarnedOnce) {
        GLint chkProg = 0, chkVAO = 0, chkVBO = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM,      &chkProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &chkVAO);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &chkVBO);
        if (chkProg != prevProg || chkVAO != prevVAO || chkVBO != prevVBO) {
            fprintf(stderr,
                "[DEBUGDRAW v1] event=restore_mismatch "
                "prog exp=%d got=%d  vao exp=%d got=%d  vbo exp=%d got=%d\n",
                prevProg, chkProg, prevVAO, chkVAO, prevVBO, chkVBO);
            s_restoreWarnedOnce = true;
        }
    }

    // Draw-buffer preservation assertion: our flush must not touch draw buffers.
    if (!s_drawBufWarnedOnce) {
        GLint chkDrawBuf0 = GL_NONE;
        glGetIntegerv(GL_DRAW_BUFFER0, &chkDrawBuf0);
        if (chkDrawBuf0 != drawBuf0Was) {
            fprintf(stderr,
                "[DEBUGDRAW v1] event=drawbuf_mismatch "
                "expected=0x%X got=0x%X\n",
                (unsigned)drawBuf0Was, (unsigned)chkDrawBuf0);
            s_drawBufWarnedOnce = true;
        }
    }

    // Drain any errors generated by our GL calls (e.g. glLineWidth > 1.0 on some
    // drivers, or driver-side validation during glDrawArrays). shader_builder::apply()
    // checks glGetError() after each glUniform* call, so a stale error here would be
    // mis-attributed to the next program that calls apply().
    while (glGetError() != GL_NO_ERROR) {}

    s_verts.clear();
}

void flushScreenPrims() {
    // reserved for future crisp HUD/debug-text pass
}

} // namespace DebugRenderer
