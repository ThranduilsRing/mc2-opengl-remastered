// Locals the GpuStaticPropBatcher::finalizeGeometry phases hand to each other.
#ifndef GAMEOS_GAMEOS_GOS_STATIC_PROP_FINALIZE_FRAME_H
#define GAMEOS_GAMEOS_GOS_STATIC_PROP_FINALIZE_FRAME_H

#include "gos_static_prop_batcher_internal.h"

// Locals the static-prop finalize phases share.
struct StaticPropFinalizeFrame {
    std::chrono::steady_clock::time_point coalesceStart;
    uint32_t typeCount;
    std::vector<int32_t> layerForType;
    std::vector<int32_t> layerForPacket;
    std::vector<float> uvScaleXByPacket;
    std::vector<float> uvScaleYByPacket;
};

#endif  // GAMEOS_GAMEOS_GOS_STATIC_PROP_FINALIZE_FRAME_H
