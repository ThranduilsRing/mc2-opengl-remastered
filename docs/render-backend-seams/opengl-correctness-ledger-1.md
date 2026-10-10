<!-- doc-status: current | reviewed:2026-10-11 -->
# OPENGL-CORRECTNESS-LEDGER-1

Scoreboard for the OpenGL correctness campaign. Goal: **every known GL correctness
debt is either FIXED, PROVEN_DEAD, PROVEN_COVERED, DEFERRED_LOW_RISK with reason, a
tracked LIVE_BUG, or NEEDS_VENDOR_TEST.** Not a cleanup binge — one correctness issue
per slice, each with before/after trace, test, or reasoned proof.

Part of the RENDER-BACKEND-SEAMS arc. Sibling: `gpu-buffer-owner-recon-1.md`.

## Status taxonomy
- **FIXED** — shipped + validated; commit recorded.
- **PROVEN_DEAD** — code path has no live caller / no loader; documented, ignore.
- **PROVEN_COVERED** — already handled by existing code; evidence recorded.
- **DEFERRED_LOW_RISK** — real but low impact; reason recorded, revisit later.
- **LIVE_BUG** — confirmed correctness bug; needs a fix slice.
- **NEEDS_VENDOR_TEST** — suspected vendor-specific; needs NVIDIA/AMD run to confirm.

## Operating rules (for every slice + agent)
1. One correctness issue per slice.
2. No speculative visual changes. No PBR/color changes. No Vulkan code. No broad rewrites.
3. Every fix needs a before/after trace, a test, or a reasoned proof.
4. Every "not a bug" claim needs code-path evidence (file:line).

## Definition of done (campaign exit)
No known: live GL-state leaks · unfenced buffer reuse · missing barriers · raw-texture
binding leaks on live paths · high-severity GL debug messages · unexplained
vendor-specific disappearances. All dead/legacy paths documented or ignored.

## Work order
1. ✅ WATER-THINRING-FENCE-1 — **FIXED** `bc424dc2`
2. NVIDIA-TREE-DISAPPEAR-TRACE-2 / fix — recon in flight
3. OVERLAY-MISSING-TEXTURE-TRACE-1 / fix — recon in flight
4. TEXTURE-HANDLE-LEAK-RECON-1 — recon in flight
5. GPU-BUFFER-WRAPPER-DESIGN-1 — queued (architecture, after correctness)
6. GLSTATE-TEXTURE-ARRAY-RESTORE-1 (if still live) — recon in flight
7. PASS-DESC-POSTPROCESS-RECON-1 (dead bloom/HDR vs live) — recon in flight
8. GLSL-UB-AUDIT-2 — recon in flight
9. VENDOR-CERT-PACK-1 — capstone (run the NEEDS_VENDOR_TEST set on both vendors)

Principle: **known correctness bugs first, architecture wrappers second, fidelity features later.**

## Ledger
Row schema: `id | subsystem | risk | vendor | status | evidence | commit | test | remaining action`

