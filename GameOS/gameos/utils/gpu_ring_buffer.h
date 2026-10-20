#pragma once
// GpuRingBuffer / GpuMeshRing — GPU-BUFFER-WRAPPER-TIER0-HUD-1.
//
// Pilot of the GpuRingBuffer<N> SHAPE (gpu-buffer-wrapper-design-1.md §2/§4 row B)
// proving it on the smallest, safest live buffer: the fixed-capacity gosMesh
// HUD/2D meshes (gameos_graphics.cpp). NOT a generalized buffer system.
//
// SCOPE/INVARIANTS (why this is the safe pilot — gpu-buffer-wrapper-tier0-recon-2.md):
//   * Fixed capacity. gosMesh allocates VBO/IBO once at makeMesh and rejects
//     overflow in addVertices/addIndices — there is NO grow path. So a ring
//     sized to that fixed capacity (xN slots) NEVER reallocs. ensureCapacity /
//     drain-remap (the hard part of the full design) is intentionally absent here.
//   * Self-contained orphan-then-draw. Each gosMesh::draw / drawIndexed writes
//     its prefix then draws immediately from the same call; no cross-pass reader.
//     A 3-frame ring's in-flight slots therefore only need the per-frame fence.
//
// FENCE DISCIPLINE (reuses gos_mech_batcher.cpp's proven pattern):
//   beginFrame(): advance slot, glClientWaitSync the fence from when this slot
//                 was last used (so we never write a slot the GPU may still read).
//   endFrame():   glFenceSync the slot AFTER the draw that consumed it.
//   Debug assert: exactly one endFrame() per beginFrame() per ring.
//
// This header is GL-call-bearing; include AFTER <GL/glew.h>. It deliberately does
// NOT include glew so the host TU controls include order (same convention as the
// mc2_hitch_trace.h / gl_utils.cpp comments).

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "mc2fmt.h"

namespace mc2gpu {

// 3-frame ring: matches MECH_RING_FRAMES — enough slots that the oldest fence is
// virtually always signaled by the time we wrap to it, so beginFrame's wait is
// non-blocking in steady state.
static const uint32_t kHudRingFrames = 3;

// ---------------------------------------------------------------------------
// GpuMeshRing — owns a VBO ring (always) and an optional IBO ring for one
// gosMesh, plus a single per-slot fence array shared by both (one draw consumes
// both buffers, so one fence per slot is correct and gives the clean
// one-endFrame-per-beginFrame contract).
//
// Buffers are allocated with glBufferStorage(PERSISTENT|COHERENT) at
// N * capacityBytes and mapped once. Per-frame writes are a memcpy into the
// current slot's base; draws use first/baseVertex/indexByteOffset to read from
// that slot. No glBufferData orphan, no per-frame map/unmap.
// ---------------------------------------------------------------------------
class GpuMeshRing {
public:
    GpuMeshRing() = default;
    ~GpuMeshRing() { destroy(); }

    GpuMeshRing(const GpuMeshRing&) = delete;
    GpuMeshRing& operator=(const GpuMeshRing&) = delete;

    bool valid() const { return vbBuf_ != 0 && vbMap_ != nullptr; }

    // Lazy one-time allocation. vertexStride/indexStride in bytes; *Capacity in
    // ELEMENTS (matches gosMesh capacity units). indexCapacity==0 => no IBO ring.
    // Returns false (and leaves valid()==false) on map failure — caller must
    // fall back to the legacy orphan path.
    bool create(const char* tag,
                uint32_t vertexStride, uint32_t vertexCapacity,
                uint32_t indexStride,  uint32_t indexCapacity) {
        if (valid()) return true;
        tag_           = tag;
        vertexStride_  = vertexStride;
        vertexCapacity_ = vertexCapacity;
        indexStride_   = indexStride;
        indexCapacity_ = indexCapacity;

        const GLbitfield flags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

        const GLsizeiptr vbBytes =
            (GLsizeiptr)kHudRingFrames * vertexCapacity_ * vertexStride_;

        glGenBuffers(1, &vbBuf_);
        glBindBuffer(GL_ARRAY_BUFFER, vbBuf_);
        glBufferStorage(GL_ARRAY_BUFFER, vbBytes, nullptr, flags);
        vbMap_ = static_cast<uint8_t*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, vbBytes, flags));
        labelBuffer(vbBuf_, tag_, "vb");

