#pragma once
// GPU-SYNC-CONTRACT v1 -- typed GPU producer/consumer memory-barrier helper.
//
// WHY THIS EXISTS: this engine has a GL render-state contract (render_contract.*,
// MC2_RENDER_CONTRACT_ASSERT) that validates depth/blend/FBO/MRT state -- but it
// asserts ZERO memory barriers, and there was no barrier helper at all. Every
// glMemoryBarrier was hand-placed and individually commented, so structurally
// identical compute->draw paths drifted: one author remembered the barrier, the
// next didn't. That ad-hoc gap shipped >=3 producer/consumer-ordering bugs
// (static-prop cull, terrain-indirect cmd clear, GPU mech instance/bone) that
// AMD's driver tolerated but NVIDIA's stricter ordering exposed as invisible or
// garbled geometry. Framed correctly this is an OpenGL sync-correctness class,
// not an NVIDIA quirk.
//
// This helper names the producer->consumer EDGE and maps it to the correct
// glMemoryBarrier bits via ONE audited table, so the "which bits?" decision is
// made once and can't drift. Call it AFTER the producer (write/clear/dispatch)
// and BEFORE the consumer (copy/draw/dispatch).
//
// v1 SCOPE: only the edges actually used are mapped -- no overgeneralization.
// A runtime producer/consumer assert tracker (no such gate) is a deliberate
// deferred v2. Optional once-per-tag debug log via MC2_GPU_SYNC_TRACE=1.

enum class GpuProducer {
    CpuCoherentWrite,  // CPU write through a persistent GL_MAP_COHERENT_BIT mapping
    ClearBuffer,       // glClear*BufferData / glClearNamedBufferSubData
    ComputeShader,     // a compute dispatch that wrote an SSBO / indirect-cmd buffer
    ComputeImageWrite, // a compute dispatch that wrote an image2D/image3D via imageStore
};

enum class GpuConsumer {
    BufferCopy,         // server-side glCopyBufferSubData reads the produced buffer
    ComputeShader,      // a later compute dispatch reads/atomics the buffer
    ShaderStorageRead,  // a graphics-stage shader reads it as SSBO
    MultiDrawIndirect,  // glMultiDraw*Indirect / glDraw*Indirect consumes it
    InstancedDraw,      // glDraw*Instanced* reads it (e.g. SSBO instance/bone data)
    CpuMappedRead,      // CPU reads through a persistent GL_MAP_PERSISTENT_BIT mapping
    BufferReadback,     // glGetBufferSubData / glGetNamedBufferSubData reads the buffer
    TextureReadback,    // glGetTexImage / glReadPixels reads an image written via imageStore
    TextureSample,      // a later shader stage samples the imageStore'd texture
};

// Issue the correct glMemoryBarrier for the producer->consumer edge. `tag` is a
// short static string used only by the MC2_GPU_SYNC_TRACE log (once per tag).
// An unmapped edge logs a loud warning and falls back to GL_ALL_BARRIER_BITS
// (fail-safe + visible, never silent).
void gpuSyncBarrier(GpuProducer producer, GpuConsumer consumer, const char* tag);

// SSBO-BIND-ALIGN: glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ..., offset, ...)
// REQUIRES offset % GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT == 0. NVIDIA enforces
// it (rejects a misaligned offset with GL_INVALID_VALUE -> the SSBO never binds ->
// the shader reads garbage -> invisible geometry); AMD silently tolerates it. Any
// ring-slot SSBO whose per-slot byte stride is not a multiple of this alignment
// produces a misaligned bind on slots 1+ on NVIDIA. Pad the per-slot stride with
// gpuAlignUp() so every slot offset is aligned. Returns the cached GL value (>=256
// fallback); valid only after a GL context exists.
int gpuSsboOffsetAlignment();

// Round `v` up to the next multiple of `a` (a must be a power of two; the GL
// alignment always is). size_t-based; safe for byte sizes and element counts.
inline unsigned long long gpuAlignUp(unsigned long long v, unsigned long long a) {
    return (a == 0ull) ? v : ((v + (a - 1ull)) & ~(a - 1ull));
}

// Wrapper for glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ...).
//
// Checks (always logged to stderr; abort() under MC2_RENDER_CONTRACT_ASSERT=1):
//   buffer != 0         — zero GL object means forgot to create/upload
//   size > 0            — zero-size bind is invalid
//   offset % alignment  — NVIDIA enforces; AMD silently tolerates
//
// Alignment violation also logs once per tag so a single NVIDIA run identifies
// every offending site. Plain int types so this header needs no GL include.
// `tag` must be a static string. Route ALL glBindBufferRange(SSBO,...) through
// this; forbid raw calls in new GPU-batcher code.
void gpuBindSsboRange(unsigned int index, unsigned int buffer,
                      long long offset, long long size, const char* tag);
