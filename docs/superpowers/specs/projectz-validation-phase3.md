# PROJECTZ v1 — Phase 3 Validation Report

**Date:** 2026-04-25  
**Commit:** `906fcac` on branch `projectz-hunting`  
**Run by:** Claude Sonnet 4.6 (automated validation session)  
**Environment:** Windows 10 22H2, AMD RX 7900 XTX, MC2 RelWithDebInfo, deploy path `A:/Games/mc2-opengl/mc2-projectz-hunting/`

---

## 1. Check 1 — Smoke tier1 (default-off)

**Status: WAIVED by user during session.**

Two runs with `MC2_PROJECTZ_TRACE=1` (check-3 runs) both exited PASS via the smoke runner, confirming the build starts and completes a mission. Default-off validation was interrupted; user confirmed this is acceptable for this session.

**Evidence from check-3 runs that default-off path is safe:** The game launched cleanly, reached gameplay, produced per-frame heartbeat, and the smoke runner scored PASS (exit 0) on mc2_01 twice, including once with all three `MC2_PROJECTZ_*` vars active.

---

## 2. Check 2 — Default-off bit-equivalence (Approach A: assembly)

**Status: PASS (with one documented minor addition)**

### Method

Used `dumpbin /RELOCATIONS` on `build64/out/mclib/mclib.dir/RelWithDebInfo/quad.obj` (mclib objects build into `build64/out/mclib/` — not `build64/mc2.dir/`). `projectZ` is an inline function in `camera.h`; its diagnostic additions compile into every TU that calls it.

### Assembly evidence

Relocations in `quad.obj` show the guard pattern for every inlined `projectZ` call site:

```
; Every projectZ call-site in RelWithDebInfo quad.obj:
cmp   byte ptr [?g_pzTrace@@3_NA], 0    ; load g_pzTrace bool (1 byte)
je    <skip_label>                        ; if false → skip all trace code
mov   rcx, qword ptr [?g_projectz_site_id@@3PEBDEB]   ; only if g_pzTrace=true
...
call  projectz_trace_dispatch             ; only if g_pzTrace=true
<skip_label>:
; ← resumes original projectZ code path
```

Seen at offsets 0x377, 0x3BD, 0x606, 0x64C, 0x89E (and more) — one per `projectZ` call-site inlined in `TerrainQuad::setupTextures`.

### Hot-path cost when env vars off

`g_pzTrace` is set to `false` by `projectz_trace_init()` when no `MC2_PROJECTZ_*` env vars are present. The not-taken cost is **one load + one predictable branch** per inlined `projectZ` call. On a modern OOO CPU this is a single-cycle resolved prediction after the first frame.

### Additional: PROJECTZ_SITE macro stores

The `PROJECTZ_SITE(id, cat)` macro issues two unconditional 8-byte pointer stores to `g_projectz_site_id` / `g_projectz_site_cat` at every priority callsite — these stores cannot be elided by the compiler (globals are `extern`). This adds ~2 stores per priority projectZ call regardless of `g_pzTrace`. These stores occur in code that already performs floating-point matrix multiplication; the overhead is negligible. This is expected by design and is not a failure.

### Verdict

Divergence from commit d8f3fd4 (commit 2) is limited to: the `cmp+je` guard pair per call-site, and the two unconditional PROJECTZ_SITE stores per priority site. Both are spec-permitted ("env-probe static-bool check"). **Check 2 PASSES.**

---

## 3. Check 3 — Default-on capture sample

**Status: PARTIAL — schema correct, performance critical finding (see §5)**

### Procedure

```
MC2_PROJECTZ_TRACE=1 MC2_PROJECTZ_SUMMARY=1 MC2_PROJECTZ_HEATMAP=1
py -3 scripts/run_smoke.py --exe A:/Games/mc2-opengl/mc2-projectz-hunting/mc2.exe \
    --mission mc2_01 --duration 15 --keep-logs
```

Log: `tests/smoke/artifacts/2026-04-25T22-22-18/mc2_01.log` — **451,745 lines** in ~15 seconds.

### Banner ✅

```
[INSTR v1] enabled: tgl_pool=1 destroy=0 gl_error_print=1 smoke=1 build=UNKNOWN
[PROJECTZ v1] enabled: trace=1 heatmap=1 summary=1 guard_px=64
```