        if (indexCapacity_ > 0) {
            const GLsizeiptr ibBytes =
                (GLsizeiptr)kHudRingFrames * indexCapacity_ * indexStride_;
            glGenBuffers(1, &ibBuf_);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibBuf_);
            glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, ibBytes, nullptr, flags);
            ibMap_ = static_cast<uint8_t*>(glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, ibBytes, flags));
            labelBuffer(ibBuf_, tag_, "ib");
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        if (!vbMap_ || (indexCapacity_ > 0 && !ibMap_)) {
            std::fprintf(stderr, "[GPUBUF v1] event=ring_map_fail tag=%s\n", tag_);
            destroy();
            return false;
        }
        std::fprintf(stderr,
            "[GPUBUF v1] event=ring_alloc tag=%s frames=%u vcap=%u vstride=%u icap=%u istride=%u\n",
            tag_, kHudRingFrames, vertexCapacity_, vertexStride_, indexCapacity_, indexStride_);
        return true;
    }

    // Advance to the next slot and wait for that slot's last fence to signal
    // before any write into it. Marks the begin/end pairing for the debug assert.
    void beginFrame() {
        // WATCHPOINT 2: one endFrame() per beginFrame(). pending_ must be false
        // here (the previous frame's endFrame cleared it). If it is true a draw
        // path advanced the ring twice without fencing — that is exactly the
        // unsafe reuse the negative test (MC2_GPUBUF_RING_FORCE_WRAP) forces.
        //
        // The detection LOG fires unconditionally (build is RelWithDebInfo =>
        // NDEBUG, so assert() compiles out — the log is the observable proof the
        // guard is live). The hard assert additionally aborts in debug builds.
        if (pending_) {
            std::fprintf(stderr,
                "[GPUBUF v1] event=begin_without_end tag=%s slot=%u — fence-per-frame invariant violated (unsafe slot reuse, prior endFrame() missing)\n",
                tag_, slot_);
            std::fflush(stderr);
            assert(!pending_ && "GpuMeshRing: beginFrame() without matching endFrame()");
        }
        slot_ = (slot_ + 1) % kHudRingFrames;
        if (fence_[slot_]) {
            glClientWaitSync(fence_[slot_], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(fence_[slot_]);
            fence_[slot_] = 0;
        }
        pending_ = true;
    }

    // Fence the current slot AFTER the draw that consumed it.
    void endFrame() {
#ifndef NDEBUG
        assert(pending_ && "GpuMeshRing: endFrame() without matching beginFrame()");
#endif
        if (fence_[slot_]) {
            // Defensive: should never happen (beginFrame deletes it), but never leak.
            glDeleteSync(fence_[slot_]);
        }
        fence_[slot_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        pending_ = false;
    }

    // Copy `count` vertices into the current slot; returns the baseVertex
    // (element offset) the draw must use.
    GLint writeVertices(const void* src, uint32_t count) {
        const uint32_t base = slot_ * vertexCapacity_;
        std::memcpy(vbMap_ + static_cast<size_t>(base) * vertexStride_, src,
                    static_cast<size_t>(count) * vertexStride_);
        return (GLint)base;
    }

    // Copy `count` indices into the current slot; returns the byte offset the
    // draw must pass to glDrawElements*BaseVertex.
    GLsizeiptr writeIndices(const void* src, uint32_t count) {
        const uint32_t base = slot_ * indexCapacity_;
        const size_t byteOff = static_cast<size_t>(base) * indexStride_;
        std::memcpy(ibMap_ + byteOff, src, static_cast<size_t>(count) * indexStride_);
        return (GLsizeiptr)byteOff;
    }

    GLuint vb() const { return vbBuf_; }
    GLuint ib() const { return ibBuf_; }

    void destroy() {
        // Drain all fences before deleting the mapped storage (WATCHPOINT 4).
        for (uint32_t i = 0; i < kHudRingFrames; ++i) {
            if (fence_[i]) {
                glClientWaitSync(fence_[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
                glDeleteSync(fence_[i]);
                fence_[i] = 0;
            }
        }
        if (vbBuf_) {
            glBindBuffer(GL_ARRAY_BUFFER, vbBuf_);
            glUnmapBuffer(GL_ARRAY_BUFFER);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &vbBuf_);
            vbBuf_ = 0; vbMap_ = nullptr;
        }
        if (ibBuf_) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibBuf_);
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            glDeleteBuffers(1, &ibBuf_);
            ibBuf_ = 0; ibMap_ = nullptr;
        }
        pending_ = false;
    }

    // Residency report (MC2_GPUBUF_RESIDENCY).
    void reportResidency() const {
        if (!valid()) return;
        uint32_t liveFences = 0;
        for (uint32_t i = 0; i < kHudRingFrames; ++i) if (fence_[i]) ++liveFences;
        const unsigned long long vbBytes =
            static_cast<unsigned long long>(kHudRingFrames) * vertexCapacity_ * vertexStride_;
        const unsigned long long ibBytes =
            static_cast<unsigned long long>(kHudRingFrames) * indexCapacity_ * indexStride_;
        std::fprintf(stderr,
            "[GPUBUF v1] residency tag=%s kind=meshring frames=%u vbBytes=%llu ibBytes=%llu liveFences=%u\n",
            tag_, kHudRingFrames, vbBytes, ibBytes, liveFences);
    }

private:
    static void labelBuffer(GLuint buf, const char* tag, const char* which) {
        // KHR_debug label (design §5). GLEW exposes glObjectLabel on 4.3+;
        // guard on availability so a missing extension never crashes.
        if (glObjectLabel) {
            char name[96];
            (*std::format_to_n(name, (sizeof(name)) - 1, "GpuMeshRing.{}.{}", mc2::fmt::safe(tag ? tag : "?"), mc2::fmt::safe(which)).out = '\0');
            glObjectLabel(GL_BUFFER, buf, -1, name);
        }
    }

    const char* tag_       = "?";
    GLuint   vbBuf_        = 0;
    GLuint   ibBuf_        = 0;
    uint8_t* vbMap_        = nullptr;
    uint8_t* ibMap_        = nullptr;
    uint32_t vertexStride_ = 0;
    uint32_t vertexCapacity_ = 0;
    uint32_t indexStride_  = 0;
    uint32_t indexCapacity_ = 0;
    uint32_t slot_         = 0;
    GLsync   fence_[kHudRingFrames] = {};
    bool     pending_      = false;  // true between beginFrame() and endFrame()
};

} // namespace mc2gpu
