// GameOS/gameos/view_uniforms_gl.cpp
//
// GL implementation: init and upload of the ViewUniforms UBO.
// UBO binding: RenderCore::kViewUniformsBinding (3).
// Gate: MC2_VIEW_UNIFORMS env var (default OFF).
// F1-3A: upload only. No shader consumption yet (F1-3B).
//
// No explicit cleanup: GL context lifetime == process lifetime in this engine.
// Pattern mirrors gpu_cull_compute.cpp s_frustumUbo.

#include "view_uniforms_gl.h"

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>

#include "../../RenderCore/GpuBufferOwner.h"
#include "../../RenderCore/RenderResourceRegistry.h"
#include "utils/gpu_debug_labels.h"  // VULKAN-DEBUG-NAMES-1

// TERRAIN-VIEW-UBO-OWNER-1: the view-uniforms UBO is no longer a bare GLuint.
// It is now narrowed behind a GpuBufferOwner identity record (logical id +
// lifetime + debug name + the GLuint value). GL calls still happen at the same
// sites with the same args; the raw handle is reached only via owner.glName.
static RenderCore::GpuBufferOwner s_viewUniformsUbo{
    RenderCore::RenderResourceId::ViewUniformsUbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "ViewUniformsUbo",
    0u};
static int    s_vuFrame         = 0;

// Registry: fixed-size array of registered EngineViews, upserted by id.
// s_currentView mirrors the last setCurrentView call for backward-compat callers.
static constexpr uint32_t kMaxRegisteredViews = 8u;
static RenderCore::EngineView s_viewRegistry[kMaxRegisteredViews]{};
static uint32_t               s_viewCount = 0;
static RenderCore::EngineView s_currentView{};

void RenderCore::initViewUniformsUbo() {
    GLuint ubo = 0;
    glGenBuffers(1, &ubo);
    s_viewUniformsUbo.glName = static_cast<uint32_t>(ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferStorage(GL_UNIFORM_BUFFER, sizeof(RenderCore::ViewUniforms), nullptr,
                    GL_DYNAMIC_STORAGE_BIT);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, RenderCore::kViewUniformsBinding, ubo);

    // VULKAN-DEBUG-NAMES-1: gated (MC2_GPU_DEBUG_NAMES) glObjectLabel proof site.
    setGpuBufferDebugLabel(ubo, s_viewUniformsUbo.debugName);

    // TERRAIN-VIEW-UBO-OWNER-1: register the live view-uniforms UBO at creation
    // (observe-only metadata; never read by the draw path). Mirrors the
    // s_heightSsbo registration in gos_terrain_lod_chunk.cpp.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = s_viewUniformsUbo.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_viewUniformsUbo.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_viewUniformsUbo.debugName;
        d.glName    = s_viewUniformsUbo.glName;
        d.sizeBytes = static_cast<uint64_t>(sizeof(RenderCore::ViewUniforms));
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    fprintf(stderr, "[VIEW_UNIFORMS v1] event=init binding=%u size=%zu\n",
            RenderCore::kViewUniformsBinding, sizeof(RenderCore::ViewUniforms));
    fflush(stderr);
}

void RenderCore::uploadViewUniforms(const RenderCore::ViewUniforms& vu) {
    if (s_viewUniformsUbo.glName == 0)
        initViewUniformsUbo();
    const GLuint ubo = static_cast<GLuint>(s_viewUniformsUbo.glName);
    glBindBufferBase(GL_UNIFORM_BUFFER, RenderCore::kViewUniformsBinding, ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(RenderCore::ViewUniforms), &vu);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    ++s_vuFrame;
    if (s_vuFrame == 1 || s_vuFrame % 600 == 0) {
        fprintf(stderr, "[VIEW_UNIFORMS v1] frame=%d uploaded=1 binding=%u size=%zu\n",
                s_vuFrame, RenderCore::kViewUniformsBinding,
                sizeof(RenderCore::ViewUniforms));
        fflush(stderr);
    }
}

void RenderCore::registerOrUpdateView(const RenderCore::EngineView& view) {
    for (uint32_t i = 0; i < s_viewCount; ++i) {
        if (s_viewRegistry[i].id == view.id) {
            s_viewRegistry[i] = view;
            return;
        }
    }
    if (s_viewCount < kMaxRegisteredViews) {
        fprintf(stderr, "[ENGINE_VIEW_REGISTRY v1] registered id=%u name=%s count=%u\n",
                view.id, view.debugName ? view.debugName : "?", s_viewCount + 1u);
        fflush(stderr);
        s_viewRegistry[s_viewCount++] = view;
    }
}

void RenderCore::setCurrentView(const RenderCore::EngineView& view) {
    s_currentView = view;
    registerOrUpdateView(view);
    static int s_evFrame = 0;
    ++s_evFrame;
    if (s_evFrame == 1 || s_evFrame % 600 == 0) {
        fprintf(stderr, "[ENGINE_VIEW v1] frame=%d id=%u name=%s mode=%s\n",
                s_evFrame, view.id, view.debugName ? view.debugName : "?",
                RenderCore::toString(view.mode));
        fflush(stderr);
    }
}

const RenderCore::EngineView& RenderCore::getCurrentView() {
    return s_currentView;
}

const RenderCore::EngineView* RenderCore::resolveView(RenderCore::ViewId viewId) {
    for (uint32_t i = 0; i < s_viewCount; ++i) {
        if (s_viewRegistry[i].id == viewId)
            return &s_viewRegistry[i];
    }
    return nullptr;
}

uint32_t RenderCore::getViewCount() {
    return s_viewCount;
}

const RenderCore::EngineView* RenderCore::getViewByIndex(uint32_t index) {
    if (index >= s_viewCount) return nullptr;
    return &s_viewRegistry[index];
}
