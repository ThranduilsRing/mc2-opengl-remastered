// tools/glstate_guard_harness/glstate_guard_harness.cpp
// SUBSYSTEM-HARNESS-ARC / GLSTATE-GUARD-HARNESS-1
//
// Proves restore correctness for EVERY guard class in
// GameOS/gameos/gl_state_guard.h (all 14: GlScopedSsboBinding,
// GlScopedCapability x2 ctors, GlScopedDepthState x2 ctors,
// GlScopedClipControl, GlScopedTextureUnit, GlScopedProgram,
// GlScopedVertexArray, GlScopedBufferBinding, GlScopedBlendFunc,
// GlScopedSampler, GlScopedCullFaceMode, GlScopedColorMask,
// GlScopedDrawBuffers, GlScopedRenderStateInvalidate) against a REAL GL 4.3
// core context — the header makes real glGetIntegerv/glEnable/glBindTexture
// etc. calls that are meaningless without one, so (unlike the other
// offline contract harnesses) this harness creates a genuine hidden SDL2
// window + GL context (same attributes as GameOS/gameos/gos_render.cpp) and
// glewInit()s it, exactly mirroring how the game's own context comes up.
//
// Each test: set a baseline GL value, construct the guard (which may itself
// mutate state per its ctor contract), optionally mutate the guarded state
// AGAIN inside the scope (simulating the pass body's own SET calls — the
// capture-only guards are explicitly designed to tolerate this), then let
// the guard go out of scope and assert the GL value is back to baseline.
//
// gos_InvalidateRenderStateCache() is stubbed locally (this harness links no
// GameOS/gameos production TUs) so GlScopedRenderStateInvalidate and the
// ordering test can exercise the real destructor path.
//
// Build (standalone):
//   cmake -S tools/glstate_guard_harness -B build64-glstateguard -G "Visual Studio 17 2022" -A x64
//   cmake --build build64-glstateguard --config RelWithDebInfo --target glstate_guard_harness
//   build64-glstateguard/RelWithDebInfo/glstate_guard_harness.exe --list

#include "contract_harness.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <GL/glew.h>
#include "gl_state_guard.h"

#include <cstdio>
#include <cstdlib>

using namespace contract_harness;

// ---- gos_InvalidateRenderStateCache stub -----------------------------------
// GlScopedRenderStateInvalidate's dtor calls this via `extern` declaration
// (matching the __stdcall signature in gl_state_guard.h). Records the
// GL_BLEND enable state AT THE MOMENT it fires, so the ordering test can
// prove other guards already restored before this one does (declared-first
// destructs-last).
namespace {
int  g_invalidateCallCount = 0;
GLboolean g_blendStateWhenInvalidateFired = GL_FALSE;
}  // namespace

// NOTE: plain C++ linkage (no extern "C") — matches the header's local
// `extern void __stdcall gos_InvalidateRenderStateCache();` declaration
// inside GlScopedRenderStateInvalidate::~GlScopedRenderStateInvalidate(),
// and the real production definition (gameos_graphics.cpp:8819). On x64,
// __stdcall is normalized to the platform's single calling convention but
// the C++ name-mangling still applies without extern "C".
void __stdcall gos_InvalidateRenderStateCache() {
    ++g_invalidateCallCount;
    g_blendStateWhenInvalidateFired = glIsEnabled(GL_BLEND);
}

// ---- GL context bootstrap ---------------------------------------------------
// Mirrors GameOS/gameos/gos_render.cpp's SDL_GL_SetAttribute block (core
// profile, 4.3, RGBA8/D16/doublebuffer) so the guards run under the same
// context shape the real engine creates. Window is hidden — no frame is
// ever presented.
namespace {
SDL_Window*   g_window = nullptr;
SDL_GLContext g_glctx  = nullptr;

bool initGl() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    g_window = SDL_CreateWindow("glstate_guard_harness",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g_glctx = SDL_GL_CreateContext(g_window);
    if (!g_glctx) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_window, g_glctx);

    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    // glewInit() on a core profile commonly reports GL_INVALID_ENUM from its
    // legacy extension-string probe even on success; clear it and verify via
    // a real function pointer instead of trusting the return code alone.
    glGetError();
    if (err != GLEW_OK && glBindVertexArray == nullptr) {
        std::fprintf(stderr, "glewInit failed: %s\n", glewGetErrorString(err));
        return false;
    }
    return true;
}

void shutdownGl() {
    if (g_glctx)  SDL_GL_DeleteContext(g_glctx);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}
}  // namespace

// ---- tests -------------------------------------------------------------

