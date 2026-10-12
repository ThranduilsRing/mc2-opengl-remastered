# Vulkan best-practices design review

Read-only review at `HEAD` (recorded in the accompanying commit). Scope:
`GameOS/gameos/vulkan_*.[ch]pp`, `engine_present_vk.cpp`, backend selection,
the VK umbrella gate, Vulkan shaders, flip checklist and mech-shadow plan.
Target: a Vulkan 1.2 minimum tech/feature demo; GL is removed after port
acceptance. The recommendations below are design deltas, not a feature audit
claiming unmeasured performance.

## Current implementation and evidence

### Version, selection, validation and ownership

- The instance and VMA allocator are configured as Vulkan 1.1 today, not the
  owner's 1.2 minimum: `VulkanContext::create` sets
  `VkApplicationInfo::apiVersion` at `GameOS/gameos/vulkan_context.cpp:196-199`
  and `VmaAllocatorCreateInfo::vulkanApiVersion` at `:311-318`. The device
  create path requests only swapchain when presentation is enabled (`:282-296`);
  there is no 1.2 physical-device floor check.
- Validation is opt-in through `resolveValidationPreset` and
  `VulkanContext::create` (`vulkan_context.cpp:150-158`). If
  `VK_LAYER_KHRONOS_validation` is missing, the code clears
  `validationEnabled` and continues (`:173-191`). Debug utils are enabled with
  the layer (`:185-186`), but missing messenger entry point or creation failure
  only logs (`:221-236`). The context reports the boolean and error count
  (`vulkan_context.cpp:350-358`; `vulkan_context.h:42-43`). This is a useful
  runtime diagnostic, not a fail-closed validation test tier.
- `VulkanContext::create` owns the single VMA allocator
  (`vulkan_context.cpp:311-319`); `VulkanContext::destroy` destroys allocator,
  device, messenger and instance in that order (`:327-347`). Islands instead
  keep raw Vulkan handles and VMA allocation pairs in state structs and provide
  manual teardown, e.g. `ColorPropIsland::destroy_island`
  (`vulkan_color_prop_island.cpp:506-539`) and terrain's `destroy_island`
  (`vulkan_terrain_island_island.cpp:338-380`).
- Backend selection is memoized. Empty `MC2_RENDER_BACKEND` selects GL and an
  explicit `vulkan`/`vk` value selects Vulkan (`render_backend_select.cpp:32-50`).
  The umbrella gate is `MC2_VK_SHARED_GBUFFER && !g_mc2VkSessionForcedGL`
  (`mclib/vk_umbrella_gate.cpp:7-10`). Thus the implementation does not yet
  boot Vulkan by default.

### Render passes, waits and per-file handle-site count

Render-pass ownership is repeated by island rather than centralized. The
following creation sites identify the owner function and pass; recording uses
the matching render pass in the listed file:

| Owner / render pass | Create site | Recorded pass |
|---|---|---|
| Color props, `init_island` | `vulkan_color_prop_island.cpp:812,973` | `:1592-1667` |
| Legacy shapes, `init_island` | `vulkan_legacy_shape_island.cpp:315,407` | `:640-670` |
| Mechs, `init_island` | `vulkan_mech_island.cpp:629,756` | `:1217-1258` |
| Fog postprocess, `build_state` | `vulkan_post_fog_native.cpp:238,335` | `:707-714` |
| HZB postprocess, `build_state` | `vulkan_post_hzb_native.cpp:194,297` | `:690-698` |
| Screen effects, `build_state` | `vulkan_post_screenfx_native.cpp:312,426` | `:872-876` |
| SSAO occlusion + apply, `build_state` | `vulkan_post_ssao_native.cpp:260,395,440` | `:811-840` |
| Shadow island, `init_island` | `vulkan_shadow_island_live.cpp:347,460` | `:1146-1190`, `:1300-1334`, `:1430-1468` |
| Sky, `init_island` | `vulkan_sky_island.cpp:293,359` | `:475-488` |
| Terrain overlay, `ovl_init` | `vulkan_terrain_island_overlay.cpp:190,234` | `:503-536` |
| Terrain, `init_island` | `vulkan_terrain_island_island.cpp:398,520` | terrain run `vulkan_terrain_island_run.cpp:500-547` |
| VFX, `init_island` | `vulkan_vfx_island.cpp:260,342` | `:487-500` |
| Water, `init_island` | `vulkan_water_island_init.cpp:12,147` | `vulkan_water_island_run.cpp:192-196` |
| Present composite, `ensureCompositeObjects` | `engine_present_vk.cpp:647,685` | `:1995-2025` |
| Present shadow overlay, `ensureShadowDebugOverlayObjects` | `engine_present_vk.cpp:800,838` | `:2304-2319` |
| HUD color, `ensureHudColorObjects` | `engine_present_vk.cpp:1022,1049` | `:1613-1678` |
| ImGui, `ensureImguiRenderPass` | `engine_present_vk.cpp:1325-1352` | `:1410-1412` |

