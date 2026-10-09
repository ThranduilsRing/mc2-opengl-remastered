// RenderCore/StaticPropInstanceDesc.h
//
// Slice M1 (C1 fix 2026-05-22): POD mirror of GpuStaticPropInstance.
// Pure C++ standard types only -- no GL, no Stuff, no mclib.
//
// Translation between this and GpuStaticPropInstance happens at the
// two engine-game seams:
//   - Game-side seam: GameAdapters/StaticPropRenderAdapter.cpp
//   - Engine-side seam: RenderWorld/legacy/static_prop_backend.cpp
//
// Both seams compile-time assert sizeof and offsetof match. If
// GpuStaticPropInstance ever drifts, the asserts fail loudly and this
// mirror must be updated in lockstep. The firewall script
// (scripts/check-include-firewall.sh) enforces that no OTHER TU
// includes gos_static_prop_batcher.h from RenderCore/ or RenderWorld/
// outside the legacy-backend TU.

#pragma once

#include <cstdint>
#include <cstddef>
#include "gpu_abi/GpuStaticPropInstance.h"

namespace RenderCore {

// Alias of the single-source GPU record (RenderCore/gpu_abi/GpuStaticPropInstance.h,
// generated layout asserts). No longer a hand-kept mirror: there is one struct.
using StaticPropInstanceDesc = ::GpuStaticPropInstance;

} // namespace RenderCore
