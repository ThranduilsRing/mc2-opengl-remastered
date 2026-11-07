# Temporal reconstruction inputs and FSR native AA for the Vulkan renderer

Owner-approved track, 2026-11-06 (Astra report sections 3-5, rank 1; memory
`decision-fable-fsr-dlss-plan-2026-10-29.md`). Revised after two adversarial reviews:
`A:/Games/mc2-ops/studies/opus-review-coopmat-2026-11-06.md` and
`A:/Games/mc2-ops/studies/astra-review-p1-2026-11-06.md`. That study's default-OFF policy is
discarded: every slice ships ON or ships no visible change.

## Decisions

1. **No hand-rolled TAA resolve.** One was built and retired (`docs/temporal-ab-decision.md`,
   arm C): softer roof/fence detail, higher static-hold variance on all three maps, disocclusion
   unproven, no depth-history or reactive rejection. A custom resolve is a small custom upscaler
   (clamps, thin-feature locks, reactive handling, sharpening). The first consumer is **FSR 3.1
   in native-AA mode** (analytical, every vendor, Vulkan). FSR upscaling (render scale < 1, the
   4.x ML path on RX 7000 on Windows) and DLSS follow at the same slot. A custom resolve is only a
   fallback if FSR integration is blocked, and it must then clear the arm-C bar.
2. **Merge boundary: nothing that accumulates history ships ON until the motion and rejection
   inputs are complete for every geometry path** in the coverage table below. Input slices merge
   first and change no pixel; the resolve slice is last and ships ON.
3. **Mechs get a reactive mask before per-object MVs.** Per-object vectors are added only if a
   measurement (below) shows that depth reprojection plus the mask ghosts or shimmers on tiny mechs.
4. **Quality gates are image sequences across all six mods; RGP proves cost only.**

Licences, checked per vendored file before the resolve slice lands: the FidelityFX licence lists
the FSR3 upscaler source and the signed loader / VK DLLs under MIT-style terms; files not listed
are binary-only. DLSS SDK sec. 4e forbids use that makes it "subject to an open source software
license", and supplements 1 and 7.1b add NVIDIA-only and splash-mark terms, so DLSS waits for a
licence ruling for an open-source engine port plus mod ZIPs. XeSS: unmodified binary only.

## Motion-vector contract (internal)

| Property | Value |
|---|---|
| Camera target | `VK_FORMAT_R16G16_SFLOAT`, render resolution (extent of scene depth, display * `MC2_RENDER_SCALE`) |
| Units / direction | UV of the render target, `previousUV - currentUV` (add to the current UV to fetch history) |
| Y | `uv = 0` is the first row, VK viewports are positive-height, `ndc = uv * 2 - 1` both ways. The final present blit flips Y (`engine_present_vk.cpp` ~2282); that flip stays outside every reprojection |
| Depth | Reverse-Z `[0,1]`, GEQUAL; depth `0` (sky clear) is handled by the sky row below, not by "zero motion" |
| Jitter | Excluded from the stored vectors (see jitter algebra) |
| Validity | Not encoded as zero motion. Validity is the history epoch (below); zero motion means "static", never "no history" |
| Sync | Written in a graphics pass; the provider barrier adds `COMPUTE_SHADER` readers (FSR is compute), not only fragment readers |

Provider boundary (adapted there, internal vectors stay in UV): FSR takes the current-to-previous
offset, which is this direction, with `motionVectorScale = (renderW, renderH)` and no flip; DLSS
takes render-pixel vectors and render-pixel jitter. The provider descriptor carries: render extent
and display extent per frame, jitter in render pixels and its phase count (provider-chosen, not a
fixed 8), `reset`, exposure / pre-exposure (linear HDR before tonemap), the reactive mask and the
transparency-and-composition mask as separate images, and the depth convention (reverse-Z, inverted
infinite = no). Slice A adds a precision check: RG16F error in pixels for a 0.1 px/frame pan and a
full-screen pan at 4K.

## Pass order and object vectors

The camera pass must never overwrite object vectors. Order inside the frame:

1. Scene passes write color and depth. Object passes that own motion write a separate **object MV
   target** (RG16F) plus a **validity / reactive R8** target, both cleared to 0 at scene begin,
   depth-tested with the same depth state as their color draw (hidden objects cannot write over
   foreground vectors because they fail the same depth test; equal-depth decals do not write).
2. The camera pass runs after all scene passes: it computes camera motion from depth and
   **composes**: where object validity is set, it outputs the object vector, else the camera vector.
