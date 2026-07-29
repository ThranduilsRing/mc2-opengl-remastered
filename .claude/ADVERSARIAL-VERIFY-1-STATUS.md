# ADVERSARIAL-VERIFY-1 — hostile verification of this session's default-ON changes

**Lane:** ADVERSARIAL-VERIFY-1 (read-only; no code changes)
**Worktree:** A:/Games/mc2-main, branch `claude/nifty-mendeleev`
**HEAD at start:** `6440c84c` — **HEAD at end: `4aadd393`** (repo moved under this lane mid-session; sibling lanes are committing)
**Date:** 2026-07-28/29

---

## META REVIEW (state read from reader source, not status docs)

- Preflight: `branch_ok=true root_ok=true`, `safe_to_touch=false`. The 37 "requires ack"
  entries are all **untracked `scratch/` + `debug_state/` dirs**, not elevated tracked files.
  This lane touches none of them.
- **Contracts: 74/74 PASS** — CONFIRMED by running `scripts/check-contracts.sh`.
- **Harnesses: 12/12, NOT 16/16** — `tools/build_contract_harnesses.py` reports
  `built 12/12; 0 failed`. All pass, but the expected count of 16 is wrong: only 12
  harnesses are registered. Either 4 were never registered or the "16/16" figure is stale.

---

## VERDICT TABLE (ranked by user impact)

| # | Gate | Claimed default | **Actual default (source)** | Claimed effect | **Independently verified** | Verdict |
|---|---|---|---|---|---|---|
| 1 | *(infra)* protected reference installs | v0.3 + 0.4c untouched | — | user's A/B references preserved | **v0.3, v0.4c AND v0.4 all overwritten with 2026-07-26 session builds** | **REFUTED** |
| 2 | `MC2_WATER_SHORE_MEANDER` | now `0` | **`0.0f` when unset** ✔ | mc2_10 byte-identical at 75 vs 0 on 3 water poses | could not reproduce — no attributable exe exists (see §Unverifiable) | **UNVERIFIABLE** |
| 3 | `MC2_TERRAIN_CONCRETE_DETAIL_NORMAL` | now OFF | **OFF** (`v[0]=='1' && v[1]=='\0'`) ✔ | "restores the original overlay look" | asset census: `concrete_normal.ktx2` ships in **only** the `0.5 testing` install | **OVERSTATED** |
| 4 | `MC2_ROAD_PBR` | retired tombstone, inert | **no reader anywhere in source** | inert | confirmed: absent from all source *and* from every deployed exe binary | **CONFIRMED** |
| 5 | `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` | ON | **ON** (`!(v=="0")`) ✔ | 70.0%→2.9%; 14 poses byte-identical | default confirmed; effect not re-measured | **CONFIRMED (default only)** |
| 6 | `MC2_STATIC_PROP_IBL_NIGHT_SCALE` | floor 0.25 | **0.25f when unset** ✔ | day byte-identical *by arithmetic* | day-identity claim is **sound by construction** (nightFactor==0 → ×1.0f exactly), not a measurement | **CONFIRMED (default + day identity)** |
| 7 | `MC2_STATIC_PROP_FIXTURE_LIGHTS` | ON | **1.0f when unset** ✔ | 4/773 `.ini`, zero buildings | default confirmed; counts not re-derived | **CONFIRMED (default only)** |
| 8 | `MC2_LIGHT_GRID` | ON | **ON**, `MC2_LIGHT_GRID_DIM` 64 ✔ | ≤20 lights, exact-not-approximate | default confirmed; perf not re-measured | **CONFIRMED (default only)** |
| 9 | `MC2_LIGHT_POOL_MAX` | 256 | **`defaultCap = 256u`** ✔ | 0 dropped where 187–201 wanted | default confirmed | **CONFIRMED (default only)** |
| 10 | `MC2_LETTERBOX_OPAQUE` | ON | **ON** (`!(v=="0")`) ✔ | — | default confirmed | **CONFIRMED** |
| 11 | `MC2_VIS_SEMANTICS` | ON | **ON** via `env_default_on()` ✔ | — | default confirmed | **CONFIRMED** |
| 12 | `MC2_LIGHT_CONE_NODES` | ON | **ON** ✔ | — | default confirmed | **CONFIRMED** |
| 13 | `MC2_CEMENT_MISSION_LIGHT` | ON | **ON** ✔ | — | default confirmed | **CONFIRMED** |
| 14 | `MC2_LIGHT_CULL_RADIUS` | ON | **ON** ✔ | — | default confirmed | **CONFIRMED** |
| 15 | `MC2_SHADOW_ALT_SCALE` | ON | **ON** ✔ | — | default confirmed | **CONFIRMED** |
| 16 | `MC2_SHADOW_FADE_RAMP0` | ON | **ON** ✔ | — | default confirmed | **CONFIRMED** |

