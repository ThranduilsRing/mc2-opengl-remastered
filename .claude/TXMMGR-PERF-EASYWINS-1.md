# TXMMGR-PERF-EASYWINS-1 — renderLists() modernization ledger

Lane: `A:/Games/mc2-txmperf` (branch `claude/txmperf-1`, base `e2618336`), deploy
`A:/Games/mc2-opengl/mc2-win64-v0.4c`, registered in lane_registry (claims:
gos_static_prop_registry.cpp, txmmgr.cpp, RendererFeatureRegistry.h — batcher
files are claimed by the concurrent `propshadow` lane and were NOT touched).

User-reported: `textureManagerRenderLists` ~500µs+ of a 2.7ms frame (user Tracy).
All numbers below are **smoke headless** (`[RENDERLISTS_COST v1]` per-frame mean µs,
60-frame windows, mc2.exe 0.4c) — wolfman-zoom user Tracy numbers will be larger.

## 1. Cost structure (NEW counter `MC2_RENDERLISTS_COST_SPLIT=1`, default-OFF)

Baseline (all gates default), steady-state windows:

| Phase | mc2_01 | mc2_24 | Notes |
|---|---:|---:|---|
| light_upload | 231–325 | 958–1057 | **ORPHAN-1 defeated the upload split** (see §2.1) |
| sp_batcher_flush | 155–210 | 735–975 | `GpuStaticPropBatcher::flush` — propshadow-lane file, §4.1 |
| dyn_shadow | 250–530 | 150–225 | CasterCull + shadow draw submit + CSM replay |
| sp_registry_flush | 98–217 | 47–305 | SPFLUSH internals ~85–90µs (cached path); zone gap §4.3 |
| gpusp_prep_cull | 24–52 | 24–41 | prep + cull dispatch |
| vfx_hud | 9–16 | 9–15 | |
| everything else | <5 | <5 | preamble/scene/obj3d/overlays/water/blobs ≈ dead |
| **total** | **776–1067** | **1949–2900** | self_us ~2–3 (fully attributed) |

SPFLUSH cross-check (`MC2_STATIC_PROP_FLUSH_COST_SPLIT=1`): legacy buckets all 0
(cached path is the live default), CACHED total ~85–87µs @1665 leaves (~51ns/leaf),
builds_pf=0 / hits_pf=1665 steady.

## 2. Wins executed (each gated, default-OFF, kill = unset/=0)

### 2.1 LIGHT-PREFIX-GPU-COPY-1 (`MC2_LIGHT_PREFIX_GPU_COPY`) — the headline
Root cause: LIGHTSSBO-ORPHAN-1 (NVIDIA stall fix) orphans the slot-20 light SSBO
every frame, forcing a full PCIe re-upload of the immutable static prefix [0..S)
that STATIC_LIGHT_UPLOAD_SPLIT used to skip — mc2_24: 2676 records × 3600 B ≈
9.6 MB/frame. Fix: VRAM stash of the prefix (refreshed only on `prefixDirty` /
S-growth — observed exactly 3 refreshes per mission, all at load), per frame
`glCopyBufferSubData` stash→fresh-orphaned store; PCIe = dynamic suffix only.
NVIDIA-safe by the same orphan discipline (writes only into fresh store; stash
read-only in steady state). Subsumed by `MC2_GPUBUF_LIGHT_GROWONCE` if that ships.

Measured (clean A/B, 30s smoke, PASS, +0 destroys):
- mc2_01: light_upload **231–325 → 2.4–3.8µs (−99%)**; renderLists total 776–1067 → **429–653µs (~−40%)**; FPS 74/72 parity.
- mc2_24: light_upload **958–1057 → 3.0–4.6µs**; renderLists total 1949–2900 → **~1063–1235µs (~−45%)**; FPS 57→64 (traced rerun; see §3 contention).

### 2.2 SHADOW-CASTER-CULL-CACHE-1 (`MC2_SHADOW_CASTER_CULL_CACHE`)
Shadow.CasterCull re-culls the whole registry caster set per frame; result is a
pure function of (registry generation, light-space matrix, margin). Cache the
culled vector; reuse when generation + 16-float matrix (memcmp) unchanged.
Camera motion / registry mutation → recompute verbatim.

Measured (clean A/B mc2_01, 30s smoke, PASS, FPS 73/73): **NO smoke win** —
dyn_shadow OFF 225–412µs vs ON 250–303µs (noise). Root cause: the smoke harness
is an idle **fly-through** — the camera-fit light matrix changes every frame, so
the cache never hits. Payoff exists only on a truly stationary camera (same
class as the PAUSED lifecycle gate: RTS players pan constantly). Verdict:
shipped default-OFF, harmless, kill-switched — **do NOT flip** without
user-driven stationary-camera Tracy evidence; the real dyn_shadow reduction is
proposal §4.2.

