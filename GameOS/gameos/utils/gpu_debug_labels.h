#pragma once
// gpu_debug_labels.h — VULKAN-DEBUG-NAMES-1.
//
// Tiny gated helper to attach a KHR_debug object label to a GPU buffer so
// RenderDoc / RGP / Vulkan-prep captures show human-readable buffer names
// instead of anonymous handles. Mirrors the safety pattern of
// gpu_ring_buffer.h::labelBuffer (null-check glObjectLabel before calling).
//
// Gated behind env MC2_GPU_DEBUG_NAMES (presence-gated, default OFF). Read once
// into a static bool so the steady-state cost when the gate is OFF is a single
// predictable branch. glObjectLabel availability is NOT guaranteed (the KHR
// debug output path is only wired under MC2_GL_DEBUG), so we ALSO null-check the
// function pointer — a missing extension must never crash.

#include <cstdlib>

#include <GL/glew.h>

// Attach a debug label to a GL buffer object. No-op unless MC2_GPU_DEBUG_NAMES
// is set AND glObjectLabel is available. glBufferHandle must be a valid,
// already-created GL buffer name (label a buffer only after glGenBuffers +
// first bind/storage, per the GL spec object-existence rule).
inline void setGpuBufferDebugLabel(unsigned int glBufferHandle, const char* name) {
    static const bool s_enabled = (std::getenv("MC2_GPU_DEBUG_NAMES") != nullptr);
    if (!s_enabled) return;
    if (!glObjectLabel) return;
    glObjectLabel(GL_BUFFER, static_cast<GLuint>(glBufferHandle), -1,
                  name ? name : "?");
}
