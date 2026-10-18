// tools/asset_viewer/ModelPreviewEngineShader.h
// Backend-A: the preview that rendered one static prop with the REAL engine static_prop.{vert,frag} GLSL through a GL context
// (+ StandaloneSceneStubs). NOT PORTED TO VULKAN: the engine's Vulkan static-prop path is the color_prop island (Slang SPIR-V behind a
// bindless texture array and the scene SSBO set), which has no standalone entry point yet. The surface keeps its API so the app still
// fails open: ok() is false, the report says why, and the caller draws Backend-B (MeshPreview3D).
#pragma once
#include "PreviewSurface.h"
#include "ShaderContractReport.h"
#include <string>

class ModelPreviewEngineShader : public PreviewSurface {
public:
    ~ModelPreviewEngineShader() override = default;
    void setShaderRoot(const std::string& dir) { shaderRoot_ = dir; }
    void setDeployDir(const std::string& dir)  { deployDir_ = dir; }
    void setSource(const std::string& tglName) override { tglName_ = tglName; }
    void draw(const ImVec2& availableSize) override;       // always unavailable: the caller falls open to Backend-B
    const char* label() const override { return "Engine Shader (Backend-A, not ported to Vulkan)"; }

    bool ok() const { return report_.ok(); }               // false => caller draws Backend-B
    const ShaderContractReport& report() const { return report_; }

private:
    std::string shaderRoot_ = "shaders", deployDir_ = ".", tglName_;
    ShaderContractReport report_;
    bool reported_ = false;
};
