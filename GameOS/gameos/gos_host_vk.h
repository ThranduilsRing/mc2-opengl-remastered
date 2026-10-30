// gos_host_vk.h - Vulkan host glue shared by the game (gameosmain) and the editor (EditorGameOS).
#pragma once
#include "gos_render.h"

extern bool g_vkPresentInitDeferred;
extern unsigned g_vkPresentRetryFails;

// Initialize the shared native device and the present owner on the actual SDL window.
void initVulkanBackend(graphics::RenderWindowHandle win, int w, int h);

namespace gos { namespace host {
// The engine frame clock (g_mc2FrameCounter): ONE advance per rendered frame, called by the host loop after the scene is drawn and before the
// present (game: gameosmain; editor: EditorGameOS). Sim substeps in gameosmain_frame.cpp add their own extra ticks; this is the frame tick.
void advanceFrameCounter();
// Destroy islands, present resources and the shared Vulkan device before the SDL window.
void shutdownVulkanBackend();
void retirePresentHandles(unsigned long frameCounter);
// Submit/present native work, or hold the previous frame while a minimized surface recovers.
void presentOrSwap(graphics::RenderWindowHandle win, unsigned long frameCounter);
}}
