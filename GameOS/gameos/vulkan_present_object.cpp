// vulkan_present_object.cpp
//
// L6.6-COMPOSITE-PRESENT-SCAFFOLD -- implementation of the PERSISTENT Vulkan
// present object, extracted from the L5 throwaway probe
// (vulkan_swapchain_present.cpp). The per-image render-semaphore discipline and
// the OUT_OF_DATE/SUBOPTIMAL rebuild are copied VERBATIM (see SYNC NOTE on
// SwapchainResources in the header). The probe now DRIVES this object -- see
// mc2_vulkan_probe_swapchain_present() in vulkan_swapchain_present.cpp -- so the
// 16-frame present regression is the extraction's fidelity check.
//
// SCOPE: own SEPARATE Vulkan window; GL game window untouched. Whole TU inert
// unless MC2_VULKAN is defined -> GL shipping build64 byte-identical.

#include "vulkan_present_object.h"

#ifdef MC2_VULKAN

#include "vulkan_backend_skeleton.h"   // mc2_vulkan_load_spv / mc2_vulkan_free_shader

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc2vk {

namespace {

void po_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VULKAN_SKELETON] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// VULKAN-VALIDATION-PRESETS-1 (local copy, kept self-contained). Resolve
// MC2_VULKAN_VALIDATION into an enable/disable + optional feature set + name.
bool po_resolve_validation_preset(std::vector<VkValidationFeatureEnableEXT>& feats,
                                  const char*& resolvedName) {
    feats.clear();
    resolvedName = "off";
    const char* env = std::getenv("MC2_VULKAN_VALIDATION");
    if (env == nullptr) return false;
    auto eq = [&](const char* s) { return std::strcmp(env, s) == 0; };
    if (eq("0") || eq("off")) { resolvedName = "off"; return false; }
    if (eq("1") || eq("core") || eq("")) { resolvedName = "core"; return true; }
    if (eq("sync")) {
        resolvedName = "sync";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        return true;
    }
    if (eq("gpu-assisted")) {
        resolvedName = "gpu-assisted";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
        return true;
    }
    if (eq("best-practices")) {
        resolvedName = "best-practices";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
        return true;
    }
    if (eq("debug-printf")) {
        resolvedName = "debug-printf";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
        return true;
    }
    po_log("present-object: validation preset '%s' unrecognized; falling back to 'core'", env);
    resolvedName = "core";
    return true;
}

// Debug-messenger error counter. The present object is single-threaded, so a
// plain file-static counter is sufficient (mirrors the L5 probe).
uint32_t g_po_validation_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL po_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_po_validation_errors;
        po_log("present-object: VALIDATION: %s", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        po_log("present-object: validation-warning: %s", msg);
    }
    return VK_FALSE;
}

