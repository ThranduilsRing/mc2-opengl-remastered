// GameOS/gameos/gpu_memory_report.h -- how much GPU memory the process holds, read from whatever the driver offers.
//
// WHAT: one snapshot of the GPU memory counters that exist on this machine, the one-line `[VRAM v1]` rendering of it, and a Tracy plot of the
// process's dedicated-video-memory use. Every counter that is not available is -1, never a guess.
// WHY: frame time alone cannot say that a change doubled texture memory or leaked a render target; VRAM is tracked next to the Tracy trace and
// the smoke report so a perf baseline names the memory it was taken at. The counters come from two independent sources because neither exists
// everywhere: the Vulkan Memory Allocator (the VK backend) and DXGI (the process's own dedicated-video-memory use on Windows, whichever API allocated it).
// Nothing here allocates GPU memory or syncs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mc2::gpumem {

inline constexpr int64_t kUnavailable = -1;
inline constexpr unsigned long kPlotEveryFrames = 120;

struct Snapshot {
    const char* backend = "gl";                    // "gl" or "vk": the backend that drew
    // Vulkan Memory Allocator, bytes, summed over the device-local heaps: the OS budget, the process usage, the bytes handed out by allocations.
    int64_t vkBudgetBytes = kUnavailable;
    int64_t vkUsageBytes = kUnavailable;
    int64_t vkAllocatedBytes = kUnavailable;
    // DXGI, bytes: the adapter's dedicated memory, this process's use of the local segment, and the budget the OS gives the process.
    int64_t dxgiAdapterDedicatedBytes = kUnavailable;
    int64_t dxgiProcessUsageBytes = kUnavailable;
    int64_t dxgiProcessBudgetBytes = kUnavailable;
};

// The counters available right now. `backend` is "vk" when the shared Vulkan device is live, else "gl".
Snapshot take();

// `[VRAM v1] phase=<phase> backend=... key=value ...` with every size in whole MiB (-1 = unavailable). Pure: no GPU or driver access.
std::string format(std::string_view phase, const Snapshot& s);

// The same numbers as one JSON object (whole MiB, -1 = unavailable) for the debug-state dump.
std::string toJson(const Snapshot& s);

// Print the line when the smoke harness or MC2_DIAG_VERBOSE asked for diagnostics (a player's console stays quiet).
void log(std::string_view phase);

// Once per `kPlotEveryFrames` frames: plot the process's dedicated video memory (MiB) to Tracy as "VRAM.Process.MB".
void tick(unsigned long frame);

}  // namespace mc2::gpumem
