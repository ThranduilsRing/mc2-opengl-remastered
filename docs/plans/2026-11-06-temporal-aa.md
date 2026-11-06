# Temporal reconstruction inputs and FSR native AA for the Vulkan renderer

Owner-approved track, 2026-11-06 (Astra report sections 3-5, rank 1; memory
`decision-fable-fsr-dlss-plan-2026-10-29.md`; Opus review
`A:/Games/mc2-ops/studies/opus-review-coopmat-2026-11-06.md`). That study's default-OFF policy is
discarded: every slice ships ON or ships no visible change.

**No hand-rolled TAA resolve.** A hand-rolled TAA was already built and retired
(`docs/temporal-ab-decision.md`, arm C): softer fine roof/fence detail, higher static-hold
variance on all three maps, disocclusion unproven, no depth-history or reactive rejection. A
custom resolve would be a small custom upscaler that needs its own clamps, thin-feature locks,
reactive handling and sharpening. The first consumer of these inputs is therefore **FSR 3.1 in
native-AA mode** (analytical, every vendor, Vulkan). FSR upscaling (render scale < 1, plus the
4.x ML path on RX 7000 on Windows) and DLSS follow at the same slot. A custom resolve exists only
as a fallback if the FSR integration is blocked, and it must then clear the arm-C bar above.

Licences, to check per vendored file before slice 2 lands: the FidelityFX licence lists the FSR3
upscaler source and the signed loader / VK DLLs under MIT-style terms; files not listed are
binary-only. DLSS SDK sec. 4e forbids use that makes it "subject to an open source software
license", and supplements 1 and 7.1b add NVIDIA-only and splash-mark terms. An open-source engine
port plus mod ZIPs needs a licence ruling before any DLSS work. XeSS: unmodified binary only.

## Motion-vector contract

| Property | Value |
|---|---|
| Target | `scene.motionVectors`, `VK_FORMAT_R16G16_SFLOAT`, render resolution (same extent as scene depth, i.e. display * `MC2_RENDER_SCALE`) |
| Units | UV units of the render target (not pixels, not NDC) |
| Direction | `previousUV - currentUV`: add it to the current UV to fetch the history sample |
| Y | Same Y as the UV the post passes sample with (`uv = 0` is the first row; VK viewports are positive-height, no flip), so `ndc = uv * 2 - 1` both ways |
| Depth | Reverse-Z, `[0,1]`, GEQUAL; depth `0` (sky / far clear) writes zero motion |
| Jitter | Excluded. MVs are computed from unjittered current and previous view-proj; FSR takes the jitter offset separately (`jitterOffset`, render pixels) |
| FSR mapping | FSR expects the current-to-previous offset, which is this contract's direction; it is in UV with first-row-0 like ours, so the dispatch passes `motionVectorScale = (renderW, renderH)` and no flip |
| No history | Zero motion when there is no previous frame, the previous frame is not `frame - 1`, or the camera did not move (identity reprojection) |
| Layout at consumers | `SHADER_READ_ONLY_OPTIMAL` after the motion pass, inside the composite command buffer |

## Motion source per pass

| Content | Source | Slice |
|---|---|---|
| Terrain, static props, buildings, sky | Depth reprojection: `prevClip = prevViewProj * inverse(curViewProj) * (ndc.xy, depth, 1)` in a fullscreen pass | 1 |
| Mechs (skinned) and rigid vehicles | **First:** camera MV plus a reactive / low-history mask written by the mech and vehicle passes, so mech pixels look like today while terrain and props (where the shimmer is) converge. **Only if that measurably ghosts on tiny mechs:** previous rigid world transform (translation dominates in an RTS), then the previous bone palette (`gos_mech_batcher_flush.cpp`, `mech_skin.comp`) last | 2 (mask), 4 (per-object, conditional) |
| Vegetation sway | Previous sway time (`prevTime` next to `misc.w` in `vegetation_vk.glsl`, and next to `color_prop_tier1.vert` time) evaluated twice in the vertex shader | 3 |
| Water | Geometry is static, the waves are in the fragment shader: camera MV plus the reactive mask | 2 |
| VFX / particles / impact decals | Reactive mask (FSR transparency-and-composition mask for additive, short-lived content) | 2 |

The mech mask cannot reuse `MC2_OBJECT_ID_BUFFER` as-is: no Vulkan fragment shader writes the
object-ID attachment today (GL-path MRT, `RenderPassContract.h`). Slice 2 adds a dedicated R8
reactive output to the mech / vehicle / water / VFX passes instead.

Measurement before per-object MVs (slice 2 exit criterion): mc2_10 `wolfman` and mc2_03 `scout_*`
with moving mechs, 1080p, FSR native AA vs SMAA, lossless six-frame crops around the smallest
moving mech. Per-object MVs are scheduled only if mech trails or mask-induced shimmer are visible
in those crops.
| HUD, ImGui, cursor | Not in the scene: composited after the resolve | n/a |

## Jitter

- Halton(2,3), 8 phases, as a sub-pixel offset in render-resolution pixels, applied to the main
  view projection only (clip-space `xy += jitter * 2 / renderSize * w`). Published next to
  `previousWorldToClipGL` in `ViewUniforms` (slice 2).
- Unjittered: simulation and every CPU visibility test, HZB build and cull
  (`vulkan_post_hzb_native.cpp`), shadow cascades and the CSM fit, HUD / ImGui, the MV pass itself.
