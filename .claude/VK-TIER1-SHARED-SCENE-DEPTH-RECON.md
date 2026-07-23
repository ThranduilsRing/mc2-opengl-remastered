# VK Tier-1 SHARED SCENE DEPTH — recon verdict (parity strategy for the whole geometry-color endgame)

**Worktree:** `A:/Games/mc2-vk-interop-bridge` @ `ecbdf98c`. Verified by file reads + the P5-T0 D32S8 spike log.

## THE PROBLEM
Every VK-native COLOR geometry region (props P11, terrain P10, mechs P12) must depth-test its VK
draws against the GL scene depth to occlude correctly (vs terrain ridges, other geometry). Without
it, VK geometry composites OVER the scene (wrong occlusion) → no pixel parity. Engine scene depth is
`GL_DEPTH24_STENCIL8`, reverse-Z, `GL_GEQUAL`.

## VERDICT — Option (a): GL-produced D32_SFLOAT_S8_UINT shared scene-depth image. Size **M**.
GL renders scene depth into ONE VK-built exportable D32S8 `VkImageResource`; GL imports it via
`GlInteropView` as a `GL_DEPTH24_STENCIL8` texture and substitutes it for `sceneDepthTex_` at the
**single attach site** (`gos_postprocess.cpp:938-939`, alloc `:931-933`). Each VK color region then
imports the SAME image as a depth attachment and depth-tests (reverse-Z, `VK_COMPARE_OP_GREATER_OR_EQUAL`,
clear 0.0 far).

**Why M not XL:** the scary part (42 `sceneDepthTex_` sample consumers — SSAO/edge-fog/OOB-fog/HZB/
subgraph/DOF/god-rays/cluster/particle-copy/stencil-view) is UNTOUCHED. P5-T0 spike PROVED a
D32S8-backed image imported to GL as `GL_DEPTH24_STENCIL8` samples the DEPTH aspect EXACTLY, 0 val
errors. GL-side substitution is transparent to samplers → only the 1 attach site + alloc change.
Direction is GL-produces / VK-imports (inverse of the shadow island's VK-produces / GL-imports).

## REJECTED
- **(b) VK depth pre-pass** (VK re-renders terrain+props+mechs opaque depth) = **XL**, it IS the P14
  endgame in disguise + depth-divergence (cracks/z-fight) risk. Moot once P10/P11/P12 are all VK.
- **(c) Approximate occlusion** (P11 Tier-0's private VK depth) = **S, already built, NOT parity** —
  interim visible probe only; no pixel-parity path.

## EXECUTION LADDER (STOP-gate first)
1. **Offline D32S8 depth-TEST spike** (~1-2h, no game launch): extend
   `tools/vk_gl_interop_spike/main_d24s8.cpp` — GL-owns/renders depth into the shared D32S8 under
   `GL_GEQUAL` reverse-Z; VK imports it as a **depth-stencil attachment** and depth-TESTs a color draw
   (`depthCompareOp=GREATER_OR_EQUAL`) — assert VK frags correctly kept/killed by GL-authored depth
   (P5-T0 only SAMPLED, never depth-tested — this is the true new capability). Also round-trip the
   `glCopyImageSubData`→`sceneDepthCopyTex_` path (`:1275`) + stencil `glTextureView` (`:950`) to catch
   the 64bpp-vs-32bpp size hazard, and re-sample DEPTH after (42-consumer regression guard).
   **Exit 0 = STOP-gate for the whole geometry-color endgame.**
2. **Shared-depth substrate slice** — gate `MC2_VK_SHARED_SCENE_DEPTH` (default-OFF, byte-identical):
   swap the `sceneDepthTex_` alloc+attach (`:931-939`) to the imported shared image, GL-ONLY, NO VK
   color draw yet. Prove all 42 GL consumers visually unchanged (SSAO/fog/HZB parity vs baseline) —
   isolates the attach-swap risk from any new VK draw.
3. **P11 prop-color Tier-1** — rebase the prop-color island onto the shared depth as the FIRST VK
   depth-test consumer → pixel parity.
4. **Terrain P10 / Mech P12** — inherit shared depth + the shared-GBuffer plumbing from P11.

## TOP-3 RISKS of (a)
1. **Inverted ownership / barrier direction (HIGH)** — first GL-produced/VK-consumed-as-depth-attachment
   shared image. GL→VK acquire layout + `glSignalSemaphore` depth-layout correctness = #1 sync-val
   hazard. The step-1 spike exercises exactly this; run `MC2_VULKAN_VALIDATION=sync`.
2. **64bpp D32S8 is NOT a byte-alias of old 32bpp D24S8 (MED)** — any size-based (not format-class)
   GL copy could mis-copy: round-trip the copy path + stencil view in the spike.
3. **Reverse-Z / GEQUAL convention on the VK side (MED)** — must use `GREATER_OR_EQUAL` + clear 0.0
   far or occlusion inverts. Cheap to get right, catastrophic if wrong; assert with a known depth field.

## KEY FILE:LINE
- Scene depth alloc/attach (the ONLY attach site): `gos_postprocess.cpp:931-933` (alloc), `:938-939` (attach).
- Accessor `getSceneDepthTexture()` `gos_postprocess.h:106`; copy path `glCopyImageSubData` `:1275` →
  `sceneDepthCopyTex_` (`getSceneDepthCopyTexture()` `:116`); stencil view `:949-951`.
- Reverse-Z GEQUAL scene depth writes: `gameos_graphics.cpp:4078, 4717, 4937, 5104`.
- Spike to extend: `tools/vk_gl_interop_spike/main_d24s8.cpp` (+ P5-T0 result log).
- Live template for VK depth-attachment interop: `vulkan_shadow_island_live.cpp` (inverse direction).
