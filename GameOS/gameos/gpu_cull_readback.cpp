// Native Vulkan visibility results and their CPU-owned lagged history.
#include "gpu_cull_readback.h"
#include "vk_actor_cull_native.h"
#include "gpu_cull_substrate.h"
#include "gos_profiler.h"
#include "mc2_diag_verbose.h"
#include "Gates/gate.h"
#include <gameos.hpp>
#include <cstdio>
#include <array>

namespace gpu_cull {
namespace {
RenderCore::VisibilityHistory history;
std::array<bool,RenderCore::VisibilityHistory::kSlots> pending{};
bool initialized=false, forceNotReady=false;
uint32_t snapshots=0, consumes=0;
uint64_t dilatedTotal=0, orTotal=0;
bool trace() { return false; }
bool conservativeOr() { return true; }
}
bool readback_isEnabled() { return true; }
bool readback_init(uint32_t maxActors) {
    if (!readback_isEnabled()) return true;
    readback_shutdown();
    history=RenderCore::VisibilityHistory(maxActors); pending.fill(false);
    forceNotReady=false;
    initialized=true; snapshots=consumes=0; dilatedTotal=orTotal=0;
    MC2_DIAGF("[GPU_CULL v1] event=readback_init producer=vulkan maxActors=%u ring=3 forceNotReady=%d\n",maxActors,int(forceNotReady));
    readback_selftest();
    return true;
}
void readback_shutdown() {
    history=RenderCore::VisibilityHistory{}; pending.fill(false);
    initialized=false; snapshots=consumes=0; dilatedTotal=orTotal=0;
}
bool readback_submit(std::span<const GpuActorRecord> records,const float* matrix,float dilation,uint32_t frame) {
    if (!initialized) return false;
    const uint32_t slot=history.writeSlot();
    const auto result=nativeCullSubmit(slot,records,matrix,dilation,frame);
    pending[slot]=result==NativeCullSubmit::Submitted;
    if (result==NativeCullSubmit::Failed) {
        STOP(("[GPU_CULL] native actor visibility submit failed"));
        return false;
    }
    if (result==NativeCullSubmit::Busy && trace())
        MC2_DIAGF("[GPU_CULL v1] event=readback_skip reason=slot_busy slot=%u frame=%u\n",slot,frame);
    return true; // a busy slot advances the frame with no new result (fail open).
}
void readback_frameEnd(uint32_t frame) {
    if (!initialized) return;
    if (trace()) MC2_DIAGF("[GPU_CULL v1] event=frame_end slot=%u frameIndex=%u producer=vulkan\n",history.writeSlot(),frame);
    history.advance();
}
ReadbackTier readback_tryConsume() {
    if (!initialized || !readback_isEnabled()) return ReadbackTier::Tier3_Conservative;
    const auto previous=history.lastGoodSlot();
    const auto tier=history.consume([](uint32_t slot,RenderCore::VisibilityHistory::Sample& sample) {
        if (forceNotReady || !pending[slot]) return false;
        NativeCullHeader header{}; uint32_t count=0;
        if (!nativeCullPoll(slot,header,count,sample.bits,sample.ids)) return false;
        sample.count=count; sample.visibleCount=header.visible;
        sample.dilatedAdmits=header.dilated; sample.frame=header.frame;
        pending[slot]=false;
        return true;
    });
    if (tier!=ReadbackTier::Tier3_Conservative) {
        TracyPlot("GPU.VisibleCount.GPU",int64_t(history.visibleCount()));
        TracyPlot("GPU.VisibleCount.CPU",int64_t(substrate_getCpuVisibleCount()));
        ++consumes;
        if (trace() && (consumes==1 || consumes%600==0))
            MC2_DIAGF("[GPU_CULL v1] event=readback_ok slot=%u visibleCount=%u tier=%d producer=vulkan\n",history.lastGoodSlot(),history.visibleCount(),int(tier));
    } else if (previous!=RenderCore::VisibilityHistory::kNone && history.lastGoodSlot()==RenderCore::VisibilityHistory::kNone) {
        MC2_DIAGF("[GPU_CULL v1] event=readback_stale_reset stale_frames=%u producer=vulkan\n",history.staleFrames());
    }
    return tier;
}
uint32_t readback_getLastGoodVisibleCount() { return history.visibleCount(); }
void readback_buildActorVisSnapshot(uint32_t maxActorHandle) {
    ++snapshots;
    const bool log=trace() && (snapshots==1 || snapshots%600==0);
    const auto stats=history.buildSnapshot(maxActorHandle,conservativeOr(),log);
    if (stats.overflow) STOP(("[GPU_CULL] readback actor overflow: id=%u cap=%u",stats.overflow,maxActorHandle));
    dilatedTotal+=history.dilatedAdmits(); orTotal+=stats.orAdmits;
    if (log) {
        MC2_DIAGF("[GPU_CULL v1] event=lifecycle_snapshot slot=%u visible=%u invisible=%u conservative=%d or_admits=%u producer=vulkan\n",
                  history.lastGoodSlot(),stats.visible,stats.invisible,int(history.lastGoodSlot()==RenderCore::VisibilityHistory::kNone),stats.orAdmits);
        if (snapshots%600==0) {
            MC2_DIAGF("[GPU_CULL v1] event=motion_tolerance frames=600 dilated_admits=%llu conservative_or_admits=%llu or_enabled=%d producer=vulkan\n",
                      static_cast<unsigned long long>(dilatedTotal),static_cast<unsigned long long>(orTotal),int(conservativeOr()));
            dilatedTotal=orTotal=0;
        }
    }
}
bool readback_isActorVisibleLagged(uint32_t actorId) {
    return !initialized || !readback_isEnabled() || history.isVisible(actorId);
}
void readback_selftest() {
    // Exercise the production history's tier and identity logic without altering
    // this mission's state. The native producer separately probes the actual GPU.
    RenderCore::VisibilityHistory test(2);
    std::array<bool,3> ready{};
    auto fetch=[&](uint32_t slot,RenderCore::VisibilityHistory::Sample& sample) {
        if (!ready[slot]) return false;
        ready[slot]=false; sample.count=2; sample.ids[0]=1; sample.ids[1]=2;
        sample.bits[0]=slot==0; sample.bits[1]=slot==2;
        return true;
    };
    unsigned pass=0;
    test.advance();
    if (test.consume(fetch)==ReadbackTier::Tier3_Conservative) ++pass;
    ready[2]=true;
    if (test.consume(fetch)==ReadbackTier::Tier2_NMinusTwo) ++pass;
    ready[0]=true;
    if (test.consume(fetch)==ReadbackTier::Tier1_NMinusOne) ++pass;
    const auto stats=test.buildSnapshot(8192,true);
    if (test.isVisible(1) && test.isVisible(2) && stats.orAdmits==1) ++pass;
    MC2_DIAGF("[GPU_CULL v1] event=readback_selftest pass=%u fail=%u producer=vulkan history=cpu\n",pass,4-pass);
    if (pass!=4) STOP(("[GPU_CULL] native visibility history selftest failed"));
}
}
