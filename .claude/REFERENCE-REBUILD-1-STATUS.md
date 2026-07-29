# REFERENCE-REBUILD-1 — repairing the evidence base

**Lane:** REFERENCE-REBUILD-1
**Worktree:** `A:/Games/mc2-main` (branch `claude/nifty-mendeleev`); **build** done in a
private clean worktree `A:/Games/mc2-refbuild` (detached at `cf97ed52`)
**HEAD at lane start:** `cf97ed52` — **HEAD at lane end: `7ca604d1`** (four sibling
commits landed mid-lane: `be445b02`, `c6db4b3a`, `7c80c004`, `7ca604d1`)
**Date:** 2026-07-28/29
**Predecessor:** `.claude/ADVERSARIAL-VERIFY-1-STATUS.md` (`cf97ed52`)

---

## META REVIEW (read from reader source, not status docs)

- Preflight: `branch_ok=true root_ok=true`, `safe_to_touch=false` — the "requires ack"
  entries were sibling lanes' in-flight tracked edits plus untracked `scratch/`.
  Those siblings committed during this lane; the tree is clean at write time.
- `register_lane` warned of a 2-path overlap with **CEMENT-ATLAS-CELL-2**
  (`RendererFeatureRegistry.h`, `docs/tier1_env_vars.md`). I waited until that lane's
  work was committed, then made a purely additive edit. No sibling work was scooped.
- **Contracts at clean `cf97ed52` (private worktree): 73 pass / 1 fail** (`doc_links`).
  **Contracts in `mc2-main` at lane end: 71 pass / 3 fail.** All three failures are
  sibling-attributable and **none is mine** — see §6.

---

## 1 (P0, NEW) — the load-bearing meander claim is **REFUTED**

`MC2_WATER_SHORE_MEANDER=75` vs `0` on `mc2_10` was claimed **byte-identical on all
three water poses**, and *the user's approval of parking the default at 0 rests on that
claim*. It is false. Measured on the clean `cf97ed52` exe, poses
`tests/visual/bookmarks/mc2_10_water.json` (the pak-derived set the audit certified as
trustworthy), `--runs 3 --warmup 1` per arm, `--env-declare`, ambient `MC2_*` scrubbed:

| pose | **floor** (within-arm, both arms) | **signal** (arm 0 vs arm 75) |
|---|---|---|
| `shore_west_wide` | `0.0000%` changed, max=0 | **4.0708%** changed, max delta **73** |
| `shore_southeast_wide` | `0.0000%` changed, max=0 | **6.5228%** changed, max delta **73** |
| `shore_south_wide` | `0.0000%` changed, max=0 | **9.7375%** changed, max delta **115** |

The floor is **measured, not recorded**: post-warmup runs were hash-identical in both
arms on all three poses (`STABILITY PASS`), so the noise floor is exactly zero and the
signal is unambiguous — this is not a marginal call.

**Why the original claim was probably made:** it is the shape of trap #3 — a "0.000%"
recorded rather than measured. The gate demonstrably took effect (the two arms differ),
so this was not an ENV-DROP.

**What this changes:** the *decision* to park the default at 0 still stands on its own
merits — `75` is independently documented (`0888bbcd`, `67b25f47`) to **carve away water**
on `mc2_10` and `mc2_17`. What is void is the *reassurance* that flipping the default was
visually free on `mc2_10`. It was not: it changes 4–10% of pixels on every water pose
tested. **The user approved the park partly on that reassurance and should be told.**

## 2 — binary-inspection recipe (`strings` is not installed)

`scripts/pe_gate_scan.py` (new). Dependency-free byte scan of the PE for ASCII **and**
UTF-16LE forms, with a **mandatory self-test on every run**: a positive control
(`[BUILD_FINGERPRINT v1]`, in every MC2 exe) and a negative control (an impossible
string). If the positive control is missing or the negative one matches, it exits 2 and
prints nothing else. An empty result can therefore never be silently mistaken for a real
"gate absent" finding — which is exactly how the broken `strings` probe failed.

