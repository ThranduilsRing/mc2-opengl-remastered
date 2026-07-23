# VK-FULL-OWNERSHIP-RECON-1 — path to VK-owns-everything + MoltenVK audit

Read-only recon. Branch `claude/vk-interop-bridge-1` @ `d60c6eb5`. No code changed.
User north star (2026-07-06): (1) VK owns full scene/all LODs/all passes/present;
(2) macOS/Metal(MoltenVK) portability — don't build in Metal-incompatible deps.

---

## A. OWNERSHIP INVENTORY — what GL still owns

| Region | Where | Why GL owns it now | VK ownership requires |
|---|---|---|---|
| Coarse terrain LODs (LOD1+, non-displaced coarse bands) | `gos_terrain_lod_chunk.cpp` draw dispatch ~:1600-1650; VK island only ports LOD0 non-displaced path (comment :1604-1605: "visual-displace / edge-stitch are DEFERRED") | VK terrain island explicitly scoped to LOD0 only; coarser bands + skirts never ported | Port `TransformMultiShape`-equivalent coarse-band draw + skirt VBO/IBO (`skirtVbo`/`skirtIbo`, gos_terrain_lod_chunk.cpp:323-331) into the VK terrain island; extend shared-GBuffer co-write to all `cmd.lodStep` values, not just `lodStep==1` |
| Skirts + edge-stitch | same file, `skirtEdgeOffset[4]`/`skirtEdgeCount[4]` (:330-331), per-edge N/S/W/E strips | Tied 1:1 to the coarse-LOD path above — skirts only exist to hide cracks between LOD-mismatched neighbor chunks | Falls out of the coarse-LOD port; skirt geometry/index buffers need a VK VBO/IBO mirror + the `u_skirtDepth` uniform ported to the VK terrain shader |
| visualDisplace path (LOD0 4x-finer + far-band fade) | `gos_terrain_lod_chunk.cpp:1094-1730`, `u_visualDisplace` modes 0/1/2, `displaceLod0`/`displaceFar` | **Interacts with geomorph**: displacement mode changes vertex count/topology per LOD band (4x-finer builder for LOD0 vs normal for LOD1+); VK island's LOD0-only scope sidesteps this entirely by not porting the coarse bands where displace mode 2 (far-band fade) applies | Port both displace modes' vertex generation to VK compute/vertex path; the far-band fade (`u_visualDisplace==2`) scales verts continuously 0..1 — must match GL's per-band fade math bit-for-bit or LOD transitions will pop differently between GL/VK-owned regions during the transition period |
| Water | no `vulkan_water_island.cpp` render coverage confirmed beyond scaffold — file exists (`GameOS/gameos/vulkan_water_island.cpp`) but per VK-FLIP-EVIDENCE-1 mc2_17 water poses show "low VK terrain co-write" | Water fast-path (`renderWaterFastPath`, gos_terrain_water_fast*.vert) reflection/depth-fudge lockstep with terrain_depth_bias is GL-only tuned (per mc2-water-depth-expert domain); not yet proven at parity in the shared-GBuffer arc | Extend `vulkan_water_island.cpp` to full parity capture (mc2_17 water_near/land_near currently pass at low coverage, not full water-surface ownership); must preserve the camera-independent reversed-Z lockstep with terrain |
| Postprocess chain — remainder | `gos_postprocess.cpp` composite tail :4025-4217 (HZB→SSAO→screen-shadow→cloud→shoreline→box-decals→fog→composite/tonemap→pillarbox) | Per VK-P14b-PRESENT-TAIL-RECON: **already has VK island variants for several regions** (`vulkan_hzb_island.cpp`, `vulkan_ssao_island.cpp`, `vulkan_oob_fog_island.cpp`, `vulkan_edge_fog_island.cpp`, `vulkan_postprocess_subgraph.cpp`) but the *composite/tonemap/FXAA/viewmode draw* and everything after stays GL, sampling the shared GBuffer | VK-P14b's own finding: **nothing here needs to move for VK-owns-present** — but for VK-owns-everything (not just present), port composite/tonemap/pillarbox into `vulkan_postprocess_subgraph.cpp`; this is optimization-tier, not blocking |
| Shadow-map GENERATION | GL renders (`gos_postprocess.cpp` static+dynamic shadow passes); VK **copies**: `vulkan_shadow_island_live.cpp` — comment says "zero-copy exportable D32 **shadow map**" for the depth-interop seam (:28,:363) but that's the *consuming* side (VK samples GL's shadow map via export, not VK-rendered). Static shadow copy is now cadence-gated (commit d60c6eb5, `gosPostProcess::staticShadowGeneration_` counter) — only re-copies on mission load/rebuild. **Dynamic (building/mech) shadow + CSM full-map subset still copy every frame** — no cadence gate exists because GL genuinely re-renders those every frame from moving casters | GL still GENERATES all shadow maps (static sun + dynamic + CSM); VK only imports/samples via export-then-zero-copy-read, i.e. VK never issues the depth-only shadow draw itself. Full VK ownership = VK renders shadow depth itself | `95095a94` already built "ShadowDepth region contract + depth-only instanced pipeline + headless shadow-depth-render fixture (single + CSM array)" per commit log — **sub-slice A landed, live backend-compare deferred to sub-slice B**. This is the closest-to-ready ownership rung: kills the remaining per-frame dynamic/CSM copy tax entirely (biggest live perf reason left after copy-1 present-tail elimination) |
| UI/HUD layer | `gameosmain.cpp:639` `gos_RendererFlushHUDBatch`, `:643` `GuiRuntime::Render` (ImGui), `:636` projectz overlay — all GL, all AFTER the shared final-composite bind | GameOS 2D HUD batch renderer + ImGui both draw straight to whatever FBO-0/copyFbo is bound at that point; no VK path exists for either | ImGui has an official Vulkan backend (`imgui_impl_vulkan`) — would need to wire it in alongside/replacing `imgui_impl_opengl3`; GameOS 2D HUD batch has no VK equivalent, would need a new `vulkan_hud_island.cpp` mirroring the sprite/quad batching in `gos_RendererFlushHUDBatch` |
| Present | `engine_present_vk.cpp` — present path EXISTS from earlier arc (`MC2_RENDER_BACKEND=vulkan` selectable, confirmed working per VK-ARC-ROADMAP history) | Per VK-P14b-PRESENT-TAIL-RECON: today's flow is copy-1 (GL FBO-0 → staging) + interop-signal + copy-2 (staging → VK swapchain blit). Copy-1 is **removable NOW** via `MC2_VK_PRESENT_DIRECT_FBO` (redirect :4038 bind to `EnginePresentVk`'s `s.copyFbo` directly) — landable independent of geometry regions | Land `MC2_VK_PRESENT_DIRECT_FBO` (already spec'd, Size S-M, default-OFF) — retires copy-1, leaves copy-2 (irreducible until VK produces every pixel itself) |
| Remaining interop copies | light SSBO, height SSBO, paint-scheme SSBO — height already gated+audited (`MC2_VK_TERRAIN_HEIGHT_AUDIT`, `audit_height()` in `vulkan_terrain_island.cpp`); light/paint-scheme copy mechanisms not directly greped this session but implied by "shared-GBuffer arc" co-write model | These exist because GL's CPU-authored per-frame state (lights, mech paint schemes) needs to reach VK-side shading; height copy already down to cadence-gated (mission-load only, since GPU heightfield isn't runtime-deformed per VK-FLIP-EVIDENCE-1 §c finding) | Light SSBO copy dies once VK owns the pass that consumes lighting (mech/prop shading rungs, already landed per VK-FLIP-EVIDENCE-1); paint-scheme SSBO copy dies once VK mech material/lighting rung lands (flagged open in memory as "mech material/lighting=flat probe deferred") — i.e. **these die at the NEXT mech-material ownership rung, not a new slice** |

