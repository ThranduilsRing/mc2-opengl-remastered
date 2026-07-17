# DEFERRED-2 sweep status (branch claude/deferred-sweep-2, base nifty 994c3776)

Worktree: A:/Games/mc2-deferred-2. Deploy lane: A:/Games/mc2-opengl/releases/mc2-win64-deferred2-test.
Three items from DEFERRED-BOARD-2026-07-16.md §A, one commit each, in order. NEVER push.

## Item 1 — GPU-BUFFER-OWNER TIER0-LODCHUNK RAII (DONE — sha 99847ed7)

Scope (design gpu-buffer-wrapper-design-1.md §4 row A + handoff 9afdb47b): the 9 lod-chunk
buffer statics in gos_terrain_lod_chunk.cpp = 5 SSBO owner records (height, visualHeight,
visualDamp, type, cement) + 4 per-patch buffers (ps.vbo/ibo/skirtVbo/skirtIbo).

State:
- [x] Recon: precedents 156bd9da (skeleton) / 105bf8da (type+cement identity) / 68f98cb8 (design);
      Tier-2 fenced-ring NOT applicable (lod-chunk = no ring, no fence — design row A).
- [x] NEW RenderCore/GpuBuffer.h — GL-free thin owning class (HAS-A GpuBufferOwner; move-only;
      bind/bufferData/bufferSubData/bindBase/unbind mirror raw GL 1:1; create/destroy + registry
      upsert/invalidate + gated KHR_debug label centralized). No DSA (OD-3 open), no hitch-macro
      change (OD-4 open), no ring.
- [x] NEW GameOS/gameos/gpu_buffer_gl.cpp — GL impl (mirrors view_uniforms_gl.cpp split).
- [x] gos_terrain_lod_chunk.cpp: ALL 9 converted (5 SSBOs GpuBufferOwner→GpuBuffer w/ same
      (id,lifetime,name) args; PatchShape 4 members → untracked GpuBuffer; Init/Destroy/5 upload
      entries/dirty-patch/draw-binds). Registration blocks + teardown blocks folded into class.
- [x] CMake: gpu_buffer_gl.cpp added to GameOS/gameos/CMakeLists.txt.
- [x] scripts/check-gpu-buffer-owners.py: OWNER_DECL extended to parse GpuBuffer{id,lifetime,name,kind}
      sites. Gate run: PASS, 14 sites (5 lod-chunk ones still identity-checked), partition clean.
- [x] Build exit 0 (mc2 + mc2_launcher). Deploy COMPLETE to lane, manifest src_commit
      99847ed772d5 (clean, no -dirty).
- [x] tier1 smoke: PASS 5/5 (mc2_01/03/10/17/24), +0 destroys all, DEPLOY_FINGERPRINT OK
      (exe sha=99847ed772d5 == HEAD). Artifacts: tests/smoke 2026-07-16T10-31-45.
- [x] Parity counters (lane dump latest_render_state.json): terrain_path lod_chunk=4205,
      indirect=0, patch_stream=0, legacy_mlr=0 (all tripwires silent). Registry rows via new
      GpuBuffer::registerNow: TerrainHeightSsbo/TypeSsbo/CementSsbo sizeBytes=57600 (=120²×4 ✓).
- [x] Offline doctests 325/325 passed (114822 assertions).
- [x] Commit sha: 99847ed7

## Item 2 — Shadow VRAM: far cascade → separate 4096 texture (DONE — sha eebccc60; board ask was already shipped at 13365011, this slice adds the audit surface + evidence)

