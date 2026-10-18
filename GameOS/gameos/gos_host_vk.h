// gos_host_vk.h - Vulkan host glue shared by the game (gameosmain) and the editor (EditorGameOS).
#pragma once
#include "gos_render.h"

extern bool g_vkPresentInitDeferred;
extern unsigned g_vkPresentRetryFails;

// Birth the engine Vulkan context and present path on `win` when the Vulkan backend is selected; a no-op on GL.
void initVulkanBackend(graphics::RenderWindowHandle win, int w, int h);

namespace gos { namespace host {
// Once per frame, after the frame is finished in the default framebuffer: Vulkan present or GL swap.
void presentOrSwap(graphics::RenderWindowHandle win, unsigned long frameCounter);
}}
