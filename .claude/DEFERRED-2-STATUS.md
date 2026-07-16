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
- [ ] Commit, clean rebuild/deploy, canonical tier1 5/5.
