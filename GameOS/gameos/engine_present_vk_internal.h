// Shared surface of the EnginePresentVk translation units (engine_present_vk*.cpp).
//
// engine_present_vk.cpp keeps the swapchain, composite, HUD, readback and submit path; engine_present_vk_imgui.cpp records the
// GuiRuntime (ImGui) draw data into the present tail. Both see the same Impl; nothing outside the pair includes this header.
#ifndef ENGINE_PRESENT_VK_INTERNAL_H
#define ENGINE_PRESENT_VK_INTERNAL_H
#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)

#include "../../RenderCore/TextureService.h"
#include "engine_present_vk.h"
#include "vulkan_context.h"
#include "vk_island_util.h"
#include "vk_image_resource.h"
#include "vulkan_spatial_aa.h"
#include "present_imgui_snapshot.h"
#include "imgui.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mc2vk {

struct EnginePresentVk::Impl {
    VulkanContext*   ctx   = nullptr;
    VkInstance       inst  = VK_NULL_HANDLE;
    VkPhysicalDevice phys  = VK_NULL_HANDLE;
    VkDevice         dev   = VK_NULL_HANDLE;
    VkQueue          gfxQ  = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;

    mc2vk::OwnedSurface surface;   // released with the instance it was made on (the engine context's), before the swapchain below
    uint32_t         presentFamily = UINT32_MAX;
    bool offscreen = false;   // Linux: the window surface is not presentable from this GPU; composite and capture readback run, nothing is presented
    VkQueue          presentQ  = VK_NULL_HANDLE;

    // swapchain
    mc2vk::OwnedSwapchain    swapchain;
    VkFormat                 scFormat  = VK_FORMAT_UNDEFINED;
    VkExtent2D               scExtent  = {0, 0};
    VkSurfaceTransformFlagBitsKHR scTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;   // preTransform the chain was built with
    VkPresentModeKHR         presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     scImages;
    std::vector<mc2vk::OwnedSemaphore> renderSems;   // one per image (present-wait target)

    // per-frame sync: one acquire semaphore, one in-flight fence and one pair of command buffers per frame slot. slot() = engine frame % slotCount;
    // slotCount defaults to kMaxFramesInFlight; MC2_VK_FRAMES_IN_FLIGHT=1 pins every frame to slot 0.
    mc2vk::OwnedCommandPool   cpool;   // readback command buffer only
    mc2vk::OwnedCommandPool   tailPool[mc2vk::kMaxFramesInFlight];        // per-slot pools: vkResetCommandPool instead of per-cb reset
    mc2vk::OwnedCommandPool   compositeCmdPool[mc2vk::kMaxFramesInFlight];   // (tail and composite are recorded at different points of a frame)
    VkCommandBuffer cbufs[mc2vk::kMaxFramesInFlight] = {};   // present tail: overlay + HUD/ImGui + blit
    mc2vk::OwnedSemaphore     acquireSems[mc2vk::kMaxFramesInFlight];
    mc2vk::OwnedFence         inFlights[mc2vk::kMaxFramesInFlight];
    uint32_t slotCount = 1;
    uint32_t slot() const { return ctx->frameContext().slot; }
    VkCommandBuffer&       curCbuf()          { return cbufs[slot()]; }
    VkCommandBuffer&       curCompositeCbuf() { return compositeCbufs[slot()]; }
    mc2vk::OwnedSemaphore& curAcquireSem()    { return acquireSems[slot()]; }
    mc2vk::OwnedFence&     curInFlight()      { return inFlights[slot()]; }
    // Composite and tail share one submission. Resize drops a pending recording;
    // declined presents keep the fence signaled until a submit can signal it.
    VkCommandBuffer compositeCbufs[mc2vk::kMaxFramesInFlight] = {};
    bool            compositePending = false;
    bool frameFinished = false;
    uint32_t captureFrame = 0, captureFireFrame = 0;
    VkResult lastWsiResult = VK_SUCCESS;
    // Shadow debug joins the tail after the composite.
    VkImageView     pendingShadowDebugView = VK_NULL_HANDLE;
    int             pendingShadowDebugLayer = 0;

