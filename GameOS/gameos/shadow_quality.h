// Startup quality selection shared by every shadow producer and receiver.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include "Gates/render_options.h"
#include "mc2fmt.h"
#include "vk_render_infra.h"
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
constexpr ShadowQuality qualityForLimit(int level, int limit) {
    while(level>0 && qualityForLevel(level).maskCap>limit) --level;
    auto q=qualityForLevel(level);
    q.maskCap=(std::min)(q.maskCap,limit);
    q.mapSide=(std::min)(q.mapSide,limit);
    q.windowSide=(std::min)(q.windowSide,limit);
    return q;
}
inline const ShadowQuality& shadowQuality() {
    // Options are published before renderer startup; keep image dimensions
    // fixed for the lifetime of the renderer and include them in cook keys.
    static const ShadowQuality value=[] {
        int level=mc2::options::current().shadowQuality;
        level=qualityForLimit(level,static_cast<int>(mc2vk::infra::imageLimit())).level;
        // Reserve room for the complete shadow family, including bake scratch and old/new overlap.
        while(level>0 && !mc2vk::infra::admit(level==2 ? 384ull*1024*1024 : 192ull*1024*1024)) --level;
        if(level!=mc2::options::current().shadowQuality)
            mc2::fmt::print_err("[VK_RESIDENCY v1] refused=shadow_family quality={:d} fallback=quality_{:d} reason=budget_or_device_limits\n",mc2::fmt::as_i32(mc2::options::current().shadowQuality),mc2::fmt::as_i32(level));
        return qualityForLimit(level,static_cast<int>(mc2vk::infra::imageLimit()));
    }();
    return value;
}
}
