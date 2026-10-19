#pragma once
// Render-thread CPU view registry shared by game, editor and native consumers.

#include "../../RenderCore/ViewUniforms.h"
#include "../../RenderCore/EngineView.h"

namespace RenderCore {

    void setCurrentView(const EngineView& view);
    const EngineView& getCurrentView();

    // Returns pointer to registered EngineView for the given id, or nullptr if unknown.
    // kMainSceneViewId always resolves if setCurrentView has been called this frame.
    const EngineView* resolveView(ViewId viewId);

    // View registry: upsert by id, read back by index.
    // Thread-unsafe — call only from the render thread.
    void registerOrUpdateView(const EngineView& view);
    uint32_t getViewCount();
    const EngineView* getViewByIndex(uint32_t index);
}