**No gate default in the audited set contradicts its claim.** Trap #6 (docs disagreeing with
source) did **not** fire this time — all 15 read correctly at the `getenv` site.

---

## FINDING 1 (P0) — the user's protected reference installs were overwritten

The instruction was: do not deploy over `mc2-win64-v0.3` (user's concrete reference) or
`0.4c` (blessed golden baseline). Both — plus `v0.4` — now contain **session builds**:

| Install | mc2.exe mtime | Embedded build time | Embedded branch | Embedded src worktree |
|---|---|---|---|---|
| `mc2-win64-v0.3` | Jul 26 10:03 | **2026-07-26T10:03:42** | `claude/nifty-mendeleev` | `A:/Games/mc2-main` |
| `mc2-win64-v0.4c` | Jul 26 05:34 | **2026-07-26T05:07:58** | `claude/nifty-mendeleev` | `A:/Games/mc2-main` |
| `mc2-win64-v0.4` | Jul 26 11:26 | **2026-07-26T11:25:53** | `claude/nifty-mendeleev` | `A:/Games/mc2-main` |

Corroborated independently of timestamps by binary content: all three exes contain
`MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN`, `MC2_LIGHT_GRID`, `MC2_STATIC_PROP_IBL_NIGHT_SCALE`
— gates that did not exist when v0.3/v0.4/v0.4c were cut. A genuine historical release
cannot contain them.

The gate-presence pattern **dates each overwrite precisely**:
`MC2_TERRAIN_CONCRETE_DETAIL_NORMAL` (commit `62f789c2`, 07-26 10:31) is **absent** from
v0.4c (built 05:07) and v0.3 (built 10:03) but **present** in v0.4 (built 11:25). The
build times and the binary contents agree exactly.

**Consequence beyond the lost references:** any lane that graded "vs the 0.4c golden
baseline" or "vs the v0.3 concrete reference" **after 2026-07-26 05:07** was comparing
against a build that already carried the gates under test. Those baselines are
contaminated, and the affected comparisons are not evidence of anything.

---

## FINDING 2 (P1) — no deployed exe carries HEAD; the newest is ~2 days stale

Newest deployed `mc2.exe` anywhere is `mc2-win64-abl-validate`, built **2026-07-26 11:36**.
HEAD is `4aadd393` with commits well past that point (`67b25f47` water-meander park at
07-26 11:25 is the *last* audited commit any install could carry; `6440c84c`, `0b26eebe`,
`62c77d29` and HEAD itself post-date every install).

`build64/RelWithDebInfo/mc2.exe` is **not** a substitute:
- `build64/generated/build_fingerprint.h` says sha `4aadd393acbd`, `DIRTY=1`
- the **exe actually on disk** reports `[BUILD_FINGERPRINT v1] sha=d6ae45f9a150 dirty=1`
  and `WARNING: BUILT-FROM-DIRTY-TREE`

So the header and the binary disagree — the header was regenerated after the last link.
This is precisely the trap-#1/#2 class: an exe that looks like HEAD by its sidecar and
is not HEAD by its own embedded fingerprint. **Any lane that smoked `build64` directly
this session and reported a sha from the header rather than from the runtime
`[BUILD_FINGERPRINT v1]` line reported the wrong sha.**

---

## FINDING 3 — `MC2_TERRAIN_CONCRETE_DETAIL_NORMAL` flip is a no-op on almost every install

Asset census for `*_normal.ktx2`:

| Install | count | concrete_normal.ktx2? |
|---|---|---|
| `0.5 testing/mc2-win64-v0.5.0` | 9 | **YES** |
| `mc2-win64-v0.3` | 1 | no (only `corrugatedsteel006a_normal`) |
| `mc2-win64-v0.4c` | 1 | no |
| `mc2-win64-v0.4` | 1 | no |
| `mc2-win64-v0.5.1` | 1 | no |

The lane's premise is **correct**: only the `0.5 testing` install ever shipped
`concrete_normal.ktx2`. But the framing "this restores the original overlay look" is
**OVERSTATED** — on every install the user is likely to run (v0.3/v0.4/v0.4c/v0.5.1)
there is no concrete normal map to suppress, so the flip changes nothing there. The
change is real only on `0.5 testing`.

Caveat I could not close: whether a *missing* `concrete_normal.ktx2` resolves to a flat
fallback or a magenta placeholder. If it resolves to a placeholder, the pre-flip default
was doing something visible on those installs after all. Unresolved.

---

## FINDING 4 — `MC2_WATER_SHORE_MEANDER` is UNREGISTERED

`gate_status` reports `registered: false`, `registry_entry: null` — it has **no entry in
`RendererFeatureRegistry.h`**, despite being a gate whose default was *changed* this
session and whose new default is load-bearing for a user approval. It survives
`check-env-registry.sh` only because it sits in that script's ALLOWLIST. It is tier1
documented and smoke-allowlisted, so it will not silently drop in a smoke run — but a
default-behaviour change riding an unregistered gate is the same shape as the
CONE-SPILL-COUPLING-1 defect the registry exists to catch.

---

## FINDING 5 — the two most-cited goldens have no vendored poses

`scripts/check-visual-bookmark-honesty.py` (37 tracked poses, 27 lane bookmarks clean):

- `tests/visual/goldens/v0.5.1-baseline/` — **no vendored `poses/` dir**
- `tests/visual/goldens/v0.5.2-baseline-PROPOSED/` — **no vendored `poses/` dir**

Both are UNAUDITABLE: nothing can confirm the poses defining them frame their subjects.
`v0.5.2-baseline-PROPOSED` is the set awaiting user promotion. It should not be promoted
while its defining poses exist only inside a deploy install.

Also still flagged: `mc2_01_water.json` carries acknowledged unverified framing — a zero
diff between two arms there is equally consistent with **no water in frame**. Do not
cite it.

By contrast `tests/visual/bookmarks/mc2_10_water.json` is **clean and well-founded** —
its three poses were derived by decoding the mission `.pak` MapData packet (side=120,
half-extent 7616wu, water mask elev ≤ 80), each centred on a ~50% water/land boundary
cell. This is the correct pattern and did **not** fall into trap #3.

---

## FINDING 6 — trap #4 is real and still live in this shell

`run_visual_capture.py` reported:

> `[env-pin] WARNING: 3 ambient MC2_* variable(s) were present in this shell and have been
> SCRUBBED: MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE, MC2_GPU_CULL_STATIC_FROZEN_RECORDS,
> MC2_STATIC_POP_SPLIT`

`scripts/mc2_env_pin.py` **is working and did its job**. But the contamination is still
present in the ambient environment, so any capture taken this session through a path that
does *not* route through the env pin is suspect.

---

## FINDING 7 (methodology, self-inflicted) — `strings` does not exist on this machine

My first binary probe used `strings -a <exe> | grep ...` and returned **0 hits for every
gate in every exe**, which reads exactly like the trap-#7 "gate absent from the deployed
binary" finding. It was an artifact: `strings` is not installed (`command not found`),
stderr was being discarded, and the empty pipe produced a clean, plausible, entirely
false zero.

I caught it only because I ran a **control** — total count of `MC2_*` in the same binary,
which also returned 0, which is impossible. The real probe (Python byte-scan) then showed
1300+ `MC2_*` strings per exe.

**Any lane this session that concluded "gate string absent from the deployed exe" using
`strings` reached that conclusion through a broken probe and must re-run it.** This is
the single most transferable finding in this report.

---

## ADVERSARIAL SELF-REVIEW — what I could NOT verify, and why

I verified **defaults** for all 16 items from the `getenv` site. I did **not**
independently reproduce any of the **quantitative effect claims** (70.0%→2.9%; 2.49%→0.13%;
69 px / 0.00083%; 200→≤20 lights; p50 6.481 vs 6.679 ms; 0 dropped of 187–201;
byte-identity of meander 75 vs 0). Reason, established empirically rather than assumed:

1. I attempted the load-bearing measurement — `mc2_10_water`, 3 runs, meander default —
   and it failed with `MISSING` on all three poses. The log shows the cause:
   `build64/RelWithDebInfo/mc2.exe` run from the worktree cannot find `shaders/` or
   `options.cfg` (`sniffer branch reached; options.cfg not found at CWD`). Captures must
   run from a deployed install.
2. Every deployed install is ≥2 days stale (Finding 2), and the three most relevant ones
   have been overwritten with mid-session builds (Finding 1). None carries HEAD.
3. Producing an attributable exe would require a rebuild + deploy. `build64` is contended
   by live sibling lanes, the tree is dirty, and disk is ~12 GB. I judged deploying
   against the read-only mandate of this lane and stopped rather than manufacture a
   number.

So: **the effect claims in rows 2 and 5–9 are neither confirmed nor refuted by me.** They
are marked CONFIRMED *(default only)* — meaning I verified the gate is wired the way the
lane said, and nothing more. Do not read those as endorsement of the percentages.

Specifically **not** done, and it matters:
- **No three-run floor measurement.** Trap #3 (a "0.000% floor" that was recorded rather
  than measured, and was really 15%) is therefore **unaudited** for every lane in this
  set. This was my highest-priority empirical target and I could not reach it.