---

## B. MOLTENVK PORTABILITY AUDIT

Grepped `GameOS/gameos/vulkan_context.cpp` (instance/device creation) + all `vulkan_*.cpp` islands +
`shaders/vulkan/*.{vert,frag}`.

| Item | Found in code | Verdict |
|---|---|---|
| VK API version targeted | `VK_API_VERSION_1_1` (vulkan_context.cpp:214, :332) | **Metal-clean.** MoltenVK targets Vulkan 1.2-equivalent portability subset; 1.1 baseline is well within range. |
| Instance extensions | `VK_KHR_EXTERNAL_MEMORY_CAPABILITIES`, `VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES`, `VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2`, `VK_KHR_SURFACE`, `VK_KHR_WIN32_SURFACE`, `VK_EXT_DEBUG_UTILS` (vulkan_context.cpp:160-199) | `VK_KHR_SURFACE` Metal-clean (swaps for `VK_MVK_MACOS_SURFACE`/`VK_EXT_METAL_SURFACE`). `VK_EXT_DEBUG_UTILS` Metal-clean. `VK_KHR_WIN32_SURFACE` **dies with full ownership + macOS port** — trivially swapped, not a real risk, just needs a `#ifdef` branch (mirrors the existing `VK_USE_PLATFORM_WIN32_KHR` gate at vulkan_context.cpp:20-21, which is already conditionally defined — good sign, the codebase already has the ifdef seam). External-memory/-semaphore *capabilities* query extensions: **UNVERIFIED** whether MoltenVK exposes these on macOS — training knowledge says MoltenVK's external-memory story is thin/absent since there's no GL-interop target on Metal (there's no GL side once VK owns everything, so this entire family is moot post-full-ownership, see below). |
| Device extensions | `VK_KHR_EXTERNAL_MEMORY_WIN32`, `VK_KHR_EXTERNAL_SEMAPHORE_WIN32`, `VK_KHR_DEDICATED_ALLOCATION`, `VK_KHR_GET_MEMORY_REQUIREMENTS_2`, `VK_KHR_SWAPCHAIN` (vulkan_context.cpp:298-307) | `VK_KHR_SWAPCHAIN` Metal-clean (MoltenVK implements it). `VK_KHR_DEDICATED_ALLOCATION`/`GET_MEMORY_REQUIREMENTS_2` Metal-clean (core-promoted, widely supported). **`VK_KHR_EXTERNAL_MEMORY_WIN32` + `VK_KHR_EXTERNAL_SEMAPHORE_WIN32` are Win32-named and literally do not exist on macOS** — these ARE the GL↔VK interop plumbing (shared D32 shadow, shared scene color/depth). Per the user's own framing: **irrelevant on macOS since there's no GL side once VK owns everything** — but today's code *assumes* these exist (`vulkan_shared_scene_color.cpp`, `vulkan_shared_scene_depth.cpp`, `vulkan_shadow_island_live.cpp`'s zero-copy seam). **Dies-with-full-ownership**, not needs-alternative — once VK renders everything there is no GL surface to import from, so this whole extension family + the interop TUs that use it (color/depth/shadow shared-resource wrappers) get deleted, not ported. |
| Descriptor indexing / bindless | No `VK_EXT_descriptor_indexing` / `push_descriptor` hits in any `vulkan_*.cpp` | Not used. **Metal-clean by omission** — nothing to port, nothing to risk. |
| Geometry shaders | No `VK_SHADER_STAGE_GEOMETRY` / geometry-stage hits in `vulkan_*.cpp` or `shaders/vulkan/*` | **Metal-clean by omission.** Good — MoltenVK has NO geometry shader support; if this arc had used them it would be a hard port blocker. Confirmed absent. |
| Tessellation shaders | No tessellation-stage hits in `vulkan_*.cpp` or `shaders/vulkan/*` | **Metal-clean by omission.** MoltenVK's tessellation support is limited/partial; absence avoids the whole problem. Note: the GL side DOES use tessellation (per CLAUDE.md "GLSL, SSBOs, compute, tessellation, MDI") — if a future ownership rung ports a tessellated GL pass (e.g. terrain POM/displacement literally uses vertex displacement, not HW tessellation, per the grep above showing `visualDisplace` is vertex-shader-driven, not tess-stage) into VK, that's the moment to re-audit. Current terrain displacement path is vertex-shader based (`u_visualDisplace` uniform driving vertex position in the vertex shader), NOT a tessellation-control/eval stage — **this is actually good news for portability**, it means the geomorph/displace port to VK doesn't need tessellation shaders either. |
| `sampler2DArrayShadow` + depth-compare | `shaders/vulkan/csm_array_shadow.frag:28` `uniform sampler2DArrayShadow uShadowArray`, compareOp LESS (:53) | **Metal-clean.** Depth-compare samplers map directly to Metal's `MTLSamplerState` compare function; array-shadow samplers are supported. Per user's own note in the task, this is fine on Metal. |
| Timeline semaphores | No `VK_KHR_timeline_semaphore` / timeline-type hits anywhere in `vulkan_*.cpp` | Not used — arc uses binary semaphores (implied by `vk2gl_sem` naming in vulkan_shadow_island_live.cpp:363). **UNVERIFIED** MoltenVK timeline-semaphore support level (training knowledge: MoltenVK added timeline semaphore support in a relatively recent MSL/Metal-event-backed implementation, but exact version/robustness not verified here) — moot for now since unused; flag only if a future slice adds them for cross-queue sync. |
| GL-interop extensions overall | `VK_KHR_EXTERNAL_MEMORY*`/`EXTERNAL_SEMAPHORE*` families, `vulkan_shared_scene_color.cpp`, `vulkan_shared_scene_depth.cpp`, `vulkan_shadow_island_live.cpp` zero-copy seam, `vulkan_context.cpp:20-21` Win32-platform ifdef | **Dies-with-full-ownership.** This entire family (instance-capability exts, device exts, the three shared-resource wrapper TUs, the Win32-external-memory-handle plumbing) exists ONLY because today's arc is a hybrid GL-owns-most/VK-owns-islands design. The moment VK owns the full scene there is no GL FBO/texture to import — these TUs get deleted wholesale, not Metal-ported. This is the single biggest "risk that isn't a risk" in the audit: it looks like a huge portability problem today but it's actually scaffolding that the full-ownership ladder itself removes. |
| Swapchain / present | `VK_KHR_SWAPCHAIN` device ext; `vulkan_swapchain_present.cpp`, `vulkan_present_object.cpp`; today's flow copy-1(GL FBO-0→staging)+signal+copy-2(staging→VK swapchain blit) per VK-P14b | Metal-clean at the extension level (MoltenVK implements `VK_KHR_swapchain` via `CAMetalLayer`). Present-mode assumptions (FIFO/mailbox availability, `sp_log` reports "presented %u frames, %u resize(s)") **UNVERIFIED** against MoltenVK's present-mode support matrix — MoltenVK historically has had gaps in some present-mode/HDR paths, but FIFO (vsync) is universally supported so the arc's baseline path should be safe. Mark UNVERIFIED for anything beyond FIFO. |
| Surface creation (Win32-specific) | `VK_KHR_WIN32_SURFACE` (vulkan_context.cpp:174), gated behind an existing `#ifndef VK_USE_PLATFORM_WIN32_KHR` seam (:20-21) | **Needs-alternative, low risk.** The codebase already isolates the Win32-surface-creation behind a platform ifdef pattern — swapping to `VK_EXT_METAL_SURFACE` (or `VK_MVK_MACOS_SURFACE`, now deprecated in favor of EXT) is a contained, well-understood change, not a redesign. |

