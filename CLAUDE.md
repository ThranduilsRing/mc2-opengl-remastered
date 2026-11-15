# MC2 Vulkan — mc2-main (canonical, branch `claude/nifty-mendeleev`)

MechCommander 2 engine port: Vulkan-only engine renderer, PBR terrain, shadows, ImGui UI, mod support.

## Operating mode (invariants; every new rule replaces one)

- **Lane rules (every agent, every model):** `A:/Games/mc2-ops/lane-rules.md`. Read it first. Parallelism: `A:/Games/mc2-ops/JOBS`.
- **Parallel lanes** work in their own worktree (a `tools/lane_slot.py` slot), one writer per worktree. Merges happen only in merge windows, into the active play branch when one exists (now `claude/play-1113`; `claude/nifty-mendeleev` is fast-forwarded to the gated play tip) (`tools/lane_merge.py --batch`), each followed by an adversarial review. **Architecture first:** asks the owner adds mid-session go on the backlog (memory `process/project-architecture-backlog-2026-10-27.md`). Never stop or deprioritize running architecture lanes for them unless the owner says it's urgent.
- **Model/effort:** Sol medium lanes and merges, Astra medium reviews, Sonnet medium as fallback; details in the runbook Standing rules (`A:/Games/mc2-ops/orchestrator-runbook-sonnet.md`). Avoid Haiku for build loops.
- **Game runs / perf:** defaults, off-screen rule and perf routing are in lane-rules; benches use `--acceptance` only in the orchestrator's quiet window.
- **Acceptance:** visual changes per lane-rules section 7 (montage opened by you, MAD is not a pass signal). "Delivered" means present and rendering in the owner's install. A second failed proof of the same class goes to the owner as a question, not to another continuation (orchestrator; lanes take the safe default).
- **Basics over process.** A check red on nifty for more than a week gets fixed or deleted.
- **Features ship ON or not at all.** An `MC2_*` gate is a temporary kill-switch, not a parking spot (legacy gates: `docs/flags.auto.yaml`).
- **Vulkan only.** The GL backend is removed (`MC2_RENDER_BACKEND=gl` is ignored with a `[RENDER_BACKEND]` log line); don't add GL paths or GL A/B arms.
- **Mods:** nightly slim non-blocking `mods-slim` check; a break gets a dedicated fix lane; details in lane-rules and `docs/merge-queue-v2.md`.
- **Lanes commit locally only.** Only the orchestrator pushes: ff `claude/nifty-mendeleev` -> origin main after a promotion. No PRs, releases or uploads unless the user explicitly says so. `README.md` holds owner edits: never touch, stage or commit it; commit by explicit path, never `-a`.

## Paths

- Source: `A:/Games/mc2-main/` → dev build `build64-ninja/mc2.exe` (Ninja). VS release/fallback tree and CMake path: `docs/repo-intel.md`.
- Owner's play install and gate target: `A:/Games/mc2-opengl/mc2-win64-v0.4/` (no deploy or game run there while the owner plays) · Golden: `mc2-win64-v0.4c/` (never overwrite)
- Archive of old lanes, installs, ledgers, plans and gate proofs: `E:/mc2-archive-2026-10/` (also git history)

## Build → deploy

```bash
py -3 scripts/ninja_build.py                     # dev build (fresh tree: py -3 scripts/configure_ninja.py)
py -3 tools/lane_slot.py acquire --branch <name> [--base <ref>] [--reset-branch] [--no-warm]   # existing branch keeps its tip; release <slot> [--discard] | status
py -3 scripts/ninja_build.py --warm --base <ref> -j3   # after a base switch: detach at <ref>, build in the background (log build64-ninja/warm.log)
py -3 scripts/deploy_payload.py <assigned install> --ninja --allow-new-target   # lane; sha256-verified; COMMIT FIRST
py -3 scripts/deploy_payload.py --target game --ninja                         # mc2-main / merge gate only (v0.4)
```

## Proof

Proof ladder (offline first), lane proof rules and default run lengths: lane-rules section 7. Merge candidate and schedule run the full board:

```bash
bash scripts/check-contracts.sh                                # full board (~50-90 s)
py -3 tools/build_contract_harnesses.py --run --unit           # C++ harnesses + doctest tier
py -3 -B -m pytest scripts/tests/ -q -p no:cacheprovider -n auto   # full suite, parallel (xdist; cap workers: PYTEST_XDIST_AUTO_NUM_WORKERS=<JOBS>)
```

**Runtime change:** one smoke-lite on your own install, `py -3 scripts/run_smoke.py --lite --keep-logs --exe <install>/mc2.exe` (mc2_10 night + mc2_01 day, 10 s each, all gates incl. shader_error and the deploy fingerprint `[DEPLOY_FINGERPRINT] OK: exe sha=… matches worktree HEAD`). None for docs/tests/tools/scripts. Full tier1, the gate tiers (per merge / `--nightly` / `--weekly`) and the nightly schedule: `docs/merge-queue-v2.md`.

## Where things are

- `docs/README.md` (docs index) · `docs/tier1_env_vars.md` (`MC2_*` knobs) · `docs/known_issues.md` · `docs/cxx17-coding-rules.md` · `docs/GLOSSARY.md`
- Gate registry: `RenderCore/RendererFeatureRegistry.h` (generated view: `docs/flags.auto.yaml`) · live design docs: `docs/superpowers/strategy/`
- Repo intel (rg, clangd, graphify, `repo_query.py`, `ab.py`): `docs/repo-intel.md`
- Memory (maintainer machine): orchestrator rules in `~/.claude/projects/a--Games-mc2-main/memory/MEMORY.md` (auto-loaded); domain root causes in `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`. Before domain work, read its "Topic sub-indexes" table and the matching `INDEX-*.md`; past root causes live there.

## Code rules

- C++23 (`MC2_CXX_STANDARD=23`, MSVC `/std:c++latest`), RelWithDebInfo. Full relink after class-layout changes.
- Tracy zones: 100ns floor, none in per-element hot loops.
- No emoji in code or logs.