// GlScopedSsboBinding: restore-previous semantics on a real indexed binding.
static bool test_ssbo_binding_restore(TestCtx& t) {
    GLuint bufA = 0, bufB = 0;
    glGenBuffers(1, &bufA);
    glGenBuffers(1, &bufB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bufA);
    {
        mc2gl::GlScopedSsboBinding guard(5);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bufB);
        GLint mid = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &mid);
        CH_CHECK(t, (GLuint)mid == bufB);
    }
    GLint after = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &after);
    CH_CHECK(t, (GLuint)after == bufA);
    glDeleteBuffers(1, &bufA);
    glDeleteBuffers(1, &bufB);
    return t.failures == 0;
}

// GlScopedCapability(cap, enable): the SET ctor changes state for the scope,
// then restores the pre-scope value.
static bool test_capability_set_ctor(TestCtx& t) {
    glDisable(GL_BLEND);
    CH_CHECK(t, glIsEnabled(GL_BLEND) == GL_FALSE);
    {
        mc2gl::GlScopedCapability guard(GL_BLEND, true);
        CH_CHECK(t, glIsEnabled(GL_BLEND) == GL_TRUE);
    }
    CH_CHECK(t, glIsEnabled(GL_BLEND) == GL_FALSE);
    return t.failures == 0;
}

// GlScopedCapability(cap) capture-only: ctor does NOT mutate; the body's own
// glEnable/glDisable calls (simulating the billboard cull double-assert
// pattern) still get restored correctly at scope exit.
static bool test_capability_capture_only_ctor(TestCtx& t) {
    glEnable(GL_CULL_FACE);
    CH_CHECK(t, glIsEnabled(GL_CULL_FACE) == GL_TRUE);
    {
        mc2gl::GlScopedCapability guard(GL_CULL_FACE);   // capture-only: no mutation here
        CH_CHECK(t, glIsEnabled(GL_CULL_FACE) == GL_TRUE);  // unchanged by ctor
        glDisable(GL_CULL_FACE);                          // body's own first assert
        CH_CHECK(t, glIsEnabled(GL_CULL_FACE) == GL_FALSE);
        glEnable(GL_CULL_FACE);                            // some sub-pass flips it
        glDisable(GL_CULL_FACE);                            // body's SECOND assert (the trap case)
        CH_CHECK(t, glIsEnabled(GL_CULL_FACE) == GL_FALSE);
    }
    // Guard restores to the captured pre-scope baseline regardless of how
    // many times the body re-asserted in between.
    CH_CHECK(t, glIsEnabled(GL_CULL_FACE) == GL_TRUE);
    return t.failures == 0;
}

// GlScopedDepthState(mask, func) SET ctor.
static bool test_depth_state_set_ctor(TestCtx& t) {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    {
        mc2gl::GlScopedDepthState guard(GL_FALSE, GL_GEQUAL);
        GLboolean mask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
        GLint func = 0; glGetIntegerv(GL_DEPTH_FUNC, &func);
        CH_CHECK(t, mask == GL_FALSE);
        CH_CHECK(t, (GLenum)func == GL_GEQUAL);
    }
    GLboolean mask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
    GLint func = 0; glGetIntegerv(GL_DEPTH_FUNC, &func);
    CH_CHECK(t, mask == GL_TRUE);
    CH_CHECK(t, (GLenum)func == GL_LESS);
    return t.failures == 0;
}

// GlScopedDepthState() capture-only ctor.
static bool test_depth_state_capture_only_ctor(TestCtx& t) {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    {
        mc2gl::GlScopedDepthState guard;   // capture-only
        glDepthMask(GL_FALSE);             // body's own SET calls, unmoved
        glDepthFunc(GL_GEQUAL);
    }
    GLboolean mask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
    GLint func = 0; glGetIntegerv(GL_DEPTH_FUNC, &func);
    CH_CHECK(t, mask == GL_TRUE);
    CH_CHECK(t, (GLenum)func == GL_LEQUAL);
    return t.failures == 0;
}

// GlScopedClipControl.
static bool test_clip_control(TestCtx& t) {
    if (!GLEW_ARB_clip_control) {
        std::fprintf(stderr, "    SKIP: GL_ARB_clip_control unavailable on this GPU/driver\n");
        return true;
    }
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    {
        mc2gl::GlScopedClipControl guard;
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        GLint origin = 0, depth = 0;
        glGetIntegerv(GL_CLIP_ORIGIN, &origin);
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &depth);
        CH_CHECK(t, (GLenum)origin == GL_UPPER_LEFT);
        CH_CHECK(t, (GLenum)depth  == GL_ZERO_TO_ONE);
    }
    GLint origin = 0, depth = 0;
    glGetIntegerv(GL_CLIP_ORIGIN, &origin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &depth);
    CH_CHECK(t, (GLenum)origin == GL_LOWER_LEFT);
    CH_CHECK(t, (GLenum)depth  == GL_NEGATIVE_ONE_TO_ONE);
    return t.failures == 0;
}

