# QUIET-DEFAULT-BUILD-1 — status

Lane: QUIET-DEFAULT-BUILD-1 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Baseline at lane start: `49b00c14` · HEAD at lane end: `b06f91f0`
Mission: make a default build quiet, and fix the failing contract.

---

## Headline

**A default build went from 79 diagnostic tags / 439 lines to 9 tags / 44 lines
— 90% quieter — and every one of the 9 survivors is a tag the verification
harness parses.** Measured, not inferred: same mission (`mc2_10`), same scrubbed
environment, before-exe vs after-exe.

Contract suite: **65 passed / 1 failed → 67 passed / 0 failed.**

---

## META REVIEW — premise corrections (verified from source, not status docs)

The brief was accurate about the defects. Three of its inherited claims were
not, and one of them would have produced a checker that cried wolf.

1. **The quiet ratchet was never wired up.** The brief says VERIFY-INTEGRITY-3
   "added a quiet ratchet — extend or wire it up". The script existed but
   `grep -c check-default-build-quiet scripts/check-contracts.sh` returned
   **0**. The suite went 65→66 because of `bookmark_honesty`, not the ratchet.
   Nothing ever ran it. Now wired as `default_build_quiet` (67 checks).

2. **Wiring it up revealed it would have failed on things the user never
   sees** — twice over. Both fixed, because a checker that raises false alarms
   earns the habit of being ignored, which is how this debt accumulated:
   * It graded **diagnostic runs**. The selector took the newest smoke run
     unconditionally, but the canonical tier1 invocation exports
     `MC2_DEBUG_STATE_DUMP` and `MC2_DIAG_TAGS` — so the usual newest run is not
     a default build. `DIAG_ENV_HINTS` was declared in the original and never
     consulted. Runs are now filtered on the manifest's `env_gates`.
   * It graded **sidecar trace files**. `rglob("*.log")` swept up files the
     engine writes to disk on purpose — `mc2_10.ring_trace.log` is an `fprintf`
     to a `FILE` handle that never reaches the console. `[RING_SINK v1]` failed
     the ratchet on that basis alone.

3. **"Guarded" is not the same as "silent"** — the correction that mattered
   most. My first pass skipped any site whose enclosing condition referenced a
   gate, reasoning that gating it again would silently require
   `MC2_DIAG_VERBOSE` on top of an existing facility. But a **default-ON**
   feature gate still prints for every user, every launch. That left 32 tags
   still printing. Only the runtime measurement exposed it — which is precisely
   the argument for the ratchet being a runtime check rather than a grep.

4. **The `spirv_artifacts` fix was a no-op in generated code.** See Task B.

---

## Task A — silencing the default build

### The numbers (measured, `mc2_10`, scrubbed env, both exes)

| | distinct tags | diagnostic lines |
|---|---|---|
| BEFORE — exe `01bfcc055ff2` | **79** | **439** |
| AFTER — exe `6de1872c9e27` | **9** | **44** |
| silenced | **70** | **395** (90%) |

Regressions: **none** (zero tags present after that were absent before).

### The gate

ONE registered umbrella gate, **`MC2_DIAG_VERBOSE`** (default OFF), registered
in `RenderCore/RendererFeatureRegistry.h` as `MC2_TRACE_DIAG_VERBOSE` and
documented in `docs/tier1_env_vars.md`. `gate_status` verdict: **OK, no flags.**