    // Composite output, blitted into the swapchain after the UI.
    VkImageResource sharedImg{};
    VkImageResource scaledComposite{};
    mc2vk::OwnedFramebuffer scaledCompositeFramebuffer;
    VkDescriptorSet upscaleSet = VK_NULL_HANDLE;
    SpatialAA spatialAA;
    uint32_t        imgW = 0, imgH = 0;

    // Asynchronous readback; the copy buffer is reused only after completion.
    mc2vk::ReadbackToken readbackToken;
    uint64_t        readbackTag = 0;
    uint32_t        readbackW = 0, readbackH = 0;
    VkCommandBuffer readbackCbuf = VK_NULL_HANDLE;

    // GuiRuntime draw data is recorded in the native present tail (engine_present_vk_imgui.cpp).
    bool            imguiVkInited = false;
    bool            imguiVkFailed = false;
    mc2vk::OwnedRenderPass    imguiRenderPass;
    mc2vk::OwnedFramebuffer   imguiFramebuffer;
    ImDrawData*     pendingImguiDrawData = nullptr;
    PresentImguiSnapshot ownedImgui;
    void clearImgui() {
        ownedImgui.clear();
        pendingImguiDrawData = nullptr;
    }
    // GL texture name (the ImTextureID the game passes) -> a Vulkan image and the descriptor set ImGui_ImplVulkan binds for it.
    // Members release in reverse order: view, image, memory. The upload's staging bytes live in the frame ring.
    struct ImguiTexEntry {
        RenderCore::TextureHandle serviceTexture; // descriptor cache borrows identity; GPU retirement retains its image
        VkImageView serviceView=VK_NULL_HANDLE;
        std::shared_ptr<const void> serviceImage; // owns this exact view until the retired set is freed
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t lastUsedFrame = 0;
    };
    std::unordered_map<uint64_t, ImguiTexEntry> imguiTex;
    mc2vk::OwnedSampler imguiTexSampler;
    uint64_t imguiTexFrame = 0;
    // HUD vertices live in the frame ring; image versions retire by GPU ticket.
    std::vector<VkHudDraw> pendingHudDraws;
    // HUD coordinates use the authoring canvas, which can differ from drawable
    // size under DPI scaling. Zero falls back to the present extent.
    uint32_t        hudCanvasW = 0, hudCanvasH = 0;
    bool            hudColorVkFailed = false;
    mc2vk::OwnedRenderPass    hudColorRenderPass;
    mc2vk::OwnedFramebuffer   hudColorFramebuffer;
    mc2vk::OwnedPipelineLayout hudColorLayout;
    mc2vk::OwnedPipeline      hudColorTriPipe;
    mc2vk::OwnedPipeline      hudColorTriOneInvAlphaPipe; // VK-FIELD-VFX-3: additive untextured effect ribbons
    mc2vk::OwnedPipeline      hudColorTriOneOnePipe;      // VK-FIELD-VFX-7: pure-additive (ONE,ONE) effect ribbons
    mc2vk::OwnedPipeline      hudColorLinePipe;
    mc2vk::OwnedPipeline      hudColorTriOpaquePipe;
    mc2vk::OwnedPipeline      hudColorLineOpaquePipe;
    mc2vk::OwnedShaderModule  hudColorVert;
    mc2vk::OwnedShaderModule  hudColorFrag;
    mc2vk::OwnedDescriptorSetLayout hudTexDsl;
    mc2vk::OwnedDescriptorPool hudTexPool;
    mc2vk::OwnedPipelineLayout hudTexLayout;
    mc2vk::OwnedPipeline      hudTexPipe;
    mc2vk::OwnedPipeline      hudTexOpaquePipe;
    mc2vk::OwnedPipeline      hudTexOneInvAlphaPipe; // VK-FIELD-VFX-3: GL OneInvAlpha (ONE,1-srcA) effect glow
    mc2vk::OwnedPipeline      hudTexOneOnePipe;      // VK-FIELD-VFX-7: GL OneOne (ONE,ONE) muzzle-flash/tracer sprites
    mc2vk::OwnedShaderModule  hudTexVert;
    mc2vk::OwnedShaderModule  hudTexFrag;
    struct HudTextureMirror {
        uint32_t serial = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        mc2vk::OwnedDeviceMemory memory;   // declared first, released last: the buffer bound to it goes first
        mc2vk::OwnedRawBuffer buffer;
        VkDeviceSize bytes = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t lastFrame = 0;      // VK-HUD-GLYPH-CORRUPTION: eviction watermark
    };
    // VK-HUD-GLYPH-CORRUPTION FIX: keyed by (textureHandle<<32 | textureSerial), NOT
    // handle alone. The game re-bakes many distinct runtime strings into ONE reused GL
    // texture handle within a single HUD frame (e.g. handle 275 cycles serials 2..9),
    // bumping the content serial each bake. The old handle-only mirror destroyed +
    // re-uploaded its single buffer in place, so every draw referencing that handle
    // ended up sampling the LAST bake -> earlier strings rendered the wrong glyphs
    // (OBJECTIVES/QUICK SAVE/pilot names garbled while pre-baked serial==1 labels stayed
    // clean). A per-(handle,serial) mirror gives each distinct bake its own persistent
    // buffer+descriptor for the frame. Entries are evicted a few frames after last use.
    std::unordered_map<uint64_t, HudTextureMirror> hudTextureMirrors;
    uint64_t hudMirrorFrameCounter = 0;

