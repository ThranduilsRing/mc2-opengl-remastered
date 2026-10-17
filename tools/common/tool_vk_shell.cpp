// tool_vk_shell -- see tool_vk_shell.h.
#include "tool_vk_shell.h"

#include <SDL.h>
#include <SDL_vulkan.h>
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace toolvk {

static std::atomic<uint32_t> g_valErrors{0};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_valErrors;
        std::fprintf(stderr, "[TOOLVK] VALIDATION ERROR: %s\n", d->pMessage);
    }
    return VK_FALSE;
}

uint32_t validationErrors() { return g_valErrors.load(); }

bool captureRequested() {
    const char* c = std::getenv("MC2_TOOL_CAPTURE");
    return c && c[0];
}

#define VKOK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "[TOOLVK] %s failed: %d\n", #call, (int)r_); return false; } } while (0)

static bool createSwapchain(Shell& s) {
    VkSurfaceCapabilitiesKHR caps{};
    VKOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface, &caps));
    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(s.window, &dw, &dh);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = std::clamp((uint32_t)dw, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp((uint32_t)dh, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (ext.width == 0 || ext.height == 0) return false;  // minimized

    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &nf, fmts.data());
    // UNORM, not SRGB: the GL ImGui backend wrote into a non-sRGB framebuffer, so this keeps the tools' colors identical.
    VkSurfaceFormatKHR pick = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { pick = f; break; }
    s.format = pick.format;
    s.extent = ext;

    uint32_t want = std::max<uint32_t>(caps.minImageCount, 2);
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = s.surface;
    ci.minImageCount = want;
    ci.imageFormat = pick.format;
    ci.imageColorSpace = pick.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s.swap;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VKOK(vkCreateSwapchainKHR(s.dev, &ci, nullptr, &fresh));
    vkDeviceWaitIdle(s.dev);
    for (auto fb : s.fbs) vkDestroyFramebuffer(s.dev, fb, nullptr);
    for (auto v : s.views) vkDestroyImageView(s.dev, v, nullptr);
    for (auto sem : s.renderDone) vkDestroySemaphore(s.dev, sem, nullptr);
    s.fbs.clear(); s.views.clear(); s.renderDone.clear();
    if (s.swap) vkDestroySwapchainKHR(s.dev, s.swap, nullptr);
    s.swap = fresh;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(s.dev, s.swap, &n, nullptr);
    s.images.resize(n);
    vkGetSwapchainImagesKHR(s.dev, s.swap, &n, s.images.data());

    if (!s.pass) {
        VkAttachmentDescription a{};
        a.format = s.format;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &a;
        rp.subpassCount = 1; rp.pSubpasses = &sp;
        rp.dependencyCount = 1; rp.pDependencies = &dep;
        VKOK(vkCreateRenderPass(s.dev, &rp, nullptr, &s.pass));
    }
    s.views.resize(n); s.fbs.resize(n); s.renderDone.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKOK(vkCreateImageView(s.dev, &vi, nullptr, &s.views[i]));
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = s.pass; fi.attachmentCount = 1; fi.pAttachments = &s.views[i];
        fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
        VKOK(vkCreateFramebuffer(s.dev, &fi, nullptr, &s.fbs[i]));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKOK(vkCreateSemaphore(s.dev, &si, nullptr, &s.renderDone[i]));
    }
    if (s.imguiInit) ImGui_ImplVulkan_SetMinImageCount(2);
    s.needRebuild = false;
    return true;
}

