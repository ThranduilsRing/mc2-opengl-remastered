// CPU-owned views published by the game and editor for native consumers.
#include "view_registry.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include <cstdio>

// Registry: fixed-size array of registered EngineViews, upserted by id.
// s_currentView mirrors the last setCurrentView call for backward-compat callers.
static constexpr uint32_t kMaxRegisteredViews = 8u;
static RenderCore::EngineView s_viewRegistry[kMaxRegisteredViews]{};
static uint32_t               s_viewCount = 0;
static RenderCore::EngineView s_currentView{};

void RenderCore::registerOrUpdateView(const RenderCore::EngineView& view) {
    for (uint32_t i = 0; i < s_viewCount; ++i) {
        if (s_viewRegistry[i].id == view.id) {
            s_viewRegistry[i] = view;
            return;
        }
    }
    if (s_viewCount < kMaxRegisteredViews) {
        MC2_DIAGFE(stderr, "[ENGINE_VIEW_REGISTRY v1] registered id=%u name=%s count=%u\n",
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
        MC2_DIAGFE(stderr, "[ENGINE_VIEW v1] frame=%d id=%u name=%s mode=%s\n",
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
