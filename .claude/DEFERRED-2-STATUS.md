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

## Item 2 — Shadow VRAM: far cascade → separate 4096 texture (IN PROGRESS)

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
- [ ] Build/deploy/tier1; A/B evidence (default vs MC2_SHADOW_FULLMAP_SEPARATE=0): registry
      sizeBytes delta (67,108,864 vs 268,435,456 = 201.3 MB depth saved by shipped default) +
      [CSM] init log lines + far-shadow captures both ways (long-view mission) eyeballed.
- [ ] Commit sha: (pending)

## Item 3 — ASan #3 ABLModule::compile return check (NOT STARTED)
Plan per A:/Games/mc2-issue-fixes/docs/asan-follow-ups.md §3: mission init hard-fail on ABL compile
error (mclib/ablexec.cpp + mission-init call site). Probe: deliberately broken .abl overlay → clean
refusal w/ error line, no corrupt codeSegmentPtr deref. tier1 = zero behavior change (stock compiles).