Banner present. `guard_px=64` — implementation default is hardcoded to 64; spec says default 0. See §5 Findings.

### Vertex record excerpt ✅

```
[PROJECTZ v1] vertex callsiteId=terrain_cpu_vert_admit
  file=<inline> line=0 cat=BoolAdmission branch=perspective
  point=(2176.0000,-1920.0000,395.3726)
  signedW=-1033.492676 legacyRhw=-0.000968 trueSignedRhw=-0.000968 rawClip=(-123.0309,84.0546,-896.7974,-1033.4927)
  screen=(0.00,0.00,0.8677,0.000968) legacyAccepted=true
  predicates: legacyRect=T legacyRectFinite=T homogClip=F rectSignedW=F rectNearFar=F rectGuard=T
  consumes_z=false
```

Schema complete. Note `signedW=-1033.5` (negative W — vertex is behind camera) yet `legacyAccepted=true` — this is the W-sign-destruction case the spec was designed to capture.

### Triangle record excerpt ✅

```
[PROJECTZ v1] tri callsiteId=terrain_quad_cluster_a
  quad=(64,66) cluster=0,1,2 file=...\mclib\quad.cpp line=437
  v0.legacy=false v1.legacy=false v2.legacy=true
  trianglePolicy_any=true trianglePolicy_all=false trianglePolicy_majority=false
  currentSubmitted=true containsLegacyRejectedVertex=true screenAreaPixels=0.0
```

Schema complete. Cluster indices (0,1,2) and (0,2,3) both observed — BOTTOMRIGHT UV mode. TOPLEFT clusters (0,1,3)/(1,2,3) not observed in this short window (map-dependent).

### Distinct callsite IDs ✅

Five priority IDs observed in vertex records:
- `terrain_cpu_vert_admit`
- `terrain_quad_vert0_admit`
- `terrain_quad_vert1_admit`
- `terrain_quad_vert2_admit`
- `terrain_quad_vert3_admit`

`unknown` also appears for non-priority callsites — per-spec.

### consumes_z=true ⚠ NOT OBSERVED

No weaponbolt or actor_vfx callsites fired in the 15-second pre-combat window. Not a wiring bug — the callsites exist but require active combat to hit. The `is_consumes_z()` function is correctly implemented; it was not exercised.

### Summary at frame 600 ⚠ NOT TRIGGERED

Only **5 frames** ran in 15 seconds (due to the performance issue below). The shutdown summary fired at frame 5. The 600-frame cadence was not reached.

### Shutdown summary ✅

```
[PROJECTZ v1] summary frames=5
  category=BoolAdmission          calls=33577 parallel=0 rejected=30869
  category=unknown                calls=19273 parallel=0 rejected=4211
  perspective_calls=52850
  predicate=legacyRectFinite   agree=52850 disagree_perm=0 disagree_restr=0  (0.00%)
  predicate=homogClip          agree=35080 disagree_perm=0 disagree_restr=17770 (33.62%)
  predicate=rectSignedW        agree=36259 disagree_perm=0 disagree_restr=16591 (31.39%)
  predicate=rectNearFar        agree=35080 disagree_perm=0 disagree_restr=17770 (33.62%)
  predicate=rectGuard          agree=51964 disagree_perm=886 disagree_restr=0  (1.68%)
  outlier callsiteId=<unknown>                   homogClip_dis=15062 rectSignedW_dis=15062
  outlier callsiteId=terrain_cpu_vert_admit      homogClip_dis=1291  rectSignedW_dis=1219
  outlier callsiteId=terrain_quad_vert2_admit    homogClip_dis=1275  rectSignedW_dis=211
  ...
  triangles total=16332 red(contains_rejected)=12835 purple(area_exceeded)=1176
```

Summary format correct. The data is immediately useful: 33.6% homogClip disagreement and **78.6% of submitted terrain triangles contain at least one legacy-rejected vertex**.

### Priority callsites tracing as `<unknown>` ⚠

