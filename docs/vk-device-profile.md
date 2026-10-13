<!-- doc-status: current | reviewed:2026-10-13 -->
# Vulkan 1.2 device profile

WHAT: the table in `GameOS/gameos/vk_device_profile.h` states what the engine requires of a GPU (REQUIRED: the device is refused) and what it uses when the GPU has it
(WANTED: enabled when present, the engine keeps its current path when not). The shared context (`vulkan_context.cpp`) gathers the capabilities of every physical
device once (`vk_device_profile.cpp`), picks the first discrete device that meets every REQUIRED item (else the first device that does), enables only the supported
WANTED bits, and prints one `[VK_PROFILE v1]` line. When no device qualifies the context fails soft: the session falls back to GL with a logged reason, never a crash.

WHY: the context asked for Vulkan 1.1 and enabled no feature, so the 1.2 minimum was not checked anywhere. Raising the instance version does not enable a device
feature: a feature is used only if it was queried, found and enabled.

| item | need | route | without it |
|---|---|---|---|
| `api_1_2` | required | device and instance version >= 1.2 (the instance asks for 1.3 when the loader has it) | device refused |
| `swapchain` | required when the context drives a present window | `VK_KHR_swapchain` | device refused |
| `limits` | required | image dimension 8192, 4 bound descriptor sets, 128 byte push constants, 128 MiB storage buffer range | device refused |
| `timeline_semaphore` | wanted | core 1.2 (`timelineSemaphore`) | fences |
| `buffer_device_address` | wanted | core 1.2 (`bufferDeviceAddress`; VMA is told) | ordinary buffer bindings |
| `descriptor_indexing` | wanted | core 1.2: the umbrella bit plus `runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingPartiallyBound` | conventional descriptor sets |
| `dynamic_rendering` | wanted | core 1.3 when device and instance have it, else `VK_KHR_dynamic_rendering` | render passes |
| `synchronization2` | wanted | core 1.3 when device and instance have it, else `VK_KHR_synchronization2` | legacy submit and barriers |

Open owner question (best-practices review, question 1): whether the floor should REQUIRE dynamic rendering and the other selected extensions. Making a WANTED item
REQUIRED is the one-word change of its `Need` in `evaluate()`.

The line: `[VK_PROFILE v1] verdict=ok|refused device="..." api=.. instance_api=.. floor=1.2 required_missing=.. wanted_missing=.. items=id:route,.. enabled=.. extensions=..`
(`-` is none; a route is `core`, `ext`, `limit` or `-`). `scripts/smoke_lib/vk_profile.py` parses it; the smoke runner grades `verdict=refused` red (marker bucket
`vk_profile_refused`) and the report has a `Vulkan profile:` line. Unit tests: the `vk profile:` cases in `tests/unit/test_gate.cpp` (the table) and
`tests/smoke/test_vk_profile.py` (the parser and the grading).