**Summary verdict:** No geometry shaders, no tessellation shaders, no descriptor-indexing/bindless,
no timeline semaphores — the arc is conservative Vulkan 1.1 core + a few interop/platform extensions.
The ONLY extension family that looks scary (`VK_KHR_EXTERNAL_MEMORY_WIN32` /
`VK_KHR_EXTERNAL_SEMAPHORE_WIN32`) is exactly the part that full VK ownership deletes on its own —
it is GL-interop scaffolding, not a rendering-feature dependency. **Biggest genuine Metal-risk item is
UNVERIFIED, not confirmed-bad**: present-mode matrix beyond FIFO, and MoltenVK's exact timeline-semaphore
maturity (currently moot since unused). Recommend treating the audit as **provisionally all-clean**
pending a real MoltenVK-on-macOS smoke test once VK owns enough of the frame to attempt one.

---

## C. LADDER — VK-owns-everything, smallest-first, each rung kills a copy

Every rung is gated (`MC2_VK_*`), default-OFF until proven, oracle-able with the existing
near-oracle/bookmark/footprint machinery (`mech_capture_witness.py`, height-audit pattern,
tier1 soak both-states).

1. **`MC2_VK_PRESENT_DIRECT_FBO`** (copy-1 elimination) — **RECOMMENDED SLICE 1.** Already spec'd
   in VK-P14b-PRESENT-TAIL-RECON.md, Size S-M, default-OFF, **landable independent of everything
   else**. Redirect `gos_postprocess.cpp:4038` FBO-0 bind to `EnginePresentVk`'s `s.copyFbo`; move
   Y-flip from copy-1 into copy-2 in the SAME commit (top risk #1 in that doc). Kills copy-1 tax
   immediately. Proof: existing `verifyReadbackContent()` + backend-compare bookmarks, no new
   fixtures. Not Metal-blocking-risk — pure FBO-bind redirection, no new VK API surface.

2. **Shadow-generation ownership (finish `95095a94` sub-slice B)** — live backend-compare for the
   already-built depth-only instanced shadow pipeline (single + CSM array). This flips VK from
   *copying* GL's shadow map to *rendering* it. Kills the remaining per-frame dynamic/CSM shadow
   copy (the ONE per-frame copy tax the static-shadow cadence-gate commit d60c6eb5 explicitly did
   NOT touch). Oracle: `compare_shadow.frag`/`.vert` already exist in `shaders/vulkan/` — reuse.
   Not Metal-blocking — depth-only instanced rendering + `sampler2DArrayShadow` compare, both
   confirmed Metal-clean above.

3. **Coarse terrain LOD1+ port (non-displaced bands)** — extend `vulkan_terrain_island.cpp` beyond
   `cmd.lodStep==1`; port the coarse-band draw call (no visualDisplace yet, no skirts yet — just
   the plain coarse mesh). Kills nothing new interop-wise by itself but is the prerequisite for
   rungs 4-5. Oracle: reuse `mc2_17`/`mc2_24` terrain-near bookmarks + the existing whole-frame
   near-poses sweep methodology from VK-FLIP-EVIDENCE-1 §a. Not Metal-blocking.

4. **Skirt + edge-stitch port** — port `skirtVbo`/`skirtIbo` + `u_skirtDepth` alongside rung 3 (they
   share the same LOD-boundary draw call, cheap to bundle once coarse bands exist). Kills nothing
   new interop-wise; closes a visual-crack gap that would otherwise appear once VK owns coarse LODs
   without skirts. Not Metal-blocking.

5. **visualDisplace port (LOD0 4x-finer + far-band fade, modes 1/2)** — the geomorph-interacting
   piece flagged in the task. Must replicate `u_visualDisplace` mode 0/1/2 vertex math bit-for-bit
   (vertex-shader-driven, confirmed NOT tessellation-stage in the audit above — good, no MoltenVK
   tessellation risk). Kills no additional interop copy but is required before VK can own the full
   terrain surface without visible LOD-transition popping between GL/VK boundary chunks. Oracle:
   the height-audit pattern (`MC2_VK_TERRAIN_HEIGHT_AUDIT`) extends naturally — add a displaced-
   vertex-position audit alongside the existing height-SSBO audit. Not Metal-blocking (vertex-shader
   math only).

6. **Water surface full ownership** — extend `vulkan_water_island.cpp` from today's low-coverage
   proof to full parity capture on mc2_17 (water_near/land_near) and mc2_01's water bookmarks
   (pyramid_islands_topdown, oblique_fog, cross_map_view). Must preserve the reversed-Z /
   terrain_depth_bias lockstep (mc2-water-depth-expert domain — cross-check with that agent's
   contract before touching depth-fudge math). Kills nothing new interop-wise (water wasn't a
   dedicated copy path) but closes the last major "GL still owns this geometry region" gap besides
   mechs' material/lighting. Not Metal-blocking at the API level; reflection sampling may need
   scrutiny if it currently depends on any GL-side render-to-texture assumption baked into the
   shared-GBuffer contract — **flag for re-check during the slice, not a blocker today.**

7. **Mech material/lighting full PBR (kills paint-scheme SSBO copy)** — per memory, currently
   "flat probe deferred" (calc_light-Lambert approximation, not full PBR). Bringing mech shading to
   parity is what retires the paint-scheme SSBO copy identified in section A. **Metal-blocking-risk
   flag: LOW** — this is shader math/texture sampling, not new VK API surface, but if the PBR path
   leans on any GL-specific texture-array binding trick, re-verify against MoltenVK's texture-array
   support (generally fine, but UNVERIFIED for this specific codebase's exact sampler layout).

8. **Postprocess composite/tonemap/pillarbox + UI/HUD/ImGui into VK, then delete GL-interop
   scaffolding** — the final rung. Port `gos_postprocess.cpp:4109-4217` composite/tonemap/FXAA into
   `vulkan_postprocess_subgraph.cpp` (partially scaffolded already); wire `imgui_impl_vulkan` in
   place of `imgui_impl_opengl3`; write a new `vulkan_hud_island.cpp` for the GameOS 2D HUD batch
   (no existing VK equivalent — greenfield within this arc's pattern). **Only once this rung lands**
   do the `VK_KHR_EXTERNAL_MEMORY_WIN32`/`EXTERNAL_SEMAPHORE_WIN32` extensions and the three
   shared-resource wrapper TUs (`vulkan_shared_scene_color.cpp`, `vulkan_shared_scene_depth.cpp`,
   `vulkan_shadow_island_live.cpp`'s zero-copy seam) become deletable — this is the rung where ALL
   remaining interop copies die at once, because there's no more GL side to interop with. **Metal-
   blocking-risk flag: MEDIUM** — ImGui's Vulkan backend is mature and Metal-portable via MoltenVK,
   but this is the largest and riskiest single rung (UI input plumbing, HUD batch semantics, and the
   final deletion of the Win32-only extension family all land together); budget it as its own multi-
   session arc, not a single slice.

**Slices 3-5 should be sequenced together** (they share the same LOD-chunk draw call site) but are
listed separately because each is independently oracle-able and gateable. **Slice 1 and slice 2 are
fully independent of the terrain/water/mech work and of each other** — either can land first.
