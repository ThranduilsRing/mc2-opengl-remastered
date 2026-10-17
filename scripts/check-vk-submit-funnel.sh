#!/bin/sh
# Every graphics-queue submit of the engine goes through VulkanContext::submit (the completion tracker): a raw vkQueueSubmit anywhere else breaks the
# timeline ticket arithmetic. The standalone viewport-tool shell (tools/common) has its own device and is out of scope.
#
# The ONE documented synchronous exception is the vendored ImGui backend's font-atlas upload (3rdparty/imgui/backends/imgui_impl_vulkan.cpp,
# ImGui_ImplVulkan_CreateFontsTexture: submit + vkQueueWaitIdle). It runs once, inside the first NewFrame after the atlas is built (or rebuilt), never per
# frame, and it signals no timeline value, so the ticket arithmetic is unaffected. This script pins the exception: exactly one raw font-upload submit in the backend,
# two vkQueueWaitIdle (before replacing an old font texture, after the upload), and no engine caller recreating the font texture on its own (which would make it a per-frame stall).
cd "$(dirname "$0")/.." || exit 2
hits=$(git grep -n -E 'vkQueueSubmit2?(KHR)?\(' -- 'GameOS/*.cpp' 'GameOS/*.h' 'RenderCore/*.cpp' 'RenderCore/*.h' 'GuiRuntime/*.cpp' 'GuiRuntime/*.h' \
    | grep -v '^GameOS/gameos/vulkan_context\.cpp:' | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*//')
if [ -n "$hits" ]; then
    echo "raw vkQueueSubmit outside VulkanContext::submit:"
    echo "$hits"
    exit 1
fi
backend=3rdparty/imgui/backends/imgui_impl_vulkan.cpp
n=$(git grep -c -E 'vkQueueSubmit\(v->Queue, 1, &end_info' -- "$backend" | sed 's/.*://')
w=$(git grep -c -E 'vkQueueWaitIdle\(v->Queue\)' -- "$backend" | sed 's/.*://')
if [ "$n" != "1" ] || [ "$w" != "2" ]; then
    echo "imgui backend synchronous-upload exception changed: raw submits=$n (want 1) queue waits=$w (want 2: before replacing the old font texture, after the upload)"
    exit 1
fi
callers=$(git grep -n -E 'ImGui_ImplVulkan_CreateFontsTexture\(' -- 'GameOS/*.cpp' 'GuiRuntime/*.cpp' 'RenderCore/*.cpp' | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*//')
if [ -n "$callers" ]; then
    echo "engine code calls ImGui_ImplVulkan_CreateFontsTexture (a synchronous queue wait; only the backend's first NewFrame may do it):"
    echo "$callers"
    exit 1
fi
echo "vk_submit_funnel: PASS (no raw vkQueueSubmit outside vulkan_context.cpp; the imgui font upload is the one pinned synchronous exception)"
