# VK-INTEROP-BRIDGE-1 (v2) — Composable zero-copy GL↔VK interop SEAM, proven live on EdgeFog color

**Branch / worktree:** `claude/vk-interop-bridge-1` @ `A:/Games/mc2-vk-interop-bridge`
**Revision:** v2 (resolves adversarial + meta reviews; supersedes v1 in git history).

## T-minus-1 RESULT — GO (2026-07-03, executed)

Baseline oracle PROVEN. Readback EdgeFog island (`MC2_VULKAN_EDGE_FOG_ISLAND=1`, `MC2_OOB_FOG=0`)
renders edge-fog COLOR **byte-identical** to GL on all 11 mc2_01 bookmarks — incl. the transpose-
sensitive oblique set `{oblique_fog, oblique_horizon, cliff_side_on, cross_map_view}` (indices
5,6,8,10) + dedicated `edge_fog_boundary_south`(3), `edge_fog_corner_southwest`(4). Health:
`edgefog_used_vulkan=6430/6430 validation_errors=0`. fog_oob isolated via `MC2_OOB_FOG=0`
(`gos_postprocess.cpp:647`; log `[OOB_FOG v1] enabled=0`). Bookmarks live in `tests/visual/bookmarks/mc2_01.json`.
The zero-copy path must reproduce THIS island output byte-for-byte.

### Corrections folded in from T-minus-1:
- **CMake pre-existing defect (branch bug, unrelated to interop):** commit `95095a94` added
  `GameOS/gameos/vulkan_shadow_island.cpp` but never registered the TU → island variant fails to
  LINK (`LNK2019`/`LNK1120`). Fix = `list(APPEND SOURCES vulkan_shadow_island.cpp)` under the
  `if(MC2_VULKAN)` block in `GameOS/gameos/CMakeLists.txt`. Already applied in the worktree; commit
  it separately as a branch fix.
- **Proof-(b) command FIX:** the readback island is `runEdgeFogVulkan`, exercised by FEATURE-GATE
  mode (`MC2_VULKAN_EDGE_FOG_ISLAND` OFF vs ON), NOT `--backend-compare --region PostprocessFog`
  (that drives the region-iface `runFogRegionThroughBackend`). Correct reproduce command:
  ```powershell
  $env:MC2_OOB_FOG="0"; $env:MC2_VULKAN_VALIDATION="sync"
  py -3 scripts/run_golden_parity.py mc2_01 MC2_VULKAN_EDGE_FOG_ISLAND --gate-value 1 --exe <islandExe>
  ```
  The interop parity run = same command with `MC2_VK_INTEROP_BRIDGE=1` added, diffed vs the gate-ON
  baseline (interop must == readback island output). Note: harness masks `cross_map_view` (floor
  drift even OFF-vs-OFF) — supplement with a direct `cmp` of that bookmark's PNGs.
- **Cleanup:** delete the stale `[VK_EDGE_FOG] MICRO-CHECK ... island(rowmajor) ... diverge=yes
  EXPECTED` diagnostic (dead text, contradicts byte-identical pixels) while wiring T3/T4.

## What this slice IS (reframed — meta C)

This is **not** "an optimization" or "a piece migrated." It is: **prove a composable,
reusable zero-copy GL↔VK image interop SEAM works live in the MC2 frame loop**, using the
smallest possible byte-exact consumer (EdgeFog **color**). The deliverable is a reusable
`VkImageResource` (+ `GlInteropView` adapter) that later slices chain WITHOUT a GL round-trip.
The pixel result is the *proof*, not the *point*. Commit/PR wording must say "zero-copy interop
seam proven live," never "edge-fog made faster."

**The real next atom (name it now, do NOT build here):** *two Vulkan passes sharing one
`VkImageResource` zero-copy with NO GL round-trip between them* — that is the actual
"parallel renderer" unit. This slice de-risks its resource+sync primitive.

## Ladder — where present ownership sits (meta E)

```
[THIS] interop SEAM: one shared image crosses GL↔VK zero-copy, one VK consumer, GL still owns present
   ↓
VK↔VK zero-copy CHAIN: ≥2 VK passes share one VkImageResource, no GL between them (meta A/D primitive proven)
   ↓
NATIVE SUBGRAPH covers a whole screen region entirely in VK (fog edge+oob fused, no per-pass GL bridge)
   ↓
VK-OWNS-PRESENT: surface flip OR final shared-image→GL-backbuffer blit (window ownership) — L6.6 CompositePresent already scaffolds a SEPARATE vk window; blocked on this interop ladder
```
Nobody may read this slice as progress toward a flippable backend. It is step 1 of 4.

