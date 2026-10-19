#include "gos_gpu_sync.h"

#include <GL/glew.h>
#include <GL/gl.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace {

bool contractAssertEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_RENDER_CONTRACT_ASSERT">();
    }
    return cached == 1;
}

// The audited edge -> barrier-bits table. ONLY edges used in this slice are
// mapped (v1, no overgeneralization). Add a new edge here -- in one place --
// when a new compute->draw / clear->shader / coherent-write->read path appears.
GLbitfield barrierBitsFor(GpuProducer p, GpuConsumer c) {
    // A CPU write through a persistent GL_MAP_COHERENT_BIT mapping is made
    // visible without an explicit flush, but ordering it BEFORE a subsequent
    // server-side read still requires GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT. Add
    // the SSBO bit when the consumer reads the buffer as shader storage.
    if (p == GpuProducer::CpuCoherentWrite) {
        if (c == GpuConsumer::BufferCopy)
            return GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT;
        if (c == GpuConsumer::InstancedDraw ||
            c == GpuConsumer::MultiDrawIndirect ||
            c == GpuConsumer::ShaderStorageRead)
            return GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT;
    }
    // A buffer cleared via glClear* then read/atomic'd by a compute shader: the
    // clear is a buffer-update; the compute access is shader-storage.
    if (p == GpuProducer::ClearBuffer && c == GpuConsumer::ComputeShader)
        return GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT;
    // A compute dispatch that wrote an indirect command buffer, consumed by an
    // indirect draw: command-barrier orders the args; SSBO bit covers same-buffer
    // shader reads.
    if (p == GpuProducer::ComputeShader && c == GpuConsumer::MultiDrawIndirect)
        return GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT;
    // A compute dispatch that wrote an SSBO, consumed by a later shader stage.
    if (p == GpuProducer::ComputeShader &&
        (c == GpuConsumer::ShaderStorageRead || c == GpuConsumer::ComputeShader))
        return GL_SHADER_STORAGE_BARRIER_BIT;
    // Compute dispatch wrote an SSBO; a server-side glCopyBufferSubData reads it.
    if (p == GpuProducer::ComputeShader && c == GpuConsumer::BufferCopy)
        return GL_SHADER_STORAGE_BARRIER_BIT;
    // CPU reads the result through a persistent GL_MAP_PERSISTENT_BIT mapping.
    // GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT is required regardless of which GPU
    // stage wrote the buffer -- it orders GPU writes before the mapped pointer
    // is read by the CPU.
    if (c == GpuConsumer::CpuMappedRead)
        return GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT;
    // A compute dispatch wrote an SSBO that is then read back to CPU via
    // glGetBufferSubData (MC2-LIGHTGRID-BUILD-NATIVE-1 parity readback of the
    // compact light-index pool). The precise bit for API-level buffer reads is
    // GL_BUFFER_UPDATE_BARRIER_BIT (spec §7.12.1) — it subsumes SSBO write
    // visibility for API reads, so SHADER_STORAGE_BARRIER_BIT is not additionally
    // required. Stated explicitly so this typed edge is self-documenting.
    if (p == GpuProducer::ComputeShader && c == GpuConsumer::BufferReadback)
        return GL_BUFFER_UPDATE_BARRIER_BIT;
    // glGetBufferSubData / glGetNamedBufferSubData for any other producer:
    // GL_BUFFER_UPDATE_BARRIER_BIT is the precise bit (spec §7.12.1).
    if (c == GpuConsumer::BufferReadback)
        return GL_BUFFER_UPDATE_BARRIER_BIT;
    // A compute dispatch wrote an image2D/3D via imageStore (CLUSTER-DEPTH-
    // PYRAMID-NATIVE-1). Two consumers:
    //   - TextureReadback (glGetTexImage / glReadPixels API read): the precise
    //     bit is GL_TEXTURE_UPDATE_BARRIER_BIT (spec §7.12.1 — orders imageStore
    //     writes before API texture reads). GL_FRAMEBUFFER_BARRIER_BIT is not
    //     needed (no FBO read of the image here).
    //   - TextureSample (a later shader stage samples the image as a texture):
    //     GL_TEXTURE_FETCH_BARRIER_BIT orders the imageStore before texel fetch.
    if (p == GpuProducer::ComputeImageWrite) {
        if (c == GpuConsumer::TextureReadback)
            return GL_TEXTURE_UPDATE_BARRIER_BIT;
        if (c == GpuConsumer::TextureSample)
            return GL_TEXTURE_FETCH_BARRIER_BIT;
    }
    return 0;  // unmapped edge
}