The island frame paths submit with `vkQueueSubmit` and wait inline on fences:
color props (`vulkan_color_prop_island.cpp:1679,1693`), mechs
(`vulkan_mech_island.cpp:1270,1275`), terrain
(`vulkan_terrain_island_run.cpp:558,572`), water
(`vulkan_water_island_run.cpp:210,227`), VFX
(`vulkan_vfx_island.cpp:510,512`), shadows
(`vulkan_shadow_island_live.cpp:1201,1209; 1342,1344; 1475,1479`). Present
waits for its in-flight fence at `engine_present_vk.cpp:1950,2180`; present
readback submits and waits synchronously at `:2537-2538`. One-shot uploads also
create, submit, wait and destroy local fences, e.g. props
(`vulkan_color_prop_island.cpp:551-577`) and mech scheme uploads
(`vulkan_mech_island.cpp:427-452`).

`vkDeviceWaitIdle` is used beyond shutdown on capacity growth, image/descriptor
replacement and geometry refresh: props (`vulkan_color_prop_island.cpp:393,
494,509,788,1278`), mechs (`vulkan_mech_island.cpp:414,600`), shadow targets
(`vulkan_shadow_island_live.cpp:298,313,614,669,755`), terrain
(`vulkan_terrain_island_island.cpp:287,305,324,341`; images
`vulkan_terrain_island_images.cpp:28,513,526,552,565,800`), water
(`vulkan_water_island.cpp:121,227`; images `vulkan_water_island_images.cpp:34,
201,308`) and VFX (`vulkan_vfx_island.cpp:169,243`). The render-pass, fence and
idle examples are sampled call sites; the per-file census below includes all
lexical `vkCreate*` / `vkDestroy*` tokens in Vulkan-named `.cpp` and `.h` files.

Count command used:
`Get-ChildItem GameOS/gameos -File -Filter 'vulkan_*' | Where-Object { $_.Extension -in '.cpp','.h' }`, count regex `\bvkCreate[A-Za-z0-9_]*` and `\bvkDestroy[A-Za-z0-9_]*` in each file. It reports 243 create tokens and 217 destroy tokens (delta +26). These are call-site token counts, not a leak count: returned handles can be destroyed in another helper, and a destroy site may cover multiple instances. Files with a non-zero lexical delta:

| File | Create | Destroy | Delta |
|---|---:|---:|---:|
| `vulkan_color_prop_island.cpp` | 18 | 16 | +2 |
| `vulkan_context.cpp` | 8 | 3 | +5 |
| `vulkan_legacy_shape_island.cpp` | 10 | 12 | -2 |
| `vulkan_mech_island.cpp` | 10 | 12 | -2 |
| `vulkan_post_fog_native.cpp` | 18 | 15 | +3 |
| `vulkan_post_hzb_native.cpp` | 20 | 13 | +7 |
| `vulkan_post_screenfx_native.cpp` | 29 | 25 | +4 |
| `vulkan_post_ssao_native.cpp` | 27 | 21 | +6 |
| `vulkan_shadow_island_live.cpp` | 21 | 22 | -1 |
| `vulkan_sky_island.cpp` | 10 | 12 | -2 |
| `vulkan_terrain_island_images.cpp` | 9 | 5 | +4 |
| `vulkan_terrain_island_island.cpp` | 16 | 23 | -7 |
| `vulkan_terrain_island_overlay.cpp` | 8 | 10 | -2 |
| `vulkan_vfx_island.cpp` | 16 | 11 | +5 |
| `vulkan_water_island.cpp` | 1 | 15 | -14 |
| `vulkan_water_island_images.cpp` | 6 | 2 | +4 |
| `vulkan_water_island_init.cpp` | 16 | 0 | +16 |

### Modern Vulkan facilities