3. The provider reads the composed MV, the reactive mask and depth.

Slice 1's camera pass already runs after the scene (start of the composite cbuf, `DONT_CARE` load
plus full write); slice B changes it to read the object targets. No per-object replay pass.

### Geometry coverage (every path that draws into the scene)

| Path | Real code path | Motion | Rejection |
|---|---|---|---|
| Terrain (indirect LOD chunks) | `gos_terrain_lod_chunk_draw.cpp`, cached dispatch MVP | Camera (static) | Fog-of-war reveal is a shading change: reactive pulse on vertices whose visibility changed this frame |
| Static props, buildings | `GpuStaticPropBatcher` | Camera (static) | Destruction swap: reactive for the frame the shape changes |
| Mechs, GPU-skinned | `mech_skin.comp` writes posed world positions and rewrites bone indices/weights; `mech_tier1.vert` then skips blending; palettes already contain world placement, root import placement and lift (`gos_mech_batcher_flush.cpp` ~38) | Phase 1: none (reactive). Phase 2 (only if measured): `mech_skin.comp` keeps the previous **posed position stream** (ping-pong buffer, swapped per flush), and the vertex shader projects both streams with current / previous unjittered view-proj. No extra world transform is applied (palettes already hold it), so nothing is double-transformed | Mask written by the mech pass |
| Mechs, CPU fallback / detached arms | `TG_MultiShape::Render` (`mech3d_render.cpp` ~1032) | None | Mask (legacy pass writes R8 = 1) |
| Vehicles | `GpuStaticPropBatcher`, the mech batcher, or CPU fallback (`gvactor_render.cpp` ~126) | Phase 2: rigid previous world matrix per instance for the prop-batcher path; the mech-batcher path inherits the posed-stream scheme; CPU fallback stays mask-only | Mask on all three |
| Legacy pretransformed world geometry | `legacy_shape_tier1.vert` (CPU-projected) | None | Mask |
| Vegetation sway | `vegetation_vk.glsl`, `color_prop_tier1.vert` | Previous sway time evaluated in the vertex shader (both times in the uniform) | - |
| Water | `water_geom.vert`: time-dependent wave displacement attenuated by view angle (~158), shoreline mode rewrites depth independently (~222); water writes depth | The stored depth is not the visible surface, so depth reprojection is wrong on water. Water writes its own object MV from the previous wave time and the previous camera-dependent attenuation | Water is also reactive for reflections and animated shading (value fixed by the measurement, not assumed) |
| VFX: additive and alpha-blended (`vulkan_legacy_shape_island.cpp` ~414), lasers, smoke | Not depth-writing | None | Transparency-and-composition mask, written with max blend, cleared each frame; disappearance is free because the mask is per-frame |
| Decals (impact, scorch) | Surface-bound | Camera | Reactive on birth and during fade |
| Emissive animation | Mech/prop emissive pulses | Camera | Reactive where emissive changed |
| Sky (HDRI, `hdri_sky.frag` ~27) | Depends on view rotation only | **Rotation-only reprojection**: for depth 0, previous UV = project(previous view rotation, current view direction), translation ignored | - |
| Moving shadows | Dynamic casters (`vk_scene_renderer_passes.cpp` ~18) change receiver shading | Receiver motion (camera) only; no object vectors on receivers | Provider luma/colour rejection; proven by the moving-shadow sequence, not assumed |

## Jitter

### Render-only matrices

The shared camera (`Camera::worldToClipGL`, frustum extraction `camera.cpp` ~670, inverse
projection `camera_project.cpp` ~460) is **never jittered**. Jitter is a render-only clip offset
applied at the GPU-feed boundary:

- Published as `jitterClip = 2 * jitterPx / renderExtent` (x, y) in the frame packet next to
  `previousWorldToClipGL`.
- Applied to: the terrain dispatch MVP (`gos_terrain_lod_chunk_draw.cpp`), the mech MVP accessor
  (`gos_mech_batcher_flush.cpp` ~479), the prop / vegetation / water feeds, and pretransformed legacy
  world geometry as a vertex-shader offset `clip.xy += jitterClip * clip.w` in
  `legacy_shape_tier1.vert`, keyed off the world-geometry tag so HUD draws in the same shader get 0.
- Not applied to: simulation, CPU picking (`objmgr_pick.cpp`, `missiongui_input.cpp`,
  `PerPolySelect`), CPU visibility, shadow fitting and caster projection, HUD / ImGui.
