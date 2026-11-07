// Native static-prop color dispatch. CPU feeds and bucket lifetime remain shared.
#include "gos_static_prop_batcher_internal.h"
#include "vulkan_color_prop_island.h"

void GpuStaticPropBatcher::flush(const RenderSnapshot*) {
    ZoneScopedN("GpuStaticProps.Flush");
    if (auto* pp = getGosPostProcess(); pp && mc2_vk_color_prop_island_enabled())
        (void)mc2_vk_color_prop_island_run(pp->getWidth(), pp->getHeight());
    // Clear even on a declined frame: failed native submission must not accumulate buckets.
    mc2_color_prop_island_end_frame();
}