### 2.3 CACHED_BLOB "soak + flip" — RESOLVED AS SUBSUMED (truth repair)
`MC2_STATIC_PROP_FLUSH_CACHED_BLOB` keys the cached-record branch as
`(s_flushCachedBlob || s_persistentBuckets)` (gos_static_prop_registry.cpp:1204).
`MC2_STATIC_PROP_PERSISTENT_BUCKETS` went **default-ON 2026-06-03** (tier1 5/5)
— so the ~78% flush cut IS already the live default; the sweep item F2
(`STATICPROP-FLUSH-CACHED-BLOB-DEFAULT-ON-1`) was based on a stale
"default-OFF, soak pending" note. **Do NOT flip the CACHED_BLOB default**: with
`PERSISTENT_BUCKETS=0` (the soaked full-revert kill-switch) a default-ON here
would silently re-engage the cached bulk path — a never-soaked config. Comment
at the gate now documents the kill-switch matrix. Fresh soak evidence: §3.

## 3. Soak + measurement discipline

- tier1 soak (5×30s, `MC2_LIGHT_PREFIX_GPU_COPY=1` + cached-blob compare oracle
  armed, artifacts `2026-07-01T22-00-20`): **5/5 PASS, exit 0, +0 destroys,
  0 GL errors** (FPS depressed by 3 foreign concurrent lane mc2s; verdicts
  unaffected).
- Cached-blob byte-parity oracle (`CACHED_BLOB=1 + _COMPARE=1`):
  **1,965,811 leaf compares, 0 mismatches** across all 5 tier1 missions.
- **GPU contention hazard (measurement lesson):** two foreign mc2.exe processes
  (v0.4d-rc1 + releases/v0.5.0 lanes) ran concurrently during several runs and
  poisoned run-level FPS (mc2_24 ON run showed 37 FPS + p1%=3 purely from
  contention; identical config re-ran at 64 FPS). Within-run per-phase counters
  stayed coherent throughout — trust `[RENDERLISTS_COST v1]` deltas, not
  cross-run smoke FPS, whenever other lanes are active. NEVER `--kill-existing`.

## 4. Proposals (M/L — for the roadmap, not executed here)

1. **SP-BATCHER-FLUSH-COST-SPLIT (M, blocked on propshadow lane)** —
   `sp_batcher_flush` is now the biggest remaining renderLists item (735–975µs
   mc2_24, ~850µs with prefix-copy ON). `gos_static_prop_batcher.cpp` is claimed
   by the propshadow lane — hand this over or queue behind it. Shape: cost-split
   decompose (uploadAllBucketsIfNeeded vs draw submit vs state churn), then
   dirty-gate the per-frame bucket SSBO upload on `s_registryGeneration`
   (proven SNAPSHOT-FILL-DIRTYONLY / 2b pattern).
2. **DYN-SHADOW-RERENDER-SKIP (M)** — after cull-cache, the remaining dyn_shadow
   mass is the per-frame shadow-map re-render submit (drawDynamicPropShadows +
   mech flushShadow + CSM replay). When casters, light matrix AND mech poses are
   unchanged the depth map is identical — skip the re-render. Needs a cheap
   mech-motion signal; camera-fit matrix changes every camera move, so payoff is
   stationary-camera frames only.
3. **SP-REGISTRY-FLUSH-ZONE-GAP (S–M)** — whole-zone `sp_registry_flush`
   (98–305µs) minus SPFLUSH-instrumented spans (~85–90µs) leaves an unattributed
   gap: suspects are the per-frame `std::fill` of `s_recipeHasSubstrateRecord`,
   the tombstone/live-range walk, and per-range light-idx patching. Extend the
   cost-split before cutting.
4. **LIGHT-GROWONCE-NVIDIA-VALIDATION (M, hardware-gated)** — validate
   `MC2_GPUBUF_LIGHT_GROWONCE` on NVIDIA (documented hard blocker), then flip it
   default-ON; it subsumes LIGHT-PREFIX-GPU-COPY-1 with an even simpler path
   (one SubData, no orphan, no stash).
5. **Default-flip queue (S, after user-driven Tracy + visual soak):**
   `MC2_LIGHT_PREFIX_GPU_COPY` and `MC2_SHADOW_CASTER_CULL_CACHE` — both are
   kill-switch-preserving; flip via the RendererFeatureRegistry default +
   parseEnv default in one commit each.

## 5. Commits (this lane)

- `34bdd2ca` perf(txmmgr): [RENDERLISTS_COST v1] coarse per-phase CPU cost split (default-OFF)
- `81aa6fd6` perf(light): LIGHT-PREFIX-GPU-COPY-1 (default-OFF)
- `438808e4` perf(shadow): SHADOW-CASTER-CULL-CACHE-1 (default-OFF)
- `65d0972a` fix(light): stash invalidation on grow-frame early return
- (this commit) docs(perf): CACHED_BLOB subsumption truth-repair + this ledger
