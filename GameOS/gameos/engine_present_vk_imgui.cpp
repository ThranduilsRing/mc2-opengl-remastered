// EnginePresentVk, ImGui piece: the GuiRuntime (ImGui) backend over the shared present image and the per-texture descriptor
// bridge. Split out of engine_present_vk.cpp; the Impl and the cross-TU entry points live in engine_present_vk_internal.h.
#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)
#include "engine_present_vk_internal.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "imgui_impl_vulkan.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc2vk {
namespace {

bool ensureImguiRenderPass(EnginePresentVk::Impl& s) {
    if (s.imguiRenderPass.get()) return true;

    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

        // Color-attachment WAW/RAW against the previous pass on the shared present image (LOAD_OP_LOAD, no layout transition, so no implicit edge).
    VkSubpassDependency deps[2]; fillLoadPassDeps(deps);
VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2; rp.pDependencies = deps;
    return mc2vk::create(s.imguiRenderPass, s.dev, [&](auto* h) { return vkCreateRenderPass(s.dev, &rp, nullptr, h); });
}

bool ensureImguiFramebuffer(EnginePresentVk::Impl& s) {
    if (s.imguiFramebuffer.get()) return true;
    if (!ensureImguiRenderPass(s) || !s.sharedImg.view || s.imgW == 0 || s.imgH == 0)
        return false;

    VkImageView attachments[] = { s.sharedImg.view.get() };
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = s.imguiRenderPass.get();
    fb.attachmentCount = 1;
    fb.pAttachments = attachments;
    fb.width = s.imgW;
    fb.height = s.imgH;
    fb.layers = 1;
    return mc2vk::create(s.imguiFramebuffer, s.dev, [&](auto* h) { return vkCreateFramebuffer(s.dev, &fb, nullptr, h); });
}

constexpr uint32_t kImguiTexCap = 256;

}  // namespace

