// Shared surface of the gos_particle_bridge.cpp translation units (gos_particle_bridge.cpp, gos_particle_bridge_tube.cpp).
//
// gos_particle_bridge.cpp keeps the billboard state, init, the particle flush and the VFX tuning accessors; the oracle
// tube ribbon bridge is split out (see DOC in the piece).
#ifndef GOS_PARTICLE_BRIDGE_INTERNAL_H
#define GOS_PARTICLE_BRIDGE_INTERNAL_H

#include "gos_particle_bridge.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include "particles/spec.h"
#include "particles/batcher.h"  // GroupInfo

#include <gameos.hpp>
#include "../../RenderCore/PipelineRegistry.h"  // VFX-APPLYPIPELINE-ROUTING-1
#include "gos_postprocess.h"  // VFX-SOFT-PARTICLES-MVP-1: scene-depth copy + invViewProj
#include "../../mclib/camera.h"  // VFX-LIT-PARTICLES-MVP-1: eye->light*/ambient* (same source as terrain)
#include "particle_stall_probe.h"  // PARTICLE-FLUSH-STALL-MEASURE-1 (gated timing)
#include "gpu_cull_readback.h"  // READBACK_SSBO_BINDING (slot 14) — single source of truth
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_vfx_island.h"  // VK-FIELD-VFX-1: VK translucent particle pass
#endif

#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>    // MC2_VFX_ORACLE_TUBE: pos3->vec4 staging
#include <tracy/Tracy.hpp>               // Q2-S0 coarse CPU zones (flush paths)
#include "Gates/gate.h"

// Globals defined in gos_particle_bridge.cpp (or elsewhere in the engine) that the split pieces share.
extern const float* gos_GetTerrainMVPMat4();
extern const float* gos_GetProj2ScreenMat4();

// Group-log gate: defined in gos_particle_bridge.cpp, read by both pieces.
bool groupLogEnabled();

#endif // GOS_PARTICLE_BRIDGE_INTERNAL_H