```
py -3 scripts/pe_gate_scan.py <exe> MC2_GATE_A MC2_GATE_B     # named needles
py -3 scripts/pe_gate_scan.py <exe> --list                    # every MC2_* string
py -3 scripts/pe_gate_scan.py <exe> --gates-from-registry     # sweep the registry
```

**The failure it replaces, reproduced here:**
`strings` → `NOT INSTALLED`; `strings -a mc2.exe 2>/dev/null | grep -c MC2_LIGHT_GRID`
→ **`0`**, for a gate that **is** present. Clean, plausible, entirely false.

**Proof it discriminates** (not merely "it ran"). Against the `c3804cfe` exe, predicting
from commit lineage before scanning — **4/4 correct**:

| gate | introduced | in `c3804cfe`? | predicted | **scanner** |
|---|---|---|---|---|
| `MC2_WATER_SHORE_MEANDER` | `4be51440` 07-24 | yes | PRESENT | **PRESENT** ✔ |
| `MC2_LIGHT_GRID` | `93c22e39` 07-25 22:08 | no | ABSENT | **ABSENT** ✔ |
| `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` | `1b23a82b` 07-25 23:22 | no | ABSENT | **ABSENT** ✔ |
| `MC2_TERRAIN_CONCRETE_DETAIL_NORMAL` | `62f789c2` 07-26 10:31 | no | ABSENT | **ABSENT** ✔ |

**Re-run of the `strings`-era conclusions.** The scanner **independently reproduces the
audit's Finding 1 dating** from binary content alone: `MC2_TERRAIN_CONCRETE_DETAIL_NORMAL`
is ABSENT from `v0.3` and `v0.4c` but PRESENT in `v0.4` — so those installs were
overwritten before/after `62f789c2` respectively, confirming Finding 1 by a second,
independent method. `MC2_ROAD_PBR` is ABSENT from every exe scanned, confirming the
tombstone verdict.

## 3 — the new reference lane

`A:/Games/mc2-opengl/releases/mc2-win64-refbuild-cf97ed52` — a name no prior evidence
references. `v0.3`, `v0.4`, `v0.4c` were **not** touched.

| Check | Result |
|---|---|
| `MC2_BUILD_GIT_SHA` | `cf97ed524df6` |
| `MC2_BUILD_GIT_DIRTY` | **0** |
| Build worktree | `A:/Games/mc2-refbuild`, `git status --porcelain -uno` empty before **and** after |
| Manifest `src_commit` | `cf97ed524df62ea13a49f7647daf1f6a1fcc539f`, 804 rows, `--require-clean` |
| md5 build == deployed | `a6bd4e8f87c474f031731fcaeebfd804` both sides |
| Gates present (`pe_gate_scan`) | MEANDER, CONCRETE_DETAIL_NORMAL, LIGHT_GRID, SHADOW_GUARD_CLAMPED_SUN, IBL_NIGHT_SCALE — all PRESENT; ROAD_PBR ABSENT (correct) |
| `[DEPLOY_FINGERPRINT]` | `OK: exe sha=cf97ed524df6 matches worktree HEAD` |

**Honest caveat: this exe is `cf97ed52`, which was HEAD when built but is no longer HEAD**
— four sibling commits landed during the lane. It is *clean and attributable*, which is
the property that was missing; it is **not** "current tip". Smoke it from
`A:/Games/mc2-refbuild` (whose HEAD is `cf97ed52`), or the fingerprint gate will
correctly reject it. It also **does not** contain this lane's own registry commit, which
is inert metadata (no behaviour).

