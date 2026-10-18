// tools/asset_viewer/MaterialRenderBackend.h
#pragma once
#include <cstdint>
#include "tool_vk_gpu.h"

class SphereMesh;

// The four PBR slot textures as toolvk texture ids (0 = unassigned).
struct MaterialSlotTextures {
    uint32_t baseColor = 0;   // sRGB-uploaded
    uint32_t normal    = 0;   // linear; 0 => treat as flat (0,0,1)
    // MATERIAL-M0: rough fallback PINNED to 1.0 (matches MaterialGpu record
    // default + static_prop.frag + LocalPbrMaterialBackend.cpp). See docs/material-m0-contract.md.
    uint32_t orm       = 0;   // linear; R=AO, G=Roughness, B=Metallic; 0 => AO=1, rough=1.0, metal=0
    uint32_t emissive  = 0;   // sRGB; 0 => no emission
};

struct RenderInputs {
    const SphereMesh* mesh = nullptr;
    float viewProj[16];       // column-major MVP-less: this is proj*view (model is identity)
    float cameraPosWorld[3];
    float lightDirWorld[3];   // direction the light travels (surface->light = -lightDir)
    float lightColor[3];
    float ambient[3];
    int   fboWidth = 0;
    int   fboHeight = 0;
};

class MaterialRenderBackend {
public:
    virtual ~MaterialRenderBackend() = default;
    virtual bool init() = 0;                                  // compile/link program; false on failure
    virtual void setMaterial(const MaterialSlotTextures& slots) = 0;
    virtual void render(const RenderInputs& in, toolvk::Pass& pass) = 0;   // records into an open offscreen pass
    virtual void shutdown() = 0;
    virtual const char* name() const = 0;                     // e.g. "LocalPBR (approximate)"
    virtual bool isApproximate() const = 0;                   // Backend B => true
};