- Picking stays unjittered because it never reads a GPU image: click and box selection project
  candidates on the CPU with the camera's matrices (`code/objmgr_pick.cpp`
  `pickGameplaySelection`, `code/missiongui_input.cpp`, `PerPolySelect` in mclib), and jitter is
  added only to the GPU copy of the main view projection, never to the camera. The object-ID
  target is a different matter: it is an MRT of the scene draw (GL path only today), so if a VK
  writer is added it IS jittered by up to half a render pixel. Its consumers (the Object ID debug
  view) accept that; nothing may pick from it without a +-1 px tolerance or an unjittered re-draw.
- Jittered (they reconstruct world position from the jittered depth and must use the same matrix):
  fog (`vulkan_post_fog_native.cpp`), screen effects, decals, SSAO-like reconstructors via
  `inverseViewProj_` (`setTerrainMVP`).
- Jitter is zero while the resolve is not running, so slice 1 changes no pixel.
- Texture mip bias: today `log2(renderScale)` (`vk_render_infra.cpp` ~53). With FSR it becomes
  `log2(render/display) - 1` and flips together with the resolve, never alone (SMAA-only frames
  would shimmer more).

### In-world overlays under jitter

These are drawn into the scene before the HUD, so they would be jittered and enter history:

| Overlay | Behavior under jitter | Plan |
|---|---|---|
| Fog-of-war / shroud | Baked into terrain vertex colour (CPU visibility per vertex), so it moves with the terrain and camera MVs are correct. A reveal is a shading change, not motion: FSR's colour clamp follows it within a few frames | Keep in scene; verify a reveal edge in the slice 2 captures (no lag trail longer than about 4 frames) |
| Sensor / visibility overlay, terrain overlay quads | Thin, high-contrast, screen-stable lines over moving terrain: will crawl or ghost | Write the reactive mask (full reactive), or move to a post-resolve overlay layer |
| Selection circles, waypoint / path lines, tactical arcs, debug lines | Same: thin and UI-like; they must stay crisp | Draw after the resolve in a post-resolve, unjittered overlay pass at display resolution (depth-tested against the resolved depth is not needed: these already draw on top) |
| Damage / impact decals | Part of the surface, move with it | Keep in scene, camera MVs |

Slice 2 lists every pass that draws between the scene clear and the HUD and assigns it one of
these three rows; an unassigned pass fails the slice.

## Resolve placement

`scene (HDR, jittered, render res) -> motion pass + reactive mask -> FSR (native AA, or upscale
to display res) -> post-resolve overlays -> tonemap / composite -> HUD / ImGui`. SMAA/FXAA/CMAA2
are not applied on top of FSR output. FSR runs before tonemap on linear HDR (with its exposure
input) and before the HUD so UI never enters history. The upscaling and DLSS providers take the
same slot and output display resolution, so the composite reads their output instead of the
scene. Payoff note: mission frames are often CPU-bound (wolfman, UpdateRenderers), so the value
on the XTX is AA quality at native; render scale < 1 is a secondary benefit to be measured.

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
| Spawn / teleport | Covered by the reactive mask (mech pixels keep low history); per-object stamps only with slice 4 |
| Any of the above | FSR `reset = true` for that frame |
| Pause, view-mode change, frame skipped | Frame-counter gap (`frame != prevFrame + 1`) |

## Slice plan

| # | Slice | Visible change | Estimate |
|---|---|---|---|
| 1 | Previous view-proj in `ViewUniforms`, camera MV target by depth reprojection, ImGui debug view (Graphics Options, "Motion Vectors") | None (infra + debug view) | this lane |
| 2 | Vendor FSR 3.1 upscaler source (MIT-listed files only, licence receipt per file) behind a provider interface; Halton jitter on the main view; reactive mask from mech / vehicle / water / VFX passes; post-resolve overlay pass; mip-bias flip; FSR native AA replaces SMAA, ships ON with a registered kill-switch and removal plan. Exit: arm-C bar (roof/fence detail vs SMAA, static-hold variance) plus the tiny-mech measurement | Yes, ON | 5-7 days |
| 3 | Vegetation previous sway | Fixes foliage smear | 1-2 days |
| 4 | Mech / vehicle per-object MVs (rigid transform first, bone palette last), only if the slice 2 measurement shows mech ghosting | Fixes mech ghosting | 2-3 days, conditional |
| 5 | FSR upscaling presets (render scale < 1), FSR 4.x ML path on RX 7000 (Windows) | Yes, ON for the chosen preset | 3-4 days |
| 6 | DLSS / DLAA provider, blocked on the SDK licence ruling (sec. 4e); XeSS only if FSR falls short | Opt-in provider | 3+ days after ruling |
| - | Baseline the decision doc already preferred: AF16 + Toksvig-style specular AA (arm E) re-tested against FSR native AA in the slice 2 captures | Comparison only | in slice 2 |

## Slice 1 state (claude/temporal-aa)

- `ViewUniforms` grows `previousWorldToClipGL` (offset 144, 208 B). `GameCamera::renderViewUniforms`
  fills it from the previous frame's matrix, or with the current matrix on a frame gap (reset).
- `vulkan_motion_vectors.{h,cpp}` + `motion_vectors.frag` record the camera MV pass at the start of
  the composite command buffer, every frame (GPU scope `VKIsl.Post.MotionVectors`).
- `motion_vectors_debug.frag` draws an inset of the MV target over the composite when the
  Graphics Options "Motion Vectors" checkbox is on (red/green = +x/+y, 8 px = full scale).
- `docs/render-contract.md`: the "no motion targets without a temporal consumer" rule now names
  this track and the debug consumer.
