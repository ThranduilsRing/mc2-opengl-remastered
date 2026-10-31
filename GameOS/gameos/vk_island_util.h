// vk_island_util.h -- the two helpers every Vulkan island / native-post TU needs and
// used to carry its own copy of: the tagged stderr log line and the SPIR-V loader.
// Each TU keeps a one-line `vlog` wrapper (its tag is the only thing that differs) and
// a one-line `load_spv` wrapper, so call sites are unchanged.
#ifndef MC2_VK_ISLAND_UTIL_H
#define MC2_VK_ISLAND_UTIL_H

#ifdef MC2_VULKAN_ISLAND

#include <volk.h>
#include "vk_render_infra.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace vk_island_util {

// printf-style sink a TU passes to loadSpv; nullptr = fail silently.
using LogFn = void (*)(const char* fmt, ...);

// "<tag><message>\n" to stderr, flushed. `tag` includes its trailing space.
inline void vlogv(const char* tag, const char* fmt, va_list ap) {
    va_list measure;
    va_copy(measure, ap);
    const int count = std::vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    if (count < 0) return;
    std::string line(tag);
    const size_t prefix = line.size();
    line.resize(prefix + static_cast<size_t>(count) + 1);
    std::vsnprintf(line.data() + prefix, static_cast<size_t>(count) + 1, fmt, ap);
    line.back() = '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

// Same line, but the message is formatted into a 512-byte buffer first (truncating),
// then written with one fwrite so stderr cannot split a concurrent SIMID record.
inline void vlogv512(const char* tag, const char* fmt, va_list ap) {
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    std::string line(tag);
    line += buf;
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

// The directory the SPIR-V modules load from, with a trailing separator (MC2_VULKAN_SPV_DIR; the row default
// shaders/vulkan also applies to an empty value).
inline std::string spvDir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

// Reads a .spv file into a VkShaderModule; returns VK_NULL_HANDLE (after logging the
// reason through `log`, when given) if the file is missing, not a multiple of 4 bytes,
// short-read, or rejected by the driver. `openFailureOnly` keeps the sky island's
// historical quiet path: only a missing file is reported.
inline VkShaderModule loadSpv(VkDevice device, std::string_view pathView, LogFn log,
                              bool openFailureOnly = false) {
    const LogFn later = openFailureOnly ? nullptr : log;
    const std::string path(pathView);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { if (log) log("cannot open spv '%s'.", path.c_str()); return VK_NULL_HANDLE; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) {
        if (later) later("spv '%s' bad size %ld.", path.c_str(), len);
        std::fclose(f);
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) {
        if (later) later("spv short read '%s'.", path.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = static_cast<size_t>(len);
    smci.pCode = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smci, nullptr, &mod) != VK_SUCCESS) {
        if (later) later("vkCreateShaderModule('%s') failed.", path.c_str());
        return VK_NULL_HANDLE;
    }
    mc2vk::infra::shader(mod, pathView, code);
    return mod;
}

// A submission-slot wait that times out skips the frame and retries; after kMaxWaitTimeouts in a row the device is
// presumed lost and the island disables itself (5 s per frame forever is worse than the fallback). Returns `ok`.
inline constexpr int kMaxWaitTimeouts = 3;
inline bool waitStrike(int& strikes, bool& disabled, bool ok, std::string_view island) {
    if (ok) { strikes = 0; return true; }
    if (++strikes >= kMaxWaitTimeouts) {
        disabled = true;
        std::fprintf(stderr, "[%.*s] LOUD: %d consecutive 5 s wait timeouts; island DISABLED for the session (device lost?)\n", static_cast<int>(island.size()), island.data(), strikes);
        std::fflush(stderr);
    }
    return false;
}

}  // namespace vk_island_util

#endif  // MC2_VULKAN_ISLAND
#endif  // MC2_VK_ISLAND_UTIL_H
