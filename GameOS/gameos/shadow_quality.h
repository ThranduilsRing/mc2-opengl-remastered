// Startup quality selection shared by every shadow producer and receiver.
#pragma once
#include "Gates/render_options.h"
#ifdef MC2_VULKAN_ISLAND
#include "vk_render_infra.h"
#endif
namespace mc2shadow {
struct ShadowQuality {
    int level, maskCap, mapSide, windowSide;
    float maskTarget;
};
constexpr ShadowQuality qualityForLevel(int level) {
    if (level==0) return {0,4096,2048,2048,1.0f};
    if (level==1) return {1,8192,4096,4096,2.0f};
    return {2,12288,4096,4096,1.5f};
}
inline const ShadowQuality& shadowQuality() {
    // Options are published before renderer startup; keep image dimensions
    // fixed for the lifetime of the renderer and include them in cook keys.
    static const ShadowQuality value=[] {
        int level=mc2::options::current().shadowQuality;
#ifdef MC2_VULKAN_ISLAND
        // Reserve room for the complete shadow family, including bake scratch and old/new overlap.
        while(level>0 && !mc2vk::infra::admit(level==2 ? 384ull*1024*1024 : 192ull*1024*1024)) --level;
#endif
        return qualityForLevel(level);
    }();
    return value;
}
}
