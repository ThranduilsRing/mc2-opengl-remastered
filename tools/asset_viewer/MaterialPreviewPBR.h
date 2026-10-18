// tools/asset_viewer/MaterialPreviewPBR.h
#pragma once
#include "PreviewSurface.h"
#include "MaterialRenderBackend.h"
#include "LocalPbrMaterialBackend.h"
#include "MaterialTextureLoader.h"
#include "SphereMesh.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

class MaterialPreviewPBR : public PreviewSurface {
public:
    MaterialPreviewPBR();
    ~MaterialPreviewPBR() override;

    void setSource(const std::string& path) override;          // FIT path (Task 8); no-op for now
    void draw(const ImVec2& availableSize) override;
    const char* label() const override { return "Material (PBR)"; }

    // Slot assignment (called by MaterialSlots UI in Task 5). Takes ownership of the toolvk texture.
    void setSlotTexture(MaterialSlotKind kind, uint32_t tex);

    // Camera/light controls exposed so the UI (Task 5) can drive them.
    float& orbitYaw()   { return yaw_; }
    float& orbitPitch() { return pitch_; }
    float& zoom()       { return dist_; }
    float* lightDir()   { return lightDir_; }

private:
    void ensureGL(int w, int h);     // lazy backend/mesh init + (re)create the offscreen target on size change
    void destroyFbo();
    void buildViewProj(int w, int h, float out[16], float camPosOut[3]) const;

    std::unique_ptr<MaterialRenderBackend> backend_;
    SphereMesh mesh_;
    MaterialSlotTextures slots_{};
    bool glReady_ = false;
    bool backendOk_ = false;     // review fix MAJOR 6: backend_->init() succeeded
    bool fboComplete_ = false;   // review fix MAJOR 6: last FBO build was complete

    toolvk::Target target_;

    float yaw_ = 0.6f, pitch_ = 0.3f, dist_ = 3.0f;
    float lightDir_[3] = { -0.4f, -0.7f, -0.5f };
    std::string fitPath_;
};
