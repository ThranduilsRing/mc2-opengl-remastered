#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <GL/glew.h>
#include "gl_state_guard.h"

#include <cstdio>
#include <cstdlib>

namespace {
int  g_invalidateCallCount = 0;
GLboolean g_blendStateWhenInvalidateFired = GL_FALSE;
}

void __stdcall gos_InvalidateRenderStateCache() {
    ++g_invalidateCallCount;
    g_blendStateWhenInvalidateFired = glIsEnabled(GL_BLEND);
}

// Each case owns a hidden context; a failed constructor still releases its acquired resources.
namespace {
struct VideoSubsystem {
    VideoSubsystem() {
        const int rc = SDL_InitSubSystem(SDL_INIT_VIDEO);
        INFO(SDL_GetError());
        REQUIRE(rc == 0);
    }
    ~VideoSubsystem() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
};
struct DeleteWindow { void operator()(SDL_Window* p) const { SDL_DestroyWindow(p); } };
struct DeleteContext { void operator()(void* p) const { SDL_GL_DeleteContext(p); } };
struct GLFixture {
    VideoSubsystem video;
    std::unique_ptr<SDL_Window, DeleteWindow> window;
    std::unique_ptr<void, DeleteContext> context;
    GLFixture() {
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
        window.reset(SDL_CreateWindow("mc2_tests glstate guards", SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN));
        INFO(SDL_GetError());
        REQUIRE(window != nullptr);
        context.reset(SDL_GL_CreateContext(window.get()));
        REQUIRE(context != nullptr);
        REQUIRE(SDL_GL_MakeCurrent(window.get(), context.get()) == 0);
        glewExperimental = GL_TRUE;
        const GLenum error = glewInit();
        glGetError();
        REQUIRE((error == GLEW_OK || glBindVertexArray != nullptr));
    }
};
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/ssbo_binding_restore") {
    GLuint bufA = 0, bufB = 0;
    glGenBuffers(1, &bufA);
    glGenBuffers(1, &bufB);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bufA);
    {
        mc2gl::GlScopedSsboBinding guard(5);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, bufB);
        GLint mid = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &mid);
        CHECK(((GLuint)mid == bufB));
    }
    GLint after = 0;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &after);
    CHECK(((GLuint)after == bufA));
    glDeleteBuffers(1, &bufA);
    glDeleteBuffers(1, &bufB);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/capability_set_ctor") {
    glDisable(GL_BLEND);
    CHECK((glIsEnabled(GL_BLEND) == GL_FALSE));
    {
        mc2gl::GlScopedCapability guard(GL_BLEND, true);
        CHECK((glIsEnabled(GL_BLEND) == GL_TRUE));
    }
    CHECK((glIsEnabled(GL_BLEND) == GL_FALSE));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/capability_capture_only_ctor") {
    glEnable(GL_CULL_FACE);
    CHECK((glIsEnabled(GL_CULL_FACE) == GL_TRUE));
    {
        mc2gl::GlScopedCapability guard(GL_CULL_FACE);
        CHECK((glIsEnabled(GL_CULL_FACE) == GL_TRUE));
        glDisable(GL_CULL_FACE);
        CHECK((glIsEnabled(GL_CULL_FACE) == GL_FALSE));
        glEnable(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        CHECK((glIsEnabled(GL_CULL_FACE) == GL_FALSE));
    }

    CHECK((glIsEnabled(GL_CULL_FACE) == GL_TRUE));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/depth_state_set_ctor") {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    {
        mc2gl::GlScopedDepthState guard(GL_FALSE, GL_GEQUAL);
        GLboolean mask = GL_TRUE; glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
        GLint func = 0; glGetIntegerv(GL_DEPTH_FUNC, &func);
        CHECK((mask == GL_FALSE));
        CHECK(((GLenum)func == GL_GEQUAL));
    }
    GLboolean mask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
    GLint func = 0; glGetIntegerv(GL_DEPTH_FUNC, &func);
    CHECK((mask == GL_TRUE));
    CHECK(((GLenum)func == GL_LESS));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/depth_state_capture_only_ctor") {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    {
        mc2gl::GlScopedDepthState guard;
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_GEQUAL);
    }
    GLboolean mask = GL_FALSE; glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
    GLint func = 0; glGetIntegerv(GL_DEPTH_FUNC, &func);
    CHECK((mask == GL_TRUE));
    CHECK(((GLenum)func == GL_LEQUAL));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/clip_control") {
    if (!GLEW_ARB_clip_control) {
        std::fprintf(stderr, "    SKIP: GL_ARB_clip_control unavailable on this GPU/driver\n");
        return;
    }
    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
    {
        mc2gl::GlScopedClipControl guard;
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        GLint origin = 0, depth = 0;
        glGetIntegerv(GL_CLIP_ORIGIN, &origin);
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &depth);
        CHECK(((GLenum)origin == GL_UPPER_LEFT));
        CHECK(((GLenum)depth  == GL_ZERO_TO_ONE));
    }
    GLint origin = 0, depth = 0;
    glGetIntegerv(GL_CLIP_ORIGIN, &origin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &depth);
    CHECK(((GLenum)origin == GL_LOWER_LEFT));
    CHECK(((GLenum)depth  == GL_NEGATIVE_ONE_TO_ONE));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/texture_unit_default_target") {
    GLuint texA = 0, texB = 0;
    glGenTextures(1, &texA);
    glGenTextures(1, &texB);
    glActiveTexture(GL_TEXTURE3);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texA);
    glActiveTexture(GL_TEXTURE2);
    {
        mc2gl::GlScopedTextureUnit guard(0);
        GLint active = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        CHECK(((GLenum)active == GL_TEXTURE0));
        glBindTexture(GL_TEXTURE_2D, texB);
        GLint bound = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        CHECK(((GLuint)bound == texB));
    }
    GLint activeAfter = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &activeAfter);
    CHECK(((GLenum)activeAfter == GL_TEXTURE2));
    glActiveTexture(GL_TEXTURE0);
    GLint boundAfter = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundAfter);
    CHECK(((GLuint)boundAfter == texA));
    glDeleteTextures(1, &texA);
    glDeleteTextures(1, &texB);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/texture_unit_array_target") {
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
        CHECK(((GLuint)bound == arrB));
    }
    glActiveTexture(GL_TEXTURE5);
    GLint boundAfter = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &boundAfter);
    CHECK(((GLuint)boundAfter == arrA));
    glActiveTexture(GL_TEXTURE0);
    glDeleteTextures(1, &arrA);
    glDeleteTextures(1, &arrB);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/program") {

    glUseProgram(0);
    {
        mc2gl::GlScopedProgram guard;

        GLint cur = -1; glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
        CHECK((cur == 0));
    }
    GLint after = -1; glGetIntegerv(GL_CURRENT_PROGRAM, &after);
    CHECK((after == 0));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/vertex_array") {
    GLuint vaoA = 0, vaoB = 0;
    glGenVertexArrays(1, &vaoA);
    glGenVertexArrays(1, &vaoB);
    glBindVertexArray(vaoA);
    {
        mc2gl::GlScopedVertexArray guard;
        glBindVertexArray(vaoB);
        GLint bound = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &bound);
        CHECK(((GLuint)bound == vaoB));
    }
    GLint after = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &after);
    CHECK(((GLuint)after == vaoA));
    glDeleteVertexArrays(1, &vaoA);
    glDeleteVertexArrays(1, &vaoB);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/buffer_binding") {
    GLuint bufA = 0, bufB = 0, elemA = 0, elemB = 0;
    glGenBuffers(1, &bufA);  glGenBuffers(1, &bufB);
    glGenBuffers(1, &elemA); glGenBuffers(1, &elemB);

    glBindBuffer(GL_ARRAY_BUFFER, bufA);
    {
        mc2gl::GlScopedBufferBinding guard(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, bufB);
        GLint bound = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound);
        CHECK(((GLuint)bound == bufB));
    }
    GLint afterArr = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &afterArr);
    CHECK(((GLuint)afterArr == bufA));

    GLuint vao = 0; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elemA);
    {
        mc2gl::GlScopedBufferBinding guard(GL_ELEMENT_ARRAY_BUFFER);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elemB);
        GLint bound = 0; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &bound);
        CHECK(((GLuint)bound == elemB));
    }
    GLint afterElem = 0; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &afterElem);
    CHECK(((GLuint)afterElem == elemA));

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &bufA);  glDeleteBuffers(1, &bufB);
    glDeleteBuffers(1, &elemA); glDeleteBuffers(1, &elemB);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/blend_func_separate_alpha_precision") {

    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    {
        mc2gl::GlScopedBlendFunc guard;
        glBlendFunc(GL_ONE, GL_ONE);
        GLint srcRgb = 0, dstRgb = 0, srcA = 0, dstA = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        CHECK(((GLenum)srcRgb == GL_ONE && (GLenum)dstRgb == GL_ONE));
        CHECK(((GLenum)srcA == GL_ONE && (GLenum)dstA == GL_ONE));
    }
    GLint srcRgb = 0, dstRgb = 0, srcA = 0, dstA = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
    CHECK(((GLenum)srcRgb == GL_SRC_ALPHA && (GLenum)dstRgb == GL_ONE_MINUS_SRC_ALPHA));

    CHECK(((GLenum)srcA == GL_ONE && (GLenum)dstA == GL_ZERO));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/sampler") {
    GLuint sampA = 0, sampB = 0;
    glGenSamplers(1, &sampA);
    glGenSamplers(1, &sampB);
    glBindSampler(3, sampA);
    {
        mc2gl::GlScopedSampler guard(3);
        glBindSampler(3, sampB);
        GLint bound = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &bound);
        CHECK(((GLuint)bound == sampB));
    }
    GLint after = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &after);
    CHECK(((GLuint)after == sampA));
    glDeleteSamplers(1, &sampA);
    glDeleteSamplers(1, &sampB);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/cull_face_mode") {
    glCullFace(GL_FRONT);
    {
        mc2gl::GlScopedCullFaceMode guard;
        glCullFace(GL_BACK);
        GLint mode = 0; glGetIntegerv(GL_CULL_FACE_MODE, &mode);
        CHECK(((GLenum)mode == GL_BACK));
    }
    GLint after = 0; glGetIntegerv(GL_CULL_FACE_MODE, &after);
    CHECK(((GLenum)after == GL_FRONT));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/color_mask") {
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    {
        mc2gl::GlScopedColorMask guard;
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        GLboolean mask[4] = {};
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        CHECK((!mask[0] && !mask[1] && !mask[2] && !mask[3]));
    }
    GLboolean mask[4] = {};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    CHECK((mask[0] == GL_TRUE && mask[1] == GL_FALSE &&
                mask[2] == GL_TRUE && mask[3] == GL_FALSE));
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/draw_buffers_mrt") {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);

    const GLenum triple[3] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, triple);

    {
        mc2gl::GlScopedDrawBuffers guard;
        CHECK((guard.savedCount() == 3));
        const GLenum single[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, single);
        GLint d0 = 0; glGetIntegerv(GL_DRAW_BUFFER0, &d0);
        CHECK(((GLenum)d0 == GL_COLOR_ATTACHMENT0));
    }

    GLint d0 = 0, d1 = 0, d2 = 0;
    glGetIntegerv(GL_DRAW_BUFFER0, &d0);
    glGetIntegerv(GL_DRAW_BUFFER0 + 1, &d1);
    glGetIntegerv(GL_DRAW_BUFFER0 + 2, &d2);
    CHECK(((GLenum)d0 == GL_COLOR_ATTACHMENT0));
    CHECK(((GLenum)d1 == GL_NONE));
    CHECK(((GLenum)d2 == GL_COLOR_ATTACHMENT2));

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/draw_buffers_all_none_noop") {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    const GLenum none1[1] = { GL_NONE };
    glDrawBuffers(1, none1);
    {
        mc2gl::GlScopedDrawBuffers guard;
        CHECK((guard.savedCount() == 0));
    }

    CHECK((glGetError() == GL_NO_ERROR));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return;
}

TEST_CASE_FIXTURE(GLFixture, "glstate_guard_harness/invalidate_fires_last") {
    const int callsBefore = g_invalidateCallCount;
    glDisable(GL_BLEND);
    {
        mc2gl::GlScopedRenderStateInvalidate stateResync;
        mc2gl::GlScopedCapability            guardBlend(GL_BLEND, true);
        CHECK((glIsEnabled(GL_BLEND) == GL_TRUE));
    }
    CHECK((g_invalidateCallCount == callsBefore + 1));

    CHECK((g_blendStateWhenInvalidateFired == GL_FALSE));
    CHECK((glIsEnabled(GL_BLEND) == GL_FALSE));
    return;
}
