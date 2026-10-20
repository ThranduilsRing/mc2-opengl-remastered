// gpu_driven_common.cpp — Phase C GPU-driven rendering: shared infrastructure.
//
// Env-var cache helpers for all Phase C tasks (1.2-1.6, Stages 2-3).
// Each getter uses a static const bool local (lambda-init pattern from
// gos_terrain_lighting.cpp IsEnabled()), so the getenv() call is paid
// exactly once at process start.
//
// Killswitch summary:
//   MC2_GPU_DRIVEN=0           -- disable all gpu_driven paths (default ON)
//   MC2_GPU_DRIVEN_PARITY=1    -- enable parity checks (default OFF)
//   MC2_GPU_DRIVEN_TRACE=1     -- enable trace logging (default OFF)
//   MC2_GPU_DRIVEN_WATER=0     -- disable water fast-path (default ON, gated by global)
//   MC2_GPU_DRIVEN_TERRAIN_SOLID=0 -- disable terrain solid fast-path (default ON, gated by global)
//   MC2_GPU_DRIVEN_OVERLAY=0   -- disable overlay fast-path (default ON, gated by global)

#include <memory>
#include "gpu_driven_common.h"



#include "Gates/gate.h"

namespace gpu_driven {

bool IsGlobalEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_GPU_DRIVEN">();
    return s_enabled;
}

bool IsParityEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_GPU_DRIVEN_PARITY">();
    return s_enabled;
}

bool IsTraceEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_GPU_DRIVEN_TRACE">();
    return s_enabled;
}

bool IsWaterEnabled() {
    static const bool s_enabled = [] {
        if (!IsGlobalEnabled()) return false;
        return mc2::gate::get<"MC2_GPU_DRIVEN_WATER">();
    }();
    return s_enabled;
}

bool IsTerrainSolidEnabled() {
    static const bool s_enabled = [] {
        if (!IsGlobalEnabled()) return false;
        return mc2::gate::get<"MC2_GPU_DRIVEN_TERRAIN_SOLID">();
    }();
    return s_enabled;
}

bool IsOverlayEnabled() {
    static const bool s_enabled = [] {
        if (!IsGlobalEnabled()) return false;
        return mc2::gate::get<"MC2_GPU_DRIVEN_OVERLAY">();
    }();
    return s_enabled;
}

}  // namespace gpu_driven
