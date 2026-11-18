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

// Handle partition and boundary/decode helpers live in RenderWorld.h so the
// registry and offline tests share them without linking RenderWorld.cpp.

} }
