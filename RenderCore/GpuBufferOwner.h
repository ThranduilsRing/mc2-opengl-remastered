#pragma once
// RenderCore/GpuBufferOwner.h
// GPU-BUFFER-OWNER-SKELETON-1: first slice of the GPU resource-ownership arc.
//
// The RenderResourceRegistry is DESCRIPTIVE ONLY -- a registered RenderResourceId
// is a name tag, not an owner; the raw GLuint handle is still created/destroyed
// by file-static code. This header adds the OWNER record type: a plain value that
// pairs one GL buffer handle's IDENTITY with its logical id + lifetime + debug
// name. It is the foundation that later slices will use to encapsulate one raw
// GLuint global at a time.
//
// GL-FREE BY CONSTRUCTION. This header includes no GL and calls no GL. It does
// NOT glGenBuffers / glDeleteBuffers / bind -- it only records ownership identity.
// The raw backend handle is stored as a plain uint64_t, never touched: wide
// enough for a GL name (GLuint zero-extends) AND a Vulkan VkBuffer (a 64-bit
// dispatchable-object handle) -- VULKAN-BACKEND-L6-LADDER-1 L6.1 (backendHandle
// rename, boundary-recon recommendation). A thin .cpp doing the actual
// create/destroy/bind is a LATER slice.
//
// Boundaries (matches gpu-buffer-wrapper-design-1.md non-goals, scoped down):
//   - NOT an allocator / sub-allocator. One owner == one GL buffer object.
//   - NO aliasing, NO ring/fence logic, NO Vulkan abstraction.
//   - Plain trivially-copyable record. No RAII, no GL lifetime here.
#include <cstdint>

#include "RenderResourceRegistry.h"  // RenderResourceId, RenderResourceLifetime

namespace RenderCore {

// Ownership identity record for ONE GPU buffer object. Plain value type.
//   id            -- the logical RenderResourceId this buffer is registered under.
//   lifetime      -- how long the buffer stays valid (Persistent / Mission / ...).
//   debugName     -- string-literal label (KHR_debug / residency key); never heap.
//   backendHandle -- the raw backend handle value (0 == unallocated). Stored,
//                    not used. On the GL backend this is the GLuint name
//                    (zero-extended); on Vulkan it will hold the VkBuffer bits.
//                    GL call sites narrow with static_cast<GLuint>(...) at the
//                    point of use -- the record itself stays backend-neutral.
struct GpuBufferOwner {
    RenderResourceId       id            = RenderResourceId::Unknown;
    RenderResourceLifetime lifetime      = RenderResourceLifetime::Unset;
    const char*            debugName     = nullptr;
    uint64_t               backendHandle = 0;  // 0 = unallocated. Identity only.

    // A fully-formed owner: a real backend handle and a non-null debug label.
    // (Identity invariant only -- says nothing about backend-object validity,
    // which this backend-free record cannot and must not check.)
    bool valid() const {
        return backendHandle != 0 && debugName != nullptr;
    }
};

} // namespace RenderCore