★ BOARD ITEM IS STALE — THE EXACT ASK ALREADY SHIPPED: commit `13365011` (2026-06-16,
"feat(shadow): per-cascade resolution — full-map cascade in separate 4096 texture", ancestor of
base 994c3776), default ON. Near cascades 0/1 stay in the 8192² 2D_ARRAY (2 layers); the far
(full-map) cascade renders into its own 4096² 2D depth texture. Gates: MC2_SHADOW_FULLMAP_SEPARATE
(default ON) + MC2_SHADOW_FULLMAP_SIZE (default 4096; =0 A/B forces fullmap==8192, the old
all-in-array-equivalent VRAM). Alloc site: gos_postprocess.cpp initDynamicShadows (~:4657).
The board's "805 MB default today" = legacy single 8192² map (268.4 MB) + 2×8192² near array
(536.9 MB) — the far cascade is already only 67.1 MB.

Remaining honest lever (NOT taken — user-gated): the legacy single 8192² dynamic map
(dynShadowDepthTex_ 268.4 MB + 67.1 MB R8 dummy) is still allocated AND rendered every frame even
under CSM (txmmgr.cpp:2851 legacy pass runs, then the CSM cascade replay re-renders); its texture
is never SAMPLED when csmActive, but getDynamicShadowFBO() is the "dynamic shadows on" flag at
~15 consumer sites. Retiring it = the board B4 "shadow-island default-arm (unlocks CSM cut)"
decision — deferred to the human.

This item's commit: SHADOW-VRAM-ACCOUNT-1 — register the CSM near-array + full-map cascade in the
RenderResourceRegistry (observe-only, byte-identical rendering) so the shadow VRAM split is
auditable from the dump (this IS the existing VRAM counter surface: renderResources[].sizeBytes).
- [x] RenderResourceId::ShadowCsmNearArray(40)/ShadowCsmFullMap(41) + toString + init register +
      destroy invalidate.
- [x] Build 0, deploy (src_commit eebccc600deb), tier1 PASS 5/5 +0 destroys (2026-07-16T10-47-57).
- [x] VRAM counter evidence (renderResources[].sizeBytes, the registry dump = the existing counter):
      DEFAULT run: ShadowCsmNearArray 8192²×2 = 536,870,912 B; ShadowCsmFullMap 4096² = 67,108,864 B.
      MC2_SHADOW_FULLMAP_SEPARATE=0 run: ShadowCsmFullMap 8192² = 268,435,456 B.
      → shipped default saves 201,326,592 B (192 MiB) depth (+50.3 MB R8 dummy, unregistered) vs
      all-8192. Log lines: '[CSM] init nearLayers=2 arraySize=8192 fullMapSize=4096 separate=1'
      (default) vs 'fullMapSize=8192 separate=0' (A/B). Captures .claude/shadow-ab/{default,sep0}
      mc2_24 ×3 poses: hashes differ (far cascade res), long-view highangle_wide EYEBALLED —
      visually indistinguishable; far-cascade 4096 quality acceptable.
- [x] Commit sha: eebccc60 (includes dense-enum guard update to Count=42)

## Item 3 — ASan #3 ABLModule::compile return check (NOT STARTED)
Plan per A:/Games/mc2-issue-fixes/docs/asan-follow-ups.md §3: mission init hard-fail on ABL compile
error (mclib/ablexec.cpp + mission-init call site). Probe: deliberately broken .abl overlay → clean
refusal w/ error line, no corrupt codeSegmentPtr deref. tier1 = zero behavior change (stock compiles).

DONE implementation checkpoint (pending final tier1/commit): recovered foreign
WIP at 093169d4 already added a default-ON fatal in `ABLi_preProcess` after
recording `errorCount`; `MC2_ABL_SKIP_ERRORED_MODULES` remains the safe
playability escape hatch. Codex completed registry/docs/smoke passthrough and a
deterministic repro hook.

- [x] Build PASS (3 warnings after final repro hook).
- [x] Env registry PASS: 827 reads accounted, 325 registered.
- [x] Offline doctests PASS: 325/325, 114828 assertions.
- [x] Negative fixture `tests/fixtures/abl_compile_fatal/broken.abl` via
      `MC2_ABL_COMPILE_FATAL_REPRO=<absolute path>`: expected smoke FAIL at zero
      frames; `tests/smoke/artifacts/2026-07-16T12-08-57/mc2_01.log` reports
      7 syntax errors then `[ABL_COMPILE_FATAL]`, clean early exit before VM.
