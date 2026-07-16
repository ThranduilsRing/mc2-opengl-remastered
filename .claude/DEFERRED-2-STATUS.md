# DEFERRED-2 sweep status (branch claude/deferred-sweep-2, base nifty 994c3776)

Worktree: A:/Games/mc2-deferred-2. Deploy lane: A:/Games/mc2-opengl/releases/mc2-win64-deferred2-test.
Three items from DEFERRED-BOARD-2026-07-16.md §A, one commit each, in order. NEVER push.

## Item 1 — GPU-BUFFER-OWNER TIER0-LODCHUNK RAII (IN PROGRESS)

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
- [ ] Build (configure done exit 0; mc2 target build pending)
- [ ] tier1 smoke @ lane + terrain_path lod_chunk counter proof
- [ ] Commit sha: (pending)

## Item 2 — Shadow VRAM: far cascade → separate 4096 texture (NOT STARTED)
Plan: find CSM cascade array alloc in gameos_graphics.cpp shadow setup; keep cascades 0/1 @ 8192,
far cascade own 4096 texture. Proof: VRAM counter before/after, shadow parity captures, tier1.

## Item 3 — ASan #3 ABLModule::compile return check (NOT STARTED)
Plan per A:/Games/mc2-issue-fixes/docs/asan-follow-ups.md §3: mission init hard-fail on ABL compile
error (mclib/ablexec.cpp + mission-init call site). Probe: deliberately broken .abl overlay → clean
refusal w/ error line, no corrupt codeSegmentPtr deref. tier1 = zero behavior change (stock compiles).
