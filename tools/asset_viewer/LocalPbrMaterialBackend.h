// tools/asset_viewer/LocalPbrMaterialBackend.h
#pragma once
#include "MaterialRenderBackend.h"

class LocalPbrMaterialBackend : public MaterialRenderBackend {
public:
    bool init() override;
    void setMaterial(const MaterialSlotTextures& slots) override;
    void render(const RenderInputs& in, toolvk::Pass& pass) override;
    void shutdown() override;
    const char* name() const override { return "LocalPBR (approximate)"; }
    bool isApproximate() const override { return true; }

private:
    toolvk::Pipeline pipe_;
    MaterialSlotTextures slots_{};
};
