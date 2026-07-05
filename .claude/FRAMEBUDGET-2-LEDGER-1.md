# FRAMEBUDGET-2-LEDGER-1 — GOM update + renderLists remainder (7ms-frame attack)

Lane `claude/framebudget-2`, worktree `A:/Games/mc2-framebudget2`, base
`claude/controlmap-sample-1` @ `3eee4101`. Deploy/measure `mc2-win64-abl-validate`
(canonical, was free). Measurement baseline = perf gates ON
(`MC2_LIGHT_PREFIX_GPU_COPY=1 MC2_PICK_FALLBACK_COARSE=1`) — attacking the REMAINDER.
All numbers = smoke-headless window means (`[*_COST v1]` 60-frame), mission pair
mc2_24 (busy) + mc2_01 (idle). NEVER `--kill-existing`.

## Bucket breakdown (evidence, this lane)

### Block A — GameObjectManager::update (`[GOM_UPDATE_COST v1]`, new counter)

| Phase | mc2_01 (idle) | mc2_24 (busy) | Notes |
|---|---:|---:|---|
| **static_touch** | **326–411** | **754–1450** | GameLogic.Units.TerrainObjects bldg/tree touch() walk — **~75% of the block, THE remainder** |
| mover_update | 70–118 | 193–414 | Mechs+Vehicles update() — AI-activity variable |
| other_fx | 1–2 | 11–14 | turrets/weapons/carnage/lights/artillery — negligible |
| substrate_flush | 5–6 | 6 | gpu_cull substrate upload — negligible |
| self (unattributed) | 34–57 | 52–72 | framesSinceActive sweep + captureList + FRAME-JOBS prepass |
| **total** | **415–547** | **1026–2340** | |

**Verdict:** `static_touch` dominates. Root cause confirmed = the per-static-prop
`ResubmitCachedGpuLightData()` residue the ARCH-REVIEW-MCLIB-SCENE-1 doc predicted.

### Block B — renderLists remainder (`[RENDERLISTS_COST v1]`, perf gates ON)

| Phase | mc2_01 | mc2_24 | Notes |
|---|---:|---:|---|
| **sp_batcher_flush** | 170 | **595** | **biggest remainder** — mostly uploadAllBucketsIfNeeded + draw submit |
| dyn_shadow | 144 | 103 | caster-cull + shadow submit — smaller than expected |
| sp_registry_flush | 7.6 | 8.7 | **cached path is live default → already tiny; zone-gap decompose is now LOW value, DO NOT slice** |

## Wins LANDED (all gated default-OFF, before/after µs, pixel-parity-proven)

| Slice | Commit | Gate | Before→After | Parity |
|---|---|---|---|---|
| GOM cost-split (instr) | `1d16181e` | `MC2_GOM_UPDATE_COST_SPLIT` | n/a (measurement) | byte-identical (default-OFF) |
| **STABLE-LIGHT-SKIP-BROADEN-1** | `adcfad2c` | `MC2_STABLE_LIGHT_SKIP_TOUCH` (+`MC2_STABLE_LIGHT_SKIP`+`MC2_LIGHTBRIDGE_STABLE_SKIP`) | static_touch **326→254 (−22%)** mc2_01, **754→699 (−7%)** mc2_24; LBSS skip/window 404→**2537** (mechanism proven) | **8/8 bookmarks PIXEL-IDENTICAL** (mc2_01+mc2_24, byte-hash, fixed clock) |
| **SP-BATCHER-ALPHASCAN-GATE-1** | `8f794f96` | `MC2_SP_ALPHASCAN_GATE` | sp_batcher_flush **595→586 (−9µs)** mc2_24, 170→167 mc2_01 (matches ledger ~10µs) | **3/3 bookmarks PIXEL-IDENTICAL** (mc2_24) |

**STABLE-LIGHT-SKIP-BROADEN-1 mechanism:** the FRAME-JOBS `touchSerialCommit()`
already LBSS-skips the redundant resubmit, but that path only runs when
`MC2_FRAME_JOBS`+`MC2_FRAME_JOBS_TOUCH` (BOTH default-OFF). Stock config runs the
legacy `touch()` (terrobj.cpp:926 → BldgAppearance/TreeAppearance::touch), which
computed `stableLightSkipEligible` **for diagnostics only** then resubmitted
UNCONDITIONALLY. This broadens the identical skip to that live path. SAFER than the
serial variant: still calls `shape->Touch()` (advance lastTurnTransformed) so the
legacy CPU-render staleness guard (tgl.cpp:3000) stays byte-identical; skips only the
expensive resubmit. Invariant inherited from the shipped serial-commit skip (baked
static-light prefix `[0..s_staticLightHighWater)` persists under `MC2_LIGHTBAKE`).

