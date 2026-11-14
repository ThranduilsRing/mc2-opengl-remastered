// GameOS/gameos/gpu_memory_report.cpp -- see gpu_memory_report.h.
#include "gpu_memory_report.h"
#include "battle_bench.h"

#include "gos_profiler.h"          // TracyPlot
#include "gos_smoke.h"
#include "mc2_diag_verbose.h"
#include "render_backend_select.h"


#include <cstdio>
#include <cstring>
#include <future>
#include <string_view>
#include "mc2fmt.h"

#ifdef MC2_VULKAN
#include "vulkan_context.h"
#include "vk_texture_residency.h"
#include "vk_render_infra.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#endif

namespace mc2::gpumem {

namespace {



#ifdef MC2_VULKAN
void readVulkan(Snapshot& s, const mc2vk::VulkanContext& context) {
    uint64_t budget = 0, usage = 0, allocated = 0;
    if (context.deviceLocalMemory(budget, usage, allocated)) {
        s.vkBudgetBytes = static_cast<int64_t>(budget);
        s.vkUsageBytes = static_cast<int64_t>(usage);
        s.vkAllocatedBytes = static_cast<int64_t>(allocated);
    }
}
#endif

#ifdef _WIN32
bool renderAdapterLuid(uint8_t (&luid)[8]) {
#ifdef MC2_VULKAN
    if (mc2vk::engineVulkanContextValid()) return mc2vk::getOrCreateEngineVulkanContext().adapterLuid(luid);
#endif
    return false;
}

// Match the actual native adapter when its LUID is available.
const char* g_adapterRule = "unresolved";                    // "luid", "most_dedicated" (the driver gave no LUID) or "most_dedicated_luid_unmatched": which rule picked the DXGI adapter (printed once by log())

// The DXGI adapter the engine renders on, found once; queried per call. Matched by LUID to the Vulkan device or the GL context's device; when the
// driver gives no LUID the adapter with the most dedicated memory (the discrete GPU on a desktop with an iGPU) is used, which is wrong on a host with two
// discrete GPUs where the engine runs on the smaller one.
IDXGIAdapter3* dxgiAdapter() {
    static IDXGIAdapter3* adapter = nullptr;
    static bool tried = false;
    if (tried) return adapter;
    tried = true;
    HMODULE dll = LoadLibraryA("dxgi.dll");
    if (!dll) return nullptr;
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto create = reinterpret_cast<CreateFactory1>(GetProcAddress(dll, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return nullptr;
    uint8_t luid[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const bool haveLuid = renderAdapterLuid(luid);
    // One pass over the DXGI adapters. By LUID it takes the adapter the engine renders on; otherwise the one with the most dedicated memory.
    auto pass = [&](bool byLuid) {
        SIZE_T best = 0;
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            IDXGIAdapter3* a3 = nullptr;
            const bool takeIt = byLuid ? std::memcmp(&d.AdapterLuid, luid, sizeof(luid)) == 0 : (d.DedicatedVideoMemory >= best);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && takeIt &&
                SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3) {
                if (adapter) adapter->Release();
                adapter = a3;
                best = d.DedicatedVideoMemory;
            }
            a->Release();
        }
    };
    if (haveLuid) pass(true);
    g_adapterRule = "luid";
    if (!adapter) {
        // No LUID from the driver, or one that names no DXGI adapter (a remote session, WARP): report the most dedicated adapter and say which case it was.
        pass(false);
        g_adapterRule = haveLuid ? "most_dedicated_luid_unmatched" : "most_dedicated";
    }
    factory->Release();
    return adapter;
}

void readDxgi(Snapshot& s) {
    IDXGIAdapter3* a = dxgiAdapter();
    if (!a) return;
    DXGI_ADAPTER_DESC d{};
    if (SUCCEEDED(a->GetDesc(&d))) s.dxgiAdapterDedicatedBytes = static_cast<int64_t>(d.DedicatedVideoMemory);
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        s.dxgiProcessUsageBytes = static_cast<int64_t>(info.CurrentUsage);
        s.dxgiProcessBudgetBytes = static_cast<int64_t>(info.Budget);
    }
}
#endif

int64_t bytesToMib(int64_t v) { return v < 0 ? kUnavailable : v / (1024 * 1024); }

std::future<Snapshot> pendingSnapshot;
uint64_t sampleFrame = 0;
unsigned long long peakWorkingSet = 0;

void publishSnapshot() {
    const Snapshot snapshot = pendingSnapshot.get();
    ZoneScopedN("GpuMemory.BattlebenchOutput");
    std::fprintf(mc2::battlebench::output, "{\"type\":\"memory\",\"frame\":%llu,\"working_set_peak_bytes\":%llu,\"gpu\":%s}\n",
        static_cast<unsigned long long>(sampleFrame), peakWorkingSet, toJson(snapshot).c_str());
    std::fflush(mc2::battlebench::output);
#ifdef _WIN32
    if (TracyIsConnected && snapshot.dxgiProcessUsageBytes >= 0)
        TracyPlot("VRAM.Process.MB", snapshot.dxgiProcessUsageBytes / (1024 * 1024));
#endif
}

}  // namespace

Snapshot take() {
    Snapshot s;
    s.backend = "vk";
#ifdef MC2_VULKAN
    { ZoneScopedN("GpuMemory.VulkanQuery");
      if (mc2vk::engineVulkanContextValid()) readVulkan(s, mc2vk::getOrCreateEngineVulkanContext()); }
#endif
#ifdef _WIN32
    { ZoneScopedN("GpuMemory.DxgiQuery"); readDxgi(s); }
#endif
    return s;
}

std::string format(std::string_view phase, const Snapshot& s) {
    char buf[640];
    (*std::format_to_n(buf, (sizeof(buf)) - 1,
                  "[VRAM v1] phase={} backend={} vk_budget_mb={:d} vk_usage_mb={:d} vk_allocated_mb={:d} adapter_dedicated_mb={:d} proc_dedicated_mb={:d} proc_budget_mb={:d}",
                  std::string(phase).c_str(), mc2::fmt::safe(s.backend ? s.backend : "unavailable"),
                  mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.vkBudgetBytes))), mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.vkUsageBytes))), mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.vkAllocatedBytes))),
                  mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.dxgiAdapterDedicatedBytes))), mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.dxgiProcessUsageBytes))),
                  mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.dxgiProcessBudgetBytes)))).out = '\0');
    return buf;
}