- Every depth prepass uses the identical jittered matrix as its colour pass (bitwise; shared
  helper), or GEQUAL equal-depth tests fail.
- Determinism proof: the mc2_01 determinism and save-load baselines are run with jitter on and off
  and must match (jitter never reaches simulation data).

### Depth reconstruction algebra

Let `J = jitterClip` for the current frame. A jittered draw produces
`ndc_j = ndc_u + J`, where `ndc_u` is the position under the unjittered matrix `C`. A pixel at
`uv` samples jittered depth `d`, so the unjittered reconstruction is

```
ndc_u = (uv * 2 - 1) - J            // remove the current jitter: uv - jitterPx / renderExtent
world = inverse(C) * (ndc_u, d, 1)
prev  = P * world                   // P = previous UNJITTERED view-proj
mv    = (prev.xy / prev.w * 0.5 + 0.5) - (uv - J / 2)
```

`mv` is the motion of the surface point on the unjittered grid; history is stored on the
**unjittered (stabilized) grid** by the provider, which takes `jitterPx` separately. Each frame's
jitter is converted with that frame's own render extent. Required numerical cases (unit test of
the shader math in the C++ harness): stationary camera with alternating jitter gives mv = 0;
1 px horizontal and vertical pan; zoom; perspective and orthographic projections; odd extents.

### HZB under jitter

HZB samples the shared, jittered scene depth (`vulkan_post_hzb_native.cpp` ~625) while culling
uploads the prop-feed matrix (~944). With jitter on, culling tests against the jittered matrix of
the frame whose depth built the HZB, and expands every projected bound by `|J|` plus one texel
(conservative). CPU visibility stays unjittered. Proof: occlusion-on vs occlusion-off frames with
jitter on, thin occluders and object edges, zero popped objects.

### Picking and object ID

