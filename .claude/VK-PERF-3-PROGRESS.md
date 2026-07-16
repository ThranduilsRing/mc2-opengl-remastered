# VK-PERF-3 progress scratch (fold into VK-ARC-STATE.md at the end)

Session task: convert remaining islands to the water fence-deferral template
(VK-WATER-FENCE-DEFER-1, 61031785). Order: terrain -> prop -> mech -> vfx(audit) -> docs.
Worktree A:/Games/mc2-vk-interop-bridge @ base ce72d915. Both 0.4c lanes tracked below.

**Environment caveat (applies to ALL absolute numbers this session):** a foreign
mc2.exe (A:/Games/mc2-opengl/releases/mc2-win64-deferred-test, PID 30568 — NOT
touched) is live-rendering throughout, eating GPU. A/B arms share the load, so the
comparisons are fair, but inline-arm stalls are inflated vs a quiet machine (the
inline wait is documented GPU-contention-sensitive — this is exactly the regime
the deferral removes).

## 1. TERRAIN — DONE (commit f9f75448, both 0.4c lanes deployed at it)
- Audit: per-patch dynamic UBO = ONLY per-frame CPU-write the submit consumes ->
  ubo[2]/set[2] per-parity; height/cement/visual-height snapshots = rare-write ->
  device-idle guards added before in-place rewrites; bound-view descriptor switch
  (dyn-shadow/CSM direct flips) -> device-idle guard; copy-once importers safe by
  construction (one-shot inline fence retires all earlier submission-order work);
  patch/skirt caches write-once; gpuTimer.resolve() safe via WAIT_BIT (documented).
- beginFrame HOISTED above UBO fill; killswitch MC2_VK_TERRAIN_FENCE_DEFER=0;
  receipt gate MC2_VK_TERRAIN_PERF ([VK_TERRAIN_PERF] line). Both allowlisted
  (run_smoke.py + check-env-registry.sh).
- A/B (same binary f9f75448, field env mc2_01 airport pose, ~2500 steady frames/arm):
  fence_wait_us avg 1189.2 -> 2.9; island_total_us avg 1263.2 -> 78.9 (~-1.18 ms/frame
  in this contended env).
- PPM inline-vs-defer: 1707x960 MAD=0.038/ch, max=28, 2.85% bytes (time-varying
  HUD/anim residual) — parity PASS; scene real (sampled mean 82.5).
- field-check PASS (chroma 27.4; used_vulkan terrain/prop/mech/water/fog all >0,
  zero fallback_reasons; props-pose edge 12.58).
- sync-val (MC2_VULKAN_VALIDATION=sync, 40 s field run): 0 SYNC-HAZARD,
  validation_errors=21 == pre-existing baseline.
- tier1 VK-umbrella (--exe 0.4c): first run 4/5 (mc2_24 heartbeat_freeze_play),
  second run 4/5 (mc2_01 same bucket at end-of-duration teardown boundary) —
  investigated: freeze = teardown exceeding the 3 s heartbeat window; bucket has
  8 PRE-CHANGE occurrences since 07-03 in tests/smoke/artifacts (documented flake
  class), mc2_24 alone PASSes at 131-144 fps defer-ON, terrain receipts healthy in
  every failed leg, killswitch-arm tier1 5/5. Final defer-ON tier1: **5/5 PASS +0**.

## 2. PROP — audit DONE (from session recon; see below), conversion IN PROGRESS
- Audit: per-frame writes = instBuf + ubo + lightBuf (all single today) -> [2];
  set -> set[2] (pool doubling at vulkan_color_prop_island.cpp:951-967);
  layer tables + albedo imports = map-load, safe by construction after
  cp_end_oneshot inline fence; vbo/ibo geometry = upload-once latch (fresh bufs).
  Hoist beginFrame ABOVE the light-feed copy (lightBuf is per-frame) — i.e. before
  the Mc2ColorPropLightFeed block at :1324, after the material import block.
  Killswitch MC2_VK_PROP_FENCE_DEFER=0; receipt MC2_VK_PROP_PERF ([VK_PROPS_PERF]).
- NOT yet coded at the time of this checkpoint.

## 3. MECH — audit DONE, untouched
- Per-frame writes: instBuf/boneBuf/lightBuf/ubo -> [2]; scheme-cache descriptor
  sets -> set[2] per SchemeEntry (pool maxSets 32->64, sizes x2, alloc both in
  alloc_scheme_set); write_buffer_bindings gains parity param; ensure_buf_capacity
  device-idles on grow then rewrites all sets (both parities) — safe; new-scheme
  mid-session import safe by construction (cp_end inline fence drains queue).
  gpuTimer same note as terrain. Killswitch MC2_VK_MECH_FENCE_DEFER=0; receipt
  MC2_VK_MECH_PERF. Hoist beginFrame above inst/bone staging (scheme pre-import
  loop stays above the hoist).

## 4. VFX — audit DONE -> DEFER RECOMMENDATION
- vulkan_vfx_island.cpp is single-buffered end-to-end: ONE cbuf, ONE private
  fence (does NOT participate in the shared parity protocol / never calls
  vkImageResourceBeginFrame), ONE ubo+ssbo+set. Conversion would require adopting
  the shared-slot protocol (changing parity call-counts on fire frames) or
  building a private 2-deep ring — restructuring beyond the template. Runs only
  on weapon-fire frames. => DEFER with note (per task instruction).

## 5. DOCS remainder — pending
- De-stale BUG 5 (2x sim speed): run one field leg with MC2_SIM_CLOCK_TRACE=1,
  read ratio(sim/wall). BUG 6 (prop scramble): classify vs subsequent clean prop
  verifications. Commit untracked .claude/METAL-GATE-AUDIT-2.md +
  .claude/VK-PERF-SLICE-PLAN-2026-07-16.md w/ supersession note (slices 1-5 +
  water pilot DONE). Append VK-PERF-3 results block to VK-ARC-STATE.md.

## Measurement harness
- .claude/fence-defer/measure.py (A/B: killswitch arm vs defer arm, parses
  [VK_*_PERF], drops warmup) + ppmdiff.py (P6 MAD). Artifacts in .claude/fence-defer/.