std::string toJson(const Snapshot& s) {
    char buf[640];
    (*std::format_to_n(buf, (sizeof(buf)) - 1,
                  "{{\"backend\": \"{}\", \"vk_budget_mb\": {:d}, \"vk_usage_mb\": {:d}, \"vk_allocated_mb\": {:d}, \"adapter_dedicated_mb\": {:d}, \"proc_dedicated_mb\": {:d}, \"proc_budget_mb\": {:d}}}",
                  mc2::fmt::safe(s.backend ? s.backend : "unavailable"),
                  mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.vkBudgetBytes))), mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.vkUsageBytes))), mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.vkAllocatedBytes))),
                  mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.dxgiAdapterDedicatedBytes))), mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.dxgiProcessUsageBytes))),
                  mc2::fmt::as_i64(static_cast<long long>(bytesToMib(s.dxgiProcessBudgetBytes)))).out = '\0');
    return buf;
}

void log(std::string_view phase) {
    if (!SmokeMode::state().enabled && !mc2DiagVerbose()) return;
    const Snapshot snapshot = take();
#ifdef MC2_VULKAN
    if (phase == "run_end" && mc2vk::engineVulkanContextValid())
        std::fprintf(stderr, "[VK_RESIDENCY v1] phase=run_end %s\n", mc2vk::infra::ledgerJson().c_str());
    if (mc2vk::engineVulkanContextValid())
        std::fprintf(stderr,"[TEXTURE_RESIDENT] phase=%.*s bytes=%llu\n",static_cast<int>(phase.size()),phase.data(),
            static_cast<unsigned long long>(mc2vk::getOrCreateEngineVulkanContext().textures().residentBytes()));
#endif
#ifdef _WIN32
    static bool adapterLogged = false;                          // the rule that picked the DXGI adapter, once, after the first take() resolved it
    if (!adapterLogged) {
        adapterLogged = true;
        std::printf("[VRAM_ADAPTER v1] rule=%s adapter_dedicated_mb=%lld\n", g_adapterRule, static_cast<long long>(bytesToMib(snapshot.dxgiAdapterDedicatedBytes)));
    }
#endif
    const std::string line = format(phase, snapshot);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

void finish() {
    // The allocator and benchmark FILE must outlive the last driver query.
    if (pendingSnapshot.valid()) publishSnapshot();
}

void tick(unsigned long frame) {
    if (pendingSnapshot.valid() && pendingSnapshot.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        publishSnapshot();
    if (frame % kPlotEveryFrames != 0) return;
    Snapshot snapshot;
    if (mc2::battlebench::output) {
        // Never wait for a driver query in a measured frame, even if it spans a cadence.
        if (pendingSnapshot.valid()) return;
        sampleFrame = mc2::battlebench::frame;
        peakWorkingSet = 0;
        { ZoneScopedN("GpuMemory.BattlebenchSnapshot");
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        using GetMemory = BOOL (WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
        const auto getMemory = reinterpret_cast<GetMemory>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo"));
        if (getMemory && getMemory(GetCurrentProcess(), &counters, sizeof(counters))) peakWorkingSet = counters.PeakWorkingSetSize;
#endif
        }
#ifdef _WIN32
        { ZoneScopedN("GpuMemory.DxgiQuery"); readDxgi(snapshot); }
#endif
        snapshot.backend = "vk";
#ifdef MC2_VULKAN
        // Read-only VMA statistics need no present-owner fence; finish() guards lifetime.
        const auto* context = &mc2vk::engineVulkanContextNoWait();
        pendingSnapshot = std::async(std::launch::async, [snapshot, context]() mutable {
            tracy::SetThreadName("GpuMemory");
            ZoneScopedN("GpuMemory.VulkanQuery");
            if (context->valid()) readVulkan(snapshot, *context);
            return snapshot;
        });
#else
        pendingSnapshot = std::async(std::launch::async, [snapshot] { return snapshot; });
#endif
        return;
    }
#ifdef _WIN32
    if (!TracyIsConnected) return;
    ZoneScopedN("GpuMemory.TracyPoll");
    if (!mc2::battlebench::output) {
        // A benchmark snapshot already queried DXGI on this cadence; reuse it.
        IDXGIAdapter3* a = dxgiAdapter();
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (a && SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            snapshot.dxgiProcessUsageBytes = static_cast<int64_t>(info.CurrentUsage);
    }
    if (snapshot.dxgiProcessUsageBytes >= 0)
        TracyPlot("VRAM.Process.MB", snapshot.dxgiProcessUsageBytes / (1024 * 1024));
#endif
}

}  // namespace mc2::gpumem