bool ensureImguiVulkan(EnginePresentVk::Impl& s) {
    if (s.imguiVkInited) return ensureImguiFramebuffer(s);
    if (s.imguiVkFailed || !s.dev || !s.phys || !s.gfxQ || !s.imgW || !s.imgH)
        return false;
    if (!ensureImguiFramebuffer(s)) {
        s.imguiVkFailed = true;
        return false;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = s.inst;
    info.PhysicalDevice = s.phys;
    info.Device = s.dev;
    info.PipelineCache = mc2vk::infra::cacheHandle();
    info.AllocateMemory = [](VkDevice device, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory, const char* name) {
        mc2vk::infra::Admission admission(ai->allocationSize,name);
        if(!admission) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        const auto result=vkAllocateMemory(device,ai,allocator,memory);
        if(result==VK_SUCCESS) {
            admission.commit(reinterpret_cast<uint64_t>(*memory),name,true);
            mc2vk::infra::breadcrumb("resource",name,reinterpret_cast<uint64_t>(*memory));
        }
        return result;
    };
    info.MemoryFreed = [](auto memory) { mc2vk::infra::release(reinterpret_cast<uint64_t>(memory)); };
    info.QueueFamily = s.gfxFamily;
    info.Queue = s.gfxQ;
    info.RenderPass = s.imguiRenderPass.get();
    info.MinImageCount = 2;
    info.ImageCount = s.scImages.size() >= 2 ? static_cast<uint32_t>(s.scImages.size()) : 2;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.DescriptorPoolSize = 16 + 2 * kImguiTexCap;   // font + live texture sets + retired sets awaiting their frame
    if (!ImGui_ImplVulkan_Init(&info)) {
        s.imguiVkFailed = true;
        return false;
    }
    // The present image is stored with GL bottom-left origin (the composite renders it un-flipped and copy-2 flips it into the
    // swapchain), and the HUD tail pass flips its own Y. The Vulkan ImGui backend draws top-down, so without this the whole ImGui
    // layer (every defs/ImGui page: text, art, portraits) reaches the screen upside down.
    ImGui_ImplVulkan_SetFlipY(true);
    s.imguiVkInited = true;
    return true;
}

namespace {

// Per-texture ImGui bridge. The game draws ImGui images with GL texture names as the ImTextureID; the Vulkan backend binds the id as a
// VkDescriptorSet, so each name is resolved to a set over a VK image uploaded from the texture CPU texels. A name with no texels
// (render targets, not loaded) gets no set and its command is skipped at draw time.
constexpr uint32_t kImguiLogCap = 3;

// A retired ImGui texture entry waits in the context deferred-destroy queue; its destructor returns the descriptor set to the backend pool and
// releases the image, view and memory once the frame that may sample them has completed. The counter feeds the descriptor budget below.
size_t g_imguiTexRetiredPending = 0;
struct ImguiTexRetired {
    EnginePresentVk::Impl::ImguiTexEntry entry;
    explicit ImguiTexRetired(EnginePresentVk::Impl::ImguiTexEntry&& e) : entry(std::move(e)) { ++g_imguiTexRetiredPending; }
    ImguiTexRetired(const ImguiTexRetired&) = delete;
    ~ImguiTexRetired();
};

void imguiTexDestroy(EnginePresentVk::Impl::ImguiTexEntry& e, bool backendLive) {
    if (e.set && backendLive)
        ImGui_ImplVulkan_RemoveTexture(e.set);
    e.set = VK_NULL_HANDLE;
}

bool imguiBackendLive() {
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendRendererUserData != nullptr;
}

ImguiTexRetired::~ImguiTexRetired() {
    --g_imguiTexRetiredPending;
    imguiTexDestroy(entry, imguiBackendLive());
}

}  // namespace

void imguiTexReleaseAll(EnginePresentVk::Impl& s) {
    const bool live = imguiBackendLive();
    for (auto& kv : s.imguiTex) imguiTexDestroy(kv.second, live);
    s.imguiTex.clear();
    s.ctx->drainDeferred("present_imgui_release");   // the retired entries return their sets while the backend is still up
}

namespace {

void imguiTexSweep(EnginePresentVk::Impl& s) {
    const uint64_t now = ++s.imguiTexFrame;
    for (auto it = s.imguiTex.begin(); it != s.imguiTex.end();) {
        auto& e = it->second;
        const bool staleService = RenderCore::isUiTextureId(it->first) &&
            (!RenderCore::textureService().isLive(e.serviceTexture) || e.serviceView != s.ctx->textures().view(e.serviceTexture));
        if (staleService) {   // the texture was released or recreated: retire the set, destroy it once the GPU is done with it
            s.ctx->retire(std::make_unique<ImguiTexRetired>(std::move(e)));
            it = s.imguiTex.erase(it);
        } else {
            ++it;
        }
    }
}

// Moves a live entry to the deferred-destroy queue (destroyed once the frame that may sample it has completed).
void imguiTexRetire(EnginePresentVk::Impl& s, std::unordered_map<uint64_t, EnginePresentVk::Impl::ImguiTexEntry>::iterator it) {
    s.ctx->retire(std::make_unique<ImguiTexRetired>(std::move(it->second)));
    s.imguiTex.erase(it);
}

// Both legacy and service IDs share the same bounded descriptor pool.
bool imguiTexMakeRoom(EnginePresentVk::Impl& s) {
    // Descriptor budget: the pool holds kImguiTexCap live AND kImguiTexCap retired sets (a retired set is freed only once its frame completed).
    // At the live cap, the least recently used entry not drawn this frame is retired to make room.
    if (g_imguiTexRetiredPending >= kImguiTexCap)
        return false;
    if (s.imguiTex.size() >= kImguiTexCap) {
        auto victim = s.imguiTex.end();
        for (auto v = s.imguiTex.begin(); v != s.imguiTex.end(); ++v)
            if (v->second.lastUsedFrame != s.imguiTexFrame &&
                (victim == s.imguiTex.end() || v->second.lastUsedFrame < victim->second.lastUsedFrame))
                victim = v;
        if (victim == s.imguiTex.end())
            return false;
        imguiTexRetire(s, victim);
    }
    return true;
}

// Records the upload into `cb` (outside the render pass). Returns 0 when the name has no VK image. Everything that can fail
// (buffers, image, view, the descriptor set) happens BEFORE the first command is recorded, so a failure never leaves a recorded
// command naming a destroyed resource.
ImTextureID imguiTexResolve(EnginePresentVk::Impl& s, VkCommandBuffer cb, uint64_t textureId) {
    if (RenderCore::isUiTextureId(textureId)) {
        const auto handle=RenderCore::uiTextureHandle(textureId);
        auto& textures=s.ctx->textures();
        if (!RenderCore::textureService().isLive(handle)) return 0;
        const auto view=textures.view(handle);
        auto it=s.imguiTex.find(textureId);
        if (it!=s.imguiTex.end()) {
            if (it->second.serviceView==view) { it->second.lastUsedFrame=s.imguiTexFrame; return reinterpret_cast<ImTextureID>(it->second.set); }
            imguiTexRetire(s,it);
        }
        if (!imguiTexMakeRoom(s)) return 0;
        if (!s.imguiTexSampler) {
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter=sci.minFilter=VK_FILTER_LINEAR;
            sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (!mc2vk::create(s.imguiTexSampler,s.dev,[&](VkSampler* h){return vkCreateSampler(s.dev,&sci,nullptr,h);})) return 0;
        }
        EnginePresentVk::Impl::ImguiTexEntry e;
        e.serviceTexture=handle; e.serviceView=view; e.serviceImage=textures.imageOwner(handle); e.lastUsedFrame=s.imguiTexFrame;
        e.set=ImGui_ImplVulkan_AddTexture(s.imguiTexSampler.get(),view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!e.set) return 0;
        const auto set=reinterpret_cast<ImTextureID>(e.set); s.imguiTex.emplace(textureId,std::move(e)); return set;
    }
    return 0;
}

}  // namespace

bool recordPendingImgui(EnginePresentVk::Impl& s, VkCommandBuffer cb) {
    ImDrawData* drawData = s.pendingImguiDrawData;
    s.pendingImguiDrawData = nullptr;
    if (!drawData || !s.imguiVkInited || !ensureImguiFramebuffer(s))
        return false;

    // VK-IMGUI-LIFECYCLE-1: the draw data is only recorded while the Vulkan backend (and the main viewport's renderer data it installs) is alive.
    {
        const bool ctx = ImGui::GetCurrentContext() != nullptr;
        const bool bd = ctx && ImGui::GetIO().BackendRendererUserData != nullptr;
        const bool vp = drawData->OwnerViewport && drawData->OwnerViewport->RendererUserData;
        if (!(ctx && bd && vp)) {
            static uint32_t s_skipped = 0;
            if (s_skipped++ < 3)
                std::fprintf(stderr, "[VK_IMGUI] draw data skipped: ctx=%d backend=%d viewport_renderer_data=%d owner=%p\n",
                             static_cast<int>(ctx), static_cast<int>(bd), static_cast<int>(vp), static_cast<void*>(drawData->OwnerViewport));
            return false;
        }
    }

    // Every non-font command carries a GL texture name; swap in its descriptor set for the backend and restore afterwards, so the same
    // ImDrawData still reads correctly elsewhere. A name with no VK image is skipped at bind time (empty clip rect).
    struct CmdSave { ImDrawCmd* cmd; ImTextureID id; ImVec4 clip; };
    std::vector<CmdSave> saved;
    {
        imguiTexSweep(s);
        const ImTextureID fontTex = ImGui::GetIO().Fonts->TexID;
        uint32_t skipped = 0;
        for (int n = 0; n < drawData->CmdListsCount; ++n) {
            for (ImDrawCmd& c : drawData->CmdLists[n]->CmdBuffer) {
                if (c.UserCallback || c.ElemCount == 0 || c.GetTexID() == fontTex)
                    continue;
                saved.push_back({&c, c.TextureId, c.ClipRect});
                const ImTextureID set = imguiTexResolve(s, cb, static_cast<uint64_t>(c.GetTexID()));
                if (set) {
                    c.TextureId = set;
                } else {
                    c.ClipRect = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
                    ++skipped;
                }
            }
        }
        static uint32_t s_logged = 0;
        if (skipped != 0 && s_logged < kImguiLogCap) {
            ++s_logged;
            std::fprintf(stderr, "[VK_IMGUI] texture_cmds_skipped=%u (no VK image for the GL texture name)\n", skipped);
        }
    }

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.imguiRenderPass.get();
    rp.framebuffer = s.imguiFramebuffer.get();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {s.imgW, s.imgH};
    // The ids and clip rects are put back on every exit from here.
    struct RestoreGuard {
        const std::vector<CmdSave>& v;
        ~RestoreGuard() { for (const CmdSave& c : v) { c.cmd->TextureId = c.id; c.cmd->ClipRect = c.clip; } }
    } restoreGuard{saved};
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Present.ImGui");
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, cb);
    vkCmdEndRenderPass(cb);
    gpuScope.end();
    return true;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN && MC2_VULKAN_ISLAND
