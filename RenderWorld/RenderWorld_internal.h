// RenderWorld/RenderWorld_internal.h -- state shared by the RenderWorld translation units (not part of the engine-facing API).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "RenderWorld.h"

namespace RenderWorld { namespace detail {

// The unified record table (indexed by handle.index()) and its guard; the live-mech counter.
extern std::mutex objectRecordsMutex;
extern std::vector<RenderObjectRecord> objectRecords;
extern std::atomic<uint64_t> mechsAlive;

// kMechHandleBase: handle index base for mechs in the unified s_objectRecords table.
// Must exceed the maximum static-prop recipe index across all tier1 missions.
// Known max: 2641 (mc2_24). 65536 provides 24x headroom.
// INVARIANT: max static-prop recipe index < kMechHandleBase.
constexpr uint32_t kMechHandleBase = 0x00010000u;

// M3 v1 reservation (see lookupAtPixel): the first handle index of the terrain range; the mech range ends below it.
constexpr uint32_t kTerrainHandleBase = 0x00040000u;

} }