**Disk method (reusable).** A full install is ~7 GB and only ~8.9 GB was free. I
hardlink-cloned (`cp -al`, 7 s, ~0 bytes) from `v0.4d-rc1`, then broke the hardlinks on
exactly the 807 manifest-listed payload files before deploying — because
`deploy_payload.py` uses `shutil.copyfile`, which **truncates in place** and would
otherwise have corrupted the seed install through the shared inodes. A before/after md5
canary on the seed confirms it is intact. Net cost ~0.2 GB.

## 4 — was `v0.3`'s "correct concrete" reconstructible? **Yes — and better than assumed**

**Yes.** And the mechanism is not the one the mission statement assumed.

- `v0.3`'s **`data/` is intact** (mtime `2026-07-01`). Only its `mc2.exe` and manifest
  were overwritten. Its asset payload — the thing that defined the reference — survives.
- The gate is **not** an asset load. At `gos_terrain_lod_chunk.cpp:2771`, gate-OFF sets
  `nb[3] = 0.0f` — a uniform component. Default OFF reproduces the approved look
  **exactly and by construction**, in any install, shipped assets or not.
- **Closing the audit's open question (its Finding 3 "placeholder vs flat fallback"):**
  the question is moot, and the census is sharper than reported. The relief needs
  `concrete_normal.ktx2`. `terrain_layers/` ships **albedo only** — six `*_albedo.ktx2`,
  **zero normals**. Every install checked (`v0.3`, `v0.4`, `v0.4c`, `v0.5.0`, the new
  reference lane) contains exactly **one** `*_normal.ktx2`, and it is `corrugatedsteel006a`
  — a prop texture, not a terrain layer. So the concrete relief cannot appear on any of
  them regardless of the gate. It is not "terrain_layers present vs absent" as
  Finding 3 framed it; the discriminator is the concrete **normal** map specifically.

So the new reference lane reproduces `v0.3`'s concrete by **two independent guarantees**:
asset absence, and gate-default-OFF. **What is genuinely lost** is `v0.3` as a *binary
artifact* — its original exe is gone and cannot be rebuilt (no tag/commit is recorded for
it). Its *look* is fully recoverable; its *identity* is not.

## 5 — `MC2_WATER_SHORE_MEANDER` registered

Was `registered: false` despite a default change — surviving `check-env-registry.sh` only
via that script's ALLOWLIST, the same shape as the CONE-SPILL-COUPLING-1 defect the
registry exists to catch. Added `MC2_FEATURE_WATER_SHORE_MEANDER` and the companion
`MC2_TUNE_WATER_SHORE_MEANDER_FREQ` to `kFeatureTable`, with the §1 measurement recorded
in the entry so the refuted byte-identity claim cannot be re-cited. `gate_status` now
reports `registered: true`, `default: OFF`, `flags: []`.

I then **removed both from the ALLOWLIST** and re-ran the checker: it no longer flags
them. That proves the registry — not the allowlist — is now carrying them.

## 6 (CORRECTION) — the harness count: the audit was **wrong**, 16/16 is right

The mission brief (from ADVERSARIAL-VERIFY-1) said harnesses are "12/12, not 16/16" and
asked me to correct the docs. **Do not make that correction — both numbers are correct
and they measure different things.** Verified by running the tools:

- `tools/build_contract_harnesses.py` builds **12** — `run_contract_tests.REGISTERED_HARNESSES`,
  the standalone C++ harnesses at `tools/<name>/`. `built 12/12` is a **build** count.
- `tools/run_contract_tests.py` runs **16** and reports
  `CONTRACT TESTS: PASS (16 harness(es), 0 not-passing)`. The extra four come from
  `PY_HARNESSES` (`run_contract_tests.py:53`): `deploy_asset_contract_harness`,
  `deploy_release_tree_contract_harness`, `mech_glb_pack_contract_harness`,
  `shader_variant_matrix_harness` — Python harnesses with no C++ build step, so
  `build_contract_harnesses.py` correctly never builds them.

