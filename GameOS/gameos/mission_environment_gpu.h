#pragma once
#include "../../RenderCore/MissionEnvironment.h"
#include "../../RenderCore/gpu_abi/GpuEnvironment.h"
#include "../../mclib/lighting_state.h"

namespace mc2::environment {

// The one producer of the per-frame EnvironmentUBO (TERRAIN_RESOURCE_SET binding 26). Pure: the
// resolved MissionEnvironment record plus the camera's prelit-ground scale in, the GPU block out.
// Per-mission SH: the sky's directional SH shape is kept and only the L0 hue follows the mission's
// decoded ambient colour (luminance unchanged), weighted by the day share so the night SH, which is
// already built from the mission ambient, is not tinted twice.
void fillGpu(const RenderCore::MissionEnvironment& env, float sceneLightScale, EnvironmentUBO& out,
             const mc2::lighting::LightingState& live = {}, const mc2::lighting::LightingState& bake = {});

// The L0 chroma multiplier fillGpu applies for an ambient colour (exposed for the doctest).
void missionL0Chroma(const std::array<float,3>& ambient, float out[3]);

}