Absence audit: `rg -n -i` across `GameOS/gameos shaders/vulkan` returned zero
hits for each listed pattern: `vkCmdBeginRendering`, `vkQueueSubmit2`,
`vkCmdPipelineBarrier2`, `VkSemaphoreTypeCreateInfo|VK_SEMAPHORE_TYPE_TIMELINE`,
`VK_EXT_descriptor_indexing|VkDescriptorIndexingFeatures|descriptorBindingPartiallyBound`,
`VK_KHR_buffer_device_address|vkGetBufferDeviceAddress`,
`vkCreatePipelineCache|VK_EXT_graphics_pipeline_library`,
`vkCmdDraw(Indexed)?Indirect`,
`VK_KHR_fragment_shading_rate|VK_EXT_fragment_shading_rate|VK_COLOR_SPACE_HDR`,
`VK_KHR_ray_query|vkCmdTraceRaysKHR`, and `GFXReconstruct|gfxrecon`.

| Topic | What exists | Gap and concrete design change |
|---|---|---|
| Dynamic rendering | Render passes and framebuffers are created per island (table above). | No dynamic rendering command exists. Keep Vulkan 1.2 as the device floor; make dynamic rendering a capability-selected path where `VK_KHR_dynamic_rendering` is present, with the existing render-pass path for 1.2 devices without it. This avoids silently turning the floor into 1.3. |
| Synchronization2 / timelines | Fences protect island slots and WSI; one-shot uploads are synchronous (evidence above). | No synchronization2 submits/barriers or timeline semaphores. Make dependencies explicit with synchronization2; use timeline values for upload completion and deferred resource retirement. Keep binary semaphores at acquire/present. Restrict device-idle waits to shutdown/recovery after the retirement model exists. |
| Descriptor indexing / bindless | Islands create conventional descriptor set layouts/pools and write per-island sets, e.g. `vulkan_mech_island.cpp:321-353,518-579` and `vulkan_terrain_island_island.cpp:155-184`. | No descriptor indexing, partially-bound array, or non-uniform indexing path. Add a bounded sampled-image/material table on capable devices and retain a descriptor-set fallback; retire per-object texture-set churn only after image parity and lifetime validation. |
| Buffer device address | VMA buffers serve as vertex, index, instance, UBO and storage data; e.g. `vulkan_color_prop_island.cpp:305-317`. | No buffer-device-address feature or address fetch in the VK shaders. Keep BDA optional: adopt it only with a measured GPU scene/mesh-arena design that benefits from device pointers; it is not needed for the first descriptor-table step. |
| VMA ownership | One VMA allocator has a clear context owner (`vulkan_context.cpp:311-319,327-330`). | Raw handle/allocation pairs and manual cleanup remain in island state; no `vk::Unique*`/move-only Vulkan resource owner matches the island lifetime. Treat the +26 lexical imbalance as audit evidence only. Introduce move-only buffer/image owners that retain allocator identity; verify partial-init unwind and exactly-once teardown with resource-class accounting before deleting the manual paths. |
| Pipeline caching / libraries | `init_island`/`build_state` creates graphics pipelines, e.g. props `vulkan_color_prop_island.cpp:812-820,1176`; shadow `vulkan_shadow_island_live.cpp:347-...`. | No `VkPipelineCache` creation/use or graphics pipeline library hit. Persist a driver/device-keyed cache with stale/corrupt-cache fallback. Evaluate pipeline libraries only if cold-start profiling shows compile cost that justifies added driver/variant complexity. |
| GPU-driven and indirect draws | CPU builds batches and issues instanced draws; e.g. props `vulkan_color_prop_island.cpp:1592-1667`, mech `vulkan_mech_island.cpp:1217-1258`. | No indirect draws. First consolidate compatible materials and object lists; then use indirect commands/GPU culling on a named high-volume scene if CPU submission is a measured bottleneck. Preserve the CPU-built fallback on Vulkan 1.2. |
| Async compute | SSAO and HZB postprocess islands exist (`vulkan_post_ssao_native.cpp:689-893`; `vulkan_post_hzb_native.cpp:601-768`). Context chooses a graphics queue only (`vulkan_context.cpp:254-275`). | No compute queue ownership or timeline handoff. Keep work on graphics until GPU timestamps show useful overlap; then move independent HZB/SSAO work only when a compute-capable queue and transfer cost make it faster. |
| VRS / HDR | A present swapchain and HDRI sky texture exist (`engine_present_vk.cpp:432-514`; `vulkan_water_island_images.cpp:48-145`). | No fragment-shading-rate state or HDR swapchain format/color space. VRS is a low-priority 7900 XTX experiment for heavy low-detail regions, accepted only with quality and timestamp evidence. HDRI input does not mean HDR output; add HDR only with a tone-mapped linear output, color-space/metadata path and a real HDR-display capture. |
| Ray query for shadow / AO | Raster shadow maps/CSM (`vulkan_shadow_island_live.cpp:1146-1479`), screen shadows (`vulkan_post_screenfx_native.cpp:721-1158`) and SSAO exist. | No acceleration structures or ray-query path. The RX 7900 XTX has 96 Ray Accelerators per AMD's [specification](https://www.amd.com/en/products/graphics/desktops/radeon/7000-series/amd-radeon-rx-7900xtx.html), but that alone does not prove a win. Test optional ray-query contact shadows or short-range AO against raster/SSAO on that device; measure GPU time and image quality. Keep CSM/SSAO for all Vulkan 1.2 devices and defer full-scene ray shadows until dynamic-scene acceleration and denoising pay for their cost. |
| Validation test tier | Context counts validation errors and reports whether the layer was enabled (`vulkan_context.cpp:350-358`). | A requested but absent layer degrades to no validation (`:189-191`), so tiered verification can appear green without coverage. VK test tier must require the Khronos layer/debug messenger and fail on unavailable validation or non-zero error count; retail remains optional. |
| GFXReconstruct replay | Flip checklist already states VK goldens currently read the GL FBO (`vk-default-flip-checklist.md:25-31`) and requires a VK-aware present readback before the flip (`:46-48`). | No GFXReconstruct integration was found by the absence audit. Capture/replay the golden day and night frames, compare stream and replay image, and include a deliberate shader perturbation that identifies the first differing command/resource. Keep replay as a command-stream oracle alongside region-digest A/B and validation. |

