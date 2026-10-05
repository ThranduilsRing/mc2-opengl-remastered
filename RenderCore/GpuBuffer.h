#pragma once
// RenderCore/GpuBuffer.h
// GPU-BUFFER-TIER0-LODCHUNK-RAII-1: the thin OWNING GpuBuffer over one GL
// buffer object -- the "LATER slice" flagged in GpuBufferOwner.h:15 and the
// TIER0-LODCHUNK adoption row of gpu-buffer-wrapper-design-1.md Sec 4.
//
// GpuBufferOwner (the identity record) stays untouched: this class HAS-A
// GpuBufferOwner and adds the actual create/destroy/bind/upload lifecycle in
// a GL-side .cpp (GameOS/gameos/gpu_buffer_gl.cpp), mirroring the
// ViewUniforms.h / view_uniforms_gl.cpp split. This header remains GL-free:
// no GL types, no GL includes -- kinds/usages are engine enums translated to
// GL enums inside the .cpp.
//
// DELIBERATELY THIN (byte-identical adoption): bind()/bufferData()/
// bufferSubData()/bindBase()/unbind() mirror the raw GL calls 1:1 so a
// converted call site issues the SAME GL commands in the SAME order with the
// SAME args as the raw-GLuint code it replaces. What the class centralizes:
//   - create/destroy pairing (glGenBuffers / glDeleteBuffers),
//   - RenderResourceRegistry upsert on upload + invalidate on destroy
//     (previously 5 hand-copied desc blocks in gos_terrain_lod_chunk.cpp),
//   - the gated KHR_debug label (VULKAN-DEBUG-NAMES-1, MC2_GPU_DEBUG_NAMES),
//   - move-only RAII (dtor destroys a still-live buffer).
//
// Boundaries (same as GpuBufferOwner.h, plus):
//   - NOT an allocator. One GpuBuffer == one GL buffer object.
//   - NO ring/fence logic (GpuRingBuffer<N> stays a separate future type;
//     Tier-2 rings are explicitly out of scope per design Sec 4 row E).
//   - NO DSA adoption (OD-3 open): keeps glGenBuffers + bind-to-edit so the
//     GL call stream is bit-identical to the pre-RAII code.
//   - NO hitch-accounting change: the lod-chunk sites were Tier-0 (raw, not
//     MC2_GL_BufferData-accounted) before and stay unaccounted (OD-4 open).
//
// Static/exit-time note: the engine tears these down explicitly
// (gos_TerrainLodChunk_Destroy at renderer destroy) BEFORE GL context
// teardown, so for static-duration instances the dtor is a glName==0 no-op
// at process exit on every clean path. (On paths that never tore the
// renderer down, the context is still live at static-destruction time, so a
// dtor delete remains safe there too.)
//
// CI: registry-tracked construction sites `RenderCore::GpuBuffer s_x{...}`
// carry the same (id, lifetime, debugName) positional identity args as
// GpuBufferOwner sites and are checked by scripts/check-gpu-buffer-owners.py.

#include <cstdint>

#include "GpuBufferOwner.h"          // GpuBufferOwner identity record
#include "RenderResourceRegistry.h"  // RenderResourceId, RenderResourceLifetime

namespace RenderCore {

// Which GL bind target this buffer edits/binds through (translated to a GL
// enum in gpu_buffer_gl.cpp; this header stays GL-free).
enum class GpuBufferKind : uint8_t {
    Vertex,   // GL_ARRAY_BUFFER
    Index,    // GL_ELEMENT_ARRAY_BUFFER
    Storage,  // GL_SHADER_STORAGE_BUFFER
    Uniform,  // GL_UNIFORM_BUFFER
};

// glBufferData usage hint (subset actually used by the adopted sites).
enum class GpuBufferUsage : uint8_t {
    StaticDraw,   // GL_STATIC_DRAW
    DynamicDraw,  // GL_DYNAMIC_DRAW
};

class GpuBuffer {
public:
    // Untracked buffer (no RenderResourceId): vertex/index data. VBO/IBO are
    // out of registry scope by design (check-gpu-buffer-owners.py partition
    // comment) -- they still get RAII + optional debug label.
    GpuBuffer() = default;
    explicit GpuBuffer(GpuBufferKind kind, const char* debugTag = nullptr)
        : kind_(kind) {
        rec_.debugName = debugTag;
    }

    // Registry-tracked buffer. Positional identity args deliberately mirror
    // the GpuBufferOwner initializer order (id, lifetime, debugName) so
    // scripts/check-gpu-buffer-owners.py parses both site families the same
    // way (arg3 = kind instead of the record's glName).
    GpuBuffer(RenderResourceId id, RenderResourceLifetime lifetime,
              const char* debugName, GpuBufferKind kind)
        : kind_(kind) {
        rec_.id        = id;
        rec_.lifetime  = lifetime;
        rec_.debugName = debugName;
    }

    ~GpuBuffer() { destroy(); }

    GpuBuffer(const GpuBuffer&)            = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;

    GpuBuffer(GpuBuffer&& o) noexcept
        : kind_(o.kind_), rec_(o.rec_), labeled_(o.labeled_) {
        o.rec_.backendHandle = 0;
        o.labeled_    = false;
    }
    GpuBuffer& operator=(GpuBuffer&& o) noexcept {
        if (this != &o) {
            destroy();
            kind_        = o.kind_;
            rec_         = o.rec_;
            labeled_     = o.labeled_;
            o.rec_.backendHandle = 0;
            o.labeled_    = false;
        }
        return *this;
    }

    // --- lifecycle + GL mirror ops (GameOS/gameos/gpu_buffer_gl.cpp) -------
    bool create();    // glGenBuffers (name only, no storage). Idempotent.
    void destroy();   // glDeleteBuffers + registry invalidate. Safe when 0.
    void bind() const;    // glBindBuffer(target(kind_), glName)
    void unbind() const;  // glBindBuffer(target(kind_), 0)
    // glBufferData on the bound target. Call between bind()/unbind().
    // Applies the gated KHR_debug label on first definition.
    void bufferData(intptr_t bytes, const void* data, GpuBufferUsage usage);
    // glBufferSubData on the bound target. Call between bind()/unbind().
    void bufferSubData(intptr_t offset, intptr_t bytes, const void* data);
    // glBindBufferBase(target(kind_), slot, glName). Storage/Uniform only.
    void bindBase(uint32_t slot) const;
    // RenderResourceRegistry upsert (kind=Buffer, format=BufferRaw, valid).
    // No-op for untracked (id == Unknown) buffers.
    void registerNow(uint64_t sizeBytes) const;

    // --- identity / state (GL-free, inline) --------------------------------
    uint32_t    name() const { return static_cast<uint32_t>(rec_.backendHandle); }
    bool        created() const { return rec_.backendHandle != 0; }
    const char* debugName() const { return rec_.debugName; }
    const GpuBufferOwner& owner() const { return rec_; }

private:
    GpuBufferKind  kind_ = GpuBufferKind::Storage;
    GpuBufferOwner rec_{};
    bool           labeled_ = false;
};

}  // namespace RenderCore
