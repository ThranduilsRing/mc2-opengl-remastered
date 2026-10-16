// GameOS/gameos/gpu_memory_report.cpp -- see gpu_memory_report.h.
#include "gpu_memory_report.h"

#include "gos_profiler.h"          // GLEW + Tracy (TracyPlot)
#include "gos_smoke.h"
#include "mc2_diag_verbose.h"
#include "render_backend_select.h"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <string_view>

#ifdef MC2_VULKAN
#include "vulkan_context.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_4.h>
#endif

namespace mc2::gpumem {

namespace {

bool hasGlExtension(std::string_view name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const GLubyte* e = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (e && name == reinterpret_cast<const char*>(e)) return true;
    }
    return false;
}

#ifdef MC2_VULKAN
void readVulkan(Snapshot& s) {
    if (!mc2vk::engineVulkanContextValid()) return;
    uint64_t budget = 0, usage = 0, allocated = 0;
    if (mc2vk::getOrCreateEngineVulkanContext().deviceLocalMemory(budget, usage, allocated)) {
        s.vkBudgetBytes = static_cast<int64_t>(budget);
        s.vkUsageBytes = static_cast<int64_t>(usage);
        s.vkAllocatedBytes = static_cast<int64_t>(allocated);
    }
}
#endif

#ifdef _WIN32
// GL_EXT_memory_object_win32: the LUID of the device the current GL context renders on.
constexpr GLenum kGlDeviceLuidExt = 0x9599;                 // GL_DEVICE_LUID_EXT
using PfnGetUnsignedBytevExt = void(APIENTRY*)(GLenum, GLubyte*);

// The LUID of the adapter the engine renders on: the Vulkan device when the shared context is live, else the GL context's device when the driver
// implements GL_EXT_memory_object_win32. False when neither says (the caller then falls back to the adapter with the most dedicated memory).
bool renderAdapterLuid(uint8_t (&luid)[8]) {
#ifdef MC2_VULKAN
    if (mc2vk::engineVulkanContextValid()) return mc2vk::getOrCreateEngineVulkanContext().adapterLuid(luid);
#endif
    if (!SDL_GL_GetCurrentContext() || !hasGlExtension("GL_EXT_memory_object_win32")) return false;
    auto get = reinterpret_cast<PfnGetUnsignedBytevExt>(SDL_GL_GetProcAddress("glGetUnsignedBytevEXT"));
    if (!get) return false;
    while (glGetError() != GL_NO_ERROR) {}
    GLubyte bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    get(kGlDeviceLuidExt, bytes);
    if (glGetError() != GL_NO_ERROR) return false;
    for (size_t i = 0; i < sizeof(luid); ++i) luid[i] = bytes[i];
    return true;
}

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

}  // namespace

Snapshot take() {
    Snapshot s;
    s.backend = (mc2::renderBackend() == mc2::RenderBackend::Vulkan) ? "vk" : "gl";
#ifdef MC2_VULKAN
    readVulkan(s);
#endif
#ifdef _WIN32
    readDxgi(s);
#endif
    return s;
}

std::string format(std::string_view phase, const Snapshot& s) {
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "[VRAM v1] phase=%s backend=%s vk_budget_mb=%lld vk_usage_mb=%lld vk_allocated_mb=%lld adapter_dedicated_mb=%lld "
                  "proc_dedicated_mb=%lld proc_budget_mb=%lld",
                  std::string(phase).c_str(), s.backend ? s.backend : "gl",
                  static_cast<long long>(bytesToMib(s.vkBudgetBytes)), static_cast<long long>(bytesToMib(s.vkUsageBytes)), static_cast<long long>(bytesToMib(s.vkAllocatedBytes)),
                  static_cast<long long>(bytesToMib(s.dxgiAdapterDedicatedBytes)), static_cast<long long>(bytesToMib(s.dxgiProcessUsageBytes)),
                  static_cast<long long>(bytesToMib(s.dxgiProcessBudgetBytes)));
    return buf;
}

std::string toJson(const Snapshot& s) {
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "{\"backend\": \"%s\", \"vk_budget_mb\": %lld, \"vk_usage_mb\": %lld, \"vk_allocated_mb\": %lld, "
                  "\"adapter_dedicated_mb\": %lld, \"proc_dedicated_mb\": %lld, \"proc_budget_mb\": %lld}",
                  s.backend ? s.backend : "gl",
                  static_cast<long long>(bytesToMib(s.vkBudgetBytes)), static_cast<long long>(bytesToMib(s.vkUsageBytes)), static_cast<long long>(bytesToMib(s.vkAllocatedBytes)),
                  static_cast<long long>(bytesToMib(s.dxgiAdapterDedicatedBytes)), static_cast<long long>(bytesToMib(s.dxgiProcessUsageBytes)),
                  static_cast<long long>(bytesToMib(s.dxgiProcessBudgetBytes)));
    return buf;
}

void log(std::string_view phase) {
    if (!SmokeMode::state().enabled && !mc2DiagVerbose()) return;
    const Snapshot snapshot = take();
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

void tick(unsigned long frame) {
    if (frame % kPlotEveryFrames != 0) return;
#ifdef _WIN32
    IDXGIAdapter3* a = dxgiAdapter();
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (a && SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        TracyPlot("VRAM.Process.MB", static_cast<int64_t>(info.CurrentUsage / (1024 * 1024)));
#else
    (void)frame;
#endif
}

}  // namespace mc2::gpumem