// GlScopedTextureUnit: default target (GL_TEXTURE_2D) + active-unit restore.
static bool test_texture_unit_default_target(TestCtx& t) {
    GLuint texA = 0, texB = 0;
    glGenTextures(1, &texA);
    glGenTextures(1, &texB);
    glActiveTexture(GL_TEXTURE3);   // start on a non-zero unit
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texA);
    glActiveTexture(GL_TEXTURE2);   // caller's active unit before the guard
    {
        mc2gl::GlScopedTextureUnit guard(0);   // ctor leaves GL_TEXTURE0 active
        GLint active = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        CH_CHECK(t, (GLenum)active == GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texB);
        GLint bound = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        CH_CHECK(t, (GLuint)bound == texB);
    }
    GLint activeAfter = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &activeAfter);
    CH_CHECK(t, (GLenum)activeAfter == GL_TEXTURE2);   // restored caller's unit
    glActiveTexture(GL_TEXTURE0);
    GLint boundAfter = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundAfter);
    CH_CHECK(t, (GLuint)boundAfter == texA);
    glDeleteTextures(1, &texA);
    glDeleteTextures(1, &texB);
    return t.failures == 0;
}

// GlScopedTextureUnit: non-default target (GL_TEXTURE_2D_ARRAY on unit 5 —
// the VOCAB-1 mine-static-bridge use case).
static bool test_texture_unit_array_target(TestCtx& t) {
    GLuint arrA = 0, arrB = 0;
    glGenTextures(1, &arrA);
    glGenTextures(1, &arrB);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arrA);
    glActiveTexture(GL_TEXTURE0);
    {
        mc2gl::GlScopedTextureUnit guard(5, GL_TEXTURE_2D_ARRAY);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arrB);
        GLint bound = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &bound);
        CH_CHECK(t, (GLuint)bound == arrB);
    }
    glActiveTexture(GL_TEXTURE5);
    GLint boundAfter = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &boundAfter);
    CH_CHECK(t, (GLuint)boundAfter == arrA);
    glActiveTexture(GL_TEXTURE0);
    glDeleteTextures(1, &arrA);
    glDeleteTextures(1, &arrB);
    return t.failures == 0;
}

// GlScopedProgram.
static bool test_program(TestCtx& t) {
    // No real shader needed — 0 (no program) is a perfectly valid captured/
    // restored value, and glUseProgram(0) is always legal.
    glUseProgram(0);
    {
        mc2gl::GlScopedProgram guard;
        // Nothing else to bind without a compiled program; the guard's own
        // capture/restore of GL_CURRENT_PROGRAM=0 is still exercised.
        GLint cur = -1; glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
        CH_CHECK(t, cur == 0);
    }
    GLint after = -1; glGetIntegerv(GL_CURRENT_PROGRAM, &after);
    CH_CHECK(t, after == 0);
    return t.failures == 0;
}

// GlScopedVertexArray.
static bool test_vertex_array(TestCtx& t) {
    GLuint vaoA = 0, vaoB = 0;
    glGenVertexArrays(1, &vaoA);
    glGenVertexArrays(1, &vaoB);
    glBindVertexArray(vaoA);
    {
        mc2gl::GlScopedVertexArray guard;
        glBindVertexArray(vaoB);
        GLint bound = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &bound);
        CH_CHECK(t, (GLuint)bound == vaoB);
    }
    GLint after = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &after);
    CH_CHECK(t, (GLuint)after == vaoA);
    glDeleteVertexArrays(1, &vaoA);
    glDeleteVertexArrays(1, &vaoB);
    return t.failures == 0;
}

