#pragma once
#include "gpu_cull_record.h"
#include "../../RenderCore/VisibilityHistory.h"
#include <span>
namespace gpu_cull {
using ReadbackTier=RenderCore::VisibilityTier;
bool readback_init(uint32_t maxActors);
void readback_shutdown();
bool readback_isEnabled();
// Queue the current frame; an occupied native slot is skipped without waiting.
bool readback_submit(std::span<const GpuActorRecord> records,const float* matrix,float dilation,uint32_t frame);
void readback_frameEnd(uint32_t frameIndex);
ReadbackTier readback_tryConsume();
uint32_t readback_getLastGoodVisibleCount();
void readback_selftest();
void readback_buildActorVisSnapshot(uint32_t maxActorHandle);
bool readback_isActorVisibleLagged(uint32_t actorId);
}
