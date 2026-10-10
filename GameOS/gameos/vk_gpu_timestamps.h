// vk_gpu_timestamps.h — VK-DRAW-PERF-1 GPU timestamp instrumentation (header-only).
//
// A tiny, self-contained GPU-timestamp bracket for the island render passes. Gated
// behind MC2_VK_GPU_TIMESTAMPS (default-OFF, verification-only — same class as
// MC2_VK_TERRAIN_HEIGHT_AUDIT). Uses vkCmdWriteTimestamp (VK 1.1 core, no device
// feature required — only the queue-family timestampValidBits + limits.timestampPeriod
// gate it, so it is MoltenVK-clean). No pipeline-statistics query (that WOULD need a
// device feature the shared engine context does not enable).
//
// Usage (per island, once fenced per frame so a single pool is race-free):
//   GpuTimer t; t.init(device, phys, gfxFamily, N);   // in island init
//   ... per frame, on the recording command buffer:
//   t.reset(cb);                          // BEFORE vkCmdBeginRenderPass
//   t.write(cb, 0);                       // stamp 0 (pass start)
//   vkCmdBeginRenderPass(...); ... binds ...
//   t.write(cb, 1);                       // stamp 1 (draws about to start)
//   ... draw loop ...
//   vkCmdEndRenderPass(cb);
//   t.write(cb, 2);                       // stamp 2 (all GPU work done)
//   ... vkQueueSubmit + vkWaitForFences ...
//   t.resolve();                          // AFTER the fence is signalled
//   // then t.avgTotalUs(), t.avgGapUs(1) etc. for the breakdown.
#pragma once

#include <volk.h>
#include <vector>
#include <cstdlib>
#include <cstdint>
#include "Gates/gate.h"

namespace mc2vkts {

inline bool timestampsEnabled() {
    static int s = -1;
    if (s < 0) s = (mc2::gate::get<"MC2_VK_GPU_TIMESTAMPS">()) ? 1 : 0;
    return s == 1;
}

struct GpuTimer {
    VkDevice    device   = VK_NULL_HANDLE;
    VkQueryPool pool     = VK_NULL_HANDLE;
    float       periodNs = 0.0f;      // props.limits.timestampPeriod (ns per tick)
    uint32_t    count    = 0;         // number of timestamps in the pool
    bool        supported = false;

    // Exponential-moving-average of each inter-stamp gap, in microseconds.
    // gapAvgUs[i] = avg( stamp[i] - stamp[i-1] ) for i in [1..count-1].
    double   gapAvgUs[8] = {0};
    double   totalAvgUs  = 0.0;
    uint64_t samples     = 0;

    bool init(VkDevice dev, VkPhysicalDevice phys, uint32_t queueFamily, uint32_t nQueries) {
        if (!timestampsEnabled()) { supported = false; return false; }
        device = dev;
        count  = (nQueries > 8) ? 8 : nQueries;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        periodNs = props.limits.timestampPeriod;
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfc);
        if (qfc) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfc, qfs.data());
        if (periodNs <= 0.0f || queueFamily >= qfc || qfs[queueFamily].timestampValidBits == 0) {
            supported = false; return false;
        }
        VkQueryPoolCreateInfo qpci{};
        qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = count;
        if (vkCreateQueryPool(device, &qpci, nullptr, &pool) != VK_SUCCESS) {
            supported = false; return false;
        }
        supported = true;
        return true;
    }

    // Reset the whole pool — MUST be called OUTSIDE any render pass (before Begin).
    void reset(VkCommandBuffer cb) {
        if (supported) vkCmdResetQueryPool(cb, pool, 0, count);
    }

    // All stamps use BOTTOM_OF_PIPE so each gap = execution time of the work recorded
    // between the two stamps (completion-to-completion), which is what we want for a
    // per-segment GPU cost breakdown.
    void write(VkCommandBuffer cb, uint32_t idx) {
        if (supported && idx < count)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, idx);
    }

    // Fetch results (fence for this frame must already be signalled) and fold into EMA.
    void resolve() {
        if (!supported) return;
        uint64_t ts[8] = {0};
        VkResult r = vkGetQueryPoolResults(
            device, pool, 0, count, sizeof(uint64_t) * count, ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS) return;
        ++samples;
        const double a = 0.05;   // EMA weight for the newest sample
        for (uint32_t i = 1; i < count; ++i) {
            double us = (double)(ts[i] - ts[i - 1]) * (double)periodNs / 1000.0;
            gapAvgUs[i] = (samples == 1) ? us : (gapAvgUs[i] * (1.0 - a) + us * a);
        }
        double tot = (double)(ts[count - 1] - ts[0]) * (double)periodNs / 1000.0;
        totalAvgUs = (samples == 1) ? tot : (totalAvgUs * (1.0 - a) + tot * a);
    }

    double avgTotalUs()          const { return totalAvgUs; }
    double avgGapUs(uint32_t i)  const { return (i < count) ? gapAvgUs[i] : 0.0; }

    void destroy() {
        if (pool) { vkDestroyQueryPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
        supported = false;
    }
};

} // namespace mc2vkts