- [x] Escape proof: same fixture + `MC2_ABL_SKIP_ERRORED_MODULES=1` PASS,
      2097 frames, +0 destroys (`2026-07-16T12-09-21`).
- [x] Commit `4b4fdc5d`; clean rebuild/deploy fingerprint matched; canonical
      tier1 PASS 5/5, +0 destroys (`2026-07-16T12-12-10`). Item complete.

## Item 4 — GlStateGuard RAII meta-fix (RESUMED after limit-kill; DONE)

Board: DEFERRED-BOARD-2026-07-16.md §A backlog "GlStateGuard RAII meta-fix
(~14 GPU-direct sites)". Prior agent left `GameOS/gameos/gl_state_guard.h`
dirty with the full Slice-4/VOCAB-1 guard vocabulary (9 new capture-only
guard classes, bringing the header to exactly 14 `GlScoped*` classes total —
this IS the board's "~14 sites" figure) plus one converted call site
(`gos_tube_ribbon_flush`), then died mid-conversion ("Now the mech flush
restore block and billboard restore tail").

Resume findings + work, per `docs/testing/glstate-guard-epilogue-recon-1.md`
(the authoritative site classification — Sites 1-5, STOP list, ADOPTION-3/
VOCAB-1/ADOPTION-4+ ordering):

- [x] **Critical bug found and fixed**: `gos_tube_ribbon_flush_deferred` had
      a leftover hand-rolled restore tail (lines ~866-883) referencing
      variables (`savedProgram`, `savedVAO`, `savedCullFace`, `savedTex2D0`,
      `savedSampler`, `savedActiveTex`, `savedDepthMask`, `savedDepthFunc`,
      `savedDepthTest`, `savedSrcRGB`, `savedDstRGB`, `savedBlend`,
      `nSavedDrawBufs`, `savedDrawBufs`) that had already been deleted from
      the top of the function when the RAII guards were introduced — the
      dirty tree did NOT compile. Removed the dead tail; the RAII guards
      (declared at function top, invalidate-guard-first-so-it-destructs-last)
      now own the full restore.
- [x] `gos_particle_bridge.cpp` was missing `#include "gl_state_guard.h"`
      entirely (build error: `mc2gl` not a namespace) — added.
- [x] Site 4 (particle billboard flush, `gos_particle_bridge_flush`) —
      "the billboard restore tail": converted the VOCAB-1-guardable subset
      (program, VAO, blend enable+func, depth-test enable, depth mask+func,
      sampler 0, tex unit 0, + `GlScopedRenderStateInvalidate` replacing the
      trailing explicit invalidate call). Cull stays **fully manual by
      design** — VFX-CARD-CULL-1 re-asserts `glDisable(GL_CULL_FACE)` a
      second time after `copySceneDepthForParticles()` re-enables it as a
      side effect (double-assert trap documented in the recon as Site 4's
      reason to STOP; a capture-only guard would not re-assert and
      reintroduces the spinning-explosion-card flicker bug). SSBO slot 14 and
      the soft-particle/distortion unit-1/unit-2 binds also stay manual
      (bind-then-zero hygiene / conditional mid-function resource binds, not
      the top-block/bottom-block epilogue shape VOCAB-1 targets).
- [x] Site 1 (`GpuMechBatcher::flush()`) — the recon's "one genuinely clean,
      behavior-preserving win": converted SSBO 0/1/2 to
      `GlScopedSsboBinding`, preserving the exact original capture points
      (slot 2 before Step 6 rebinds it; slots 0/1 after — unchanged
      semantics, not "fixed"). The recon requires the guard to close BEFORE
      the function's trailing `gos_InvalidateRenderStateCache()`, which sits
      well before the function's natural end (~430 lines of unrelated
      render-state save/restore between); rather than re-scope that whole
      block, the guards are held in `std::optional<mc2gl::GlScopedSsboBinding>`
      and `.reset()` explicitly at the original manual-restore point. Render
      state (program/VAO/buffers/samplers/depth/blend/cull) stays fully
      manual, per the recon's explicit recommendation ("leave all
      render-state manual" — half-converting is churn with no risk
      reduction until a dedicated slice revisits it).
