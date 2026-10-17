// tools/asset_viewer/ModelPreviewEngineShader.cpp
// Backend-A is not ported to Vulkan (see the header). The surface reports that and fails open to Backend-B.
#include "ModelPreviewEngineShader.h"
#include "imgui.h"

void ModelPreviewEngineShader::draw(const ImVec2&) {
    if (!reported_) {
        reported_ = true;
        report_.shaderRoot = shaderRoot_;
        report_.vertPath = shaderRoot_ + "/static_prop.vert";
        report_.fragPath = shaderRoot_ + "/static_prop.frag";
        report_.activeDefines = "(none: not built)";
        report_.lastError = "Backend-A (engine static_prop GLSL) is not ported to Vulkan: the engine Vulkan static-prop shaders need "
                            "the color_prop island bindless/SSBO set, which has no standalone entry point.";
    }
}