## No-wins / de-scoped (honest verdicts)

- **sp_registry_flush zone-gap decompose (TXMMGR ledger §4.3):** NOT worth a slice —
  the bucket is already 7.6–8.7µs (cached path is live default). No remainder to cut.
- **dyn-shadow re-render skip (TXMMGR §4.2):** dyn_shadow is 103–144µs and, like the
  shipped `MC2_SHADOW_CASTER_CULL_CACHE`, its payoff is stationary-camera-only (the
  smoke fly-through changes the camera-fit light matrix every frame → cache never
  hits). Same no-win precedent as SHADOW-CASTER-CULL-CACHE. Kept as an M spec below;
  DO NOT flip without user-driven stationary-camera Tracy evidence.

## Ledger — remaining specs (M/L, for opus/roadmap)

1. **[M] SP-BATCHER-BUCKET-UPLOAD-DIRTY-GATE-1** — `sp_batcher_flush` (595µs mc2_24,
   the biggest renderLists remainder) is dominated by `uploadAllBucketsIfNeeded()`
   (`gos_static_prop_batcher.cpp:4972`) + draw submit, NOT the alpha scan (that's the
   ~10µs ALPHASCAN-GATE just landed). Spec: cost-split-decompose the flush into
   {bucket-upload, coalesce-pool-build, draw-submit, fence-wait} sub-spans (extend
   `[SPFLUSH_COST_SPLIT v1]`), then dirty-gate the per-frame bucket SSBO upload on
   `s_registryGeneration` (proven SNAPSHOT-FILL-DIRTYONLY / 2b pattern). Parity: the
   uploaded bucket data is a pure function of the registry generation — gate is
   byte-identical when clean. Measure before cutting; the fence-wait may be the real
   mass, in which case this is a no-win (document it, caster-cull precedent).

2. **[M, stationary-camera-gated] DYN-SHADOW-RERENDER-SKIP-1** — after caster-cull,
   the remaining dyn_shadow mass is the per-frame shadow-map re-render submit. When
   casters + light matrix + mech poses are all unchanged the depth map is identical
   → skip. Needs a cheap mech-motion signal; payoff is stationary-camera frames only
   (the smoke fly-through will show NO win — same as SHADOW-CASTER-CULL-CACHE). Only
   pursue with user Tracy on a paused/panning RTS camera.

3. **[S, flip-queue] STABLE-LIGHT-SKIP-BROADEN default-on** — `MC2_STABLE_LIGHT_SKIP_TOUCH`
   is pixel-parity-proven (8/8 bookmarks) + kill-switch-preserving. Candidate for a
   default flip AFTER: (a) user-driven wolfman-zoom Tracy confirming the win at scale,
   (b) a combat soak with props being destroyed (exercises the not-eligible fallback).
   Flip via RendererFeatureRegistry default + the `ParseEnvBool` default-true in one
   commit. Same for `MC2_SP_ALPHASCAN_GATE` (parity-proven, ~10µs, destruction-soak
   recommended first since it touches the damage-texture path).

4. **[L] REDUNDANT-PASS-HUNT item 1 — idle-mover dynamic-shadow cull** — fold
   idle-long-enough movers into the once-per-mission static shadow bake, promote back
   to dynamic on first motion. HIGH parity risk (sun drift, LOD pops,
   markTerrainDrawn-revives-dead-passes landmine). Own arc + pixel-parity harness.

## Discipline notes / gotchas

- All 4 target files were multiply-claimed by DEAD lanes (`framebudget`/`txmperf`
  PIDs 25372/50288, both dead). `framebudget-1` branch never committed its planned
  GOM work (`GOM-UPDATE-COST-1.md` was never written). This lane executed it clean.
- `MC2_GOM_UPDATE_COST_SPLIT` / `MC2_RENDERLISTS_COST_SPLIT` / `MC2_LIGHT_PREFIX_GPU_COPY`
  / `MC2_STABLE_LIGHT_SKIP` / `MC2_SP_ALPHASCAN_GATE` were NOT in the smoke Popen
  allowlist → gate-ON measurement was silently inert. Added (`cddaa0c1`+later). If a
  future perf gate shows no delta, CHECK THE ALLOWLIST FIRST.
- Parity method when byte-hash golden is stale-exe: capture skip-OFF, stash PNGs,
  capture skip-ON, sha256-diff — same-build A/B with fixed clock. Beats a
  FAIL_IDENTITY_MISMATCH refusal.