void destroy_swapchain_resources(VkDevice device, SwapchainResources& r) {
    for (VkSemaphore s : r.renderSems) {
        if (s) vkDestroySemaphore(device, s, nullptr);
    }
    for (VkFramebuffer fb : r.framebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    for (VkImageView v : r.views) {
        if (v) vkDestroyImageView(device, v, nullptr);
    }
    r.renderSems.clear();
    r.framebuffers.clear();
    r.views.clear();
    r.images.clear();
    if (r.have) {
        vkb::destroy_swapchain(r.swapchain);
        r.have = false;
    }
}

// Build (or rebuild) swapchain + image views + framebuffers for the current
// window/surface size. On failure returns false with `reason` set. `oldSwapchain`
// (may be VK_NULL_HANDLE) is passed to vkb so the driver can reuse resources
// across a resize. VERBATIM from the L5 probe.
bool build_swapchain_resources(VkPhysicalDevice phys, VkDevice device,
                               VkSurfaceKHR surface, uint32_t gfxFamily,
                               uint32_t presentFamily, VkRenderPass rpass,
                               uint32_t desiredW, uint32_t desiredH,
                               VkSwapchainKHR oldSwapchain,
                               SwapchainResources& out, std::string& reason) {
    vkb::SwapchainBuilder scb(phys, device, surface, gfxFamily, presentFamily);
    scb.set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_SRGB,
                                               VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
       .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
       .set_desired_extent(desiredW, desiredH)
       .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (oldSwapchain != VK_NULL_HANDLE) scb.set_old_swapchain(oldSwapchain);

    auto scRet = scb.build();
    if (!scRet) {
        reason = "SwapchainBuilder.build failed: " + scRet.error().message();
        po_log("present-object: %s (VkResult=%d). fail-soft.",
               reason.c_str(), (int)scRet.vk_result());
        return false;
    }
    out.swapchain = scRet.value();
    out.have = true;

    auto imgs = out.swapchain.get_images();
    auto imgViews = out.swapchain.get_image_views();
    if (!imgs || !imgViews) {
        reason = "swapchain get_images/get_image_views failed";
        po_log("present-object: %s. fail-soft.", reason.c_str());
        return false;
    }
    out.images = imgs.value();
    out.views  = imgViews.value();

    out.framebuffers.resize(out.views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < out.views.size(); ++i) {
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = rpass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &out.views[i];
        fci.width           = out.swapchain.extent.width;
        fci.height          = out.swapchain.extent.height;
        fci.layers          = 1;
        VkResult fr = vkCreateFramebuffer(device, &fci, nullptr, &out.framebuffers[i]);
        if (fr != VK_SUCCESS) {
            reason = "vkCreateFramebuffer failed (" + std::to_string((int)fr) + ")";
            po_log("present-object: %s. fail-soft.", reason.c_str());
            return false;
        }
    }

    // One render-finished semaphore per image (see SYNC NOTE on the struct).
    out.renderSems.resize(out.views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < out.renderSems.size(); ++i) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkResult sr = vkCreateSemaphore(device, &sci, nullptr, &out.renderSems[i]);
        if (sr != VK_SUCCESS) {
            reason = "vkCreateSemaphore(per-image) failed (" + std::to_string((int)sr) + ")";
            po_log("present-object: %s. fail-soft.", reason.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

bool VulkanPresentObject::init(const char* spvDir, bool hidden, uint32_t w, uint32_t h) {
    g_po_validation_errors = 0;
    health_ = PresentHealth{};

    // ---- volk (Vulkan runtime) --------------------------------------------
    if (volkInitialize() != VK_SUCCESS) {
        health_.fallback_reason = "volkInitialize failed -- no Vulkan runtime";
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }

    // ---- SDL video (visible Vulkan window) --------------------------------
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            health_.fallback_reason = std::string("SDL_InitSubSystem(VIDEO): ") + SDL_GetError();
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            return false;
        }
        sdlVideoInitedHere_ = true;
    }
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
        health_.fallback_reason = std::string("SDL_Vulkan_LoadLibrary: ") + SDL_GetError();
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        destroy();
        return false;
    }
    sdlVulkanLoaded_ = true;

    // Separate window from the GL window -- shares no GL context.
    Uint32 winFlags = SDL_WINDOW_VULKAN | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    window_ = SDL_CreateWindow(
        "MC2 Vulkan present object (L6.6 scaffold)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)w, (int)h, winFlags);
    if (!window_) {
        health_.fallback_reason = std::string("SDL_CreateWindow(VULKAN): ") + SDL_GetError();
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        destroy();
        return false;
    }
    po_log("present-object: created %s %ux%u Vulkan window (separate from GL window).",
           hidden ? "HIDDEN" : "SHOWN", w, h);

    // ---- Surface instance extensions --------------------------------------
    unsigned int extCount = 0;
    if (SDL_Vulkan_GetInstanceExtensions(window_, &extCount, nullptr) != SDL_TRUE) {
        health_.fallback_reason = std::string("GetInstanceExtensions(count): ") + SDL_GetError();
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        destroy();
        return false;
    }
    std::vector<const char*> instExts(extCount);
    if (extCount && SDL_Vulkan_GetInstanceExtensions(window_, &extCount, instExts.data()) != SDL_TRUE) {
        health_.fallback_reason = std::string("GetInstanceExtensions(names): ") + SDL_GetError();
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        destroy();
        return false;
    }

    // ---- Validation preset (optional) -------------------------------------
    std::vector<VkValidationFeatureEnableEXT> valFeatures;
    const char* valPreset = "off";
    wantValidation_ = po_resolve_validation_preset(valFeatures, valPreset);

    // ---- VkInstance via vk-bootstrap (shares volk's loader) ---------------
    vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
    ib.set_app_name("MC2 (Vulkan present object)")
      .set_engine_name("MC2-GameOS")
      .require_api_version(1, 1, 0);
    for (unsigned i = 0; i < extCount; ++i) ib.enable_extension(instExts[i]);
    if (wantValidation_) {
        ib.request_validation_layers(true);
        ib.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        for (auto feat : valFeatures) ib.add_validation_feature_enable(feat);
        po_log("present-object: validation ENABLED (preset=%s, +%zu feature(s))",
               valPreset, valFeatures.size());
    }

    auto instRet = ib.build();
    if (!instRet) {
        health_.fallback_reason = "InstanceBuilder.build failed: " + instRet.error().message();
        po_log("present-object: %s (VkResult=%d). fail-soft.",
               health_.fallback_reason.c_str(), (int)instRet.vk_result());
        destroy();
        return false;
    }
    vkbInstance_ = instRet.value();
    instance_ = vkbInstance_.instance;
    volkLoadInstance(instance_);

    if (wantValidation_) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT mci{};
            mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = po_debug_cb;
            pfnCreate(instance_, &mci, nullptr, &messenger_);
        } else {
            po_log("present-object: vkCreateDebugUtilsMessengerEXT not found; no error capture.");
        }
    }

    // ---- Surface (SDL) ----------------------------------------------------
    if (SDL_Vulkan_CreateSurface(window_, instance_, &surface_) != SDL_TRUE) {
        health_.fallback_reason = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        destroy();
        return false;
    }
    health_.surface_created = true;

    // ---- Bespoke physical-device pick: devs[0] with present+graphics ------
    {
        uint32_t devCount = 0;
        VkResult r = vkEnumeratePhysicalDevices(instance_, &devCount, nullptr);
        if (r != VK_SUCCESS || devCount == 0) {
            health_.fallback_reason = "no physical devices";
            po_log("present-object: %s (VkResult=%d count=%u). fail-soft.",
                   health_.fallback_reason.c_str(), (int)r, devCount);
            destroy();
            return false;
        }
        std::vector<VkPhysicalDevice> devs(devCount);
        vkEnumeratePhysicalDevices(instance_, &devCount, devs.data());
        phys_ = devs[0];
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_, &props);
        po_log("present-object: using physical device [0] %s", props.deviceName);

        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qfCount, qfs.data());
        for (uint32_t q = 0; q < qfCount; ++q) {
            if ((qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && gfxFamily_ == UINT32_MAX)
                gfxFamily_ = q;
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(phys_, q, surface_, &present) == VK_SUCCESS &&
                present == VK_TRUE && presentFamily_ == UINT32_MAX)
                presentFamily_ = q;
        }
        if (gfxFamily_ == UINT32_MAX || presentFamily_ == UINT32_MAX) {
            health_.fallback_reason = "no graphics/present queue family";
            po_log("present-object: %s (gfx=%u present=%u). fail-soft.",
                   health_.fallback_reason.c_str(), gfxFamily_, presentFamily_);
            destroy();
            return false;
        }
        po_log("present-object: queue families gfx=%u present=%u", gfxFamily_, presentFamily_);

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qcis[2]{};
        uint32_t qciCount = 0;
        qcis[qciCount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[qciCount].queueFamilyIndex = gfxFamily_;
        qcis[qciCount].queueCount       = 1;
        qcis[qciCount].pQueuePriorities = &prio;
        ++qciCount;
        if (presentFamily_ != gfxFamily_) {
            qcis[qciCount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qcis[qciCount].queueFamilyIndex = presentFamily_;
            qcis[qciCount].queueCount       = 1;
            qcis[qciCount].pQueuePriorities = &prio;
            ++qciCount;
        }
        const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo dci{};
        dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = qciCount;
        dci.pQueueCreateInfos       = qcis;
        dci.enabledExtensionCount   = 1;
        dci.ppEnabledExtensionNames = devExts;
        r = vkCreateDevice(phys_, &dci, nullptr, &device_);
        if (r != VK_SUCCESS) {
            health_.fallback_reason = "vkCreateDevice failed (" + std::to_string((int)r) + ")";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, gfxFamily_, 0, &gfxQueue_);
        vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
    }

    // ---- Shader modules (reuse fullscreen triangle) -----------------------
    {
        std::string dir = spvDir ? spvDir : "shaders/vulkan";
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        vert_ = mc2_vulkan_load_spv(device_, (dir + "fullscreen.vert.spv").c_str());
        frag_ = mc2_vulkan_load_spv(device_, (dir + "fullscreen.frag.spv").c_str());
        if (vert_ == VK_NULL_HANDLE || frag_ == VK_NULL_HANDLE) {
            health_.fallback_reason = "fullscreen shader module load failed";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
    }

    // ---- Render pass (1 color, CLEAR->STORE, -> PRESENT_SRC) --------------
    {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_B8G8R8A8_SRGB;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass      = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass      = 0;
        dep.srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask   = 0;
        dep.dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{};
        rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &att;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies   = &dep;
        VkResult r = vkCreateRenderPass(device_, &rpci, nullptr, &rpass_);
        if (r != VK_SUCCESS) {
            health_.fallback_reason = "vkCreateRenderPass failed (" + std::to_string((int)r) + ")";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
    }

    // ---- Graphics pipeline (dynamic viewport/scissor -> survives resize) --
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert_;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag_;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount  = 1;

        VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dynStates;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &cba;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VkResult r = vkCreatePipelineLayout(device_, &plci, nullptr, &playout_);
        if (r != VK_SUCCESS) {
            health_.fallback_reason = "vkCreatePipelineLayout failed (" + std::to_string((int)r) + ")";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount          = 2;
        gp.pStages             = stages;
        gp.pVertexInputState   = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState      = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState   = &ms;
        gp.pColorBlendState    = &cb;
        gp.pDynamicState       = &dyn;
        gp.layout              = playout_;
        gp.renderPass          = rpass_;
        gp.subpass             = 0;
        r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe_);
        if (r != VK_SUCCESS) {
            health_.fallback_reason = "vkCreateGraphicsPipelines failed (" + std::to_string((int)r) + ")";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
    }

    // ---- Initial swapchain + framebuffers ---------------------------------
    {
        std::string reason;
        if (!build_swapchain_resources(phys_, device_, surface_, gfxFamily_, presentFamily_,
                                       rpass_, w, h, VK_NULL_HANDLE, scr_, reason)) {
            health_.fallback_reason = reason;
            destroy();
            return false;
        }
        health_.swapchain_available = true;
        health_.image_count = scr_.swapchain.image_count;
        health_.format      = scr_.swapchain.image_format;
        health_.present_mode = scr_.swapchain.present_mode;
        health_.extent_w    = scr_.swapchain.extent.width;
        health_.extent_h    = scr_.swapchain.extent.height;
        po_log("present-object: swapchain OK image_count=%u extent=%ux%u",
               scr_.swapchain.image_count,
               scr_.swapchain.extent.width, scr_.swapchain.extent.height);
    }

    // ---- Command pool/buffer + sync primitives (1 frame in flight) --------
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = gfxFamily_;
        VkResult r = vkCreateCommandPool(device_, &pci, nullptr, &cpool_);
        if (r != VK_SUCCESS) {
            health_.fallback_reason = "vkCreateCommandPool failed (" + std::to_string((int)r) + ")";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = cpool_;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(device_, &cai, &cbuf_);
        if (r != VK_SUCCESS) {
            health_.fallback_reason = "vkAllocateCommandBuffers failed (" + std::to_string((int)r) + ")";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &sci, nullptr, &acquireSem_) != VK_SUCCESS) {
            health_.fallback_reason = "vkCreateSemaphore(acquire) failed";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so frame 0 doesn't block forever
        if (vkCreateFence(device_, &fci, nullptr, &inFlight_) != VK_SUCCESS) {
            health_.fallback_reason = "vkCreateFence failed";
            po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
            destroy();
            return false;
        }
    }

    health_.validation_errors = g_po_validation_errors;
    return true;
}

bool VulkanPresentObject::rebuildSwapchain(uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(device_);
    VkSwapchainKHR old = scr_.swapchain.swapchain;
    SwapchainResources fresh;
    std::string reason;
    bool built = build_swapchain_resources(phys_, device_, surface_, gfxFamily_,
                                           presentFamily_, rpass_,
                                           (w > 0 ? w : 1), (h > 0 ? h : 1),
                                           old, fresh, reason);
    destroy_swapchain_resources(device_, scr_); // frees old (incl. old handle)
    scr_ = std::move(fresh);
    if (!built) { health_.fallback_reason = reason; return false; }
    ++health_.resize_count;
    health_.image_count = scr_.swapchain.image_count;
    health_.extent_w = scr_.swapchain.extent.width;
    health_.extent_h = scr_.swapchain.extent.height;
    return true;
}

bool VulkanPresentObject::handleResize(uint32_t w, uint32_t h) {
    if (device_ == VK_NULL_HANDLE) return false;
    return rebuildSwapchain(w, h);
}

bool VulkanPresentObject::acquireAndPresentOneFrame() {
    if (device_ == VK_NULL_HANDLE || !scr_.have) {
        health_.fallback_reason = "present-object not initialized";
        return false;
    }

    // Pump SDL events so the window stays responsive.
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { /* drain; do not honor quit */ }

    // Wait for the previous frame's work, then reset the fence.
    VkResult r = vkWaitForFences(device_, 1, &inFlight_, VK_TRUE, UINT64_MAX);
    if (r == VK_ERROR_DEVICE_LOST) {
        health_.fallback_reason = "VK_ERROR_DEVICE_LOST at fence wait";
        return false;
    }
    vkResetFences(device_, 1, &inFlight_);

    // Acquire.
    uint32_t imageIndex = 0;
    r = vkAcquireNextImageKHR(device_, scr_.swapchain.swapchain, UINT64_MAX,
                              acquireSem_, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        po_log("present-object: acquire OUT_OF_DATE -- recreating swapchain.");
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window_, &w, &h);
        if (!rebuildSwapchain((uint32_t)w, (uint32_t)h)) return false;
        // The fence is currently unsignaled (we reset it). Re-signal via an empty
        // submit so next frame's wait does not deadlock -- VERBATIM from L5.
        VkSubmitInfo empty{};
        empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        vkQueueSubmit(gfxQueue_, 1, &empty, inFlight_);
        return true; // retry acquire next call
    } else if (r == VK_ERROR_DEVICE_LOST) {
        health_.fallback_reason = "VK_ERROR_DEVICE_LOST at acquire";
        return false;
    } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        health_.fallback_reason = "vkAcquireNextImageKHR failed (" + std::to_string((int)r) + ")";
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }

    // Record: begin render pass (clear to teal), draw fullscreen tri.
    vkResetCommandBuffer(cbuf_, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cbuf_, &bi);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.5f, 0.5f, 1.0f}}; // distinct teal
    VkRenderPassBeginInfo rbi{};
    rbi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass      = rpass_;
    rbi.framebuffer     = scr_.framebuffers[imageIndex];
    rbi.renderArea      = {{0, 0}, scr_.swapchain.extent};
    rbi.clearValueCount = 1;
    rbi.pClearValues    = &clear;
    vkCmdBeginRenderPass(cbuf_, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)scr_.swapchain.extent.width,
                  (float)scr_.swapchain.extent.height, 0.0f, 1.0f};
    VkRect2D   sc{{0, 0}, scr_.swapchain.extent};
    vkCmdSetViewport(cbuf_, 0, 1, &vp);
    vkCmdSetScissor(cbuf_, 0, 1, &sc);
    vkCmdBindPipeline(cbuf_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
    vkCmdDraw(cbuf_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cbuf_);
    vkEndCommandBuffer(cbuf_);

    // Submit: wait acquire sem at COLOR_ATTACHMENT_OUTPUT, signal the PER-IMAGE
    // render sem (indexed by the acquired image -- see SYNC NOTE).
    VkSemaphore imgRenderSem = scr_.renderSems[imageIndex];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &acquireSem_;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cbuf_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &imgRenderSem;
    r = vkQueueSubmit(gfxQueue_, 1, &si, inFlight_);
    if (r == VK_ERROR_DEVICE_LOST) {
        health_.fallback_reason = "VK_ERROR_DEVICE_LOST at submit";
        return false;
    }
    if (r != VK_SUCCESS) {
        health_.fallback_reason = "vkQueueSubmit failed (" + std::to_string((int)r) + ")";
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }

    // Present: wait render sem.
    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &imgRenderSem;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &scr_.swapchain.swapchain;
    pi.pImageIndices      = &imageIndex;
    r = vkQueuePresentKHR(presentQueue_, &pi);
    if (r == VK_SUCCESS) {
        ++health_.present_frames;
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        // Present still handed the image over; count it, then recreate.
        ++health_.present_frames;
        po_log("present-object: present %s -- recreating swapchain.",
               r == VK_SUBOPTIMAL_KHR ? "SUBOPTIMAL" : "OUT_OF_DATE");
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window_, &w, &h);
        if (!rebuildSwapchain((uint32_t)w, (uint32_t)h)) return false;
    } else if (r == VK_ERROR_DEVICE_LOST) {
        health_.fallback_reason = "VK_ERROR_DEVICE_LOST at present";
        return false;
    } else {
        health_.fallback_reason = "vkQueuePresentKHR failed (" + std::to_string((int)r) + ")";
        po_log("present-object: %s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }

    health_.validation_errors = g_po_validation_errors;
    return true;
}

void VulkanPresentObject::destroy() {
    if (device_) vkDeviceWaitIdle(device_);
    if (inFlight_)   { vkDestroyFence(device_, inFlight_, nullptr); inFlight_ = VK_NULL_HANDLE; }
    if (acquireSem_) { vkDestroySemaphore(device_, acquireSem_, nullptr); acquireSem_ = VK_NULL_HANDLE; }
    if (cpool_)      { vkDestroyCommandPool(device_, cpool_, nullptr); cpool_ = VK_NULL_HANDLE; cbuf_ = VK_NULL_HANDLE; }
    if (device_)     destroy_swapchain_resources(device_, scr_);
    if (pipe_)    { vkDestroyPipeline(device_, pipe_, nullptr); pipe_ = VK_NULL_HANDLE; }
    if (playout_) { vkDestroyPipelineLayout(device_, playout_, nullptr); playout_ = VK_NULL_HANDLE; }
    if (rpass_)   { vkDestroyRenderPass(device_, rpass_, nullptr); rpass_ = VK_NULL_HANDLE; }
    if (vert_)    { mc2_vulkan_free_shader(device_, vert_); vert_ = VK_NULL_HANDLE; }
    if (frag_)    { mc2_vulkan_free_shader(device_, frag_); frag_ = VK_NULL_HANDLE; }
    if (device_)  { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (messenger_) {
        auto d = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (d) d(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_) { vkb::destroy_instance(vkbInstance_); instance_ = VK_NULL_HANDLE; }
    if (window_)   { SDL_DestroyWindow(window_); window_ = nullptr; }
    if (sdlVulkanLoaded_)    { SDL_Vulkan_UnloadLibrary(); sdlVulkanLoaded_ = false; }
    if (sdlVideoInitedHere_) { SDL_QuitSubSystem(SDL_INIT_VIDEO); sdlVideoInitedHere_ = false; }

    gfxQueue_ = presentQueue_ = VK_NULL_HANDLE;
    gfxFamily_ = presentFamily_ = UINT32_MAX;
    phys_ = VK_NULL_HANDLE;

    health_.validation_errors = g_po_validation_errors;
}

uint32_t vulkan_present_object_validation_errors() { return g_po_validation_errors; }

}  // namespace mc2vk

#endif // MC2_VULKAN