## R1 batches, ranked against these findings

| Rank | Batch (effort) | Payoff | Proof |
|---|---|---|---|
| 1 | Declare/enforce Vulkan 1.2 device profile (S, 1-2 days) | Current context requests 1.1 and does not reject older devices. Names the real floor and feature fallback contract before architecture changes. | Context reports device API/extensions; Vulkan 1.1 is rejected; 1.2 device passes; capability fallbacks are recorded. |
| 2 | VK acceptance and replay oracle (M, 3-5 days) | Current selector defaults to GL and the flip checklist says goldens read GL output. Establishes a trustworthy Vulkan image/stream before parity work. | VK present readback; day + night GFXReconstruct capture/replay stream matches; deliberate shader change identifies its first differing draw/resource; region digests recorded. |
| 3 | Island resource owners and balance ledger (L, 5-8 days) | The measured +26 call-site delta and manual partial teardown are cross-cutting risks for subsequent changes. | Move-only VMA owners by resource kind; per-kind counters balance over successful init, injected partial-init failures and shutdown; validation reports zero lifetime errors. |
| 4 | Frame synchronization / deferred retirement (L, 4-7 days) | Inline frame fences and many growth-time device-idle calls block CPU/GPU overlap. | Synchronization2 and timeline retirement; validation clean; Tracy records lower fence/idle wait time on shadow and terrain workloads. |
| 5 | Dynamic rendering, capability path (M, 3-5 days) | Removes repeated render-pass/framebuffer scaffolding while preserving the 1.2 floor. | Identical VK region digests on a 1.2 extension device and 1.2 fallback device; no validation errors. |
| 6 | Pipeline cache then descriptor table (M/L, 2-3 + 5-8 days) | Cache reduces repeat pipeline compile; descriptor indexing can reduce texture-set churn. | Cold/warm startup timing and safe cache invalidation; texture-heavy scene image parity, descriptor lifetime validation and Tracy submission delta. |
| 7 | GPU-driven draws and async compute (XL, 8-15 days each, evidence-gated) | Potential CPU submission and independent compute overlap gains, but no present data shows either is currently limiting. | Named target scene, same visible-instance digest, GPU timestamps and Tracy deltas; CPU path retained for Vulkan 1.2 fallback. |
| 8 | VRS, HDR, ray-query experiments (M-XL; optional) | Device-specific image/performance improvement, not a portability baseline. | Per-device capability record, quality review, controlled GPU-time comparison, and raster/full-rate/SDR fallback. |

## Owner questions

1. Should the Vulkan 1.2 floor require `VK_KHR_dynamic_rendering` or other selected extensions, or must every conforming 1.2 device use fallbacks when those extensions are absent?
2. Is HDR output a required part of this demo, with an HDR display available for acceptance captures?
3. Should an optional ray-query contact-shadow/AO experiment on the RX 7900 XTX be part of R1, or remain deferred until a raster/SSAO gap is measured?
