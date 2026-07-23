# VK-P8 SHADOW RECON — first geometry region (VK produces, GL consumes)

Worktree `A:/Games/mc2-vk-interop-bridge`, branch `claude/vk-interop-bridge-1`, HEAD `19cf4fb8`.
Scaffold origin: VK-REGION-SHADOW-1 sub-slice A, commit `95095a94`.

---

## VERDICT

**P8 is a REAL machinery lift, not a small step — but the smallest first sub-slice IS tractable.**
The postprocess seam was "no geometry, GL produces / VK consumes color". P8 is the mirror image on
BOTH axes at once: **VK PRODUCES** (direction flip) **AND needs a geometry feed** (new machinery).
Doing both in one slice is the trap. The scaffold (`vulkan_shadow_island.cpp`) is *only* a pipeline
builder + headless synthetic-quad fixture — there is **no live path, no live gate, no real caster
feed, and it builds its own throwaway device**. Everything live is still to build.

**Recommended first sub-slice (P8a): terrain-only, single-cascade, static caster feed.**
Do NOT start with mechs (bone-skinning in VK is P12-heavy) or the 3-layer CSM array. Prove the
**seam direction flip first** with the least geometry: one VK-produced D32 shadow map from a small,
CPU-supplied static instance set, GL samples it. Format is the easy part (D32 dodges the P5 blocker).
The geometry feed is the crux.

**Is shadow the right first geometry pick vs water?** Water (P9) is arguably EASIER as a first
*geometry* region: WATER-REGISTRY-0 buffers are already registered, WATER-MVP-ORACLE-1 already proved
column-major MVP + reverse-Z, water is GL-produces/VK-consumes (same direction as postprocess, no flip),
and its output is a COLOR contribution (RGBA16F, the proven seam) not a depth map GL must sample with
compare-mode. **Shadow's only advantage is the pipeline scaffold already exists.** If the goal is
"smallest first geometry step", water is the safer pick; if the goal is "exercise the vk2gl back-channel
+ depth-produce seam that P14 present-ownership will also need", shadow is more strategically load-bearing.
**Flag this as a user decision in the plan.**

---

## 1. SHADOW ISLAND STATE (`vulkan_shadow_island.cpp`, 203 lines)

- **What exists:** ONLY `mc2_vulkan_build_shadow_depth_pipeline()` + its destroy. A depth-only,
  instanced graphics-pipeline builder (island:43-191). NO color attachment; `depthTestEnable`+
  `depthWriteEnable`=TRUE, `depthCompareOp=LESS` (island:149-151); vertex input = one `vec3` position
  stream drawn INSTANCED (island:110-124); set0 = per-frame view/cascade UBO, set1 = instance ring as
  `STORAGE_BUFFER_DYNAMIC` (island:56-84).
- **What it produces:** nothing by itself — the *fixture* `mc2_vulkan_probe_shadowdepth_fixture()`
  (in `vulkan_backend_skeleton.cpp:~6395`) drives it into (a) a single D32 shadow map and (b) a 3-layer
  CSM D32 array, reads depth back, validates BIT-TIGHT vs a CPU reference (tolerance 0, geometry chosen
  so projected depths are binary-exact). Header contract at `vulkan_backend_skeleton.h:150-188`.
- **Formats:** `kDepthFmt = VK_FORMAT_D32_SFLOAT` (fixture render pass, skeleton.cpp:~6417); depth-only
  render pass, `finalLayout = TRANSFER_SRC_OPTIMAL` for readback. CSM target = D32 `TEXTURE_2D_ARRAY`-equivalent.
- **What is NOT done (sub-slice B):** the LIVE path. No live render into a shared image; no live gate;
  no real caster geometry; the fixture spins up its **own instance+device+volkLoadDevice** (skeleton.cpp
  ~6377, `vkCreateDevice`). **Fixtures-only. Not wired into any live gate.** The read side
  (`sampler2DShadow` compare) was proven separately by `shaders/vulkan/compare_shadow.frag` +
  `csm_array_shadow.frag` fixtures.

