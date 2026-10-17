#!/bin/sh
# Every graphics-queue submit of the engine goes through VulkanContext::submit (the completion tracker): a raw vkQueueSubmit anywhere else breaks the
# timeline ticket arithmetic. The standalone viewport-tool shell (tools/common) has its own device and is out of scope.
cd "$(dirname "$0")/.." || exit 2
hits=$(git grep -n -E 'vkQueueSubmit2?(KHR)?\(' -- 'GameOS/*.cpp' 'GameOS/*.h' 'RenderCore/*.cpp' 'RenderCore/*.h' 'GuiRuntime/*.cpp' 'GuiRuntime/*.h' \
    | grep -v '^GameOS/gameos/vulkan_context\.cpp:' | grep -v -E '^[^:]+:[0-9]+:[[:space:]]*//')
if [ -n "$hits" ]; then
    echo "raw vkQueueSubmit outside VulkanContext::submit:"
    echo "$hits"
    exit 1
fi
echo "vk_submit_funnel: PASS (no raw vkQueueSubmit outside vulkan_context.cpp)"
