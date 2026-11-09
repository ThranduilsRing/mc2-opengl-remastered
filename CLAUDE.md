# MC2 Vulkan — mc2-main (canonical, branch `claude/nifty-mendeleev`)

MechCommander 2 engine port: Vulkan-only engine renderer, PBR terrain, shadows, ImGui UI, mod support.

## Operating mode (invariants; every new rule replaces one)

- **Lane rules (every agent, every model):** `A:/Games/mc2-ops/lane-rules.md`. Read it first. Parallelism: `A:/Games/mc2-ops/JOBS`.
- **Parallel lanes** work in their own worktree (a `tools/lane_slot.py` slot), one writer per worktree. Merges into `claude/nifty-mendeleev` happen only in merge windows (`tools/lane_merge.py --batch`), each followed by an adversarial review. **Architecture first:** asks the owner adds mid-session go on the backlog (memory `process/project-architecture-backlog-2026-10-27.md`). Never stop or deprioritize running architecture lanes for them unless the owner says it's urgent.
- **Deploy:** a lane deploys only to the install its assignment names (default `E:/mc2-installs/<branch>`, which `lane_slot.py release` deletes), always with an explicit target. Only the orchestrator's merge gate deploys to v0.4.
- **Model/effort:** Sonnet coding = medium; heavy-conflict merges, reviews and root-causing = high; Sol = medium; Opus case by case; avoid Haiku for build loops.
- **Game runs** default to about 15 s; benches use `--acceptance` only in the orchestrator's quiet window. Lanes do not measure ms on the PC; perf pairs go to the farm laptop (`tools/farm/host.py submit`). RGP is a weekly orchestrator checkpoint, never a per-change proof.
- **Acceptance:** accept a visual change only after opening the exact-tip montage yourself and writing what you saw (MAD is not a pass signal). "Delivered" means present and rendering in the owner's install. A brief quotes the owner's symptom verbatim and adds no requirements. A second failed proof of the same class goes to the owner as a question, not to another continuation.
- **Basics over process.** A check red on nifty for more than a week gets fixed or deleted.
- **Features ship ON or not at all.** An `MC2_*` gate is a temporary kill-switch, not a parking spot. No new default-OFF feature gates (count legacy ones from `docs/flags.auto.yaml`).
- **Vulkan only.** The GL backend is removed (`MC2_RENDER_BACKEND=gl` is ignored with a `[RENDER_BACKEND]` log line); don't add GL paths or GL A/B arms.
- **Mods** (MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech, MCO-Carver5; installs in `A:/Games/mc2-opengl/`): the nightly gate runs a slim, non-blocking `mods-slim` check (one short load per mod, PASS/FAIL per mod in the summary, never fails the gate). A break gets a dedicated fix lane. The full battery runs weekly, and per merge only when the diff touches mod content (`data/`, `mods/`, `examples/mods/`, `tests/mods/`, mod tooling). See `docs/merge-queue-v2.md`.
- **Commit locally only.** Never push, open PRs, or upload unless the user explicitly says so.

## Paths

- Source: `A:/Games/mc2-main/` → dev build `build64-ninja/mc2.exe` (Ninja). VS release/fallback tree and CMake path: `docs/repo-intel.md`.
- Dev install (merge-gate target): `A:/Games/mc2-opengl/mc2-win64-v0.4/` · Golden: `mc2-win64-v0.4c/` (never overwrite)
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

**Offline first** (owner 2026-11-08): proof starts with a unit test, harness, pytest or static check; write a quick new test when none covers the change. Launch the game or capture only for what pixels or runtime alone can show.

| Change | Offline proof |
|---|---|
| Docs | `git diff --check` |
| Shader | `bash scripts/check-contracts.sh --rows 'slang\|shader\|vk_shaders'` + production shader compile |
| Tooling (Python/scripts) | targeted pytest (`py -3 scripts/run_scripts_tests.py scripts/tests/test_<x>.py ...`) + the owning row via `--rows` |
| Data / art | the loader/validator row; `kit_art` only when art paths changed |
| C++ engine | covering rows + `py -3 tools/build_contract_harnesses.py --run --unit --only <test>` |

```bash
bash scripts/check-contracts.sh                                # full board (~50-90 s): merge candidate and schedule
py -3 tools/build_contract_harnesses.py --run --unit           # C++ harnesses + doctest tier
py -3 -B -m pytest scripts/tests/ -q -p no:cacheprovider       # full suite: merge candidate and schedule
```

Generated inventories and count ratchets are not lane proof: the merge commit regenerates them once, and `log_tag_census` fails only on a tag a Python parser consumes.

**Runtime change:** one smoke-lite on your own install, `py -3 scripts/run_smoke.py --lite --keep-logs --exe <install>/mc2.exe` (mc2_10 night + mc2_01 day, 10 s each, all gates incl. shader_error and the deploy fingerprint `[DEPLOY_FINGERPRINT] OK: exe sha=… matches worktree HEAD`). None for docs/tests/tools/scripts. Full tier1, the gate tiers (per merge / `--nightly` / `--weekly`) and the nightly schedule: `docs/merge-queue-v2.md`.

## Where things are

- `docs/README.md` (docs index) · `docs/tier1_env_vars.md` (`MC2_*` knobs) · `docs/known_issues.md` · `docs/cxx17-coding-rules.md` · `docs/GLOSSARY.md`
- Gate registry: `RenderCore/RendererFeatureRegistry.h` (generated view: `docs/flags.auto.yaml`) · live design docs: `docs/superpowers/strategy/`
- Repo intel (rg, clangd, graphify, `repo_query.py`, `ab.py`): `docs/repo-intel.md`
- Memory (maintainer machine): `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`. Before domain work, read its "Topic sub-indexes" table and the matching `INDEX-*.md`; past root causes live there.

## Code rules

- C++23 (`MC2_CXX_STANDARD=23`, MSVC `/std:c++latest`), RelWithDebInfo. Full relink after class-layout changes.
- Tracy zones: 100ns floor, none in per-element hot loops.
- No emoji in code or logs.
