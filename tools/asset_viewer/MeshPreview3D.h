// tools/asset_viewer/MeshPreview3D.h
// PreviewSurface impl: loads a .tgl via TglMeshLoader, uploads to MeshGpu,
// renders on a turntable orbit camera into an offscreen Vulkan target, blits via ImGui::Image.
#pragma once
#include "PreviewSurface.h"
#include "MeshGpu.h"
#include <cstdint>
#include <string>
#include <vector>

class MeshPreview3D : public PreviewSurface {
public:
    MeshPreview3D();
    ~MeshPreview3D() override;

    // PreviewSurface interface
    void setSource(const std::string& tglName) override;
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Model (3D)"; }

    // Set the deploy directory before calling setSource().
    void setDeployDir(const std::string& dir) { deployDir_ = dir; }

    // Render an arbitrary MeshData (e.g. a GLB override). Untextured preview.
    void setMeshData(const MeshData& md);

    // Test hook: reload albedo textures at the given tier and update tier_.
    // Thin wrapper over mesh_.reloadAlbedo; safe to call without ImGui.
    void reloadAlbedoAtTier(const std::string& deployDir, int tier) {
        tier_ = tier;
        mesh_.reloadAlbedo(deployDir, tier);
    }

    // Camera / light controls (for UI extensions).
    float& orbitYaw()   { return yaw_; }
    float& orbitPitch() { return pitch_; }
    float& zoom()       { return dist_; }
    float* lightDir()   { return lightDir_; }
    float* modelRotDeg() { return modelRotDeg_; }
    void setCompactControls(bool c) { compact_ = c; }   // collapse the slider block (shape panels keep the viewport tall)

private:
    void ensureGL(int w, int h);
    void destroyFbo();
    void buildViewProj(int w, int h,
                       const float center[3],
                       float out[16], float camPosOut[3]) const;
    void renderScene(int w, int h);

    toolvk::Pipeline pipe_;   // created once in ensureGL

    MeshGpu mesh_;

    // Offscreen target
    toolvk::Target target_;

    bool backendOk_   = false;
    bool fboComplete_ = false;
    bool glReady_     = false;

    // Camera / light
    float yaw_   = 0.6f;
    float pitch_ = 0.35f;
    float dist_  = 3.0f;
    float center_[3] = {0.0f, 0.0f, 0.0f};  // bounds center for orbit pivot
    float lightDir_[3] = { -0.4f, -0.7f, -0.5f };

    // Model rotation: Euler angles in degrees applied as Rx * Ry * Rz (X first).
    // Default -90° X stands most MC2 props upright (they're stored Z-up in TGL).
    float modelRotDeg_[3] = { -90.0f, 0.0f, 0.0f };

    // Texture resolution tier and spotlight-beam toggle.
    int  tier_       = 512;    // active albedo resolution: 128 / 256 / 512 / 1024
    bool compact_    = false;
    bool showLights_ = false;  // when false, SpotLight_* beam submeshes are hidden

    std::string deployDir_ = ".";
    std::string tglName_;
    std::string errorMsg_;
};
