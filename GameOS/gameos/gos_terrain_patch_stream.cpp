// GameOS/gameos/gos_terrain_patch_stream.cpp
#include "gos_terrain_patch_stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gameos.hpp"   // gos_VERTEX, gos_TERRAIN_EXTRA, DWORD
#include "gl/glew.h"    // OpenGL — same include the static-prop batcher uses

namespace {
    bool s_killswitch = false;
    bool s_initOk     = false;
    bool s_traceOn    = false;

    // GL handles. Two separate buffers — one for color, one for extras.
    GLuint s_colorBuf  = 0;
    GLuint s_extrasBuf = 0;

    // Persistent-mapped CPU pointers. Indexed by [slot * bytesPerSlot + offset].
    void* s_colorMap   = nullptr;
    void* s_extrasMap  = nullptr;

    // Fences per slot. Created at end of flush(); consumed at beginFrame() before
    // re-using the slot. NULL means "no fence yet" (first N frames).
    GLsync s_fence[kPatchStreamRingFrames] = { 0, 0, 0 };

    uint32_t s_slot = 0;  // index of the slot currently being written

    // Drop GL state we touched, mirroring gos_static_prop_batcher's save/restore.
    struct SavedGLState {
        GLint  arrayBuf      = 0;
        GLint  vao           = 0;
        GLboolean blend      = GL_FALSE;
        GLboolean depthTest  = GL_FALSE;
    };

    void saveGLState(SavedGLState& s) {
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING,  &s.arrayBuf);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING,  &s.vao);
        s.blend     = glIsEnabled(GL_BLEND);
        s.depthTest = glIsEnabled(GL_DEPTH_TEST);
    }

    void restoreGLState(const SavedGLState& s) {
        glBindBuffer(GL_ARRAY_BUFFER, s.arrayBuf);
        glBindVertexArray(s.vao);
        if (s.blend)     glEnable (GL_BLEND);     else glDisable(GL_BLEND);
        if (s.depthTest) glEnable (GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    }
}

static GLuint allocPersistentBuffer(GLsizeiptr totalBytes, void** outMappedPtr) {
    const GLbitfield flags = GL_MAP_WRITE_BIT
                           | GL_MAP_PERSISTENT_BIT
                           | GL_MAP_COHERENT_BIT;

    GLuint id = 0;
    glGenBuffers(1, &id);
    if (!id) return 0;

    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferStorage(GL_ARRAY_BUFFER, totalBytes, nullptr, flags);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteBuffers(1, &id);
        return 0;
    }

    void* p = glMapBufferRange(GL_ARRAY_BUFFER, 0, totalBytes, flags);
    if (!p) {
        glDeleteBuffers(1, &id);
        return 0;
    }
    *outMappedPtr = p;
    return id;
}

bool TerrainPatchStream::init()
{
    const char* env = getenv("MC2_MODERN_TERRAIN_SURFACE");
    s_killswitch = (env != nullptr) && (env[0] == '1');
    s_traceOn    = (getenv("MC2_PATCH_STREAM_TRACE") != nullptr);

    if (!s_killswitch) return true;

    // Step 4 bailout — env-gated forced init failure for testing the
    // fallback path without driver stress. Default off; intentional debug
    // instrumentation, leave in tree (memory/debug_instrumentation_rule.md).
    if (getenv("MC2_PATCH_STREAM_FORCE_INIT_FAIL")) {
        fprintf(stderr,
            "[PATCH_STREAM v1] event=init_fail reason=force_env\n");
        fflush(stderr);
        s_killswitch = false;
        return true;  // engine continues on legacy
    }

    SavedGLState saved;
    saveGLState(saved);

    const GLsizeiptr colorTotal  = (GLsizeiptr)kPatchStreamColorBytesPerSlot  * kPatchStreamRingFrames;
    const GLsizeiptr extrasTotal = (GLsizeiptr)kPatchStreamExtrasBytesPerSlot * kPatchStreamRingFrames;

    s_colorBuf  = allocPersistentBuffer(colorTotal,  &s_colorMap);
    s_extrasBuf = allocPersistentBuffer(extrasTotal, &s_extrasMap);

    restoreGLState(saved);

    if (!s_colorBuf || !s_extrasBuf) {
        // Init-fail path. Force killswitch off for the rest of the process.
        fprintf(stderr,
            "[PATCH_STREAM v1] event=init_fail reason=glBufferStorage_or_map "
            "colorBuf=%u extrasBuf=%u\n", s_colorBuf, s_extrasBuf);
        fflush(stderr);
        if (s_colorBuf) {
            glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
            if (s_colorMap) { glUnmapBuffer(GL_ARRAY_BUFFER); s_colorMap = nullptr; }
            glDeleteBuffers(1, &s_colorBuf);
            s_colorBuf = 0;
        }
        if (s_extrasBuf) {
            glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
            if (s_extrasMap) { glUnmapBuffer(GL_ARRAY_BUFFER); s_extrasMap = nullptr; }
            glDeleteBuffers(1, &s_extrasBuf);
            s_extrasBuf = 0;
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);  // leave clean
        s_killswitch = false;
        return true;  // engine continues on legacy path
    }

    s_initOk = true;
    fprintf(stderr,
        "[PATCH_STREAM v1] event=init slots=%u colorBytes=%u extrasBytes=%u "
        "colorBuf=%u extrasBuf=%u trace=%d\n",
        kPatchStreamRingFrames,
        kPatchStreamColorBytesPerSlot,
        kPatchStreamExtrasBytesPerSlot,
        s_colorBuf, s_extrasBuf, (int)s_traceOn);
    fflush(stderr);
    return true;
}

void TerrainPatchStream::destroy()
{
    if (!s_initOk) return;
    fprintf(stderr, "[PATCH_STREAM v1] event=shutdown\n");
    fflush(stderr);

    for (uint32_t i = 0; i < kPatchStreamRingFrames; ++i) {
        if (s_fence[i]) { glDeleteSync(s_fence[i]); s_fence[i] = 0; }
    }
    if (s_colorBuf) {
        glBindBuffer(GL_ARRAY_BUFFER, s_colorBuf);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &s_colorBuf);
        s_colorBuf = 0; s_colorMap = nullptr;
    }
    if (s_extrasBuf) {
        glBindBuffer(GL_ARRAY_BUFFER, s_extrasBuf);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDeleteBuffers(1, &s_extrasBuf);
        s_extrasBuf = 0; s_extrasMap = nullptr;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_initOk = false;
}

bool TerrainPatchStream::isReady()       { return s_killswitch && s_initOk; }
bool TerrainPatchStream::isOverflowed()  { return false; }   // Task 3
void TerrainPatchStream::beginFrame()    { /* Task 4 */ }
void TerrainPatchStream::appendTriangle(DWORD, const gos_VERTEX*, const gos_TERRAIN_EXTRA*) { /* Task 3 */ }
bool TerrainPatchStream::flush()         { return false; /* Task 6 */ }