`.claude/AUDIT-TRUTH-1-STATUS.md:228` already had this exactly right
(`16/16 PASS … (12/12 built, 0 failed)`). The audit read the builder's `12/12` and
concluded the runner's `16/16` was stale. **The lanes reporting 16/16 were right.**
No doc change made. Ran fresh: **16/16 PASS, 0 not-passing.**

## 7 — baselines and vendored poses

- `tests/visual/goldens/baseline-6c47e149/` — **10 vendored poses, and the honesty
  checker passes it clean.** It is **auditable**. It is *not* a third unauditable
  baseline, so there was nothing to fix here.
- `v0.5.1-baseline` and `v0.5.2-baseline-PROPOSED` — still **no `poses/` dir**, still
  UNAUDITABLE. `v0.5.2-baseline-PROPOSED` remains the set awaiting user promotion.
  **Do not promote it until its poses are vendored.** Unchanged from the audit; I did not
  fix these because the poses exist only inside deploy installs and recovering them is a
  judgement call about which install is authoritative — a user gate, not a lane call.

## 8 — contract failures in `mc2-main` are sibling-attributable, not mine

My diff touches one header and one shell allowlist. The three failures, run individually:

- `sampler_bindings` — 3 FAILs, **all** on hard-anchor `ssaoTex` now being multiplexed
  across `shaders/ssao_apply.frag` **and `shaders/ssao_blur.frag`** — the new shader from
  **SSAO-BLUR-1** (`be445b02`).
- `spirv_artifacts` — 4 FAILs, all `mech.vert.*: source drift — GLSL changed since bake`.
  I touched no shader. Needs `build_variants.py` rebake.
- `env_registry` — one var, `MC2_CEMENT_ATLAS_DUMP`, introduced by **CEMENT-ATLAS-CELL-2**
  `7c80c004`. My edit only *added* registrations and cannot cause this.

Flagged for those lanes; not fixed here (their claimed files).

---

## ADVERSARIAL SELF-REVIEW — what I could NOT verify

- **The `v0.3` original binary is unrecoverable and I did not prove what it contained.**
  My claim is about the *look* and rests on source (`nb[3]=0.0f`) plus an asset census —
  both solid. But I never ran the original `v0.3` exe, because it no longer exists. Anyone
  claiming the reference lane is pixel-identical to what the user graded in `v0.3` is
  overreaching, and so would I be. **I did not capture a concrete/runway pose A/B**; the
  gate is inert on every available install (no `concrete_normal.ktx2`), so such a capture
  would have shown zero and proven nothing about the relief path.
- **The meander refutation is `mc2_10` only, at three poses.** It refutes the specific
  claim made, which was about exactly these poses. It is **not** a general statement about
  every map, and I did not test `mc2_01`/`mc2_17`.
- **I did not re-measure any other lane's effect claims** — the 70.0%→2.9%, the
  200→≤20 lights, the p50 timings. They remain in the audit's "CONFIRMED (default only)"
  state. Only the meander claim was in my mandate and only it was measured.
- **`pe_gate_scan.py` proves a *string* is in the binary, not that the code path is
  reachable.** A gate whose literal is present may still be dead. It is the right tool for
  "was this build made after commit X" and the wrong tool for "does this gate do anything".
  Do not over-read it.
- **The `--list` mode's `MC2_*` extraction is heuristic** (printable-run scan). Named-needle
  lookups are exact byte searches and are the trustworthy mode; treat `--list` as
  indicative.
- **Contracts were run once, not three times.** They are deterministic checkers, so I
  consider that adequate — but it is not a floor measurement.
- **Attribution of the sibling contract failures is by inspection of the failure text**,
  which names their files explicitly. I did not bisect.

**Confidence:** high on §1 (a measured zero floor against a 4–10% signal, both arms
independently stable), §2, §3, §5, §6 (each rests on tool output reproduced here).
High on §4's mechanism and census; medium on §4's "matches what the user approved",
which is an inference from source and assets, not an observation of the lost binary.
