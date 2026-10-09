#include "vulkan_spv_loader.h"

#ifdef MC2_VULKAN

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VULKAN_SKELETON] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

} // namespace

VkShaderModule mc2_vulkan_load_spv(VkDevice device, const char* path) {
    if (device == VK_NULL_HANDLE || !path) {
        log("load_spv: null device/path. fail-soft.");
        return VK_NULL_HANDLE;
    }
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        log("load_spv: cannot open '%s'. fail-soft.", path);
        return VK_NULL_HANDLE;
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) {
        log("load_spv: '%s' bad size %ld (must be >0 and 4-byte multiple). fail-soft.", path, len);
        std::fclose(f);
        return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) {
        log("load_spv: short read on '%s' (%zu/%ld). fail-soft.", path, got, len);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = static_cast<size_t>(len);
    smci.pCode    = code.data();

    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device, &smci, nullptr, &mod);
    if (r != VK_SUCCESS) {
        log("load_spv: vkCreateShaderModule('%s') failed (VkResult=%d). fail-soft.", path, static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    log("load_spv: '%s' -> VkShaderModule (%ld bytes)", path, len);
    return mod;
}

void mc2_vulkan_free_shader(VkDevice device, VkShaderModule module) {
    if (device != VK_NULL_HANDLE && module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
}

#endif // MC2_VULKAN
