<!-- doc-status: current | reviewed:2026-10-13 -->
# Vulkan device profile

WHAT: the table in `GameOS/gameos/vk_device_profile.h` states what the engine requires of a GPU. The shared context (`vulkan_context.cpp`) gathers the
capabilities of every physical device once (`vk_device_profile.cpp`), picks the first discrete device that meets every REQUIRED item (else the first device
that does), enables the feature bits and extensions the table names, and prints one `[VK_PROFILE v1]` line. When no device qualifies the context fails soft:
the session falls back to GL with a logged reason, never a crash. That engine-level fallback is the only one: a Vulkan island has no per-facility alternative path.

WHY: the context asked for Vulkan 1.1 and enabled no feature, so the 1.2 minimum was not checked anywhere. Raising the instance version does not enable a device
feature: a feature is used only if it was queried, found and enabled. Owner ruling 2026-10-13: dynamic rendering, synchronization2, timeline semaphores,
descriptor indexing and buffer device address are REQUIRED, so an island may call any of them without checking for a fallback and the code that adopts a
facility deletes the older arm instead of keeping it.

| item | need | route | without it |
|---|---|---|---|
| `api_1_2` | required | device and instance version >= 1.2 (the instance asks for 1.3 when the loader has it) | device refused |
| `swapchain` | required when the context drives a present window | `VK_KHR_swapchain` | device refused (optional, only reported, for a context with no window) |
| `limits` | required | image dimension 8192, 4 bound descriptor sets, 128 byte push constants, 128 MiB storage buffer range | device refused |
| `timeline_semaphore` | required | core 1.2 (`timelineSemaphore`) | device refused |
| `buffer_device_address` | required | core 1.2 (`bufferDeviceAddress`; VMA is told) | device refused |
| `descriptor_indexing` | required | core 1.2: the umbrella bit plus `runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingPartiallyBound` | device refused |
| `dynamic_rendering` | required | core 1.3 when device and instance have it, else `VK_KHR_dynamic_rendering` | device refused |
| `synchronization2` | required | core 1.3 when device and instance have it, else `VK_KHR_synchronization2` | device refused |
| `independent_blend` | required | core 1.0 feature `independentBlend` | device refused |
| `subgroup_compute` | required | core 1.1 `VkPhysicalDeviceSubgroupProperties`: compute stage plus basic, arithmetic and ballot ops (size and ops are logged in the `subgroup size=` context line); shaders build as SPIR-V 1.5 | device refused |
| `sampler_anisotropy` | optional | core 1.0 `samplerAnisotropy` | vegetation keeps trilinear mips; no change to the hardware floor |
| `memory_budget` | optional | `VK_EXT_memory_budget` | the VRAM line is VMA's own estimate, not the OS's number (reported in `wanted_missing`, never a refusal) |

## What the context keeps and enables

The context keeps the `Evaluation` of the device it chose: `VulkanContext::profile()`, with `featureEnabled(name)` and `route(item)`. An island asks the ROUTE of a
facility (core, or the KHR extension on a 1.2 device, where the entry points to load differ) and whether the optional memory budget is on; it never asks
whether a required facility exists. Nine feature bits are enabled. Eight are the ones the ruling requires: `timelineSemaphore`, `bufferDeviceAddress`,
`descriptorIndexing` with the three bits that define the bindless table (`runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`,
`descriptorBindingPartiallyBound`), `dynamicRendering` and `synchronization2`; no island consumes them yet, they are on because the ruling requires them.
The ninth, `independentBlend`, has consumers today: the terrain, water, VFX and legacy-shape pipelines write two colour attachments with different blend states,
which the validation layer flagged as invalid without it (first run of the validation tier). Vegetation additionally enables `samplerAnisotropy` when available, bounded to 8x and the device limit. A missing optional bit keeps trilinear mips; it never refuses a device. A bit nothing requires and nothing uses is not enabled. The device extensions enabled are the ones on the `extensions=` list of
the line (the swapchain of a present window, the KHR routes on a 1.2 device, the memory budget), read from the context's one enumeration of the device.

## Hardware floor (owner ruling 2026-10-13)

Playable on Vulkan: any GPU whose current driver exposes the five facilities, in practice NVIDIA Maxwell (GeForce GTX 900 series, 2014) and newer, AMD GCN 2
and newer (R9 290 / 390, 2013; the RX 400 Polaris series, 2016, and everything after it on the current driver) and Intel Skylake graphics (2015) and newer.
Dropped from Vulkan: NVIDIA Kepler (GeForce GTX 600 / 700), first-generation GCN (Radeon HD 7000, R9 280 / 270 on the legacy driver) and anything older. Those GPUs
play the OpenGL build; the Vulkan path logs `verdict=refused` with the missing items and the session continues on GL.

## The log line

`[VK_PROFILE v1] verdict=ok|refused device="..." api=.. instance_api=.. floor=1.2 required_missing=.. wanted_missing=.. items=id:route,.. enabled=.. extensions=..`
(`extensions` is every device extension enabled; `-` is none; a route is `core`, `ext`, `limit` or `-`). A refused device lists what is missing in `required_missing` and gets nothing in `enabled` /
`extensions`. `scripts/smoke_lib/vk_profile.py` parses it; the smoke runner grades `verdict=refused` red (marker bucket `vk_profile_refused`) and the report has a
`Vulkan profile:` line. Unit tests: the `vk profile:` cases in `tests/unit/test_gate.cpp` (the table, including one case per required facility) and
`tests/smoke/test_vk_profile.py` (the parser and the grading).