| id | subsystem | risk | vendor | status | evidence | commit | test | remaining action |
|---|---|---|---|---|---|---|---|---|
| NVIDIA-TREE-DISAPPEAR | objmgr R2b static-natural update-skip vs static-prop registry | H | NVIDIA (CPU defect vendor-independent; symptom NVIDIA-only) | FIXED `07a1f8ac` | **CPU update/touch CONTRACT bug, NOT a GPU class** (corrects the TREE-N* GPU hypotheses below — all PROVEN_COVERED/not-cause). R2b bare `continue` (objmgr.cpp) skipped update() → registered multi's cachedFrame_ frozen → registry flush drops it (gos_static_prop_registry.cpp:986). Black-tree-bug class reintroduced (.planning/PROJECT.md). Fix = stamp cachedFrame_ in skip path (gate MC2_R2B_TOUCH_PRESERVE default-on) + registry stale_after_drawn guard (counts/logs/opt-in-fatal) so it can't silently recur | `07a1f8ac` | **counter A/B**: preserve=0 → stale_after_drawn=15336 (tree typeIDs dropped, cachedFrame=frame-1); preserve=1 → 0, touched_liveness=11. tier1 PASS | on-NVIDIA eyeball pending user tester (AMD draws stale instance so symptom invisible there) |
| WATER-THINRING-FENCE-1 | terrain/water thin-record ring | H | both (NVIDIA-leaning) | FIXED | g_thinBuffer was barrier-only; solid ring fenced. gos_terrain_water_stream.cpp | `bc424dc2` | tier1 5/5 normal+trace; MC2_WATER_THINRING_TRACE shows waited 0→1, 0 GL_TIMEOUT_EXPIRED | none |
| RENDER-PASS-CONTRACT-ENFORCEMENT-1 | render-pass scope discipline (tooling) | — | both | FIXED | render_contract.cpp scope tracker | `8d250041` | tier1 5/5 gate on/off, 0 violations | extend scopes (ENFORCEMENT-2) |
| TREE-N8 | static-prop cull → indirect draw barrier | H | NVIDIA | PROVEN_COVERED | barrier present + intentional: `gpu_cull_compute.cpp:1297` `glMemoryBarrier(SHADER_STORAGE\|COMMAND)` after patch dispatch, before MDI draw (verified) | — | none (refutes the #1 NVIDIA-tree hypothesis) |
| TREE-N6/N7 | static-prop tree ring (align + fence) | L | NVIDIA | PROVEN_COVERED | alignment query+disarm `gos_static_prop_batcher.cpp:2622-2656`; fence ring `5479-5484`; project_nvidia_hardening_s2 | — | none |
| TREE-N2 | veg cards blockVis SSBO OOB read | M | NVIDIA | NEEDS_VENDOR_TEST | SSBO 1024 slots `gos_vegetation.cpp:228-237` vs unbounded `chunkSide²` index `gos_vegetation_card.vert:74-77`; NVIDIA robust-read→0u→clip-corner cull. ONLY live if `MC2_VEGETATION_CARDS` set | log blockCount vs 1024; run >32×32 map on NVIDIA | size SSBO to actual blockCount; bounds-guard |
| TREE-N1/N3 | veg cards instance VBO reuse / depth state | M | both | NEEDS_VENDOR_TEST | unfenced `glBufferData(GL_STATIC_DRAW)` per frame `gos_vegetation.cpp:305-307`; `GL_GREATER` vs TERRAIN_DEPTH_FUDGE `366-376` | `MC2_VEG_DEBUG_FORCE_VISIBLE=1`; depth-func override | only if veg-cards path is the reporter's symptom |
| OMT-1 | overlay/decal batch texture bind | M | HIGH (bind-0 sampling driver-defined) | FIXED `36d6a254` | bound GL name 0 on resolve-fail → incomplete texture (driver-defined). Now `bindBatchTextureOrFallback()` binds explicit 1×1 magenta fallback + once-per-handle log + counters; success path byte-unchanged | tier1 5/5 + mc2_01/24 trace PASS, resolve_failed=0 | none. COVERAGE CORRECTION: `drawDecalStaticBatch` (GameOS/gameos/gameos_graphics_overlay.cpp:604) IS the LIVE indirect cement-overlay/road/runway static-decal draw (driven by DrawDecalStatic), NOT dormant — OMT-1 protects it. The 0-pushes counter was only the per-frame `gos_PushTerrainOverlay` producer; the static decal bake is separate + live. So OMT-1 covers a real live draw-time bind |
| OMT-2-INDIRECT | GPU-indirect terrain texture resolution (colormap atlas / handle LUT / cement atlas / static decal / mine atlas) | M | AMD (bind-0/incomplete vendor-defined) | PROVEN_COVERED | NO indirect bind-0 equivalent — every resolve-fail is guarded: handle-LUT fail→store 0→compute SKIPS quad (`gpu_driven_terrain_solid.comp:290`; terrainHandle never sampled in frag); cement atlas fail→skip tile→cementValid=false→colormap fallthrough (`GameOS/gameos/gos_terrain_indirect_cement.cpp:385`, `gos_terrain.frag:438`); static decal fail→skip at bake (`GameOS/gameos/gos_terrain_indirect_dispatch.cpp:489`)+OMT-1 fallback at draw; mine fail→early-return+retry (`:3962`); colormap=pre-baked, no per-quad resolve | MC2_TERRAIN_INDIRECT_TRACE + MC2_OVERLAY_MAGENTA_TRACE | none. LOW residual: static-decal gosHandle captured at bake → eviction-after-bake shows OMT-1 magenta (visible+logged, not UB); optional `g_decalVBODirty=true` on texture eviction to re-bake |
| OMT-2 | engine missing-texture magenta fallback | M | low (deterministic) | PROVEN_COVERED | fill `GameOS/gameos/gameos_graphics_renderer_internal.h:342-348`; probe wired `1198-1204` (`MC2_OVERLAY_MAGENTA_TRACE`) | `MC2_OVERLAY_MAGENTA_TRACE=1 MC2_DIAG_TAGS=OVERLAY_MAGENTA` | run trace on offending mod mission |
| OMT-3 | colormap "no-data" magenta texels | M | low (CONTENT) | DEFERRED_LOW_RISK | haze guard `terrain_overlay.frag:176-187`; only fires when `mapHalfExtent>0`. All sightings on MOD missions. Primary cause of observed magenta = content, not GL | check `getMapHalfExtent()` on failing map (0→guard dead) | confirm mapHalfExtent set; widen band only if needed |
| GLSTATE-GUARDS-1 | live draw-path GL-state restore (texunit0, 2D_ARRAY unit4, SSBO, depth/blend/cull, clipControl) | M | NVIDIA | PROVEN_COVERED | `gl_state_guard.h` RAII suite; overlay/decal/decalStatic guards `gameos_graphics.cpp:9398/9516/9589`; indirect 2D_ARRAY restore `:3888-3897/:4044-4074` | `00adb32d`,`783406a8`,`b51dbc41`,`cfa538cf`,`615ead17` | tier1 + `MC2_GL_DEBUG_FATAL=1` clean | none — no live leaks in default-on paths |
| GLSTATE-TEXTURE-ARRAY-RESTORE-1 | terrain indirect transition-mask sampler2DArray (unit4) | M | NVIDIA | PROVEN_COVERED | capture `GameOS/gameos/gameos_graphics_water.cpp:1033-1040`, restore `:4044-4050` | `615ead17` | tier1 `MC2_GL_DEBUG_FATAL=1` | none (explicitly NOT live) |
| GLSTATE-SHADOWDEBUG-2DARRAY-1 | postprocess shadow-debug overlay leaves GL_TEXTURE_2D_ARRAY on unit0 | L | NVIDIA | DEFERRED_LOW_RISK | `gos_postprocess.cpp:2632-2645` binds tex, never rebinds 0; gated `showShadowDebug_` (default off, ImGui-only) | toggle showShadowDebug_ + `MC2_GL_DEBUG_FATAL=1` | add `glBindTexture(target,0)` at :2645 IF ever default-on |
| GLSTATE-DEBUG-CALLBACK-1 | GL debug HIGH-severity handling | — | both | PROVEN_COVERED | `gameosmain.cpp:738` print HIGH/MED, `:754-763` opt-in abort `MC2_GL_DEBUG_FATAL=1`, register `:1149` | — | tier1 `MC2_GL_DEBUG_FATAL=1` → no abort = no live HIGH msgs | none |
| POSTPROCESS-CONTRACT-OBS-1 | postprocess passes not pass-scope-tracked | L | n/a (observability) | DEFERRED_LOW_RISK | render_contract scopes only on TerrainOverlay/Decal/UI/PostProcess-outer; no inner gos_postprocess pass scopes | — | — | fold into ENFORCEMENT-2 / PASS-DESC-POSTPROCESS so the tracker can catch leaks in bloom/shadow/skybox |

| UB2-02 | mech.frag StandardLit PBR | H | both | FIXED `55f6cc71` | non-uniform branch on `flat in v_mechSunFound` wrapped `texture()`+`dFdx/dFdy`. Split: sampling now under uniform `u_standardLitEnabled`, application under inner `v_mechSunFound` | tier1 5/5, shader_ok prog=191, mechs render | none (gate MC2_STANDARD_LIT_V1 default-off; runtime no-op) |
| UB2-01 | terrain_lod_chunk.frag detail normals + POM + cement | H/M | both | FIXED `f4208726` | implicit-LOD in non-uniform weight branches / anti-tile / POM loop / pureConcrete branch → textureGrad (uniform-scope gradients) + textureLod(0) for POM | **byte-exact visual gate PASS 9/9** (golden ub201-pre2, mc2_10/17/24) + tier1 5/5 | none — pixel-neutral on AMD, now NVIDIA-safe |
| UB2-05 | building_pbr.frag | M | both | FIXED `1851b16d` | conditional ALPHA_TEST `discard` preceded deriv+`texture()` (`:35-38`,`:50-59`) → hoisted sampling + derivative TBN above the discard (uniform flow) | tier1 5/5, shader compiles clean (compiled at init regardless of default-off MC2_BUILDING_PBR), reorder output-neutral | none |
| UB2-04 | shadow.hglsl PCF | M | both | NEEDS_VENDOR_TEST | `dFdx/dFdy(projCoords.z)`+`texture(shadow)` after divergent early-returns `:35-48,62-73,...`. Single-mip shadow blunts real impact | force-constant adaptiveScale A/B | compute gradient before returns, or document-and-accept |
| UB2-06/07 | shadow_depth.frag / shadow_instanced.frag empty main | H(claimed)→ | AMD | NEEDS_VENDOR_TEST | empty `void main(){}` (`shadow_depth.frag:1-6`, `shadow_instanced.frag:2`); amd-driver-rules says needs explicit gl_FragDepth. **COUNTER-EVIDENCE: dev runs 7900 XTX, campaigns soak-clean WITH shadows → empirically working on AMD.** Carried unfixed since 2026-05-19 | confirm on AMD: do prop/mech shadows render? (they do today) | likely no-op; if ever broken, 1-line `gl_FragDepth=gl_FragCoord.z` |
| UB2-09 | uniform uint (cardcloud_sim.comp / particle_billboard.vert) | L | both | PROVEN_COVERED | `shader_builder.cpp:641-648` parses uint as CONSTANT_INT since VFX-FLIPBOOK; 2 live decls compile. **memory `uniform_uint_crash.md` is now STALE/over-broad** | already live | amend memory: was-crash, parse-time-fixed, int+cast now optional |
| UB2-10 | gos_grass.geom | — | — | PROVEN_DEAD | no loader (MEMORY terrain-overlay handoff) | — | none |
| TEXHANDLE-1 | raw GL texture-id extraction (gos_GetTextureGLId / gos_GetGLTextureId), 15 call sites | L | n/a (architecture, not correctness) | PROVEN_COVERED | all 15 callers hold a live gosHandle or resolve from a validated live source each frame; all glBindTexture null-safe; NO use-after-free, NO stale-id-cached-across-frames. Sites: gameos_graphics.cpp:7606/5329/2621/2550/9407/9524/9598, gos_terrain_indirect.cpp:1165/3964, gos_mech_batcher.cpp:2168, gos_static_prop_batcher.cpp:3140/7131/7518/7809 | tier1 RenderDoc | NONE for correctness. Opaque-handle migration is GpuBuffer-arc work; easy-first order: font atlas → mech diag → static-prop buildtime dedupe |

| DEAD-POST-FX-CLEANUP-1 | postprocess bloom + ACES tonemap + FXAA + god rays | L | none | REMOVED `92d3a821`+`9c2187d8` | bloom/ACES/FXAA PROVEN_DEAD (default-off, composite force-0) + god rays CONDITIONAL (default-off, RAlt+6) — all wrong-for-RTS. Deleted: members/FBOs/programs/runBloom/runGodRays/3 shader files (bloom_threshold,bloom_blur,godray)/ImGui/hotkeys/env-gates/postprocess.frag branches. LIVE composite preserved (sunset grade, exposure, ObjectIdDebug/Thermal/LowLight view-modes, HZB/SSAO/screen-shadow/shoreline/waterRefl) | tier1 5/5, composite renders, no shader-compile errors | none. NOTE: bloomThreshold/bloomIntensity profile keys + RendererFeatureRegistry MC2_HDR_POST/BLOOM/TONEMAP_ACES now inert (annotated removed) |
| POST-GODRAYS-FBBIND-1 | god-rays pass-2 FBO restore | L | none | MOOT (god rays deleted) | the FBO-restore gap no longer exists — runGodRays() removed entirely by DEAD-POST-FX-CLEANUP-1 `92d3a821` | n/a | none |
| POST-WATERREFL-PLACEHOLDER-1 | waterReflFBO_ allocated, no producer | L | none | DEFERRED_LOW_RISK | `gos_postprocess.cpp:692-749` Phase-C placeholder, reads black | — | keep + doc "producer TBD" |
| POST-STATE-RESTORE-1 | postprocess pass GL-state restore | M | both | PROVEN_COVERED | every live pass (HZB/SSAO/screenShadow/shoreline/godray) saves+restores viewport/depth/blend; endScene ends with gos_InvalidateRenderStateCache `:1917` | tier1 `MC2_GL_DEBUG_FATAL=1` | none (95% correct + cache-invalidate backstop) |

## Prioritized fix queue (distilled from all 6 recons; reordered 2026-06-21 after veg-cards ruled out)
Correctness bugs first, then cleanup, then architecture. Each = its own behavior-preserving, smoke-gated slice.
1. ✅ **UB2-02** — mech.frag StandardLit. FIXED `55f6cc71`.
2. ✅ **UB2-01** — terrain_lod_chunk.frag detail-normals/POM/cement. FIXED `f4208726` (byte-exact visual gate PASS 9/9). Established the golden-gated-shader-fix flow (capture blessed golden on same exe → change → `run_visual.py compare` byte-exact).
3. ✅ **OMT-1** — overlay/decal bind-0 on resolve-fail. FIXED `36d6a254` (explicit 1×1 magenta fallback + log + counters; behavior-neutral on stock).
4. ✅ **UB2-05** — building_pbr discard-before-sample. FIXED `1851b16d` (hoisted above discard; tier1 5/5, neutral).
5. ✅ **DEAD-POST-FX-CLEANUP-1** — bloom/ACES/FXAA + god rays DELETED `92d3a821`+`9c2187d8` (tier1 5/5; live composite preserved).
6. ✅ **POST-GODRAYS-FBBIND-1** — MOOT (god rays deleted, the FBO-restore gap is gone with it).

**Queue clear.** All confirmed LIVE_BUGs fixed + all cleanup done. POST-FX-CLEANUP-CLOSURE-AUDIT-1 (grep/build/deploy/test sweep, 8 checks) = **CONFIRMED_CLEAN**: no runtime- or build-facing ghosts; only intentional retained refs (index-preserving registry entries annotated [REMOVED], auto-gen docs, historical notes). Remaining open items are NEEDS_VENDOR_TEST only (await a vendor run): on-NVIDIA tree-disappear visual confirm, empty shadow frags UB2-06/07, shadow PCF UB2-04. Plus deferred-low-risk: TREE-N2 veg-cards OOB (veg shipped-off), static-decal stale-handle rebake.

All confirmed shader/CPU LIVE_BUGs in the queue are now FIXED. Remaining items are cleanup (dead-code) + a 1-line tidy. NEEDS_VENDOR_TEST items (empty shadow frags UB2-06/07, shadow PCF UB2-04, on-NVIDIA tree-disappear visual confirm) await a vendor run.

Parked (NEEDS_VENDOR_TEST, no repro available): TREE-N2 veg-cards OOB (veg cards shipped-DISABLED — real latent bug but not the tree symptom), NVIDIA static-prop tree-disappear (predates veg cards; known classes all covered; needs fresh capture), UB2-04 shadow PCF derivatives, UB2-06/07 empty shadow frags (empirically fine on dev AMD).

NEEDS_VENDOR_TEST (park until a vendor run is possible): UB2-04 (shadow PCF derivatives), UB2-06/07 (empty shadow frags — empirically fine on dev 7900 XTX), OMT-4 (indirect cement path).
Hygiene: amend memory `uniform_uint_crash` (parse-time-fixed, rule over-broad).

## Campaign status
Of 8 reconned items: **most GL-correctness debt is PROVEN_COVERED** (GL-state guards, texture-handle leaks, texture-array restore, cull→draw barrier all already handled). Real remaining LIVE_BUGs are narrow and listed above — dominated by shader non-uniform-flow UB + the veg-cards OOB. No high-severity GL debug messages on the live path. Wrapper design (item 5) tracked separately in gpu-buffer-wrapper-design-1.md.

### Recon status — ALL 6 background agents MERGED (2026-06-21)
- ✅ item 2 NVIDIA-TREE-DISAPPEAR-TRACE-2 — lead refuted (barrier present); narrowed to veg-cards path.
- ✅ item 3 OVERLAY-MISSING-TEXTURE-TRACE-1 — magenta = CONTENT (mod colormaps); latent GL bug OMT-1.
- ✅ item 4 TEXTURE-HANDLE-LEAK-RECON-1 — 15 sites all PROVEN_COVERED, 0 correctness bugs.
- ✅ item 6 GLSTATE / GL-state-leak audit — gl_state_guard.h suite covers it; no live leaks.
- ✅ item 7 PASS-DESC-POSTPROCESS — bloom/ACES/FXAA PROVEN_DEAD; state restore covered.
- ✅ item 8 GLSL-UB-AUDIT-2 — the real findings (UB2-01/02/05 non-uniform flow).

### RESOLVED: NVIDIA-tree-disappear — root cause found + FIXED (2026-06-22)
NOT a GPU class. The recon (TREE-N*) correctly cleared GPU buffer/barrier/alignment/veg-cards
hypotheses — but missed the actual cause, which is a **CPU update/touch contract bug** the
GPU-focused sweep didn't look for. User's deeper fan-out + RenderDoc A/B closed it:
objmgr.cpp R2b "skip static-natural update" (DEFAULT-ON) bare-`continue`s static trees at
turn>=3, skipping update() → the registered TG_MultiShape's `cachedFrame_` freezes → the
static-prop registry flush (`gos_static_prop_registry.cpp:986`) drops every range whose
`getCachedFrame()!=currentFrame` → tree instances never written to the SSBO → tree typeID
draws absent in RenderDoc. This is the 2026-05-05 black-tree-bug class (the named cachedFrame_
contract) **reintroduced** by the later R2b perf optimization. FIXED `07a1f8ac` (stamp in skip
path + registry stale_after_drawn guard). Veg cards (shipped-DISABLED) were never involved;
TREE-N2 OOB remains a separate real-but-latent item.

## GLSTATE-GUARD-ADOPTION-1 (first implementation off the recon — 2026-06-22)
Adopt-not-port. Wire existing `gl_state_guard` RAII into GPU-direct passes. One pass family / commit, behavior-neutral, smoke-gated. Order: post-process → water → particles/decals → static-prop/mech batchers → shadows LAST.

| # | Family | Status | Commit | Notes |
|---|--------|--------|--------|-------|
| 1 | post-process (`gos_postprocess.cpp`) | ✅ PARTIAL | `6de2cbb0` | Recon found post-fx is **hard-reset + single invalidate@2281**, NOT save/restore-previous — so depth/cull/blend guards would be a semantic change, and the only true save-previous patterns (viewport/VAO/compare-mode) have NO guard (would need new types). Narrowed (user: confirmed post-fx textures leaking into menus) to the genuine unrestored 2D tex-unit leak: composite unit-0/unit-2 bindings persist past invalidate (invalidate doesn't track tex units) → wrapped with `GlScopedTextureUnit`, block-scoped to close before invalidate. tier1 5/5, `MC2_GL_DEBUG_FATAL=1` clean, visual PASS. **Deferred in-family:** depth/cull/blend hard-reset paths, 2D_ARRAY shadow sites (runScreenShadow units 3/4, drawShadowDebugOverlay) — guard lacks array-target support. |
| 2 | water fast path (`renderWaterFastPath`) | ⏳ next | — | |
| 3 | particles / decals | ⏳ | — | the family `GlScopedTextureUnit` was designed for (header L160-177) |
| 4 | static-prop / mech batchers | ⏳ | — | |
| 5 | shadows | ⏳ LAST | — | most historical weirdness |

### STRATEGIC CORRECTION (2026-06-22, post family-1) — narrow to tex-unit leak closure
Original GLSTATE-GUARD-ADOPTION premise narrowed. Existing passes mostly hard-reset state and invalidate the cache; replacing that with RAII would be semantic churn, and the broad save/restore the audit implied does not exist. The live class is **unrestored texture-unit binding leakage into later 2D/menu/HUD consumers**. Continue as tex-unit leak closure only — mentally **GLSTATE-TEXUNIT-LEAK-GUARDS-1** (family 1 stays cataloged here). Drop the "~8-site invalidate list collapses" acceptance goal; those invalidates exist *because* passes hard-reset.

Work was isolated in worktree `glstate-texunit` on branch `claude/glstate-texunit-leak-guards-1` (off nifty `beec2da8`) and merged back. (nifty was being concurrently mutated by another lane — shared-tree hazard, so this lane forked its own worktree.)

**Per-family criterion (recon-first; edit ONLY on a proven live leak):**
A. list texture-unit binds · B. does the pass restore prev binding? · C. what 2D/menu/HUD runs after? · D. if a leak reaches a later consumer → wrap with `GlScopedTextureUnit` · E. if the composite/final pass overwrites it before anything observes → leave it.
YES: GL_TEXTURE_2D unit leaks; sampler/compare-mode only if an existing guard covers it. NO: depth/cull/blend hard-resets; viewport/scissor/VAO unless an existing guard already exists & span is simple; 2D_ARRAY unless proven to leak to a later 2D consumer; any new guard type in this slice.

**Revised family priority:** 2. HUD/menu/2D overlays + decals → 3. particles/water/shoreline → 4. mech/static-prop/terrain batchers → 5. shadows LAST (2D_ARRAY/depth-compare heavy; touch only on a proven leak into ordinary 2D draw). Commit only families with a real fix; no-leak family = ledger note or skip.

### CLOSURE (2026-06-22) — GLSTATE-TEXUNIT-LEAK-GUARDS-1 COMPLETE (1 real fix)
Recon swept all five families for the unrestored-GL_TEXTURE_2D-unit-leak class. Result: the composite (family 1, `6de2cbb0`) was the ONLY live leak. Everything else already covered:

| family | surface | verdict |
|--------|---------|---------|
| 1 post-process | composite unit-0/2 | ✅ FIXED `6de2cbb0` (only live leak) |
| 2 HUD/menu/2D + decals | drawTerrainOverlays / drawDecals / drawDecalStaticBatch | already guarded (`GLSTATE-TEXTURE-UNIT0-RESTORE-1`); HUD via `applyRenderStates` re-bind → MASKED. No fix. |
| 3 particles/water/shoreline | gos_particle_bridge, gos_vfx_mesh_bridge, renderWaterFastPath | per-fn manual save/restore epilogues cover unit-0/1 → MASKED. No fix. |
| 4 mech/static-prop batchers | gos_mech_batcher (units 0-4), gos_static_prop_batcher (unit 0) | flush prologue saves + epilogue restores units 0-4 + active → MASKED. No fix. |
| 5 shadows / 2D_ARRAY | drawMineStatic, terrain shadow/normal arrays, static-prop ORM/veg buckets | GL_TEXTURE_2D_ARRAY + depth-compare samplers — outside `GlScopedTextureUnit` scope → SKIP (not this class). |

Net: the tex-unit-leak class is now closed for `GL_TEXTURE_2D` across the GPU-direct path. No new guard types added. The broad "adopt RAII everywhere / collapse invalidate list" goal was correctly dropped — those passes hard-reset by design.
Residual (NOT this slice): 2D_ARRAY-target leak class (would need an array-aware guard variant) — open only if a RenderDoc NVIDIA inspection shows a 2D_ARRAY binding bleeding into a later array consumer. Vendor-visibility of all "MASKED via applyRenderStates" verdicts is AMD-tested-only; confirm on NVIDIA if certainty needed.

### RENDER-BACKEND-SEAMS arc — status snapshot (2026-06-22)
Strictness/visibility tooling banked; no immediate runtime correctness item left.

| item | status |
|------|--------|
| VULKAN-READINESS-AUDIT-1 | ✅ banked (verdict: renderer Vulkan-shaped; gap = strictness BUILT-not-ADOPTED) |
| GLSTATE-TEXUNIT-LEAK-GUARDS-1 (post-fx composite leak) | ✅ FIXED `6de2cbb0` (only live tex-unit leak; menu-bleed symptom gone) |
| GPU-BINDING-SLOTS-LOCKSTEP-1 (binding-slot lockstep + occupancy checker) | ✅ SHIPPED `00dc79dd` — replaced the disproven flat-enum idea; CI checker + occupancy doc |
| GLSTATE-SSBO-SLOT14-PARTICLE-UNBIND-1 | ✅ **PROVEN_COVERED / MOOT** — investigated, no bug: D-01 symmetric unbind already in `gos_particle_bridge.cpp` (499→537, 753→791, 1045→1150). NOT deferred. |
| GpuBuffer/GpuRingBuffer wrapper adoption (design §4) | ⏸ architectural adoption, NOT a correctness patch — do NOT start by momentum; recon-only GPUBUFFER-WRAPPER-ADOPTION-PLAN-1 if revived |

No open runtime correctness item in this arc. Remaining work is the (deferred, gated, per-subsystem) wrapper adoption — start only on explicit direction.