Picking never reads a GPU image (CPU projection with the camera's matrices), so it stays unjittered
by construction. The object-ID target, if a VK writer is ever added, is an MRT of the jittered draw
and therefore jittered by up to half a render pixel; it may be used for debug views and masks, and
nothing may pick from it without a 1 px tolerance or an unjittered redraw. No VK fragment shader
writes it today, so it is not the mech mask source.

### Texture mip bias

Today `log2(renderScale)` (`vk_render_infra.cpp` ~53). With the provider it becomes
`log2(render / display) - 1` and flips in the same commit as the resolve, never alone.

### In-world overlays

| Overlay | Plan |
|---|---|
| Fog-of-war / shroud | Baked into terrain vertex colour, moves with the terrain: camera MV, reactive pulse on reveal (coverage table) |
| Sensor / visibility overlay, terrain overlay quads | Reactive (full) or post-resolve layer, decided per pass in the resolve slice |
| Selection circles, waypoint / path lines, tactical arcs, debug lines | Post-resolve, unjittered overlay pass at display resolution |

The resolve slice lists every pass between scene clear and HUD and assigns each a row in the
coverage or overlay table; an unassigned pass blocks the merge.

## Resolve placement and routing

Existing routing: scene -> `scaledComposite` (render res) -> spatial AA -> separate upscale + CAS
(`engine_present_vk.cpp` ~1673 / ~1760 / ~1778) -> HUD tail. With temporal reconstruction:

| Mode | Route |
|---|---|
| Native (scale 1.0) | scene HDR -> provider (native AA, render = display) -> composite (tonemap / grade once) -> post-resolve overlays -> HUD |
| Scaled, provider upscales | scene HDR (render res) -> provider (output display res) -> composite at display res; the spatial upscale pass is **bypassed** |
| Any temporal mode | SMAA / FXAA / CMAA2 bypassed; sharpening is done by exactly one stage (provider sharpening, CAS off) |

Tonemap and grading run once, after HDR reconstruction. The HUD / ImGui tail is unchanged. Payoff
note: mission frames are often CPU-bound (wolfman, UpdateRenderers), so on the XTX the value is AA
quality at native; scaled modes must be measured, not assumed.

## History epochs and reset ownership

A frame gap is not a lifecycle event. Each view owns a `HistoryEpoch { uint32 id; ResetReason why; }`
in the frame packet. Any reset increments `id`; every history consumer (camera previous matrix,
colour / depth history in the provider, object MV streams, water time) compares epochs and drops
history on mismatch. The provider gets `reset = true` on that frame. History advances only when
the frame's history image was actually produced (composite recorded and submitted), not when the
camera publishes.

| Event | Owner that bumps the epoch |
|---|---|
| Mission load / unload | `Mission::destroy` / mission init (camera history is per camera object, not function-static) |
| Save load | `saveload_load.cpp` (~535 creates a new camera: new camera = new history) |
| Camera cut, zoom snap | `GameCamera`: ABL script camera cuts (`ablmc2_camera.cpp`), zoom-preset jumps, and a view-matrix delta above a threshold measured from the cross_map_view pan (logged, set in the resolve slice) |
| Pause | Not a reset: the camera is still, the history stays valid and converges; unpause is not a reset |
| View-mode change (Visual / Thermal / ...) | `gos_SetSelectedViewMode` |
| Resize, render-scale change, AA-mode change | `gosPostProcess::resize -> createSceneTargets`, `buildSharedImage`, the options apply path (scale is restart-only today; the hook still owns it) |
| Editor | `EditorCamera` publishes its own previous projection and epoch (today it leaves previous zero, so editor pans have zero camera MV) |
| Failed / skipped present | Present owner: no history produced, next frame resets |
| Actor teleport / disappear / reappear / mesh or LOD change | Per-object key = actor generation + geometry/LOD identity (not compacted instance or bone offsets); a mismatch or a position jump above a threshold marks the object reactive and drops its previous stream |

## Slice plan

| # | Slice | Visible change | Estimate |
|---|---|---|---|
| A | (this lane) Previous view-proj in `ViewUniforms`, camera MV target by depth reprojection, ImGui debug inset (Graphics Options, "Motion Vectors"); pass runs only when the inset is on | None | done |
| B | History epochs + reset owners; render-only jitter plumbing with jitter = 0; MV algebra with jitter term + harness math tests; object MV + validity/reactive targets and composition in the camera pass; sky rotation reprojection; water MV; reactive/transparency writers for every coverage row; vegetation previous sway; HZB jitter expansion | None (jitter 0, no consumer) | 5-7 days |
| C | Vendor FSR 3.1 (MIT-listed files only, licence receipt per file), provider descriptor, routing table, mip-bias flip, post-resolve overlay pass; FSR native AA replaces spatial AA, ships ON with a registered kill-switch and removal plan | Yes, ON | 5-7 days |
| D | Per-object mech / vehicle vectors (posed-stream ping-pong, rigid previous matrices), **only if** the slice C tiny-mech measurement fails | Fixes mech ghosting | 2-3 days, conditional |
| E | FSR upscaling presets, FSR 4.x ML on RX 7000 (Windows) | Yes, per preset | 3-4 days |
| F | DLSS / DLAA provider after the sec. 4e licence ruling; XeSS only if FSR falls short | Opt-in provider | 3+ days after ruling |

## Merge requirements for slice C (quality, not only cost)

Matched SMAA vs temporal sequences, archived with frame identity, settings, build and shader hashes
and full lossless sequences; detail retention and trails judged together (a lower frame difference
alone rewards blur):

- static holds across several complete jitter cycles (arm-C static-variance bar);
- subpixel pan, rotation and zoom with crops on tiny mechs, fences and foliage;
- walking, turning, firing, dismemberment, occlusion / reappearance, teleport;
- water, smoke, lasers, moving shadows, decal fade, fog-of-war reveal;
- day (mc2_01) and night (mc2_10), scale 1.0 and a reduced scale, odd resize, load / reset, pause;
- the AF16 + specular-AA spatial baseline (decision doc arm E) as a third arm;
- **all six mods** (MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech,
  MCO-Carver5): one moving-scene sequence each on a mission using their own mech / VFX content.

Tiny-mech measurement (decides slice D): mc2_10 `wolfman` and mc2_03 `scout_*` with moving mechs,
1080p, lossless six-frame crops around the smallest moving mech; slice D is scheduled only if
trails or mask-induced shimmer are visible.

RGP before/after at the default pose and the worst-case poses covers GPU cost and synchronization;
it is not a quality gate.

## Slice A state (claude/temporal-aa)

- `ViewUniforms` grows `previousWorldToClipGL` (offset 144, 208 B). `GameCamera::renderViewUniforms`
  fills it from the previous frame's matrix, or with the current matrix on a frame gap. This is the
  interim reset; slice B replaces it with epochs (it is function-static today, so it survives a
  camera replacement on save load).
- `vulkan_motion_vectors.{h,cpp}` + `motion_vectors.frag` record the camera MV pass at the start of
  the composite command buffer (GPU scope `VKIsl.Post.MotionVectors`), only while the debug inset
  is on: until a resolve exists the pass and its RG16F image (about 32 MiB at 4K) cost nothing by
  default. Once allocated, the image stays until the next target rebuild.
- `motion_vectors_debug.frag` draws the inset (red/green = +x/+y, 8 px = full scale).
- Sky (depth 0) writes zero motion in slice A; rotation-only sky reprojection is slice B.
- `docs/render-contract.md`: the "no motion targets without a temporal consumer" rule names this
  track and the debug consumer.

## Slice B state (inputs subset, claude/temporal-aa)

Landed: `RenderCore/TemporalInputs.h` (consumer counter, Halton(2,3) jitter with FSR phase count,
`frameJitterClip` = zero unless a consumer is registered, `applyJitterRowMajor`, `mipBias` hook,
CPU mirror of the MV algebra, unit-tested in `tests/unit/test_temporal_inputs.cpp`).
`motion_vectors.frag` removes the frame's jitter before reprojecting (push constant `jitterClip`)
and writes a cleared R8 reactive mask (second attachment). `MotionVectors::reactiveView()` and the
shared scene depth are the FSR inputs; all allocated only when the pass is demanded.
`vk_render_infra.cpp` samples through `mipBias(scale, false)` (flip with the resolve in slice C).
Harness toggle for the inset: `MC2_SHADOW_DEBUG_MODE=m` (no new gate).
Previous-frame matrices were already in slice A.

Left (needs the consumer to be testable): applying the jitter at the feeds (terrain MVP, mech MVP,
prop/water, `legacy_shape_tier1.vert`), history epochs and reset owners, object MV composition,
sky/water MV, coverage-row reactive writers, vegetation previous sway, HZB jitter expansion.
Picking and the shared camera are untouched by construction (no jitter is applied anywhere yet).

## Slice D state (claude/temporal-aa-c)

- Sky rotation reprojection landed: `motion_vectors.frag` reprojects depth-0 pixels with the previous view
  rotation only (translation-free x/y/w rows pushed by `vulkan_motion_vectors.cpp`; zero rows, hence zero
  motion, if the published eye position fails the clip w = 0 sanity check).
- HZB jitter expansion landed: `hzb_cull.comp` and its CPU mirror `hzb_prop_bounds.h` widen the projected
  box by 0.5 px (the jitter bound) on top of the existing one-texel margin.
- Per-object mech/vehicle vectors landed (claude/temporal-aa-e), no new gate:
  - Design: the mech island's scene pass writes a third colour attachment, the shared `MC2_SHARED_SCENE_OBJECT_MV`
    (slot 3, R16G16_SFLOAT, cleared to the sentinel 1e4). `mech_tier1.vert` passes the current clip and a previous clip
    (`prevFromCurClip * (clip - worldToClipGL * (delta, 0))`) to the FS, which writes `0.5 * (prevNdc - curNdc)`, so the
    value is the true per-pixel previousUV - currentUV on the unjittered grid. `motion_vectors.frag` samples that image
    (push `objectValid` at byte 120, range 124) and replaces the camera vector wherever it is not the sentinel.
    Mechs co-write under the same depth test, so occluded pixels never take an object vector.
  - Delta source: per instance, the actor's root bone translation (first bone, row3.xyz, Stuff space) minus its value last
    frame, keyed by `objectIdRaw`, converted with `mc2ToRender` and staged as one vec4 per instance in a tail after the
    bone palette (index `boneCount + (instance >> 2)`; UBO `miscI[2]` = boneCount). `prevFromCurClip` comes from
    `copyMainView()` (identity if the camera did not move).
  - Limitations: whole-body root translation only (limb animation and root rotation are not in the vector); an actor first
    seen this frame, or jumping more than 64 units (teleport), gets delta 0 and keeps the camera vector; the mech-lab
    preview variant is untouched; props, legacy shapes and every other draw class keep the camera vector; if the island
    falls back or does not submit, `objectValid` is 0 and the pass is the camera-only result.
  - Receipts: Ninja build exit 0; `build_contract_harnesses --unit` exit 0; spirv-val (vulkan1.2) clean on
    mech_tier1.vert/frag, mech_preview.frag, motion_vectors.frag; shader_reflect goldens regenerated for the three
    changed shaders and re-checked (0 drifted); gpu_abi_gen check clean. No capture or smoke was run (lane rule), so
    the visual ghosting fix is unverified in-game.
