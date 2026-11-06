# Temporal reconstruction (TAA, then upscaler providers) for the Vulkan renderer

Owner-approved track, 2026-11-06 (Astra report sections 3-5, rank 1; memory
`decision-fable-fsr-dlss-plan-2026-10-29.md`). That study's default-OFF policy is
discarded: every slice ships ON or ships no visible change. The 2026-10-30 retirement
of the first prototype (`docs/temporal-ab-decision.md`: too soft, disocclusion unproven,
static-hold variance) is the bar slice 2 must clear, not a reason to skip it.

## Motion-vector contract

| Property | Value |
|---|---|
| Target | `scene.motionVectors`, `VK_FORMAT_R16G16_SFLOAT`, render resolution (same extent as scene depth, i.e. display * `MC2_RENDER_SCALE`) |
| Units | UV units of the render target (not pixels, not NDC) |
| Direction | `previousUV - currentUV`: add it to the current UV to fetch the history sample |
| Y | Same Y as the UV the post passes sample with (`uv = 0` is the first row; VK viewports are positive-height, no flip), so `ndc = uv * 2 - 1` both ways |
| Depth | Reverse-Z, `[0,1]`, GEQUAL; depth `0` (sky / far clear) writes zero motion |
| Jitter | Excluded. MVs are computed from unjittered current and previous view-proj; the resolve adds `jitterPrev - jitterCur` itself (slice 2) |
| No history | Zero motion when there is no previous frame, the previous frame is not `frame - 1`, or the camera did not move (identity reprojection) |
| Layout at consumers | `SHADER_READ_ONLY_OPTIMAL` after the motion pass, inside the composite command buffer |

## Motion source per pass

| Content | Source | Slice |
|---|---|---|
| Terrain, static props, buildings, sky | Depth reprojection: `prevClip = prevViewProj * inverse(curViewProj) * (ndc.xy, depth, 1)` in a fullscreen pass | 1 |
| Mechs (skinned) and rigid vehicles | Previous bone palette and previous world transform, keyed per actor with a generation stamp (palette rebuilt each flush in `gos_mech_batcher_flush.cpp`; skin in `mech_skin.comp`) and written by the mech pass into the MV target over the camera MV | 3 |
| Vegetation sway | Previous sway time (`prevTime` next to `misc.w` in `vegetation_vk.glsl`, and next to `color_prop_tier1.vert` time) evaluated twice in the vertex shader | 4 |
| Water | Geometry is static, the waves are in the fragment shader: camera MV plus a reactive-mask bit (low history weight) | 4 |
| VFX / particles / impact decals | Reactive mask (particles carry velocity but are additive and short-lived) | 4 |
| HUD, ImGui, cursor | Not in the scene: composited after the resolve | n/a |

## Jitter

- Halton(2,3), 8 phases, as a sub-pixel offset in render-resolution pixels, applied to the main
  view projection only (clip-space `xy += jitter * 2 / renderSize * w`). Published next to
  `previousWorldToClipGL` in `ViewUniforms` (slice 2).
- Unjittered: picking and the object-ID target (render-contract.md "picking"), simulation and
  every CPU visibility test, HZB build and cull (`vulkan_post_hzb_native.cpp`), shadow cascades
  and the CSM fit, HUD / ImGui, the MV pass itself.
- Jittered (they reconstruct world position from the jittered depth and must use the same matrix):
  fog (`vulkan_post_fog_native.cpp`), screen effects, decals, SSAO-like reconstructors via
  `inverseViewProj_` (`setTerrainMVP`).
- Jitter is zero while the resolve is not running, so slice 1 changes no pixel.

## Resolve placement

`scene (HDR, jittered, render res) -> motion pass -> TAA resolve (render res, before tonemap,
history in linear HDR) -> tonemap / composite -> spatial AA (SMAA/FXAA retired when TAA is on)
-> HUD / ImGui`. The resolve runs before tonemap so the neighborhood clamp works in linear
light (with a luminance-weighted / tonemapped-space clamp to stop fireflies), and before the HUD
so UI never enters history. An upscaler provider (slice 5) replaces the resolve at the same
point and outputs display resolution, so the composite reads its output instead of the scene.

## History reset

The motion pass and the resolve both treat "no valid previous" as a reset (zero motion, history
weight 0). The previous matrix is published only when the previous view packet is from frame
`N - 1`; a reset is forced by:

| Event | Hook |
|---|---|
| Resize / render-scale change | `gosPostProcess::resize -> createSceneTargets` and `buildSharedImage` (targets are rebuilt; the MV class `reset()`s) |
| Mission load / unload | `Mission::destroy` (`code/mission.cpp`, next to `GpuMechBatcher::onMapUnload`) |
| Save load | `saveload_load.cpp` restore path |
| Camera cut | ABL script camera (`ablmc2_camera.cpp`); a world-space jump of the camera origin or a view-matrix delta above a threshold (robust fallback; `swoopy = false` is NOT a cut) |
| Spawn / teleport | Per-object: actor generation stamp mismatch writes camera MV only (slice 3) |
| Pause, view-mode change, frame skipped | Frame-counter gap (`frame != prevFrame + 1`) |

## Slice plan

| # | Slice | Visible change | Estimate |
|---|---|---|---|
| 1 | Previous view-proj in `ViewUniforms`, camera MV target by depth reprojection, ImGui debug view (Graphics Options, "Motion Vectors") | None (infra + debug view) | this lane |
| 2 | Halton jitter on the main view + TAA resolve before tonemap (depth-aware disocclusion, variance clamp in YCoCg, reset contract, CAS-style sharpen), replaces SMAA; ships ON with a registered kill-switch and removal plan | Yes, ON | 4-5 days |
| 3 | Mech / vehicle per-object MVs from previous palette + transform | Fixes mech ghosting | 2-3 days |
| 4 | Vegetation previous sway + reactive mask (water, VFX, particles) | Fixes foliage / water smear | 2-3 days |
| 5 | Upscaler provider interface (FSR 3 / XeSS / DLSS) at the resolve slot, render-scale presets | Yes, ON for the chosen preset | 5+ days |

## Slice 1 state (claude/temporal-aa)

- `ViewUniforms` grows `previousWorldToClipGL` (offset 144, 208 B). `GameCamera::renderViewUniforms`
  fills it from the previous frame's matrix, or with the current matrix on a frame gap (reset).
- `vulkan_motion_vectors.{h,cpp}` + `motion_vectors.frag` record the camera MV pass at the start of
  the composite command buffer, every frame (GPU scope `VKIsl.Post.MotionVectors`).
- `motion_vectors_debug.frag` draws an inset of the MV target over the composite when the
  Graphics Options "Motion Vectors" checkbox is on (red/green = +x/+y, 8 px = full scale).
- `docs/render-contract.md`: the "no motion targets without a temporal consumer" rule now names
  this track and the debug consumer.
