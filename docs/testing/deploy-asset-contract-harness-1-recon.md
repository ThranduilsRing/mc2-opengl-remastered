# DEPLOY-ASSET-CONTRACT-HARNESS-1-RECON

**Status:** RECON ONLY — no code changes. **Verdict: GREEN** (filesystem-only,
fast, deterministic) — implement as a **Python** harness (first non-C++ harness).
**Branch / worktree:** `claude/deploy-asset-contract-harness-1` @
`A:/Games/mc2-deploy-asset-harness`, off nifty `d55c90d8`.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## Recon questions — answered

### What declares the deploy payload? — `scripts/deploy_payload.py` (the single source of truth).
Module-level constants (src_root-relative unless noted):
| Constant | Contents | Location relative to | In repo? |
|---|---|---|---|
| `SUPPORT_SCRIPTS` | `run-mc2.bat`, `run-with-log.bat`, `run-editor.bat` | src_root | ✅ |
| `EDITOR_SUPPORT_TREES` | `tools/terrain_gen` | src_root (dir) | ✅ |
| `GAME_COOK_TOOLS` | `tools/cook_bc6h_hdri.py`, `tools/examples` | src_root | ✅ |
| `BUILDING_PBR_PAYLOAD` | 7× `data/tgl/*`, `data/materials/pbr/*` | src_root | ✅ (spot-checked HangarGLB.glb, corrugatedsteel006a_orm.ktx2) |
| `FFMPEG_DLLS` | 5× `av*/sw*.dll` | **build_dir** | ❌ build artifact |
| `LAUNCHER_*`, exe, pdb | mc2-launcher / mc2.exe | **build64/build_dir** | ❌ build artifact |
| shaders | enumerated from `src_root/shaders` | src_root | ✅ (already covered by shader harness) |

`.deployed_manifest.csv` is a **per-install runtime artifact** written at deploy
time — NOT a source contract. No other importer of these constants exists
(`docs/asset-pipeline.md` is separate prose inventory).

### Can it be filesystem-only like the shader harness? — **YES**, for the src_root-relative payload.
`SUPPORT_SCRIPTS` + `EDITOR_SUPPORT_TREES` + `GAME_COOK_TOOLS` +
`BUILDING_PBR_PAYLOAD` are all source-tracked files/dirs whose existence is a
pure filesystem check. No GL, no game, no deploy execution.

### Removed/stale paths? — minor finding.
deploy_payload.py hardcodes the **current** target preset
`A:/Games/mc2-opengl/mc2-win64-v0.4` (lines 599/614/622/625). Per MEMORY this is
the *correct* current target (v0.4, NOT v0.4c). A contract test can assert "no
`v0.4c` references survive" (the old wrong target) — cheap regression guard.

### Can the harness call the real source of truth, not duplicate it? — **YES, but only if the harness is Python.**
This is the key design point. The payload lists are Python constants. A C++
harness would have to **re-declare** them (fake-green: the C++ copy silently
drifts from deploy_payload.py) or scrape the .py (fragile). The clean
no-fake-green path is a **Python harness that `import`s `deploy_payload` and reads
its actual constants** — the test always reflects the real deploy contract.

## Design fork (needs a GO before implementing)

The arc so far is C++ harnesses (`contract_harness.h`) discovered as exes by
`run_contract_tests.py`. The deploy-asset contract's source of truth is Python,
so the no-fake-green harness must be Python. Proposed **additive, tools-only**
extension (no production touch, no C++ harness change):

1. **`tools/contract_harness_common/contract_harness.py`** — a small Python
   mirror of the C++ framework: a `Harness` with `add(name, fn, in_default)`,
   CLI `--list/--test/--json/--seed`, exit 0/1/2, and the SAME JSON shape
   (`{harness, tests[], status, elapsed_ms}`). Reusable for future Python
   harnesses.
2. **`tools/deploy_asset_contract_harness/deploy_asset_contract_harness.py`** —
   `import`s `scripts/deploy_payload.py`, validates its real constants against the
   filesystem.
3. **`tools/run_contract_tests.py`** — add a `PY_HARNESSES` registry (or a typed
   entry) invoked via `py -3 <path> --json`, parsed with the same JSON contract.
   C++ exe discovery unchanged.

This keeps every arc invariant: no production change, no root CMake change,
filesystem-only, explicit registry, framework-owns-stdout (Python harness writes
diagnostics to stderr), demo/failure tests excluded from default suite.

## Candidate tests (if GO)

- `support_scripts_exist` — every `SUPPORT_SCRIPTS` entry exists under src_root.
- `editor_support_trees_exist` — every `EDITOR_SUPPORT_TREES` dir exists.
- `game_cook_tools_exist` — every `GAME_COOK_TOOLS` file/dir exists.
- `building_pbr_payload_exists` — all 7 `BUILDING_PBR_PAYLOAD` assets exist.
- `target_presets_allowlisted` — every `TARGET_PRESETS` target dir is a
  `DEPLOY_ALLOWLIST` member, and the `game` preset does not point at the
  editor/`0.4c` install. (Was `no_stale_v04c_target`, a whole-file grep for
  `v0.4c`; retired in CONTRACT-HARNESS-REPAIR-1 because it could not
  distinguish a legitimate allowlist entry from a mis-pointed preset.)
- (demo, inDefault=false) `missing_payload_detected` — assert a bogus path is
  flagged, proving the detection path.
- **Out of scope:** `FFMPEG_DLLS`, launcher, exe — build artifacts, absent in a
  clean checkout. A test could check them only when `--build-dir` is supplied;
  default suite skips them (and `log()`s the skip — no silent caps).

## Why this is the right target
Opposite of the FIT-PARSE verdict: no subsystem to stand up, no over-stubbing,
no production touch. A pure filesystem contract over the real deploy manifest —
directly prevents "deploy references a moved/renamed/removed asset" surprises
that otherwise only surface at deploy/smoke time.

## Recon hygiene
Recon ran against current nifty HEAD `d55c90d8` (not a stale base). Per the
recurring lesson, verified the source-of-truth file and asset presence live, not
from memory.
