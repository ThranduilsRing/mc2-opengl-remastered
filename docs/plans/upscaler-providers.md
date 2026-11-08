# Upscaler providers (FSR today; DLSS and XeSS slots)

Status: the interface ships with one provider, FSR 3.1 native AA (`GameOS/gameos/vulkan_fsr_provider.cpp`).
Background: `docs/plans/2026-11-06-temporal-aa.md` and memory `process/decision-fable-fsr-dlss-plan-2026-10-29.md`.

## Shape (modelled on UE5 `ITemporalUpscaler` and Streamline)

- `RenderCore/UpscalerProvider.h` is vendor-neutral and has no Vulkan include. Images are opaque `uint64_t`
  handles (VkImage, VkImageView, VkFormat, usage), and the command list is a `void*` VkCommandBuffer, so the
  registry and math compile in the offline unit tests (`tests/unit/test_upscaler_provider.cpp`).
- `UpscalerFrame` is the per-frame contract: color, depth, motion vectors, reactive mask, transparency mask,
  jitter in render pixels, motion-vector scale, pre-exposure and auto-exposure, render and display extents,
  frame delta, frame index, reset (camera cut), and the real scene projection: near/far, vertical FOV, reverse-Z,
  infinite-far and perspective flags. The main camera publishes these in `RenderCore::temporal::projection()`
  (`mclib/camera_matrix.cpp`, which derives vertical FOV from the configurable horizontal FOV and the frustum
  aspect), and the glue logs them once per change as `[VK_PRESENT] upscaler_inputs ...`.
- `UpscalerCaps` is per provider: the quality modes and their render scales, the motion-vector units and sign,
  and whether the reactive and transparency masks are consumed. The helpers `renderScaleFor`, `renderExtentFor`,
  `motionVectorScaleFor` and `defaultJitterPhaseCount` turn caps into numbers.
- The registry (`upscalerRegistry()`) holds up to `kMaxUpscalers` providers in fallback order.
  `effectiveUpscaler(preferred)` returns the preferred provider when it is available, else the first available
  one, else nullptr (native path, mip bias 0).
- Present (`engine_present_vk.cpp`) only calls the interface, through the glue in `vk_upscalers.cpp`:
  `selectUpscaler` fills the frame from engine images, calls `prepare`, and moves the temporal-consumer
  registration (scene jitter) and `providerPhases()` with the active provider; `dispatchUpscaler` adds the
  command buffer and motion-vector inputs and calls `dispatch`; present then samples `output()`. `mipLodBias` feeds the material sampler bias (`vk_render_infra.cpp`).

## Adding DLSS or XeSS: one file plus one registration line

1. New `GameOS/gameos/vulkan_<vendor>_provider.cpp` with a class deriving from `RenderCore::IUpscalerProvider`
   and a factory `RenderCore::IUpscalerProvider* <vendor>Upscaler()` in `namespace mc2vk` that returns a static
   instance.
2. In `GameOS/gameos/vk_upscalers.cpp`, declare the factory and add one `registerUpscaler(<vendor>Upscaler())`
   line. Its position in the list sets its place in the fallback order.
3. Add the file to `GameOS/gameos/CMakeLists.txt` next to `vulkan_fsr_provider.cpp`.

What the provider must implement:

| Member | Contract |
|---|---|
| `name()` | Short stable id (`"dlss"`, `"xess"`), unique in the registry, logged as `upscaler=<name>` |
| `available()` | Cheap after the first call: loads the runtime DLL (LoadLibrary, never linked), checks GPU/extension support. False when the DLL is missing, so the game runs with no vendor DLLs present |
| `caps()` | Static table of quality modes, best first. DLSS: DLAA 1.0, Quality 0.667, Balanced 0.58, Performance 0.5, Ultra Performance 0.333. XeSS: Native AA 1.0, Ultra Quality 0.77, Quality 0.67, Balanced 0.58, Performance 0.5. MV units: both want render-pixel vectors, so `RenderPixels`, sign {1,1} for the engine's previous-minus-current UV vectors. Check the sign on the first capture |
| `jitterPhaseCount` | Override only if the vendor guide differs from 8 * (display/render)^2 |
| `mipLodBias` | Override only if the vendor guide differs from log2(render/display) - 1 |
| `prepare(frame)` | Create or resize the feature/context for the frame's extents, reset history when the input images changed. Return false on any failure, and stay failed (present falls back to native in the same frame). Honour the image-layout contract in the header |
| `dispatch(frame)` | Record the evaluate into `frame.commandList`, restore input layouts, leave the output in GENERAL and visible to fragment reads |
| `output()` | Display-size output image and view |
| `resetHistory()` | Next dispatch resets history (targets rebuilt, camera cut) |
| `deactivate()` | Optional: drop per-session state when present stops using the provider |
| `releaseResources()` | Free every GPU object; called from present teardown before the device goes. The destructor must not call Vulkan (static exit) |

Vendor notes:

- DLSS via Streamline: Streamline hooks the swapchain and command lists by default. Use its manual-hooking
  mode (`slGetFeatureFunction`, `slEvaluateFeature` on our command buffer) so present keeps ownership. DLSS wants
  depth in the engine's convention (flag inverted depth), plus `sl::Constants` per frame (camera matrices,
  jitter, mv scale), which maps onto `UpscalerFrame` with the camera fields extended as needed.
- XeSS Vulkan: `xess_vk.h`, `libxess.dll`. The device must be created with `VK_EXT_mutable_descriptor_type` and
  the extensions `xessVKGetRequiredInstanceExtensions` / `xessVKGetRequiredDeviceExtensions` return, so that
  part touches `vulkan_context.cpp` once (instance and device creation), not present.

## SDK and licence notes (do not add an SDK without these)

- All vendor runtimes are runtime-loaded DLLs, render-only, no mod impact. Keep the DLLs out of git (>1 MB rule):
  a pinned-URL fetch script into `3rdparty/`, and `deploy_payload` copies them next to `mc2.exe`. The game runs
  with none present.
- AMD FidelityFX SDK (shipped): MIT-listed files only, a licence receipt per vendored file; the ML upscaler
  ships only as signed prebuilt DLLs loaded through ffx-api.
- NVIDIA Streamline 2.14.16 (DLSS SR, DLAA; RTX only): redistribution only as object code inside an app with
  material additional functionality, attribution and RTX marks in the splash or About screen, and NVIDIA must be
  notified before a commercial release. The DLSS SDK licence sec. 4e forbids use that makes it "subject to an
  open source software license", and supplements 1 and 7.1b add NVIDIA-only and splash-mark terms. DLSS waits
  for an owner licence ruling covering an open-source engine port plus mod ZIPs.
- Intel XeSS: any DP4a GPU; Intel simplified licence (reproduce the notice), unmodified binary only.
- Policy (memory decision): every vendor upscaler beyond FSR is an opt-in user option; frame generation is
  deferred.