- **The `MC2_WATER_SHORE_MEANDER` byte-identity claim — explicitly flagged as load-bearing
  for a user approval — remains untested.** It should not be treated as established.
- I did not verify which lanes ran a valid tier1 versus printing `PASS (5/5)` alongside
  `DEPLOY_FINGERPRINT MISMATCH` (trap #1). Given Findings 1 and 2, **any tier1 run after
  2026-07-26 11:36 could not have been against a HEAD exe**, so the fingerprint question
  is live for the tail of this session — I just did not enumerate the per-lane logs.
- Finding 3's placeholder-vs-flat-fallback question is open.
- I ran the contracts and harness gates once, not three times; both are deterministic
  checkers, so I consider that adequate, but it is not a floor measurement either.

**Confidence:** high on Findings 1, 2, 3, 4, 5, 7 (each rests on directly observed bytes,
timestamps, or tool output reproduced here). Medium on Finding 6 (tool self-reported).
The verdict table's *default* column is high confidence throughout; its *effect* column
is deliberately empty where I could not measure.

---

## RECOMMENDED USER GATES

1. **Restore `mc2-win64-v0.3` and `mc2-win64-v0.4c` from archive/tag before any further
   A/B grading.** They are currently session builds. This is the blocker.
2. Re-audit any post-2026-07-26-05:07 comparison that cited 0.4c or v0.3 as baseline.
3. Do not promote `v0.5.2-baseline-PROPOSED` until its poses are vendored (Finding 5).
4. Re-run any `strings`-based binary check from this session (Finding 7).
5. Register `MC2_WATER_SHORE_MEANDER` in `RendererFeatureRegistry.h` (Finding 4).
6. Reconcile the harness count: 12 registered, 16 expected.
7. Build + deploy a HEAD exe to a scratch lane, then have a lane actually measure the
   meander 75-vs-0 identity claim with a real three-run floor.