---

## T-minus-1 BLOCKER (resolved by investigation) — the readback island is NOT a valid parity oracle today

**Adversarial was right to challenge the "parity-proven" premise. Findings from git:**

1. **The `:869-918` MICRO-CHECK comment is STALE.** It predates the fix. Commit
   **`ab8cfc7b`** ("fix invViewProj transpose (column_major) + resurrect camera-view parity
   oracle", 2026-07-02) states verbatim: the old `row_major` UBO qualifier was **the bug**;
   the honest pre-tautology values were **GL=-15.39 vs island=669.57** (row_major broken).
   The "divergence EXPECTED" comment is explicitly "kept for the record… pre-column_major."
   **`443211d4`** (VK-FOG-FIXTURE-COLMAJOR-1) aligned the fixture CPU ref to column_major
   ("the correction that should have shipped with ab8cfc7b; probe green again"). So
   column_major is CURRENT; the row_major comment is dead text.

2. **The standalone EdgeFog island TU is INACTIVE on the live build.** `ab8cfc7b` body:
   *"these standalone islands are INACTIVE on this build (the fog work is fused into
   PP_SUBGRAPH); the check is diagnostic only."* The live fog path is
   `runPostprocessSubgraph()` via `runFogRegionVulkanBackend` (gos_postprocess.cpp:2869),
   NOT `runEdgeFogVulkan()`. `runEdgeFogVulkan` is reachable only at gos_postprocess.cpp:3798
   when `MC2_VULKAN_EDGE_FOG_ISLAND=1` AND the subgraph gate is off — a legacy per-pass route.

3. **PostprocessFog is NOT byte-exact today, even after the fix.** `ab8cfc7b` proof section:
   after column_major, edge-fog *geometry* is corrected, but a **RESIDUAL GL-vs-VK divergence
   remains on oblique views** (oblique_horizon 57% px, oblique_fog 31%, cliff_side_on 9%,
   max|diff|~45). This is **fog_oob** — a procedural 3D-value-noise pass that is **NOT
   byte-exact across AMD-GLSL vs glslang-SPIR-V** (ladder S5/S6, same class as cloud/shoreline).
   Supervisor ruled the residual an acceptable **perceptual** pass; a real automated gate needs
   the deferred SSIM harness. **There is NO passing byte-exact `RESULT: PASS … within_floor`
   line for PostprocessFog as a whole** (grep of VULKAN-NEXT-SLICES.md finds none; the only
   green backend-compare RESULT lines are PostprocessSSAO N0 and PostprocessHzb).

**Consequence — zero-copy on a non-byte-exact oracle proves nothing.** If we re-home the whole
fog region and diff it, fog_oob's inherent noise divergence masks any bridge-introduced error.
**So the oracle for THIS slice is edge-fog COLOR ONLY, on transpose-SENSITIVE oblique bookmarks,
excluding fog_oob.** Edge-fog color is byte-exact after the column_major fix (fog_oob is the only
non-exact component). We prove the bridge against the byte-exact component and explicitly scope
out fog_oob.

**T-minus-1 task (do FIRST, before any bridge code):** establish the edge-fog-color byte-exact
baseline on the ACTIVE path with the CURRENT (column_major) build:
- Reactivate/route the per-pass EdgeFog island (`MC2_VULKAN_EDGE_FOG_ISLAND=1`, subgraph gate
  OFF, region-iface OFF) so `runEdgeFogVulkan()` (readback path) actually runs — confirm via
  `[VK_EDGE_FOG]` health lines (attempted>0, used_vulkan>0).
- Run `run_golden_parity.py mc2_01 --backend-compare --region PostprocessFog` on the
  **transpose-revealing oblique bookmarks** authored in `ab8cfc7b` (`oblique_fog`,
  `oblique_horizon`, `cliff_side_on`, `cross_map_view`) AND confirm the **edge-fog-color**
  contribution is byte-exact there (isolate from fog_oob — see "Proof" for how).
- If the readback island's edge-fog color is NOT byte-exact on oblique views → STOP and fix
  THAT first; the bridge slice cannot proceed on a broken baseline.