- [x] Site 5 (`gos_static_prop_batcher.cpp`, ~8900 lines, 10 SSBO + 21
      state-save sites) — **deliberately NOT touched**, per the recon's
      explicit "LAST, own slice... do NOT touch in ADOPTION-3" ruling
      (largest blast radius; SSBO restore-previous-vs-bind-then-zero
      semantics per path unconfirmed). Left as a follow-up slice.
- [x] New self-test: `tools/glstate_guard_harness/` (registered in
      `tools/run_contract_tests.py` REGISTERED_HARNESSES). Unlike the other
      contract harnesses this one needs a REAL GL context — the header makes
      real `glGetIntegerv`/`glEnable`/`glBindTexture` calls — so it creates a
      hidden SDL2 window + GL 4.3 core context (mirrors
      `GameOS/gameos/gos_render.cpp`'s attributes) and `glewInit()`s it, no
      game window / mission data / other GameOS TU linked.  18 tests, one (or
      more) per guard class, including the cull-double-assert-safe pattern,
      the separate-alpha-blend "strictly more correct" precision claim, the
      MRT gap-preservation case (`GL_NONE` at index 1 between two active
      attachments), and a declared-first-destructs-last ordering proof for
      `GlScopedRenderStateInvalidate`. Standalone build:
      `cmake -S tools/glstate_guard_harness -B build64-glstateguard -A x64`
      then `--target glstate_guard_harness`. Result: **18/18 PASS**.
- [x] Build: `cmake --build build64 --config RelWithDebInfo --target mc2`
      exit 0, zero warnings/errors on the three touched TUs
      (`gl_state_guard.h`, `gos_particle_bridge.cpp`, `gos_mech_batcher.cpp`).
- [x] Commit sha: `5c47029e`.
- [x] Deploy: `scripts/deploy_payload.py A:/Games/mc2-opengl/releases/mc2-win64-deferred2-test
      --source-root . --build-dir build64/RelWithDebInfo --exe-name mc2.exe --allow-new-target`
      (note: `--build-dir build64` alone resolves relative to cwd and misses
      the RelWithDebInfo config subdir — use the full path). manifest
      src_commit `5c47029eb20f`, 736 files, deploy COMPLETE. Pre-existing
      unrelated hygiene WARN (3 offline-cook diagnostic PNGs/JSON under
      `gaea_peaks_01.beauty/`) — not from this change.
- [x] tier1 smoke PASS 5/5 (`--exe A:/Games/mc2-opengl/releases/mc2-win64-deferred2-test/mc2.exe`,
      `MC2_SMOKE_REQUIRE_FINGERPRINT=0`): mc2_01/03/10/17/24 all PASS, +0
      destroys each (4229/4196/4222/4205/4217 frames). `DEPLOY_FINGERPRINT`
      MISMATCH lines are the expected advisory (lane's baseline compare is
      against nifty 994c3776, not this branch — hence the override flag).
      Item complete.

## Item 5 — Static building shadow rebake on destroy (DONE)

Board: DEFERRED-BOARD-2026-07-16.md §A #10 "Static building shadow rebake on
destroy" + `docs/known_issues.md` "destroyed buildings keep their shadow
until reload". Scope: `MC2_STATIC_PROP_BUILDING_SHADOW` bakes the world-fixed
building shadow map ONCE per mission behind a one-shot latch
(`gos_StaticLightMatrixBuilt()`, `mclib/txmmgr.cpp`); nothing re-armed it on
a mid-mission building destroy.

**Recon correction (important):** the initial hypothesis (hook
`BldgAppearance::destroy()`) was WRONG — traced that `destroy()` (which frees
`bldgShape`/`appearance`) is only called at mission-end/pool-recycle
(`~BldgAppearance() { destroy(); }`), never at the moment a building is
combat-destroyed. The REAL in-game "building destroyed" event is
`BldgAppearance::setObjStatus(OBJECT_STATUS_DESTROYED)` (called from
`Building::setDamage` when accumulated damage crosses the threshold), which
swaps `bldgShape` to `appearType->bldgDmgShape` (the rubble mesh) but never
touched the static-prop registry.

- [x] `BldgAppearance::setObjStatus()`: on the `OBJECT_STATUS_DESTROYED`
      transition (not `OBJECT_STATUS_DISABLED` — reversible, keeps its
      shadow), re-arm `gos_StaticLightMatrixBuilt()` via
      `gos_ResetStaticLightMatrix()`.
- [x] **Bug found by measurement, then fixed**: re-arming the latch alone
      was insufficient — the rebake it triggers fires on the VERY NEXT
      FRAME, before the engine's ordinary lazy "shape swap detected on next
      touch()" re-registration had run, so the first replay still baked the
      STALE pre-destruction shadow (measured: instances 299→299, no
      change). Fix: call `invalidateStaticRegistration()` SYNCHRONOUSLY in
      `setObjStatus` (tombstoning the recipe immediately) before triggering
      the rebake. Re-measured: 299→298, exactly the one destroyed building.
      Gated so this synchronous path (and its slight timing change vs the
      lazy path) only executes when the rebake would actually fire — a
      mission that never sets `MC2_STATIC_PROP_BUILDING_SHADOW` gets the
      exact original lazy-detection timing, unchanged.
- [x] `BldgAppearance::destroy()`: kept as cheap insurance for the
      mission-end/pool-recycle path (a building that somehow reaches
      `destroy()` while still `staticReg.registered`, e.g. never went
      through `setObjStatus` first).
- [x] New gate `MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE` (default **ON**,
      independent kill-switch; `=0` disables, `=2` traces) — only fires when
      (a) the building held a static registration and (b) the parent
      `MC2_STATIC_PROP_BUILDING_SHADOW` gate is active; inert/zero-cost
      otherwise, so the unconditional terrain-only static build is never
      forced to redundantly replay on missions that never opted in.
- [x] Deterministic repro hook (test-only):
      `MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME=<N>`
      (`code/objmgr.cpp`, top of `GameObjectManager::update`) — at frame N,
      calls `Building::setDamage(getDamageLevel())` (the real gameplay
      damage API, same effect as a lethal weapon hit) on the first live
      building. Off by default (single getenv + int compare).
- [x] **Proof (mc2_24, MC2_STATIC_PROP_BUILDING_SHADOW=2 +
      MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE=2 +
      ..._TEST_DESTROY_AT_FRAME=100)**:
      - Initial bake: `[SHADOW_STATIC_BLDG_TXM] prepass=1 instances=299`
      - Frame 100: `[SHADOW_STATIC_BLDG_REBAKE_TEST v1] event=test_destroy
        frame=100 buildingIndex=0`
      - `[SHADOW_STATIC_BLDG_REBAKE v1] event=setObjStatus_destroyed
        recipeIndex=2000`
      - Rebake: `[SHADOW_STATIC_BLDG_TXM] prepass=1 instances=298` —
        exactly one fewer, matching the single destroyed building.
      - Exactly 2 total rebakes across the ~2800-frame run (no thrashing).
      - **Gate-off control**: same repro with `MC2_STATIC_PROP_BUILDING_SHADOW`
        UNSET — `test_destroy` still fires (damage applied) but NO
        `setObjStatus_destroyed` / `SHADOW_STATIC_BLDG_TXM` rebake lines
        appear at all — confirms zero cost when the parent feature is off.
      - Visual capture attempt: `run_visual_capture.py` on mc2_24's 3 stock
        bookmarks (generic terrain-oriented, not authored around this
        specific building), baseline vs destroyed — all 3 image hashes
        differ (some render change occurred), but EYEBALLED (Read) the
        `highangle_wide` and `ridge_lowangle` pairs and the destroyed
        building/its shadow are not legibly framed in these wide/generic
        views. Captures saved at `.claude/shadow-destroy-ab/{baseline,destroyed}/`
        (uncommitted, evidence-only, mirrors the `.claude/shadow-ab/`
        precedent). The instance-count trace above is the definitive proof;
        a pixel-legible before/after would need a custom bookmark authored
        at the specific destroyed building's world coordinates — not done
        (out of time budget for this slice).
- [x] Docs: `docs/known_issues.md` entry marked addressed;
      `docs/tier1_env_vars.md` + `RenderCore/RendererFeatureRegistry.h` +
      `scripts/run_smoke.py` allowlist updated for all 3 new vars;
      `scripts/check-env-registry.sh` clean (829 vars accounted).
- [x] Build: `cmake --build build64 --config RelWithDebInfo --target mc2`
      exit 0, zero warnings on `mclib/bdactor.cpp` / `code/objmgr.cpp`.
- [x] Commit sha: `9955d28b`.
- [x] Official deploy (clean, no dirty-tree warning): manifest src_commit
      `9955d28b4099`, 736 files.
- [x] Canonical tier1 smoke PASS 5/5 (`--exe .../mc2-win64-deferred2-test/mc2.exe`,
      `MC2_SMOKE_REQUIRE_FINGERPRINT=0`): mc2_01/03/10/17/24 all PASS, +0
      destroys each (4214/4209/4211/4114/4181 frames). Item complete.

## Item 6 — docs/render-perf-snapshot.md refresh (DONE, docs-only)

Board task 3: refresh the stale bucket map for this session's shipped perf
work. Found the 4 items across their respective worktrees/branches
(`mc2-vk-interop-bridge` branch `claude/vk-interop-bridge-1`,
`mc2-haze-ubo-cleanup` branch `claude/haze-ubo-cleanup`):

- [x] **VK-PRESENT-SINGLESUBMIT-1** `e01ac03f` — composite drain deleted,
      present submits 2→1; measured 76.9→0.7 µs/frame (`ce72d915`).
- [x] **VK-PROP-LIGHTFEED-NATIVE-1** `90b36018` — prop island light feed
      goes native CPU, retires a ~1.3-1.5 ms/frame GL readback stall
      (temporary audit gate `MC2_VK_PROP_FEED_AUDIT` still present, flagged
      for deletion per the board).
- [x] Island fence-deferral across all 4 islands: water `61031785` (pilot) →
      prop `338fd468` → terrain `f9f75448` → mech `72f9c410` (close
      `2e5e7f35`). Water pilot measured: fence_wait 118-127→1.5-1.7 µs,
      island_total 152-161→33-34 µs/frame (`ce72d915`).
- [x] Terrain haze UBO cleanup `f268db51` — dead SSBO field removed,
      housekeeping (not a measured perf win).
- [x] Added a new "VK present-path and island perf" section to
      `docs/render-perf-snapshot.md` (kept separate from the existing
      CPU/GL wolfman-zoom bucket map — different measurement methodology)
      + updated "Last refreshed" + added stale-flags noting the CPU/GL
      bucket map itself is now 6+ weeks stale and needs its own
      re-measurement pass (out of scope for this docs-only item).
- [x] Docs-only diff (`docs/render-perf-snapshot.md` only) — no build/deploy/
      tier1 needed for this item.