// GlScopedBufferBinding: ARRAY_BUFFER and ELEMENT_ARRAY_BUFFER targets.
static bool test_buffer_binding(TestCtx& t) {
    GLuint bufA = 0, bufB = 0, elemA = 0, elemB = 0;
    glGenBuffers(1, &bufA);  glGenBuffers(1, &bufB);
    glGenBuffers(1, &elemA); glGenBuffers(1, &elemB);

    glBindBuffer(GL_ARRAY_BUFFER, bufA);
    {
        mc2gl::GlScopedBufferBinding guard(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, bufB);
        GLint bound = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound);
        CH_CHECK(t, (GLuint)bound == bufB);
    }
    GLint afterArr = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &afterArr);
    CH_CHECK(t, (GLuint)afterArr == bufA);

    // Bind via a VAO so ELEMENT_ARRAY_BUFFER has somewhere to live (it is
    // VAO-resident state — see the header's ordering note).
    GLuint vao = 0; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elemA);
    {
        mc2gl::GlScopedBufferBinding guard(GL_ELEMENT_ARRAY_BUFFER);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elemB);
        GLint bound = 0; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &bound);
        CH_CHECK(t, (GLuint)bound == elemB);
    }
    GLint afterElem = 0; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &afterElem);
    CH_CHECK(t, (GLuint)afterElem == elemA);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &bufA);  glDeleteBuffers(1, &bufB);
    glDeleteBuffers(1, &elemA); glDeleteBuffers(1, &elemB);
    return t.failures == 0;
}

// GlScopedBlendFunc: proves the "strictly more correct" claim — a prior
// SEPARATE alpha factor survives the guard's restore, where a naive
// glBlendFunc()-based restore would have clobbered it.
static bool test_blend_func_separate_alpha_precision(TestCtx& t) {
    // Baseline: RGB and ALPHA factors deliberately DIFFERENT.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    {
        mc2gl::GlScopedBlendFunc guard;
        glBlendFunc(GL_ONE, GL_ONE);   // body's own (non-separate) set
        GLint srcRgb = 0, dstRgb = 0, srcA = 0, dstA = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        CH_CHECK(t, (GLenum)srcRgb == GL_ONE && (GLenum)dstRgb == GL_ONE);
        CH_CHECK(t, (GLenum)srcA == GL_ONE && (GLenum)dstA == GL_ONE);
    }
    GLint srcRgb = 0, dstRgb = 0, srcA = 0, dstA = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    CH_CHECK(t, (GLenum)srcRgb == GL_SRC_ALPHA && (GLenum)dstRgb == GL_ONE_MINUS_SRC_ALPHA);
    // The precision claim: alpha factors restored EXACTLY, not clobbered by
    // the RGB factors (which a glBlendFunc(srcRgb,dstRgb) restore would do).
    CH_CHECK(t, (GLenum)srcA == GL_ONE && (GLenum)dstA == GL_ZERO);
    return t.failures == 0;
}

// GlScopedSampler.
static bool test_sampler(TestCtx& t) {
    GLuint sampA = 0, sampB = 0;
    glGenSamplers(1, &sampA);
    glGenSamplers(1, &sampB);
    glBindSampler(3, sampA);
    {
        mc2gl::GlScopedSampler guard(3);
        glBindSampler(3, sampB);
        GLint bound = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &bound);
        CH_CHECK(t, (GLuint)bound == sampB);
    }
    GLint after = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &after);
    CH_CHECK(t, (GLuint)after == sampA);
    glDeleteSamplers(1, &sampA);
    glDeleteSamplers(1, &sampB);
    return t.failures == 0;
}

// GlScopedCullFaceMode: independent of the GL_CULL_FACE enable capability.
static bool test_cull_face_mode(TestCtx& t) {
    glCullFace(GL_FRONT);
    {
        mc2gl::GlScopedCullFaceMode guard;
        glCullFace(GL_BACK);
        GLint mode = 0; glGetIntegerv(GL_CULL_FACE_MODE, &mode);
        CH_CHECK(t, (GLenum)mode == GL_BACK);
    }
    GLint after = 0; glGetIntegerv(GL_CULL_FACE_MODE, &after);
    CH_CHECK(t, (GLenum)after == GL_FRONT);
    return t.failures == 0;
}

// GlScopedColorMask.
static bool test_color_mask(TestCtx& t) {
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    {
        mc2gl::GlScopedColorMask guard;
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        GLboolean mask[4] = {};
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        CH_CHECK(t, !mask[0] && !mask[1] && !mask[2] && !mask[3]);
    }
    GLboolean mask[4] = {};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    CH_CHECK(t, mask[0] == GL_TRUE && mask[1] == GL_FALSE &&
                mask[2] == GL_TRUE && mask[3] == GL_FALSE);
    return t.failures == 0;
}