bool traceEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_GPU_SYNC_TRACE">();
    }
    return cached == 1;
}

}  // namespace

int gpuSsboOffsetAlignment() {
    static GLint s_align = 0;
    if (s_align == 0) {
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &s_align);
        if (s_align < 16) s_align = 256;  // sane fallback (NVIDIA = 256, AMD ~32)
        mc2::fmt::print_err("[GPU_ALIGN] SSBO_OFFSET_ALIGNMENT={:d}\n", mc2::fmt::as_i32(static_cast<int>(s_align)));
    }
    return static_cast<int>(s_align);
}

void gpuBindSsboRange(unsigned int index, unsigned int buffer,
                      long long offset, long long size, const char* tag) {
    const char* t = tag ? tag : "(none)";
    if (buffer == 0) {
        mc2::fmt::print_err("[GPU_ALIGN] FATAL: gpuBindSsboRange tag={} buffer=0 (zero object -- forgot to create/upload?)\n", mc2::fmt::safe(t));
        std::fflush(stderr);
        if (contractAssertEnabled()) std::abort();
    }
    if (size <= 0) {
        mc2::fmt::print_err("[GPU_ALIGN] FATAL: gpuBindSsboRange tag={} size={:d} (zero or negative size -- nothing to bind)\n", mc2::fmt::safe(t), mc2::fmt::as_i64(size));
        std::fflush(stderr);
        if (contractAssertEnabled()) std::abort();
    }
    const long long a = static_cast<long long>(gpuSsboOffsetAlignment());
    if (a > 0 && (offset % a) != 0) {
        static std::set<std::string> s_seen;
        const std::string key = t;
        if (s_seen.insert(key).second) {
            mc2::fmt::print_err("[GPU_ALIGN] MISALIGNED tag={} offset={:d} align={:d} (remainder={:d}) -- glBindBufferRange will fail on NVIDIA\n", mc2::fmt::safe(t), mc2::fmt::as_i64(offset), mc2::fmt::as_i64(a), mc2::fmt::as_i64(offset % a));
            std::fflush(stderr);
        }
        if (contractAssertEnabled()) std::abort();
    }
    glBindBufferRange(GL_SHADER_STORAGE_BUFFER, (GLuint)index, (GLuint)buffer,
                      (GLintptr)offset, (GLsizeiptr)size);
}

void gpuSyncBarrier(GpuProducer producer, GpuConsumer consumer, const char* tag) {
    GLbitfield bits = barrierBitsFor(producer, consumer);
    if (bits == 0) {
        // Unmapped edge == a contract miswire. Be loud and fail safe (never
        // silently skip a barrier -- that is exactly the bug class this helper
        // exists to kill).
        mc2::fmt::print_err("[GPU_SYNC] WARNING unmapped edge tag={} producer={:d} consumer={:d} -> GL_ALL_BARRIER_BITS (add the edge to gos_gpu_sync.cpp)\n",
            mc2::fmt::safe(tag ? tag : "(none)"), mc2::fmt::as_i32(static_cast<int>(producer)), mc2::fmt::as_i32(static_cast<int>(consumer)));
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        return;
    }
    glMemoryBarrier(bits);
    if (traceEnabled()) {
        static std::set<std::string> s_seen;  // once-per-tag, no per-frame spam
        const std::string key = tag ? tag : "(none)";
        if (s_seen.insert(key).second) {
            mc2::fmt::print_err("[GPU_SYNC] tag={} producer={:d} consumer={:d} bits=0x{:x}\n", key.c_str(), mc2::fmt::as_i32(static_cast<int>(producer)), mc2::fmt::as_i32(static_cast<int>(consumer)), mc2::fmt::as_u32(static_cast<unsigned>(bits)));
        }
    }
}