    bool            compositeVkFailed = false;
    mc2vk::OwnedRenderPass    compositeRenderPass;
    mc2vk::OwnedFramebuffer   compositeFramebuffer;
    mc2vk::OwnedSampler       compositeSampler;
    mc2vk::OwnedSampler       compositeNearestSampler;   // the R32_UINT object-id image cannot be filtered linearly
    mc2vk::OwnedDescriptorSetLayout compositeDsl;
    mc2vk::OwnedDescriptorPool compositePool;
    VkDescriptorSet  compositeSet = VK_NULL_HANDLE;
    mc2vk::OwnedPipelineLayout compositeLayout;
    mc2vk::OwnedPipeline       compositePipe;
    mc2vk::OwnedShaderModule   compositeVert;
    mc2vk::OwnedShaderModule   compositeFrag;

    bool             shadowDebugVkFailed = false;
    mc2vk::OwnedRenderPass     shadowDebugRenderPass;
    mc2vk::OwnedFramebuffer    shadowDebugFramebuffer;
    mc2vk::OwnedSampler        shadowDebugSampler;
    mc2vk::OwnedDescriptorSetLayout shadowDebugDsl;
    mc2vk::OwnedDescriptorPool shadowDebugPool;
    VkDescriptorSet  shadowDebugSet = VK_NULL_HANDLE;
    mc2vk::OwnedPipelineLayout shadowDebugLayout;
    mc2vk::OwnedPipeline       shadowDebugPipe;
    mc2vk::OwnedShaderModule   shadowDebugVert;
    mc2vk::OwnedShaderModule   shadowDebugFrag;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    // The shared present image holds raw allocations that vkImageResourceDestroy releases (and counts); the framebuffers over its view go first. Every
    // other member is an owner and follows in reverse declaration order.
    ~Impl() {
        spatialAA.reset();
        scaledCompositeFramebuffer.reset();
        if (dev != VK_NULL_HANDLE) vkImageResourceDestroy(&scaledComposite, dev);
        imguiFramebuffer.reset();
        hudColorFramebuffer.reset();
        compositeFramebuffer.reset();
        shadowDebugFramebuffer.reset();
        if (dev != VK_NULL_HANDLE) vkImageResourceDestroy(&sharedImg, dev);
    }
};

// LOAD_OP_LOAD passes on the shared present image: no layout transition, so no implicit external dependency;
// chain against the previous pass (and any later transfer/sample) explicitly.
inline void fillLoadPassDeps(VkSubpassDependency (&deps)[2]) {
    deps[0] = {}; deps[1] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
}

// engine_present_vk_imgui.cpp: the ImGui backend over the shared present image and the per-texture descriptor bridge.
bool ensureImguiVulkan(EnginePresentVk::Impl& s);
void imguiTexReleaseAll(EnginePresentVk::Impl& s);
bool recordPendingImgui(EnginePresentVk::Impl& s, VkCommandBuffer cb);

}  // namespace mc2vk

#endif  // MC2_VULKAN && MC2_VULKAN_ISLAND
#endif  // ENGINE_PRESENT_VK_INTERNAL_H
