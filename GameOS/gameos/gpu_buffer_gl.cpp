// GameOS/gameos/gpu_buffer_gl.cpp
//
// GPU-BUFFER-TIER0-LODCHUNK-RAII-1: GL implementation of RenderCore::GpuBuffer
// (the thin owning wrapper -- the "LATER slice" flagged in GpuBufferOwner.h:15).
// Split mirrors ViewUniforms.h / view_uniforms_gl.cpp: the RenderCore header is
// GL-free, this TU owns the GL translation.
//
// BYTE-IDENTICAL DISCIPLINE: every method maps 1:1 onto exactly the GL call the
// pre-RAII raw-GLuint sites issued (glGenBuffers / glBindBuffer / glBufferData /
// glBufferSubData / glBindBufferBase / glDeleteBuffers), same args, no extra
// binds, no DSA (OD-3 open), no hitch-accounting change (OD-4 open). The only
// additive behavior is the gated KHR_debug label (MC2_GPU_DEBUG_NAMES,
// default-OFF) and the RenderResourceRegistry upsert/invalidate that the
// converted call sites previously hand-copied.

#include "../../RenderCore/GpuBuffer.h"

#include <GL/glew.h>
#include <cstdio>

#include "../../RenderCore/RenderResourceRegistry.h"
#include "utils/gpu_debug_labels.h"  // VULKAN-DEBUG-NAMES-1 (gated, default-OFF)

namespace {

GLenum toGlTarget(RenderCore::GpuBufferKind kind) {
    switch (kind) {
        case RenderCore::GpuBufferKind::Vertex:  return GL_ARRAY_BUFFER;
        case RenderCore::GpuBufferKind::Index:   return GL_ELEMENT_ARRAY_BUFFER;
        case RenderCore::GpuBufferKind::Storage: return GL_SHADER_STORAGE_BUFFER;
        case RenderCore::GpuBufferKind::Uniform: return GL_UNIFORM_BUFFER;
    }
    return GL_SHADER_STORAGE_BUFFER;
}

GLenum toGlUsage(RenderCore::GpuBufferUsage usage) {
    switch (usage) {
        case RenderCore::GpuBufferUsage::StaticDraw:  return GL_STATIC_DRAW;
        case RenderCore::GpuBufferUsage::DynamicDraw: return GL_DYNAMIC_DRAW;
    }
    return GL_STATIC_DRAW;
}

}  // namespace

namespace RenderCore {

bool GpuBuffer::create() {
    if (rec_.glName != 0)
        return true;  // idempotent
    GLuint name = 0;
    glGenBuffers(1, &name);
    rec_.glName = static_cast<uint32_t>(name);
    if (rec_.glName == 0) {
        fprintf(stderr, "[GpuBuffer] glGenBuffers failed (%s)\n",
                rec_.debugName ? rec_.debugName : "?");
        fflush(stderr);
        return false;
    }
    return true;
}

void GpuBuffer::destroy() {
    if (rec_.glName == 0)
        return;
    GLuint name = static_cast<GLuint>(rec_.glName);
    glDeleteBuffers(1, &name);
    rec_.glName = 0;
    labeled_    = false;
    // Mark the logical slot unavailable on teardown -- same invalidate the
    // pre-RAII sites issued by hand (e.g. gos_TerrainLodChunk_Destroy).
    if (rec_.id != RenderResourceId::Unknown) {
        RenderResourceDesc invalid;
        invalid.id = rec_.id;
        registerOrUpdateRenderResource(invalid);
    }
}

void GpuBuffer::bind() const {
    glBindBuffer(toGlTarget(kind_), static_cast<GLuint>(rec_.glName));
}

void GpuBuffer::unbind() const {
    glBindBuffer(toGlTarget(kind_), 0);
}

void GpuBuffer::bufferData(intptr_t bytes, const void* data,
                           GpuBufferUsage usage) {
    glBufferData(toGlTarget(kind_), static_cast<GLsizeiptr>(bytes), data,
                 toGlUsage(usage));
    // Label after first definition (GL object-existence rule: label only a
    // created object). Gated MC2_GPU_DEBUG_NAMES inside the helper.
    if (!labeled_ && rec_.debugName != nullptr) {
        setGpuBufferDebugLabel(rec_.glName, rec_.debugName);
        labeled_ = true;
    }
}

void GpuBuffer::bufferSubData(intptr_t offset, intptr_t bytes,
                              const void* data) {
    glBufferSubData(toGlTarget(kind_), static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(bytes), data);
}

void GpuBuffer::bindBase(uint32_t slot) const {
    glBindBufferBase(toGlTarget(kind_), static_cast<GLuint>(slot),
                     static_cast<GLuint>(rec_.glName));
}

void GpuBuffer::registerNow(uint64_t sizeBytes) const {
    if (rec_.id == RenderResourceId::Unknown)
        return;  // untracked (VBO/IBO) -- out of registry scope by design
    RenderResourceDesc d;
    d.id        = rec_.id;
    d.kind      = RenderResourceKind::Buffer;
    d.lifetime  = rec_.lifetime;
    d.format    = RenderResourceFormat::BufferRaw;
    d.debugName = rec_.debugName;
    d.glName    = rec_.glName;
    d.sizeBytes = sizeBytes;
    d.valid     = true;
    registerOrUpdateRenderResource(d);
}

}  // namespace RenderCore
