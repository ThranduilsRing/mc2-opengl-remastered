#include "RenderResourceRegistry.h"

namespace RenderCore {

namespace {

constexpr size_t kSlots = static_cast<size_t>(RenderResourceId::Count);

RenderResourceDesc s_registry[kSlots] = {};

} // namespace

void registerOrUpdateRenderResource(const RenderResourceDesc& desc) {
    const size_t idx = static_cast<size_t>(desc.id);
    if (idx == 0 || idx >= kSlots) return;
    s_registry[idx] = desc;
}

const RenderResourceDesc* getRenderResource(RenderResourceId id) {
    const size_t idx = static_cast<size_t>(id);
    if (idx == 0 || idx >= kSlots) return nullptr;
    return s_registry[idx].valid ? &s_registry[idx] : nullptr;
}

size_t getRenderResourceCount() {
    size_t n = 0;
    for (size_t i = 1; i < kSlots; ++i)
        if (s_registry[i].valid) ++n;
    return n;
}

const RenderResourceDesc* getRenderResourceByIndex(size_t index) {
    size_t n = 0;
    for (size_t i = 1; i < kSlots; ++i) {
        if (s_registry[i].valid) {
            if (n == index) return &s_registry[i];
            ++n;
        }
    }
    return nullptr;
}

const char* toString(RenderResourceId id) {
    switch (id) {
        case RenderResourceId::Unknown:              return "Unknown";
        case RenderResourceId::MainColor:            return "MainColor";
        case RenderResourceId::MainDepth:            return "MainDepth";
        case RenderResourceId::ShadowStaticMap:      return "ShadowStaticMap";
        case RenderResourceId::TerrainHeightTexture: return "TerrainHeightTexture";
        case RenderResourceId::MaterialGpuBuffer:    return "MaterialGpuBuffer";
        case RenderResourceId::ShadowDynamicMap:     return "ShadowDynamicMap";
        case RenderResourceId::WaterReflectionColor: return "WaterReflectionColor";
        case RenderResourceId::WaterReflectionDepth: return "WaterReflectionDepth";
        case RenderResourceId::Backbuffer:           return "Backbuffer";
        case RenderResourceId::TerrainRecipeBuffer:  return "TerrainRecipeBuffer";
        case RenderResourceId::TerrainThinBuffer:    return "TerrainThinBuffer";
        case RenderResourceId::CementAtlas:          return "CementAtlas";
        case RenderResourceId::TransitionMaskArray:  return "TransitionMaskArray";
        case RenderResourceId::TerrainHeightSsbo:    return "TerrainHeightSsbo";
        case RenderResourceId::MainNormal:           return "MainNormal";
        case RenderResourceId::SceneDepthCopy:       return "SceneDepthCopy";
        case RenderResourceId::SceneObjectId:        return "SceneObjectId";
        case RenderResourceId::HzbPyramid:           return "HzbPyramid";
        case RenderResourceId::SsaoOcclusion:        return "SsaoOcclusion";
        case RenderResourceId::SceneColorCopy:       return "SceneColorCopy";
        case RenderResourceId::ClusterDepthPyramid:  return "ClusterDepthPyramid";
        case RenderResourceId::LightgridGrid:        return "LightgridGrid";
        case RenderResourceId::LightgridIndex:       return "LightgridIndex";
        case RenderResourceId::PostprocessComputeBlur: return "PostprocessComputeBlur";
        case RenderResourceId::ViewUniformsUbo:      return "ViewUniformsUbo";
        case RenderResourceId::TerrainTypeSsbo:      return "TerrainTypeSsbo";
        case RenderResourceId::TerrainCementSsbo:    return "TerrainCementSsbo";
        case RenderResourceId::StaticPropMaterialGpuBuffer: return "StaticPropMaterialGpuBuffer";
        case RenderResourceId::MechProfileMaterialGpuBuffer: return "MechProfileMaterialGpuBuffer";
        case RenderResourceId::TerrainVisualHeightSsbo: return "TerrainVisualHeightSsbo";
        case RenderResourceId::TerrainVisualDampSsbo:   return "TerrainVisualDampSsbo";
        case RenderResourceId::TerrainLightVertexInputSsbo:   return "TerrainLightVertexInputSsbo";
        case RenderResourceId::TerrainLightInputSsbo:         return "TerrainLightInputSsbo";
        case RenderResourceId::TerrainLightComputeOutputSsbo: return "TerrainLightComputeOutputSsbo";
        case RenderResourceId::LightDataSsbo:        return "LightDataSsbo";
        case RenderResourceId::DynamicFullMapFbo:    return "DynamicFullMapFbo";
        case RenderResourceId::DynamicPropShadowSsbo: return "DynamicPropShadowSsbo";
        case RenderResourceId::PostprocessSubgraphColor: return "PostprocessSubgraphColor";
        case RenderResourceId::PostprocessSubgraphDepth: return "PostprocessSubgraphDepth";
        case RenderResourceId::Count:                return "Count";
    }
    return "unknown";
}

const char* toString(RenderResourceKind kind) {
    switch (kind) {
        case RenderResourceKind::Unknown:        return "Unknown";
        case RenderResourceKind::Texture2D:      return "Texture2D";
        case RenderResourceKind::Texture2DArray: return "Texture2DArray";
        case RenderResourceKind::TextureCube:    return "TextureCube";
        case RenderResourceKind::Buffer:         return "Buffer";
    }
    return "unknown";
}

const char* toString(RenderResourceLifetime lifetime) {
    switch (lifetime) {
        case RenderResourceLifetime::Unset:      return "Unset";
        case RenderResourceLifetime::FrameLocal: return "FrameLocal";
        case RenderResourceLifetime::Mission:    return "Mission";
        case RenderResourceLifetime::Persistent: return "Persistent";
        case RenderResourceLifetime::External:   return "External";
    }
    return "unknown";
}

bool validateRenderResourceLifetimes(RenderResourceId* offending) {
    for (size_t i = 1; i < kSlots; ++i) {
        if (s_registry[i].valid &&
            s_registry[i].lifetime == RenderResourceLifetime::Unset) {
            if (offending) *offending = s_registry[i].id;
            return false;
        }
    }
    if (offending) *offending = RenderResourceId::Unknown;
    return true;
}

const char* toString(RenderResourceFormat fmt) {
    switch (fmt) {
        case RenderResourceFormat::Unknown:   return "Unknown";
        case RenderResourceFormat::R32F:      return "R32F";
        case RenderResourceFormat::RGBA16F:   return "RGBA16F";
        case RenderResourceFormat::RGBA8:     return "RGBA8";
        case RenderResourceFormat::Depth16:   return "Depth16";
        case RenderResourceFormat::Depth24:   return "Depth24";
        case RenderResourceFormat::Depth32F:  return "Depth32F";
        case RenderResourceFormat::BufferRaw: return "BufferRaw";
    }
    return "unknown";
}

} // namespace RenderCore