Not one gate per tag: ~380 call sites share a single purpose ("show me the
engine's internal chatter"), so they share a single switch. 70 registry entries
nobody would ever set is a filing cabinet, not a facility. Anything needing
selective or structured control keeps using the per-subsystem facilities
(`MC2_TERRAIN_LOD_CHUNK_DIAG` bits, `MC2_DIAG_TAGS`).

**Header-only** (`GameOS/include/mc2_diag_verbose.h`): GameOS, mclib, code,
RenderWorld and GameAdapters are five separate link units with no shared utility
library, but `GameOS/include` is on all of their include paths. An inline costs
one `getenv` per TU and needs **no CMake change** — no new source file on the
right target, no link-order risk.

Four macros, each a **one-token** replacement, chosen so the merge surface stays
minimal for the lanes editing these files concurrently:

| macro | replaces |
|---|---|
| `MC2_DIAGF` | `printf("[TAG v1] …")` |
| `MC2_DIAGFE` | `fprintf(stderr, "[TAG v1] …")` |
| `MC2_DIAG_PUTS` / `MC2_DIAG_FPUTS` | the `puts`/`fputs` sink of an assembled line |

### Gated vs deleted

**Gated: 70 tags, ~380 sites, 46 files.** Nothing was deleted. Every line the
engine used to print it still prints — behind a switch. This was deliberate:
these tags were written by people diagnosing real problems, and the audit
measured *noise*, not *uselessness*. Deleting would have destroyed evidence the
next investigator needs; gating costs one `getenv` and loses nothing.

**Proven, not asserted — the round trip:**

| run | tags | lines |
|---|---|---|
| BEFORE (old exe) | 79 | 439 |
| AFTER, `MC2_DIAG_VERBOSE` unset | 9 | 44 |
| AFTER, `MC2_DIAG_VERBOSE=1` | **79** | 438 |

Tags in BEFORE missing with the gate ON: **none**. Tags the gate adds that
BEFORE lacked: **none**. The output is restored verbatim.

### The banners advertising switched-off features

The brief named three. There were **four** — `[MECH_OBJECT_ID_SELFTEST v1]
result=SKIPPED reason=env_disabled` in `RenderWorld/RenderWorld.cpp` announces
a feature that is off, in the same shape as the three known ones
(`CLUSTER_DEPTH_PYRAMID`, `LIGHTGRID_BUILD`, `POSTPROCESS_BLUR`). **All four are
gone from a default build.**

### The nine every-600-frame repeaters

`STATIC_UPDATE` `APPEAR_ROUTE` `ANIM_GATE` `CAMERA_MOTION` `TEX_RESOLVE`
`RENDER_STATES` `TERRAIN_LIGHTING_GPU` `WATER_FULL_RECIPE_AUTH` `WATER_REFL_RT`
— **all nine gone**, plus `VISIBILITY` and `RENDER_WORLD`, two more per-600-frame
repeaters the original audit missed because `RenderWorld/` and `GameAdapters/`
were outside the roots it swept.

### Attributed debt — the 9 that still print, and why

These are **load-bearing for verification itself**. Silencing them would blind
the smoke harness, which is a worse failure than being noisy. Each is recorded
in `tests/contract/default_build_quiet_baseline.json` with a `keep` reason
naming the harness code that reads it:

| tag | read by |
|---|---|
| `BUILD_FINGERPRINT v1` | `smoke_lib/fingerprint.py` — DEPLOY_FINGERPRINT attribution |
| `SMOKE v1` | `run_smoke.py`, `runner.py`, `run_editor_playtest_smoke.py` — the result channel |
| `INSTR v1` | `whatran.py`, `smoke_lib/gates.py` (reports "banner absent" as failure) |
| `PERF v1` | `logparse.py`, `oracle_report.py` — the perf regression signal |
| `TIMING v1` | `logparse.py` `TIMING_RE` |
| `TGL_POOL v1` | `logparse.py` pool-nulls counter |
| `ASSET_SCALE v1` | `logparse.py` `oob_blit` counter |
| `ASSET_LOAD v1` | `logparse.py` `icon_oob` MarkerBucket |
| `RENDER_SNAPSHOT v3` | `oracleparse.py` — static-prop snapshot oracle |
| `QUADSETUP_SKIP v1` | `oracleparse.py` `QUADSKIP_RE` |

(Ten entries; `ASSET_LOAD` is event-conditional and did not fire in the graded
run, so the ratchet reports it as RETIRED — kept because the event is real.)

Moving any of these is a **harness-contract change** and belongs to its own
slice, not to a quieting pass.

### The ratchet, and proof it has teeth

`scripts/check-default-build-quiet.py`, wired into `check-contracts.sh` as
`default_build_quiet`. Runtime, not lexical — "is this printf gated?" is not
answerable by grep (getenv-seeded statics, `MC2_*_DIAG` bitmasks, default-ON
`IsEnabled()`, `#ifdef`s, gated enclosing functions). It grades the newest smoke
run taken with no output-affecting `MC2_*` env, reads only per-mission stdout
logs, and SKIPs (exit 0) when no clean run exists.

**Verified in both directions:** passes clean on a real default-env run
(9 observed / 10 baseline), and a synthetic `[SYNTHETIC_SPAM v9]` line injected
into that same log makes it **exit 1 naming the tag**.

Baseline **83 → 10**, and its character changed: the seed marked every tag
`"debt … needs gating"` with no owner; every survivor now carries a `keep`
reason naming its reader.

---

## Task B — the failing contract

`spirv_artifacts` failed on `mech.frag.{default,objectid,viewuniforms,`
`objectid_viewuniforms}: source drift`, because PROP-ALPHA-SHADOW-CLASS-1's
`d99bd798` changed `shaders/include/render_contract.hglsl`, which
`shaders/mech.frag:9` includes. Re-ran `tools/shader_offline_build/`
`build_variants.py`: 16/16 ok, check now PASSes with 0 fail.

**The artifacts provably match current sources rather than being re-stamped:**
only the four `.json` sidecars changed. **Zero `.spv` binaries differ** and
every `spirv_sha256` is unchanged — only `source_sha256` moved. So `d99bd798`'s
header edit did not alter `mech.frag`'s generated code at all.

**On the cache-invalidation weakness:** the brief warned `shaders/spv/*.spv` is
keyed only on `(base, stage, defines)` with no source-hash validation. The
*contract checker* does validate — it stores `source_sha256` per variant and is
exactly what caught this drift. The gap is narrower than stated and the fix is
not trivial (the runtime loader would need to hash includes transitively at
load), so per the brief I did **not** scope-creep into it. Left as noted debt.

---

## Verification

All at HEAD `b06f91f0`, clean tree, clean-fingerprint build, deployed with
`--require-clean`.

| Gate | Result |
|---|---|
| build | clean, 0 errors (only pre-existing C4267/C4996 warnings) |
| deploy | `--require-clean` OK; `--verify-only`: **806 match, 0 stale, 0 missing** |
| `tools/build_contract_harnesses.py --run` | **16/16 PASS** |
| `scripts/check-contracts.sh` | **67 passed, 0 failed** |
| tier1 | **5/5 PASS**, `DEPLOY_FINGERPRINT OK: exe sha=b06f91f0abbb` on all five |
| `MC2_DIAG_VERBOSE` in deployed exe | **PRESENT** |
| `gate_status MC2_DIAG_VERBOSE` | **verdict OK, no flags** (registered, documented, allowlisted, default OFF, 2 readers) |
| quiet ratchet | OK clean; **exit 1** on injected synthetic tag |

---

## Found along the way — NOT mine, needs an owner

**`mc2_01` crashes intermittently in `BuildCementCatalogAtlas`.**

```
#00 BuildCementCatalogAtlas   GameOS/gameos/gos_terrain_indirect.cpp:2074
#01 gos_terrain_indirect::BuildDenseRecipe                       :2240
#02 Terrain::primeMissionTerrainCache      mclib/terrain.cpp:1703
#03 Mission::init                          code/mission.cpp:3646
```

READ access violation at frame 0, mission load. **Pre-existing and not caused by
this lane** — proven by reproducing the identical stack on exe `01bfcc055ff2`
(VERIFY-INTEGRITY-3's HEAD, before any of my changes) under identical
conditions. It is **flaky**: both the old and new exe passed 3/3 isolated
`mc2_01` runs, and tier1 hit it once in five attempts. Prime suspect is
CEMENT-UNIFORM-2's committed WIP `1779bf1d` ("track alpha-vs-solid cement slots
in the catalog atlas", self-described as unfinished), which touches that exact
function.

**A false `deploy_payload` warning.** `deploy_payload.py` compares the build
fingerprint against the HEAD of whatever repo the CWD resolves to, so running it
with `--source-root A:/Games/mc2-srcdata` produces
`WARNING: build fingerprint sha X != git HEAD 19e8be13136d` where `19e8be13` is
the **data** repo's HEAD, not the code repo's. Same family of defect this lane
exists to fix: a warning that is wrong trains people to ignore warnings.

---

## Self-inflicted issues, disclosed

* **`c8f661b3` did not compile.** Two rewriter defects: overlapping edit spans
  (two `snprintf` appends sharing one `puts` sink → the same span applied twice
  → `MC2_DIAG_FPUTSPUTS`), and wrapping a variadic macro's body
  (`COMPUTE_TRACE`) in another variadic macro, which MSVC's traditional
  preprocessor does not expand. Fixed in `86fabbcf` at the source — edits are
  now span-keyed with a disjointness assertion, and the rewriter refuses to
  touch anything inside a `#define` body. Recovery was a reset to the pass-1
  tree and a clean re-run, so no corruption survives in the history's tip.
* **`86fabbcf` staged only `GameOS/` and `RenderWorld/`**, leaving three pass-2
  edits in `code/` and `mclib/` uncommitted; swept up in `605cd07e`. Briefly
  looked like a sibling lane editing under me — it was me.
* **I built before committing** more than once, producing two unattributable
  tier1 runs (`DEPLOY_FINGERPRINT MISMATCH`) that had to be redone. The final
  numbers all come from a clean-tree build at HEAD.
* **My first tier1 run smoked the wrong lane** (0.4c, the default) rather than
  0.4d-rc1 where I had deployed, and reported 5/5 against a stale exe. Caught by
  DEPLOY_FINGERPRINT, which is the control working.

---

## Merge cautions

* `docs/tier1_env_vars.md` — claimed by `EDGE-LIGHT-EMISSION-1` and
  `FORCE-ALL-VISIBLE-1`. My change is **append-only at end-of-file** and touches
  neither of their sections.
* `scripts/run_smoke.py` — claimed by `FORCE-ALL-VISIBLE-1`. One-line addition
  inside the existing allowlist tuple, same shape as VERIFY-INTEGRITY-3's entry
  directly above it.
* `tests/contract/default_build_quiet_baseline.json` — `tests/contract/` claimed
  by `CONTRACT-HARNESS-REPAIR-1`. Rewritten wholesale (83 → 10 entries).
* `GameOS/gameos/gos_terrain_indirect.cpp` (47 sites) — claimed by
  `CEMENT-ALPHA-LAND-1` and `CEMENT-UNIFORM-2`, the latter with unfinished WIP
  `1779bf1d` in `BuildCementCatalogAtlas`. All my edits there are one-token
  print-call swaps; none touch logic.
* `mclib/bdactor.cpp` (`CONE-SPILL-COUPLING-1`), `mclib/mech3d.cpp` +
  `code/objmgr.cpp` (`VISIBILITY-SEMANTICS-1`) — one-token swaps only.

---

## Not done

* The 11 baseline tags that had **no** direct print site and were never observed
  in a default run remain untouched; they cost nothing today.
* The runtime SPIR-V cache still does not validate source hashes (see Task B).
* The `deploy_payload` false-HEAD warning above.
* The `mc2_01` cement crash — needs CEMENT-UNIFORM-2 to finish or revert
  `1779bf1d`.
