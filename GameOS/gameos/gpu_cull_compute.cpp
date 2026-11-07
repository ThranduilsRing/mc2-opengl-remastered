// Native Vulkan actor culling. Static-prop draw compaction belongs to its native island.
#include "gpu_cull_compute.h"
#include "gpu_cull_substrate.h"
#include "gpu_cull_readback.h"
#include "vk_actor_cull_native.h"
#include "gos_static_prop_killswitch.h"
#include "gos_profiler.h"
#include "mc2_diag_verbose.h"
#include "Gates/gate.h"
#include "gos_cull_freeze.h"
#include <gameos.hpp>
#include <cstdio>

namespace gpu_cull {
namespace { bool initialized=false; uint32_t frame=0, summaries=0; }
bool compute_isEnabled() { return mc2::gate::get<"MC2_GPU_CULL">(); }
bool compute_init() {
    compute_shutdown();
    if (!compute_isEnabled() || !substrate_isEnabled() || !readback_isEnabled()) return true;
    if (!nativeCullInit(substrate_capacity())) {
        STOP(("[GPU_CULL] native Vulkan actor visibility initialization/selftest failed"));
        return false;
    }
    initialized=true; frame=summaries=0;
    MC2_DIAGF("[GPU_CULL v1] event=compute_selftest result=ok maxActors=%u producer=vulkan\n",substrate_capacity());
    return true;
}
void compute_shutdown() { nativeCullShutdown(); initialized=false; frame=summaries=0; }
void compute_dispatch() {
    if (!initialized || !compute_isEnabled()) return;
    ZoneScopedN("Cull.Native.Submit");
    readback_tryConsume();
    const auto records=substrate_currentRecords();
    const float* matrix=mc2cullfreeze::cullMvp(gos_GetTerrainMVPMat4());
    if (!matrix || records.empty()) return;
    const float dilation=0.08f;
    if (readback_submit(records,matrix,dilation,frame)) readback_frameEnd(frame++);
}
void compute_emitParitySummary() {
    if (!initialized || !frame) return;
    if (++summaries!=1 && summaries%600!=0) return;
    const uint32_t visible=readback_getLastGoodVisibleCount();
    if (const auto ns=nativeCullLastDispatchNs()) {
        MC2_DIAGF("[GPU_CULL v2] event=native_dispatch frame=%u visible=%u elapsed_us=%llu producer=vulkan\n",
                  frame,visible,static_cast<unsigned long long>(*ns/1000));
    } else {
        MC2_DIAGF("[GPU_CULL v2] event=native_dispatch frame=%u visible=%u timing=unavailable producer=vulkan\n",frame,visible);
    }
}
}