## 2. GL SHADOW PATH (producer + consumer) — the seam target

- **Producer (caster depth pass):** `gosRenderer::beginDynamicShadowPass()` /
  `endDynamicShadowPass()` — `gameos_graphics.cpp:6596` / `6678`. Binds the dynamic-shadow FBO
  (`pp->getDynamicShadowFBO()`), viewport = `getDynamicShadowMapSize()`, forward-Z (`glClearDepth(1.0)`,
  `GL_LESS`) — note the scene is reverse-Z, shadow stays forward-Z. Casters drawn via
  `drawShadowBatchTessellated()` (`gameos_graphics.cpp:6398`, tessellated terrain casters) +
  mech/prop casters. Uploads `lightSpaceMatrix` (skeleton.cpp:6669). Static prepass is
  `beginShadowPrePass` (once/mission); dynamic is per-frame.
- **The shadow TEXTURE (the P8 shared-image target):** **`GL_DEPTH_COMPONENT24`**, not D24S8 —
  `gos_postprocess.cpp:5214-5215` (dynamic single) + `5290-5292` (CSM `GL_TEXTURE_2D_ARRAY`) +
  `4883-4884` (static full). Created with `glTexImage2D(..., GL_DEPTH_COMPONENT24, ..., GL_DEPTH_COMPONENT,
  GL_FLOAT, ...)`. (The 933/1249 `GL_DEPTH24_STENCIL8` hits are the SCENE depth / water-refl, NOT the
  shadow map — those were the P5 blocker; the shadow map is stencil-free.)
- **Consumer (sampling):** `shaders/include/shadow.hglsl` — `uniform sampler2DShadow shadowMap;`
  `calcShadow()` (shadow.hglsl:33) with 16-tap Poisson PCF. Sampled with hardware compare:
  `GL_TEXTURE_COMPARE_MODE = GL_COMPARE_REF_TO_TEXTURE`, `GL_LEQUAL` (toggled off during the write pass,
  gameos_graphics.cpp:6614-6618 / restored 6685-6688). Consumers: `gos_terrain.frag`,
  `terrain_lod_chunk.frag`, `terrain_overlay.frag`, `gos_grass.frag`, mech PBR.

### DIRECTION FLIP (the defining P8 characteristic)
Postprocess: **GL produces → VK consumes** color; VK never signals GL (present_vk.cpp:582-588 notes GL
is a pure producer, vk2gl NOT signalled). **P8 REVERSES it: VK produces the shadow depth → GL consumes
it** in terrain/mech shading. This is **exactly the use case for the P1.5 vk2gl back-channel**:
`glVkToGlSem[2]` + `glInteropWaitFromVk()` (`gl_interop_view.h:28-31,53-57`) — VK submit SIGNALS, GL
WAITS on the GPU (no CPU fence). The primitive EXISTS and is unused on the color path — P8 is its first
real consumer. Confirmed present-vk deliberately left it dormant; P8 lights it up.

## 3. GEOMETRY INPUT — the crux ("first geometry region")

The pipeline expects: a `vec3` position vertex buffer (a unit quad in the fixture) + an instance ring
(`STORAGE_BUFFER_DYNAMIC`, per-instance transforms) + a per-frame UBO (`lightSpace[]` + cascadeIndex).
Postprocess needed NONE of this — it was a full-screen shader over an input image. **P8 must build a
real caster feed:** extract the terrain/prop caster instance transforms the GL path currently draws
(the `drawShadowBatchTessellated` batch + prop instances) and marshal them into the VK instance ring
each frame. This is genuinely new machinery — a per-frame CPU→GPU geometry marshalling path the seam
work has never needed. **This is where the slice cost lives**, not in the pipeline or the format.
Mitigation for P8a: start with a SMALL, STATIC caster set (a handful of static-prop transforms), not
the live tessellated-terrain caster batch — prove the seam, then widen the feed.

