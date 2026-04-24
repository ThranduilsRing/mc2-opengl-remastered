# ASan Autonomous Run Log — 2026-04-24

Running log for the autonomous batch started by user. One line per task
boundary, findings summarized inline, blockers called out explicitly.
Detailed ASan reports committed separately in `docs/asan-follow-ups.md`.

## Context

User dispatched autonomous ASan runs over four mod-content archives
plus a tier1 base-game matrix. Parallel session is working on the
`FunctionCallbackTable` stub registration that is expected to resolve the
Exodus `getCodeToken` AV (finding #3). That work is not in this session.

## Targets (user-specified order)

1. Brain Library (`A:/Games/Magic_MC2_archive/1. Brain Library`) → corebrain.abx + sample brain ABLs
2. Gamesys (`A:/Games/Magic_MC2_archive/2. Gamesys`) → gamesys.fit
3. Magic's Unofficial Expansion (`A:/Games/Magic_MC2_archive/Sarna.net/magicsunofficialexpansion`) → full data tree, deploy to a clean install
4. Unofficial MC2 Patch by Magic (`A:/Games/Magic_MC2_archive/Unofficial_MechCommander_2_Patch_by_Magic_files`)
5. Tier1 matrix under ASan on mc2-win64-v0.1.1 (mc2_01/03/10/17/24)

## Prior baseline (already known)

- `mc2-win64-v0.1.1/` + mc2_01, 45s: **clean pass**, zero ASan reports.
- `Carver5-feasibility/` + mc2_01, 60s: **AV in GlobalMap::clearPathExistsTable** (finding #2).
- `MC2-Exodus/` + mc2_01, 60s: **AV in getCodeToken** (finding #3).

Each mission gives one ASan report: MSVC ASan treats AVs as
non-recoverable and terminates the process, so we sample rather than
exhaust per install.

## Log

### Task 1 — Brain Library on mc2-win64-v0.1.1 (base game)
- Deploy: `corebrain.abx` → `data/missions/`, 8 sample ABLs → `data/missions/warriors/`.
- Run: `--mission mc2_01 --duration 45`. **PASS**, `result=pass`, 2127 frames, 47 FPS, zero ASan reports.
- Note: sample-brain ABLs had parser errors on `coreattacktactic` /
  `tacticstate` vocabulary (same parser-gap class as finding #3) but
  since base-game units don't invoke those broken scripts, the VM
  never dereferences the corrupt bytecode.

### Task 2 — Gamesys.fit on same install
- Deploy: `gamesys.fit` → `data/missions/`.
- Run: same mission, 45 s. **PASS**, 2272 frames, 50.5 FPS, zero ASan
  reports. Gamesys is a stat-tuning FIT; no new code paths exercised.

### Task 3a — Magic's Unofficial Expansion (fresh clone)
- Install: `MC2-MagicExpansion/` = fresh base clone (no corebrain /
  gamesys) + Expansion `data/` overlay (~56 `.fit` missions).
- Run: `--mission mc2_01 --duration 45`. **FAIL**, exit 1.
- Finding: duplicate of #3 (`getCodeToken` AV, pointer `0x12d900000001`).
  ASan log: `asan-magicexp-mc2_01.15908`.

### Task 3b — Magic's Unofficial Expansion overlaid on mc2-win64-v0.1.1
- Rationale: per user correction after task 3a — test Expansion on an
  install that already has corebrain + gamesys to match the way a
  player would actually layer content.
- Overlay: Expansion `data/` over `mc2-win64-v0.1.1/data/`.
- Run: mc2_01, 30 s. **FAIL**, exit 1.
- Finding: same as task 3a — duplicate of #3 (pointer `0x11c600000001`).
  ASan log: `asan-exp-overlay-mc2_01.10484`.

### Task 4 — Unofficial Patch archive
- **Blocker.** `A:/Games/Magic_MC2_archive/Unofficial_MechCommander_2_Patch_by_Magic_files/`
  contains only saved-webpage support files (JavaScript, CSS). The
  actual patch content is split across the numbered folders
  `3. Zoom Out` / `4. Weapons` / `5. Mechs` / `6. Mechlab` / `7. Carver V`
  / `8. FixABL` in the archive root — not tested in this batch.
- User notes `8. FixABL` will be needed to repair mod ABL scripts;
  parked for a future run.

### Task 5 — Tier1 matrix on mc2-win64-v0.1.1 (base)
- Config: `py -3 scripts/run_smoke.py --tier tier1 --duration 30
  --continue --kill-existing --exe <asan-exe>`.
- Result: **2 / 5 pass** (`mc2_01`, `mc2_17`).
- Failures and findings:
  - `mc2_03` → **finding #4** (`Turret::update` global-buffer-overflow,
    neutral turret teamId = -1 indexes `turretsEnabled[-1]`).
  - `mc2_10` → **finding #5** (ABL `getCodeToken` heap-buffer-overflow,
    31-byte segment, `MechWarrior::checkAlarms` path).
  - `mc2_24` → **finding #6** (same as #5 but 1533-byte segment,
    `MechWarrior::runBrain` path — same root cause).

## Summary

Six findings total (including the pre-existing three from earlier
this session):

| # | Mission  | Install | Severity | Class |
|---|----------|---------|----------|-------|
| 1 | _(visual)_ | any ASan run | P2 | FBO / post-process UB |
| 2 | mc2_01 | Carver5-feasibility | P1 | GlobalMap partial-init |
| 3 | mc2_01 | Exodus + Magic Expansion (×2) | P1 | ABL compile-fail fall-through |
| 4 | mc2_03 | mc2-win64-v0.1.1 base | **P0** | `Turret::update` OOB |
| 5 | mc2_10 | mc2-win64-v0.1.1 base | **P0** | ABL code-segment 1-byte overread |
| 6 | mc2_24 | mc2-win64-v0.1.1 base | **P0** | same as #5 (different path) |

Triage and proposed fixes: `docs/asan-triage.md`.
Per-finding detail: `docs/asan-follow-ups.md`.

**Headline:** three P0 bugs in stock base-game content, two of them
in the ABL VM (same root cause, 1 line fix), one in turret AI (1 line
fix). All have been latent since MC2 1.0 — ASan's memory-layout change
surfaced them in a single tier1 run.