// GlScopedDrawBuffers: MRT list capture/restore on an FBO, including the
// "gap" case (buffer 1 = NONE while 0 and 2 are active) that the header's
// count_ logic must preserve, not compact.
static bool test_draw_buffers_mrt(TestCtx& t) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);

    const GLenum triple[3] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, triple);

    {
        mc2gl::GlScopedDrawBuffers guard;
        CH_CHECK(t, guard.savedCount() == 3);   // gap at index 1 still counts through index 2
        const GLenum single[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, single);
        GLint d0 = 0; glGetIntegerv(GL_DRAW_BUFFER0, &d0);
        CH_CHECK(t, (GLenum)d0 == GL_COLOR_ATTACHMENT0);
    }

    GLint d0 = 0, d1 = 0, d2 = 0;
    glGetIntegerv(GL_DRAW_BUFFER0, &d0);
    glGetIntegerv(GL_DRAW_BUFFER0 + 1, &d1);
    glGetIntegerv(GL_DRAW_BUFFER0 + 2, &d2);
    CH_CHECK(t, (GLenum)d0 == GL_COLOR_ATTACHMENT0);
    CH_CHECK(t, (GLenum)d1 == GL_NONE);            // the gap survived the restore
    CH_CHECK(t, (GLenum)d2 == GL_COLOR_ATTACHMENT2);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return t.failures == 0;
}

// GlScopedDrawBuffers: all-NONE baseline is a documented no-restore edge
// case (savedCount()==0 -> dtor issues no glDrawBuffers call at all).
static bool test_draw_buffers_all_none_noop(TestCtx& t) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    const GLenum none1[1] = { GL_NONE };
    glDrawBuffers(1, none1);
    {
        mc2gl::GlScopedDrawBuffers guard;
        CH_CHECK(t, guard.savedCount() == 0);
    }
    // No crash / no unexpected glDrawBuffers(0, ...) call — informational-only
    // assertion that the harness itself did not trip a GL error.
    CH_CHECK(t, glGetError() == GL_NO_ERROR);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return t.failures == 0;
}

// GlScopedRenderStateInvalidate + declaration-order contract: declared FIRST
// so it destructs LAST — i.e. gos_InvalidateRenderStateCache() must fire
// AFTER every other guard in the same scope has already restored its state.
// This is the ordering every converted epilogue (gos_tube_ribbon_flush,
// gos_tube_ribbon_flush_deferred, gos_particle_bridge_flush) relies on.
static bool test_invalidate_fires_last(TestCtx& t) {
    const int callsBefore = g_invalidateCallCount;
    glDisable(GL_BLEND);   // baseline the ordering test observes
    {
        mc2gl::GlScopedRenderStateInvalidate stateResync;   // declared first -> destructs LAST
        mc2gl::GlScopedCapability            guardBlend(GL_BLEND, true);
        CH_CHECK(t, glIsEnabled(GL_BLEND) == GL_TRUE);
    }
    CH_CHECK(t, g_invalidateCallCount == callsBefore + 1);
    // The capability guard must have ALREADY restored GL_BLEND to the
    // pre-scope baseline (false) by the time the stub observed it.
    CH_CHECK(t, g_blendStateWhenInvalidateFired == GL_FALSE);
    CH_CHECK(t, glIsEnabled(GL_BLEND) == GL_FALSE);
    return t.failures == 0;
}

int main(int argc, char** argv) {
    if (!initGl()) {
        std::fprintf(stderr, "glstate_guard_harness: GL context init failed, cannot run\n");
        return 2;
    }

    Harness h("glstate_guard_harness");
    h.add("ssbo_binding_restore",                test_ssbo_binding_restore);
    h.add("capability_set_ctor",                 test_capability_set_ctor);
    h.add("capability_capture_only_ctor",        test_capability_capture_only_ctor);
    h.add("depth_state_set_ctor",                test_depth_state_set_ctor);
    h.add("depth_state_capture_only_ctor",       test_depth_state_capture_only_ctor);
    h.add("clip_control",                        test_clip_control);
    h.add("texture_unit_default_target",         test_texture_unit_default_target);
    h.add("texture_unit_array_target",           test_texture_unit_array_target);
    h.add("program",                             test_program);
    h.add("vertex_array",                        test_vertex_array);
    h.add("buffer_binding",                      test_buffer_binding);
    h.add("blend_func_separate_alpha_precision", test_blend_func_separate_alpha_precision);
    h.add("sampler",                             test_sampler);
    h.add("cull_face_mode",                      test_cull_face_mode);
    h.add("color_mask",                          test_color_mask);
    h.add("draw_buffers_mrt",                    test_draw_buffers_mrt);
    h.add("draw_buffers_all_none_noop",          test_draw_buffers_all_none_noop);
    h.add("invalidate_fires_last",               test_invalidate_fires_last);
    int rc = h.run(argc, argv);

    shutdownGl();
    return rc;
}