## 4. DEVICE — must re-parent onto the shared VulkanContext (P13 T2)

The fixture currently **builds its own device** (`vkCreateDevice` + `volkLoadDevice`,
skeleton.cpp:~6373-6386) — a throwaway. P8 live path **must** borrow the shared context via
`getOrCreateEngineVulkanContext()` (`vulkan_context.h:111`), exactly as `EnginePresentVk::init()` does
(`engine_present_vk.cpp:330-337`: pulls `ctx.instance()/physicalDevice()/device()/graphicsQueue()/
graphicsFamily()`, never a second `volkLoadDevice`). The context header §T2 explicitly names P8 as a
reuse client (vulkan_context.h:8-11). **R2 interlock:** the shared device's `init(wantInterop=true)`
requests the external-memory/-semaphore Win32 extensions (vulkan_context.h:64-68); P8's shared image +
vk2gl semaphore need those, so P8 must assert `wantInterop` was set at context birth. The shared device
created ONLY the gfx queue family (one queue) — P8 records on that same queue (fine; no present needed
for the shadow produce).

## 5. FORMAT — D32 dodges the P5 D24S8 blocker (confirmed)

The VK shadow map should be **`VK_FORMAT_D32_SFLOAT`** (pure depth, no stencil). This **sidesteps the
P5 blocker entirely**: P5 was stuck because the engine's *scene* depth is `GL_DEPTH24_STENCIL8` and
`D24_UNORM_S8_UINT` is NOT externally-shareable on the 7900 XTX (roadmap:85-94). A VK-PRODUCED shadow
map has no such constraint — we choose D32. **T0 already proved the exact transport**:
`tools/vk_gl_interop_spike/main_depth.cpp:46` `VK_FORMAT_D32_SFLOAT` SAMPLED, exported OPAQUE_WIN32,
imported into GL as `GL_DEPTH_COMPONENT32F`, sampled bit-exact (`main_depth.cpp:452-471`, DEPTH_EPS 1e-6).
**Mismatch to reconcile:** the GL shadow texture today is `GL_DEPTH_COMPONENT24` (§2), but T0 proved
`GL_DEPTH_COMPONENT32F`. **The P8 shared image must be D32/DEPTH_COMPONENT32F on BOTH sides** — i.e. the
GL consumer samples a 32F shared texture, not the legacy 24-bit one. `sampler2DShadow` + compare-mode
work identically on 32F (a filtering param, not a storage-format constraint). Confirm
`glInteropViewCreate` accepts a depth `glInternalFormat` (signature takes `GLenum glInternalFormat`,
gl_interop_view.h:41 — passed `GL_RGBA8` for color; pass `GL_DEPTH_COMPONENT32F` here) and that the
VkImageResource depth path sets the DEPTH aspect + a depth-only sampled view (T0's spike is the template).

## 6. ORACLE — how shadow parity is proven

- **Offline (build first, TOOLING CHECKPOINT for "first geometry region"):** the shadow-depth fixture
  ALREADY validates VK depth bit-tight vs a CPU reference (skeleton.cpp fixture, tolerance 0). Extend it
  into the transport-transparency harness pattern (`tools/vk_interop_transport_test`): render the shadow
  depth BOTH ways — (a) readback-bridged, (b) interop-shared VkImageResource + vk2gl — and assert the
  GL-sampled depth is byte-identical + forbidden-counters==0 + validation-clean. This is the missing
  **geometry-region parity tool** the roadmap ledger marks `❌ build at P8 checkpoint` (roadmap:219).
- **Live (final confirmation only):** shadow bookmarks in the golden-parity harness
  (`run_golden_parity.py` + `tests/visual/bookmarks/*.json`) — the shadowed terrain/mech scenes.
  Oracle = "VK-produced shadow map, GL-sampled, yields byte-identical (or within noise-floor) final
  pixels vs the GL-produced shadow map". Depth itself is D32-exact (T0), so the shadow-map bytes should
  match tightly; final-pixel parity absorbs PCF/compare-mode identical on both sides.
- **STOP-gate:** any parity/validation failure → gate stays default-OFF forever, piece not shipped
  (roadmap standing rule). Depth-tie / order sensitivity is NOT a risk here (single depth-write pass,
  compareOp LESS, deterministic) — unlike the framegraph reorder that failed on depth-tie.

## 7. TASK BREAKDOWN + RISKS + biggest unknown

**P8a (recommended first slice — terrain/static-caster, single map, no CSM):**
1. **Tooling first:** extend the shadow-depth fixture into an interop-transport parity harness (readback
   vs shared, D32, GL-sampled). Retire the depth-produce-seam risk offline (seconds), before any launch.
2. Re-parent the pipeline builder onto the shared `VulkanContext` (drop the fixture's own device;
   borrow like `engine_present_vk.cpp:330-337`). Assert `wantInterop`.
3. Build the shared D32 shadow image as a `VkImageResource` (exportable, DEPTH aspect, depth-only sampled
   view) + `GlInteropView` with `GL_DEPTH_COMPONENT32F` + import the vk2gl semaphore.
4. Marshal a SMALL static caster instance set into the instance ring + per-frame lightSpace UBO.
5. VK records the depth-only pass on the shared queue, SIGNALS vk2gl; GL `glInteropWaitFromVk()` then
   binds the shared 32F texture as `shadowMap` (compare-mode) in place of `getDynamicShadowTexture()`.
6. Gate `MC2_VK_SHADOW_INTEROP` (register + allowlist + document), default-OFF, byte-identical gate-OFF exe.
7. Prove: offline harness green → shadow golden-parity bookmarks byte-identical → forbidden-counters==0 →
   `MC2_VULKAN_VALIDATION=sync` clean.

**P8b (later):** widen the feed to the live tessellated-terrain caster batch + the 3-layer CSM array
(the fixture already covers the CSM array target format-wise). **P8c (deferred, heavy):** mech casters —
needs GPU skinning in VK (couples to P12), explicitly out of P8 scope (roadmap:135 "bone-skinned mechs
deferred").

**Risks:**
- **R1 (biggest cost):** the per-frame caster-instance marshalling feed — new machinery with no seam
  precedent. Mitigate by starting static/small (P8a).
- **R2 (biggest UNKNOWN):** does GL's hardware `sampler2DShadow` compare-mode sample an *imported*
  optimal-tiled D32 image bit-identically to a native GL depth texture? T0 proved plain `texture()`
  readback of D32; it did NOT prove the COMPARE-MODE (`GL_COMPARE_REF_TO_TEXTURE`) sampling path over
  imported memory. **This is the one thing T0 did not cover — probe it in the P8a offline harness
  before trusting the live path.**
- **R3:** GL consumer currently binds `GL_DEPTH_COMPONENT24`; P8 forces the sampled shadow map to 32F.
  Confirm no downstream code assumes 24-bit (it shouldn't — it's a sampler, not readback).
- **R4:** vk2gl back-channel is UNEXERCISED (present path deliberately skips it). P8 is its first live
  user — validate the semaphore signal/wait ordering under sync-validation.
- **R5:** forward-Z shadow vs reverse-Z scene convention (gameos_graphics.cpp:6659-6664) must be
  reproduced in the VK light projection (the fixture uses ZERO_TO_ONE `[0,1]` z, matching shadow.hglsl:39).

**Biggest unknown:** R2 (compare-mode sampling over imported D32) — resolve it in the offline harness
as the P8a step-0 spike. If it fails, fall back to VK-produce + GL-manual-PCF (sample as plain depth,
do the compare in GLSL) — a known workaround, not a blocker.
