// RenderCore/IRenderBackend.h
//
// Minimal, GL-FREE render-backend interface. First boundary slice
// (RENDER-BACKEND-IFACE-POSTPROCESS-1): only the ops the PostProcess composite
// output edge actually needs today. The interface GROWS one edge at a time as
// more render edges route through it — do NOT front-load the full op list here.
//
// HARD RULE: no GL includes, no GL types, no pass logic in this file. It is a
// pure abstract description of backend ops. The concrete GL implementation lives
// in GameOS/gameos/GLBackend.cpp (RenderCore stays GL-free by construction, same
// split as PipelineDesc -> pipeline_binder). A future VulkanBackend implements
// the same interface without any change here.
//
// Gate: MC2_RENDER_BACKEND_IFACE (default-OFF). OFF = the call site issues the
// direct GL it always has. ON = the call site routes the SAME work through this
// interface (GL-backed). Both are GL -> byte-identical output; the gate proves
// the routing seam works before any non-GL backend exists.

#pragma once

#include "RenderRegionId.h"   // RENDER-BACKEND-REGION-IFACE-1 (GL-free contract)

namespace RenderCore {

// Abstract render backend. Pure virtual; no state, no GL.
struct IRenderBackend {
    virtual ~IRenderBackend() = default;

    // RENDER-BACKEND-REGION-IFACE-1 (Layer-6 ENTRY): run a SELECTABLE render
    // region. The context is BACKEND-NEUTRAL (RenderResourceId + params + dims,
    // no raw handles); the impl resolves ids -> handles via ctx.resolver. Writes
    // which impl ran / fallback reason / equivalence counters into *out.
    //
    // Returns false if this backend is unavailable/failed for the region -> the
    // CALLER falls back to GL. A GL backend that succeeds returns true with
    // out->impl = GLInline. A Vulkan backend that can't run returns false (out
    // carries the fallback reason) so the caller runs GL. Default impl declines.
    virtual bool runRegion(RenderRegionId /*id*/,
                           const PostprocessFogRegionContext& /*ctx*/,
                           RegionOutput* /*out*/) {
        return false;
    }

    // Bind the window's default framebuffer (the backbuffer) as the current
    // render target for the composite output edge. GL impl == glBindFramebuffer
    // (GL_FRAMEBUFFER, 0). This is the single op the PostProcess composite edge
    // needs in this slice.
    virtual void bindBackbuffer() = 0;

    // Bind an arbitrary framebuffer object as the current render target. GL impl
    // == glBindFramebuffer(GL_FRAMEBUFFER, fbo). `fbo` is a plain uint handle so
    // this file stays GL-free (no GLuint / GL headers). Added by
    // RENDER-BACKEND-IFACE-FBO-1 to route one more PostProcess FBO-bind edge; the
    // interface grows one edge at a time.
    virtual void bindFramebuffer(unsigned int fbo) = 0;

    // Set the current viewport rectangle. GL impl == glViewport(x, y, w, h).
    // Plain ints keep this file GL-free. Added by
    // RENDER-BACKEND-IFACE-VIEWPORT-1 to route the composite-edge viewport ops.
    virtual void setViewport(int x, int y, int w, int h) = 0;   // RENDER-BACKEND-IFACE-VIEWPORT-1

    // Clear the current render target. `mask` is a plain uint (the GL buffer-bit
    // mask) so this file stays GL-free; GL impl == glClearColor(r,g,b,a);
    // glClear(mask). Added by RENDER-BACKEND-IFACE-CLEAR-1 to route the
    // letterbox clear on the composite edge.
    virtual void clear(unsigned int mask, float r, float g, float b, float a) = 0;  // RENDER-BACKEND-IFACE-CLEAR-1
};

}  // namespace RenderCore
