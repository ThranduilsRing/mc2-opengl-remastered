// gos_host_vk.h - Vulkan host glue shared by the game (gameosmain) and the editor (EditorGameOS).
#pragma once
#include "gos_render.h"

extern bool g_vkPresentInitDeferred;
extern unsigned g_vkPresentRetryFails;

// Birth the engine Vulkan context and present path on `win` when the Vulkan backend is selected; a no-op on GL.
void initVulkanBackend(graphics::RenderWindowHandle win, int w, int h);

namespace gos { namespace host {
// The engine frame clock (g_mc2FrameCounter): ONE advance per rendered frame, called by the host loop after the scene is drawn and before the
// present (game: gameosmain; editor: EditorGameOS). Sim substeps in gameosmain_frame.cpp add their own extra ticks; this is the frame tick.
void advanceFrameCounter();
// Tear the engine Vulkan side down, in the one order both hosts use: the render islands, then the present path (it deletes GL objects and
// VK resources on the shared device), then the shared VulkanContext. Call after gos_DestroyRenderer and before the GL context and the window
// are destroyed. A no-op when the Vulkan backend was never born.
void shutdownVulkanBackend();
// Once per frame, after the frame is finished in the default framebuffer: Vulkan present or GL swap.
void presentOrSwap(graphics::RenderWindowHandle win, unsigned long frameCounter);
}}