- Record the passing RESULT line + `region_impl` verbatim in the plan/commit. **This baseline
  is the thing zero-copy must reproduce.**

---

## Strategic reshape of the abstraction (meta A) — split core resource from GL adapter

**v1 fused VkImage + memory + VK semaphores + GL memobj + GL tex + GL semaphores into one
struct. That is hybrid-boundary code that gets deleted in the endgame** (most shared images will
have NO GL side once VK↔VK chains exist). Split it:

### `VkImageResource` (core — new `GameOS/gameos/vk_image_resource.{h,cpp}`)
The reusable, GL-free VK-owned image: **raw-allocated** (NOT VMA — blocker #1) so it is
external-export-capable by construction, plus its VK-side sync objects. Survives into the
VK↔VK-chain slice unchanged.

```cpp
struct VkImageResource {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;      // raw vkAllocateMemory (dedicated, exportable)
    VkDeviceSize   memSize = 0;
    VkImageView    view   = VK_NULL_HANDLE;
    uint32_t       w = 0, h = 0;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    bool           exportable = false;            // created with VkExternalMemoryImageCreateInfo
    HANDLE         memWin32 = nullptr;            // valid only if exportable
    // VK↔VK / VK↔GL sync objects live here so the design survives the chain slice (meta D).
    // Double-buffered binary semaphore PAIRS + per-frame fences (blocker #4).
    VkSemaphore    doneSem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};  // producer→consumer
    VkFence        frameFence[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    HANDLE         doneSemWin32[2] = {nullptr, nullptr};           // valid only if exportable
    uint32_t       frameParity = 0;               // ^=1 each frame; picks [frameParity]
};

// Raw-allocated exportable image (spike main.cpp:395-440 sequence, NOT vmaCreateImage).
bool vkImageResourceCreate(VkImageResource*, VkDevice, VkPhysicalDevice,
                           uint32_t w, uint32_t h, VkFormat, VkImageUsageFlags,
                           bool exportable /*OPAQUE_WIN32 + dedicated exported mem + exported sems*/);
void vkImageResourceDestroy(VkImageResource*, VkDevice);
// VK↔VK chaining sync (meta D): begins a frame on the [parity] slot, returns the fence to
// reset + the semaphores to wire into VkSubmitInfo. Producer signals doneSem[parity];
// the next VK consumer waits doneSem[parity] via a pipeline barrier / submit wait. NO GL,
// NO CPU stall between VK passes — this is the primitive the chain slice reuses verbatim.
struct VkFrameSync { VkFence fence; VkSemaphore signal; uint32_t parity; };
VkFrameSync vkImageResourceBeginFrame(VkImageResource*);
```

### `GlInteropView` (adapter — new `GameOS/gameos/gl_interop_view.{h,cpp}`)
Used **only at a GL boundary**; imports one `exportable` `VkImageResource` into GL. Deletable
the day the boundary moves fully into VK.

```cpp
struct GlInteropView {
    GLuint glTex = 0, glMemObj = 0;
    GLuint glSemVkToGl = 0, glSemGlToVk = 0;   // one pair; double-buffer handled by re-import? NO:
    // sems are per-VkImageResource doneSem slots imported once; GL waits the slot VK signalled.
    GLenum glInternalFormat = 0;
    const VkImageResource* src = nullptr;
};
// Import an exportable VkImageResource's memory + doneSem[] into GL (spike main.cpp:472-508).
// Asserts src->exportable. Returns false fail-soft.
bool glInteropViewCreate(GlInteropView*, const VkImageResource* exportableSrc,
                         GLenum glInternalFormat);
void glInteropViewDestroy(GlInteropView*);
// GL hands the shared image to VK (spike main.cpp:622-624). layout = GL_LAYOUT_*_EXT.
void glInteropSignalToVk(const GlInteropView&, GLenum glDstLayout, uint32_t parity);
// GL waits for the VK producer before touching the tex again (spike main.cpp:572-574).
void glInteropWaitFromVk(const GlInteropView&, GLenum glSrcLayout, uint32_t parity);
```

This split **resolves blocker #1 for free**: the core is raw-allocated by design; VMA is never in
the shared-image path.

---

## Blocker resolutions (adversarial)

### #1 — raw allocation, and how the external image view replaces the VMA colorImage
- `vkImageResourceCreate(exportable=true)` uses the spike sequence **exactly**
  (`main.cpp:395-440`): `VkExternalMemoryImageCreateInfo{handleTypes=OPAQUE_WIN32}` in the
  `VkImageCreateInfo.pNext`; `vkGetImageMemoryRequirements`; `findMemType(DEVICE_LOCAL)`;
  `VkMemoryDedicatedAllocateInfo{image}` → `VkExportMemoryAllocateInfo{OPAQUE_WIN32}` pNext
  chain → **`vkAllocateMemory`** (raw, NOT `vmaCreateImage`) → `vkBindImageMemory` →
  `vkGetMemoryWin32HandleKHR`. Then `vkCreateImageView` over it.
- **Framebuffer/descriptor/render-pass swap (island `build_island`):** when interop is on,
  the color `VkImageResource.view` (exportable) **replaces** the current VMA `colorImage`/`colorView`
  (:295, :560) as the render-pass attachment 0. Point `VkFramebufferCreateInfo.pAttachments`
  (:638) at `colorResource.view`. The depth descriptor (:682-696) is UNCHANGED in the color-only
  slice (depth stays VMA + readback — blocker #3). The VMA `colorImage` allocation is simply not
  created when interop is on (no wasted memory).
- **initialLayout reconcile (:605 `COLOR_ATTACHMENT_OPTIMAL`):** an exportable image is created
  `initialLayout=UNDEFINED` (spike :409). GL produces the scene color into it and hands it over in
  `GL_LAYOUT_COLOR_ATTACHMENT_EXT` (the GL→VK signal layout). The render pass' `initialLayout`
  stays `COLOR_ATTACHMENT_OPTIMAL` — which now matches the layout GL released it in, so the
  first-use barrier from UNDEFINED is gone (the cross-API semaphore carries the hand-off). The
  render-pass `finalLayout` changes from `TRANSFER_SRC_OPTIMAL` (:606, was for the readback copy)
  to `SHADER_READ_ONLY_OPTIMAL` or `COLOR_ATTACHMENT_OPTIMAL` — the layout GL will re-consume it
  in via `glInteropWaitFromVk` — because there is **no** `vkCmdCopyImageToBuffer` anymore.

### #2 — instance-level exts + LUID match
- **Instance exts (T2):** `VkInstanceCreateInfo` must add `VK_KHR_EXTERNAL_MEMORY_CAPABILITIES`,
  `VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES`, `VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2` (spike
  `main.cpp:238-243`) — the island `build_island` instance (:434-442) currently adds none of these.
- **Device exts:** the six from spike `main.cpp:318-325` (`EXTERNAL_MEMORY[_WIN32]`,
  `EXTERNAL_SEMAPHORE[_WIN32]`, `DEDICATED_ALLOCATION`, `GET_MEMORY_REQUIREMENTS_2`) on the
  `VkDeviceCreateInfo` (:498-503).
- **LUID match (blocker):** the island picks `devs[0]` (:473) — can be the wrong GPU. Before any
  export/import, query `VkPhysicalDeviceIDProperties.deviceLUID` via
  `vkGetPhysicalDeviceProperties2` (spike `main.cpp:291-305`) and match it against the GL device's
  LUID (`GL_EXT_memory_object_win32` exposes the GL LUID via `glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)`).
  If LUIDs differ → decline interop (fail-soft to readback). Minimum bar if GL LUID query is
  unavailable: assert single-GPU (`vkEnumeratePhysicalDevices` count == 1) else decline.

### #3 — depth OUT of the mainline (color-only slice)
- **Ship COLOR-ONLY zero-copy.** Depth stays on the existing readback path
  (`glGetTexImage(GL_DEPTH_COMPONENT)` :864 → staging → `vkCmdCopyBufferToImage` :988).
- **Honest latency scope:** because depth still round-trips, the per-frame CPU stall is **NOT
  eliminated** in this slice. The claim is narrowed to: *the color readback + color upload
  (`glGetTexImage(GL_RGBA)` :866, `vkCmdCopyImageToBuffer` :1016, `glTexSubImage2D` :1043) are
  gone; `readbackUs`→0; `copyDepthUs` drops by the color half.* Depth zero-copy is a **separate
  probe-gated future slice** (de-risked by T0 below).

### #4 — semaphore lifetime (no "builder's call")
- **Double-buffered binary semaphore PAIRS + per-frame fence**, chosen for simplicity over a
  timeline semaphore (timeline is the follow-on if profiling shows contention). `VkImageResource`
  holds `doneSem[2]` + `frameFence[2]`; `frameParity ^= 1` each frame. Frame N uses slot
  `parity`; before reusing slot `parity` in frame N+2, `vkWaitForFences(frameFence[parity])`
  guarantees the prior submit on that slot retired, so the binary semaphore is un-signalled and
  safe to re-signal. GL waits/signals the **same slot** VK used (`parity` threaded through
  `glInteropSignalToVk`/`glInteropWaitFromVk`). This is the exact L5 present-semaphore-reuse bug
  class (MEMORY.md) — the double-buffer + fence is the fix, specified, not deferred.

### #5 — HARD in-code zero-readback assertion
- Add debug counters incremented at the call sites and **asserted == 0** on the interop path:
  a global `struct { unsigned vkCmdCopyBufferToImage, vkCmdCopyImageToBuffer, glGetTexImage,
  glTexSubImage2D; } g_interopForbidden;` incremented wherever those four are issued in the fog
  TU. At end of `runEdgeFogVulkan()` interop branch: `if (useInterop && any(g_interopForbidden)!=0)
  { setFallback("interop_did_readback"); disable island; abort frame + log LOUD; }` — the run
  FAILS (falls back + flags) if any copy/readback slipped through. Parity-green alone can pass while
  silently doing readback (adversarial #5); this assertion is the guard. Surface the counters in the
  `[VK_EDGE_FOG]` health line so the proof harness asserts them ==0 externally too.

### #6 — OFF-path never probes
- `interopSupported()` / LUID query / any external probe are called **only inside the
  `MC2_VK_INTEROP_BRIDGE==1` branch** (gate resolved once, like :806-811). Gate OFF → not one
  external-ext call, not one probe → the instance/device are created with the ORIGINAL ext lists →
  behavior byte-identical to today's readback island. Proof: shipping `build64` (gate compiled but
  OFF at runtime) `cmp` byte-identical to a pre-slice build; and the gate-OFF island run emits no
  `[VK_INTEROP]` probe lines.

---

## T0 — depth-export micro-spike (meta B; de-risks the FUTURE depth slice, standalone)

Extend `tools/vk_gl_interop_spike/main.cpp` (or a sibling `main_depth.cpp` + a second CMake
target) to share a **`VK_FORMAT_D32_SFLOAT`, `VK_IMAGE_USAGE_SAMPLED_BIT` depth image** ↔
`GL_DEPTH_COMPONENT32F` GL texture, one direction (VK writes/clears depth → GL samples → verify),
reusing the exact export/import sequence. **Runs BEFORE any island code.** Exit-0 = depth export
works on the 7900 XTX → the future depth slice is viable. Non-zero = depth-export unsupported →
the depth slice needs a different strategy (learned in an afternoon, not mid-island). This slice
**ships color-only regardless** of T0's result; T0 only informs the *next* slice.

---

## Gating

- **`MC2_VK_INTEROP_BRIDGE`** default-OFF, resolved once. OFF → `useInterop=false` → readback path,
  byte-identical, zero probes (#6). Requires `MC2_VULKAN_EDGE_FOG_ISLAND=1` (the per-pass island
  must be the selected route at :3798; subgraph + region-iface gates OFF). Register + allowlist +
  document in `docs/tier1_env_vars.md` (gate_status verdict before smoke).

## Build

Island variant, `build64_island`, `MC2_VULKAN=ON -DMC2_VULKAN_ISLAND=ON`:
```powershell
$CMAKE = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
& $CMAKE -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=A:/Games/mc2-vk-interop-bridge/3rdparty `
  -DCMAKE_LIBRARY_ARCHITECTURE=x64 -DMC2_VULKAN=ON -DMC2_VULKAN_ISLAND=ON -DMC2_IMGUI=ON -B build64_island
& $CMAKE --build build64_island --config RelWithDebInfo --target mc2
```
**TUs to add** (all inside the existing `if(MC2_VULKAN_ISLAND)` block, `GameOS/gameos/CMakeLists.txt`
after :110): `vk_image_resource.cpp`, `gl_interop_view.cpp`. Both `#ifdef MC2_VULKAN_ISLAND`; no new
CMake option; no new link libs (volk + GLEW already linked into `gameos`). Full relink on the
`VulkanEdgeFogIsland` struct-layout change. T0 spike is a separate target in
`tools/vk_gl_interop_spike/CMakeLists.txt`, built with its documented standalone commands.
Deploy (LM5 spv lockstep, LM8 cmp) to lane `mc2-win64-v0.4c` via `deploy_payload.py --build64-root
build64_island`, then `cmp` exe.

## Proof plan (hard-asserted, honestly scoped)

**(a) Baseline first (T-minus-1):** readback island, current column_major build, edge-fog-color
byte-exact on `oblique_fog`/`oblique_horizon`/`cliff_side_on`/`cross_map_view`. Record RESULT line.

**(b) Interop parity vs that baseline — EDGE-FOG COLOR ONLY:**
```powershell
cmp build64_island/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe   # LM8 pre
$env:MC2_VULKAN_EDGE_FOG_ISLAND="1"; $env:MC2_VK_INTEROP_BRIDGE="1"; $env:MC2_VULKAN_VALIDATION="sync"
py -3 scripts/run_golden_parity.py mc2_01 --backend-compare --region PostprocessFog `
  --exe A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe
```
- **Isolate edge-fog from fog_oob:** fog_oob is not byte-exact (proven). Either (i) run the
  bookmarks where fog_oob contributes ~0 (map-interior oblique poses with no OOB/sky coverage —
  `cliff_side_on` had only 9% and that was fog_oob-dominated; author a fog_oob-free oblique pose if
  needed), OR (ii) temporarily gate fog_oob off for the diff so ONLY edge-fog color is compared.
  The bridge must make edge-fog color **bit-identical to the (a) baseline** (NOT to GL — to the
  readback island's own output, since that is what zero-copy is reproducing). Interop-ON vs
  readback-baseline byte-exact = the bridge is transparent. LM6: FallbackGL→exit5 ≠ pass; quote
  RESULT + `region_impl` + the forbidden-counters.
- Also: interop-**OFF** run reproduces the (a) baseline exactly (readback path unregressed).

**(c) Liveness / zero-readback (hard):**
- `[VK_EDGE_FOG]` health carries `edgefog_used_interop>0` AND
  `forbidden_copyBufToImg=0 forbidden_copyImgToBuf=0 forbidden_getTexImage=0 forbidden_texSubImage=0`
  (the #5 counters). Harness asserts all four ==0 externally; the in-code assert (#5) fails the run
  otherwise. `readbackUs≈0`, `copyDepthUs` dropped by the color half (honest, depth still copies).
- `MC2_VULKAN_VALIDATION=sync` → island `validation_errors=0` (catches the R-A-W hazard the ladder
  flagged; the double-buffer+fence + render-pass external dependency :616-622 must be clean).

**(d) OFF-path byte-identical + no-probe (#6):** `cmp` shipping `build64` mc2.exe against a
pre-slice build → identical. Gate-OFF island run log has zero `[VK_INTEROP]` probe lines. tier1
smoke (island exe, all gates off) no regression:
```powershell
$env:MC2_DEBUG_STATE_DUMP="1"; py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs `
  --exe A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe   # NEVER --kill-existing / >30 / --with-menu-canary
```

**STOP rule:** any byte-exact failure (edge-fog color vs baseline), any forbidden-counter != 0, or
any sync-validation error → gate stays default-OFF **forever**, slice not shipped (arc precedent:
the rejected StaticProp↔Mech reorder). Parity-green-with-readback is a FAIL, not a pass.

## Risks / open questions

- **fog_oob is the confounder, not a bug to fix here.** The AMD-GLSL vs glslang noise divergence is
  ledgered (S5/S6) and supervisor-ruled perceptual. This slice must not try to make fog_oob
  byte-exact; it must *exclude* it from the oracle. If a fog_oob-free oblique bookmark can't isolate
  edge-fog color cleanly, escalate before coding — the proof is void otherwise.
- **Depth-as-shared-image** deferred to a probe-gated slice (T0 informs it). Color-only ships now.
- **LUID / wrong-GPU** (#2): matched or single-GPU-asserted before import.
- **Layout/format pairing:** color `R16G16B16A16_SFLOAT`↔`GL_RGBA16F`, `TILING_OPTIMAL`↔
  `GL_OPTIMAL_TILING_EXT` (spike :406/:494). GL signal/wait `layouts` array must match the VK barrier
  `oldLayout` (spike :572/:622).
- **Semaphore reuse** (#4): double-buffered pairs + per-frame fence, specified.
- **Win32 handle ownership:** `glImportMemoryWin32HandleEXT` does not take ownership (spike
  :482-483); the resource keeps + `CloseHandle`s its own handles at teardown. No double-close.
- **All GL `*EXT` on the frame-loop GL-context thread** (island already there).

## Task breakdown (v2)

- **T0 — depth-export micro-spike** (`tools/vk_gl_interop_spike`, standalone). D32_SFLOAT sampled
  VK→GL. Exit-0/non-zero de-risks the FUTURE depth slice. Ships nothing into the island. *Proof:
  spike exit code + log.*
- **T-minus-1 — baseline parity** (no new code). Reactivate readback EdgeFog island on current
  column_major build; prove edge-fog COLOR byte-exact on oblique bookmarks (fog_oob isolated).
  Record RESULT line. **STOP if baseline fails.**
- **T1 — `VkImageResource`** (`vk_image_resource.{h,cpp}` + CMake). Raw-allocated exportable image
  (spike :395-440) + double-buffered sem/fence sync + `vkImageResourceBeginFrame`. GL-free. *Proof:
  builds; a create/destroy self-check of one exportable RGBA16F image, validation-clean.*
- **T2 — `GlInteropView`** (`gl_interop_view.{h,cpp}` + CMake). Import exportable resource into GL
  (spike :472-508) + LUID match + signal/wait wrappers. *Proof: builds; import self-check on T1's
  image; LUID assert exercised.*
- **T3 — instance+device exts + gate + struct wiring** in `build_island`. Add instance exts (#2)
  + device exts, all guarded (#6). `MC2_VK_INTEROP_BRIDGE` gate; `useInterop` = gate ∧
  interopSupported ∧ LUID-ok. Create color `VkImageResource`(exportable) + `GlInteropView`; repoint
  framebuffer attachment 0 (#1); reconcile initialLayout/finalLayout (#1). Depth UNCHANGED (#3).
  Register/allowlist/document gate. *Proof: gate-ON init OK, gate-OFF byte-identical + no probe.*
- **T4 — color-only per-frame interop path** in `runEdgeFogVulkan`. GL renders scene color into
  `GlInteropView.glTex` → `glInteropSignalToVk` → UBO (unchanged) → cmd buf with NO color copy
  (depth copy stays) → double-buffered semaphore submit + fence → `glInteropWaitFromVk` → use the
  shared tex as scene color. Forbidden-counters + hard assert (#5). Health counters. *Proof: (c)
  liveness — counters==0, sync-validation clean, used_interop>0.*
- **T5 — full proof + STOP-gate.** Run (a)/(b)/(c)/(d); LM8 bracket; quote RESULT + region_impl +
  forbidden-counters + val=0. STOP-and-default-OFF-forever on any failure. Commit wording = "zero-copy
  interop SEAM proven live (color)", name the VK↔VK-chain next atom + the ladder.
- **T6 (future, not this slice)** — depth zero-copy (probe-gated, T0-informed — DEPTH-EXPORT proven
  VIABLE by T0: D32_SFLOAT sampled bit-exact VK→GL, keep depth dedicated-allocated); VK↔VK zero-copy
  chain (the real parallel-renderer atom, reuses `VkImageResource` sync verbatim); region-iface
  zero-copy.

## Follow-on (logged from T3/T4 build) — VK→GL semaphore back-channel (removes color CPU stall)

T4's color path is zero-**copy** but not yet zero-CPU-**stall**: `VkImageResource` exposes ONE binary
semaphore per parity slot (`doneSem[parity]`, GL→VK direction only). A binary semaphore can't be both
waited and signalled by one VK submit, so the VK→GL hand-off currently uses the per-frame **fence**
(a CPU wait before GL re-samples — NOT a readback, no forbidden counter, validation-clean). This matches
the slice's narrowed latency claim (depth round-trips regardless). CLEAN FIX (small, arc-wide value):
add a second exportable VK→GL semaphore slot to `VkImageResource` (+ import it in `GlInteropView`) so
the VK→GL hand-off is a GPU semaphore wait, no CPU stall. Do this when the VK↔VK-chain / piece-#2 work
touches the sync design — it wants the same back-channel.
