// tool_vk_shell -- the Vulkan window shell the viewport tools share (asset_viewer, mc2fx_preview, mc2weapon_gui).
//
// WHAT: SDL2 Vulkan surface + instance + device + swapchain + one render pass, with Dear ImGui's Vulkan backend
//   (imgui_impl_vulkan, the same backend the game's ImGui path uses) drawn into it. Replaces each tool's private
//   SDL_GL context + imgui_impl_opengl3.
// ENV: MC2_VULKAN_VALIDATION=1 turns on VK_LAYER_KHRONOS_validation and counts errors ("[TOOLVK] validation errors=N" at exit).
//   MC2_TOOL_CAPTURE=<file.png> writes the swapchain image after frame MC2_TOOL_CAPTURE_FRAME (default 12) and end() then
//   returns true (quit) -- the launch-and-screenshot proof path the tools share.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <volk.h>

struct SDL_Window;

namespace toolvk {

struct Shell {
    SDL_Window* window = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
    std::vector<VkSemaphore> renderDone;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    bool imguiInit = false;
    bool validation = false;
    uint32_t imageIndex = 0;
    bool frameOpen = false;
    bool needRebuild = false;
    bool submitFailed = false;   // end() ended the run because vkQueueSubmit was refused
    int frameNo = 0;
    std::string capturePath;
    int captureFrame = 12;
    float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
};

// Window must have been created with SDL_WINDOW_VULKAN. Returns false (and prints why) on any failure.
bool init(Shell& s, SDL_Window* window, const char* appName);
// Acquires the next image and opens the render pass with the clear color. false = skip this frame (minimized / rebuild).
bool begin(Shell& s);
// Records ImGui's draw data (call after ImGui::Render()), ends the pass, submits, presents. Returns true when the shell
// wants the app to quit (a requested capture was written).
bool end(Shell& s);
void shutdown(Shell& s);
// True when MC2_TOOL_CAPTURE is set: the tool then creates its window hidden (the capture reads the swapchain, not the desktop).
bool captureRequested();
// Number of validation errors seen so far.
uint32_t validationErrors();

// Writes a 32-bit RGBA/BGRA image as an (uncompressed-deflate) PNG.
bool writePng(const char* path, const uint8_t* pixels, uint32_t w, uint32_t h, bool bgra);

} // namespace toolvk
