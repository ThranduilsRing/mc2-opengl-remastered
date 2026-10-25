#pragma once
#include <string>
#include "imgui.h"   // ImVec2
// The render-backend-agnostic preview seam. Stage 1 has one impl (TexturePreview2D).
// MaterialPreviewPBR provides the material sphere preview.
// (shader + pipeline-state faithful — real static_prop shaders, matching
// depth/blend/cull — NOT full engine scene render: no batcher, no mission lights,
// no shadow cascade). The app and inspector panel depend ONLY on this interface.
class PreviewSurface {
public:
    virtual ~PreviewSurface() = default;
    virtual void setSource(const std::string& path) = 0;
    virtual void draw(const ImVec2& availableSize) = 0;
    virtual const char* label() const = 0;
};