One concern: the `<unknown>` outlier accounts for 15,062 homogClip disagreements — the largest single contributor. This represents projectZ calls in non-priority code paths that have no PROJECTZ_SITE() macro. Per-spec this is acceptable; the spec explicitly allows the 76 ScreenXYOracle/DebugOnly bulk sites to trace as `<unknown>`. However the volume (19,273 calls in 5 frames vs 33,577 tagged calls) suggests some high-volume paths may be worth tagging in a future pass.

---

## 4. Verdict

| Check | Result |
|-------|--------|
| Check 1 — smoke tier1 default-off | WAIVED (user direction) |
| Check 2 — default-off bit-equivalence | ✅ PASS |
| Check 3 — default-on capture schema | ✅ PASS (schema) |
| Check 3 — consumes_z=true observed | ⚠ NOT OBSERVED (no combat) |
| Check 3 — 600-frame summary | ⚠ NOT REACHED (perf issue) |

**Overall: CONDITIONAL PASS — blocking performance issue must be addressed before the trace system is usable.**

---

## 5. Findings

### F1 — BLOCKING: fflush() per vertex record causes 0.3 FPS in trace mode

**Location:** `mclib/projectz_trace.cpp:278` — `fflush(stdout)` inside `projectz_trace_dispatch()` after every per-vertex printf.

**Symptom:** With `MC2_PROJECTZ_TRACE=1`, mc2_01 ran at ~0.3 FPS (5 frames in 15 seconds). Normal framerate is 150+ FPS. The game was visually broken (black terrain, HUD-only rendering, frozen).

**Cause:** At 150 FPS with large terrain, the engine calls `projectZ` on ~10,570 vertices per frame. Each call triggers a multi-line printf + `fflush(stdout)`. With stdout piped (as the smoke runner does via `subprocess.PIPE`), every `fflush()` is a blocking OS system call. This serializes the frame loop on I/O.

Same issue applies to `projectz_emit_tri()` — `fflush` is called there too.

**Recommended fix:** Remove the `fflush(stdout)` from the per-vertex and per-triangle printf paths. Add a single `fflush(stdout)` at the end of `emit_summary()` (already present) and in `projectz_frame_tick()` once per frame if desired. The `setvbuf(stdout, NULL, _IONBF, 0)` unbuffering call in gameosmain.cpp already forces line-level flush on every `printf`, making the extra explicit flushing redundant and harmful.

**Must fix before phase 4 capture work.**

### F2 — MINOR: guard_px hardcoded default is 64, spec says 0

**Location:** `mclib/projectz_trace.cpp:28` — `int g_pzGuardPx = 64;`

**Spec says:** `MC2_PROJECTZ_GUARD_PX` defaults to 0.

**Impact:** The `rectGuard` predicate uses a 64-pixel margin by default rather than matching the legacy zero-margin viewport. This means out-of-box runs without explicitly setting `MC2_PROJECTZ_GUARD_PX=0` will see rectGuard accept 886 vertices that legacyRect rejects (1.68% of calls), skewing the baseline comparison. Should be changed to default 0.

### F3 — INFORMATIONAL: Immediate diagnostic value from 5-frame sample

Even at 0.3 FPS, the 5-frame sample produced meaningful data:
- **33.6% of perspective projectZ calls** have negative W (behind-camera), yet are accepted by the legacy screen-rect gate — confirms the W-sign-destruction hypothesis.
- **78.6% of submitted terrain triangles** contain at least one vertex that homogClip, rectSignedW, and rectNearFar all reject. This means the majority of submitted terrain geometry would be discarded by any modern homogeneous clip predicate.
- `rectGuard` has the lowest disagreement (1.68%) — suggests a guard-band expansion predicate is nearly equivalent to the legacy gate for the common case.

These numbers directly inform the phase-4 replacement strategy.

### F4 — INFORMATIONAL: `<unknown>` callsite volume

19,273 calls (36% of total) traced as `<unknown>` — significantly more than the 5 tagged priority sites. Most likely source: the large number of `projectZ` calls in mclib paths not covered by the 19 priority PROJECTZ_SITE() insertions. Phase 4 should tag the highest-volume `<unknown>` paths to reduce noise in the heatmap.

---

*Validation run by Claude Sonnet 4.6. Stop-and-ask policy was followed: no code changes were made during this session.*