bool init(Shell& s, SDL_Window* window, const char* appName) {
    s.window = window;
    if (volkInitialize() != VK_SUCCESS) { std::fprintf(stderr, "[TOOLVK] no Vulkan loader\n"); return false; }
    if (const char* c = std::getenv("MC2_TOOL_CAPTURE")) s.capturePath = c;
    if (const char* c = std::getenv("MC2_TOOL_CAPTURE_FRAME")) s.captureFrame = std::max(1, std::atoi(c));
    const char* v = std::getenv("MC2_VULKAN_VALIDATION");
    s.validation = v && v[0] == '1';

    unsigned extN = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &extN, nullptr);
    std::vector<const char*> exts(extN);
    SDL_Vulkan_GetInstanceExtensions(window, &extN, exts.data());
    std::vector<const char*> layers;
    if (s.validation) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> lp(ln);
        vkEnumerateInstanceLayerProperties(&ln, lp.data());
        bool have = false;
        for (auto& p : lp) if (!std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation")) have = true;
        if (have) { layers.push_back("VK_LAYER_KHRONOS_validation"); exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }
        else { std::fprintf(stderr, "[TOOLVK] validation requested but the layer is missing\n"); s.validation = false; }
    }
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = appName;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)exts.size(); ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size(); ici.ppEnabledLayerNames = layers.data();
    VKOK(vkCreateInstance(&ici, nullptr, &s.inst));
    volkLoadInstance(s.inst);
    if (s.validation) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mi.pfnUserCallback = debugCb;
        vkCreateDebugUtilsMessengerEXT(s.inst, &mi, nullptr, &s.messenger);
    }
    if (!SDL_Vulkan_CreateSurface(window, s.inst, &s.surface)) { std::fprintf(stderr, "[TOOLVK] surface: %s\n", SDL_GetError()); return false; }

    uint32_t pn = 0;
    vkEnumeratePhysicalDevices(s.inst, &pn, nullptr);
    std::vector<VkPhysicalDevice> pds(pn);
    vkEnumeratePhysicalDevices(s.inst, &pn, pds.data());
    for (auto pd : pds) {  // first device with a graphics+present family; prefer discrete
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 pres = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s.surface, &pres);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) {
                VkPhysicalDeviceProperties pp;
                vkGetPhysicalDeviceProperties(pd, &pp);
                if (!s.phys || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { s.phys = pd; s.family = i; }
                break;
            }
        }
    }
    if (!s.phys) { std::fprintf(stderr, "[TOOLVK] no suitable GPU\n"); return false; }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(s.phys, &pp);
    std::printf("[TOOLVK] device=%s api=%u.%u.%u validation=%d\n", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion),
                VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion), s.validation ? 1 : 0);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = s.family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures feats{};
    VkPhysicalDeviceFeatures avail{};
    vkGetPhysicalDeviceFeatures(s.phys, &avail);
    feats.samplerAnisotropy = avail.samplerAnisotropy;
    feats.fillModeNonSolid = avail.fillModeNonSolid;
    feats.textureCompressionBC = avail.textureCompressionBC;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExt;
    dci.pEnabledFeatures = &feats;
    VKOK(vkCreateDevice(s.phys, &dci, nullptr, &s.dev));
    volkLoadDevice(s.dev);
    vkGetDeviceQueue(s.dev, s.family, 0, &s.queue);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = s.family;
    VKOK(vkCreateCommandPool(s.dev, &cpi, nullptr, &s.pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VKOK(vkAllocateCommandBuffers(s.dev, &cai, &s.cb));
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKOK(vkCreateSemaphore(s.dev, &si, nullptr, &s.acquired));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKOK(vkCreateFence(s.dev, &fi, nullptr, &s.fence));

    if (!createSwapchain(s)) return false;

    ImGui_ImplSDL2_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = s.inst; info.PhysicalDevice = s.phys; info.Device = s.dev;
    info.QueueFamily = s.family; info.Queue = s.queue;
    info.RenderPass = s.pass;
    info.MinImageCount = 2;
    info.ImageCount = (uint32_t)s.images.size();
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.DescriptorPoolSize = 1024;
    if (!ImGui_ImplVulkan_Init(&info)) { std::fprintf(stderr, "[TOOLVK] ImGui_ImplVulkan_Init failed\n"); return false; }
    s.imguiInit = true;
    return true;
}

bool begin(Shell& s) {
    if (s.needRebuild && !createSwapchain(s)) return false;
    vkWaitForFences(s.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
    VkResult r = vkAcquireNextImageKHR(s.dev, s.swap, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &s.imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { s.needRebuild = true; return false; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    vkResetFences(s.dev, 1, &s.fence);
    vkResetCommandBuffer(s.cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cb, &bi);
    s.frameOpen = true;
    return true;
}

bool end(Shell& s) {
    if (!s.frameOpen) return false;
    VkClearValue cv{};
    std::memcpy(cv.color.float32, s.clear, sizeof cv.color.float32);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = s.pass; rb.framebuffer = s.fbs[s.imageIndex];
    rb.renderArea = {{0, 0}, s.extent};
    rb.clearValueCount = 1; rb.pClearValues = &cv;
    vkCmdBeginRenderPass(s.cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    if (ImDrawData* dd = ImGui::GetDrawData()) ImGui_ImplVulkan_RenderDrawData(dd, s.cb);
    vkCmdEndRenderPass(s.cb);

    ++s.frameNo;
    const bool capture = !s.capturePath.empty() && s.frameNo >= s.captureFrame;
    VkBuffer rbBuf = VK_NULL_HANDLE;
    VkDeviceMemory rbMem = VK_NULL_HANDLE;
    VkDeviceSize bytes = (VkDeviceSize)s.extent.width * s.extent.height * 4;
    if (capture) {
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = bytes; bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(s.dev, &bc, nullptr, &rbBuf);
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(s.dev, rbBuf, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
        const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t mt = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) { mt = i; break; }
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size; ma.memoryTypeIndex = mt;
        vkAllocateMemory(s.dev, &ma, nullptr, &rbMem);
        vkBindBufferMemory(s.dev, rbBuf, rbMem, 0);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s.images[s.imageIndex];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {s.extent.width, s.extent.height, 1};
        vkCmdCopyImageToBuffer(s.cb, s.images[s.imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbBuf, 1, &cp);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(s.cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &s.acquired; sub.pWaitDstStageMask = &wait;
    sub.commandBufferCount = 1; sub.pCommandBuffers = &s.cb;
    sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &s.renderDone[s.imageIndex];
    vkQueueSubmit(s.queue, 1, &sub, s.fence);
    s.frameOpen = false;

    bool quit = false;
    if (capture) {
        void* mapped = nullptr;
        vkWaitForFences(s.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
        vkMapMemory(s.dev, rbMem, 0, bytes, 0, &mapped);
        bool ok = writePng(s.capturePath.c_str(), (const uint8_t*)mapped, s.extent.width, s.extent.height,
                           s.format == VK_FORMAT_B8G8R8A8_UNORM || s.format == VK_FORMAT_B8G8R8A8_SRGB);
        vkUnmapMemory(s.dev, rbMem);
        vkDestroyBuffer(s.dev, rbBuf, nullptr);
        vkFreeMemory(s.dev, rbMem, nullptr);
        std::printf("[TOOLVK] capture %s %ux%u -> %s\n", ok ? "OK" : "FAILED", s.extent.width, s.extent.height, s.capturePath.c_str());
        s.capturePath.clear();
        quit = true;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s.renderDone[s.imageIndex];
    pi.swapchainCount = 1; pi.pSwapchains = &s.swap; pi.pImageIndices = &s.imageIndex;
    VkResult r = vkQueuePresentKHR(s.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) s.needRebuild = true;
    return quit;
}

void shutdown(Shell& s) {
    if (s.dev) vkDeviceWaitIdle(s.dev);
    if (s.imguiInit) { ImGui_ImplVulkan_Shutdown(); s.imguiInit = false; }
    if (s.dev) {
        for (auto fb : s.fbs) vkDestroyFramebuffer(s.dev, fb, nullptr);
        for (auto v : s.views) vkDestroyImageView(s.dev, v, nullptr);
        for (auto sem : s.renderDone) vkDestroySemaphore(s.dev, sem, nullptr);
        if (s.swap) vkDestroySwapchainKHR(s.dev, s.swap, nullptr);
        if (s.pass) vkDestroyRenderPass(s.dev, s.pass, nullptr);
        if (s.acquired) vkDestroySemaphore(s.dev, s.acquired, nullptr);
        if (s.fence) vkDestroyFence(s.dev, s.fence, nullptr);
        if (s.pool) vkDestroyCommandPool(s.dev, s.pool, nullptr);
        vkDestroyDevice(s.dev, nullptr);
    }
    if (s.surface) vkDestroySurfaceKHR(s.inst, s.surface, nullptr);
    if (s.messenger) vkDestroyDebugUtilsMessengerEXT(s.inst, s.messenger, nullptr);
    if (s.inst) vkDestroyInstance(s.inst, nullptr);
    if (s.validation) std::printf("[TOOLVK] validation errors=%u\n", g_valErrors.load());
    s = Shell{};
}

// ---- PNG writer (stored deflate blocks; no zlib dependency) ----
static uint32_t crcTable[256];
static void crcInit() {
    static bool done = false;
    if (done) return;
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
    done = true;
}
static uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) crc = crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static void putBE(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((uint8_t)(x >> (i * 8))); }
static void chunk(FILE* f, const char* tag, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hdr;
    putBE(hdr, (uint32_t)data.size());
    std::fwrite(hdr.data(), 1, 4, f);
    std::fwrite(tag, 1, 4, f);
    uint32_t crc = crcUpdate(0xFFFFFFFFu, (const uint8_t*)tag, 4);
    if (!data.empty()) { std::fwrite(data.data(), 1, data.size(), f); crc = crcUpdate(crc, data.data(), data.size()); }
    std::vector<uint8_t> tail;
    putBE(tail, crc ^ 0xFFFFFFFFu);
    std::fwrite(tail.data(), 1, 4, f);
}

bool writePng(const char* path, const uint8_t* px, uint32_t w, uint32_t h, bool bgra) {
    crcInit();
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = px + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            raw.push_back(row[x * 4 + (bgra ? 2 : 0)]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + (bgra ? 0 : 2)]);
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size() ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    putBE(z, (b << 16) | a);
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    putBE(ihdr, w); putBE(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    std::fclose(f);
    return true;
}

} // namespace toolvk
