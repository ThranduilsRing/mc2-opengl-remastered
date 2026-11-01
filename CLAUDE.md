# MC2 OpenGL/Vulkan — mc2-main (canonical, branch `claude/nifty-mendeleev`)

MechCommander 2 engine port: modern GL renderer transitioning to Vulkan, PBR terrain, shadows, ImGui UI, mod support.

## Operating mode (invariants; every new rule replaces one)

- **Lane rules (every agent, every model):** `A:/Games/mc2-ops/lane-rules.md` — read it first. Parallelism: `A:/Games/mc2-ops/JOBS`.
- **Parallel lanes** live in separate `A:/Games/mc2-*` worktrees. Merges into `claude/nifty-mendeleev` happen only through merge windows (`tools/lane_merge.py --batch`), each followed by an adversarial review. **Architecture first:** asks the owner adds mid-session go on the backlog (memory `process/project-architecture-backlog-2026-10-27.md`); never stop or deprioritize running architecture lanes (rewrite phases, registries, retirements) to do them at once unless the owner says it's urgent.
- **Deploy:** lanes deploy ONLY to their own `E:/mc2-installs/<branch>` with an explicit target. The merge gate's deploy to v0.4 (mc2-main's default target, orchestrator only) is the ONLY exception to the lane rule "NEVER deploy to v0.4".
- **Model/effort:** Sonnet coding = medium; merges with heavy conflicts, reviews and root-causing = high; Sol = medium; Opus case by case; avoid Haiku for build loops.
- **Game runs** default to about 15 s; benches use `--acceptance` only in the orchestrator's quiet window.
- **GPU performance work needs an RGP capture.** Any change claimed to help GPU time (draws, culling, shaders, barriers, uploads) is proven with a Radeon GPU Profiler capture (`.rgp`) before and after, taken at the corner-pinned full zoom-out worst-case pose (mc2_01 `cross_map_view`, mc2_03 `scout_*`, mc2_10/24 `wolfman`) and at the default pose, not only an fps/CPU A/B. Suite: `C:/Users/Joe/Downloads/RadeonDeveloperToolSuite-2026-02-02-1757` (Radeon Developer Service headless + Panel, see `tools/host_inventory.py`). Attach the `.rgp` paths to the lane report; capture tooling is `rgp_capture.py` once it lands.
- **Basics over process.** A check red on nifty for more than a week gets fixed or deleted.
- **Features ship ON or not at all.** An `MC2_*` gate is a temporary kill-switch, not a parking spot. No new default-OFF feature gates (count legacy ones from `docs/flags.auto.yaml`).
- **Vulkan is the destination backend.** GL becomes removable once VK is complete.
- **Mods must keep working:** MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech, MCO-Carver5 (installs in `A:/Games/mc2-opengl/`).
- **Commit locally only.** Never push, open PRs, or upload unless the user explicitly says so.
- **Offline verification first** (below); launch the game only when an offline check can't answer the question.

## Paths

- Source + build: `A:/Games/mc2-main/` → dev build `build64-ninja/mc2.exe` (Ninja); release/fallback `build64/RelWithDebInfo/mc2.exe` (VS)
- Dev install (deploy target): `A:/Games/mc2-opengl/mc2-win64-v0.4/` · Golden: `mc2-win64-v0.4c/` (don't overwrite)
- Archive of old lanes, installs, worktrees, ledgers, plans: `E:/mc2-archive-2026-10/` (also git history)
- CMake: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Commit → build → deploy → verify

```bash
# dev build = Ninja tree (exact header deps, no stale-binary traps; fresh tree: py -3 scripts/configure_ninja.py)
py -3 scripts/ninja_build.py
# lane prep after a base switch: detach at <ref> and build the tree in the background (no daemon; log in build64-ninja/warm.log)
py -3 scripts/ninja_build.py --warm --base <ref> -j3
# deploy, mc2-main / merge gate ONLY (default v0.4); a lane passes its own E:/mc2-installs/<branch> + --allow-new-target. sha256-verified; COMMIT FIRST
py -3 scripts/deploy_payload.py --target game --ninja
# release / fallback = VS tree (fresh tree: py -3 scripts/bootstrap_worktree_build.py --build)
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2   # deploy without --ninja
```

Offline checks (no game launch) — run these first, all should be green:

```bash
bash scripts/check-contracts.sh                       # ~157 static contract rows, parallel (~50-90 s); --serial / -jN to override
py -3 tools/build_contract_harnesses.py --run --unit  # C++ harnesses + doctest tier
py -3 -B -m pytest scripts/tests/ -q -p no:cacheprovider
```

Smoke (mc2-main / merge-gate commands; a lane deploys to its own install, see Operating mode), **default inner-loop check = smoke-lite** (mc2_10 night + mc2_01 day, 10 s each, all gates incl. shader_error + deploy fingerprint; reduced soak coverage):

```bash
py -3 scripts/run_smoke.py --lite --keep-logs --deploy 0.4
```

Run full tier1 (below) before milestones / landing, not on every iteration. Captures (`run_visual_capture.py`, `ab.py`) now exit as soon as the PNG+sidecar are complete; the duration is only a timeout.

Full smoke, merge window only (launches the game off-screen, ~3 min; the runner's 15 s default duration applies):

```bash
MC2_DEBUG_STATE_DUMP=1 MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE py -3 scripts/run_smoke.py --tier tier1 --keep-logs --deploy 0.4
```

The default env runs the Vulkan backend (default since 2026-10-17); there is no GL opt-out any more (`MC2_RENDER_BACKEND=gl` is ignored with a `[RENDER_BACKEND]` log line). `tier1` = mc2_01/03/10/17/24. Evidence is the `[DEPLOY_FINGERPRINT] OK: exe sha=… matches worktree HEAD` line plus 5/5 PASS. Don't set `MC2_DIAGNOSTIC_TRACE_FILE` (the runner owns it and refuses the run).

## Where things are

- `docs/README.md` — docs index · `docs/tier1_env_vars.md` — `MC2_*` knobs · `docs/known_issues.md` · `docs/cxx17-coding-rules.md` · `docs/GLOSSARY.md`
- Registry of gates: `RenderCore/RendererFeatureRegistry.h` (generated view: `docs/flags.auto.yaml`)
- Design docs still live: `docs/superpowers/strategy/` (mod VFS, data ownership)
- Memory (maintainer machine): `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` — before domain work, read MEMORY.md's "Must-read per domain" list and the matching `INDEX-*.md`; past root causes live there.

## Repo intel tools (use before grepping blind)

Route by question: literal string/env key/filename → `rg`; C++ declaration/references/types/overloads → clangd (LSP plugin; compile DB from `py -3 scripts/configure_analysis.py`, re-run after CMake changes); how subsystems connect → graphify; gates/env/bindings/slices → `repo_query.py`. "Zero references" = deletion *candidate* only (check data/scripts/other configs).

- `py -3 tools/repo_intel/repo_query.py preflight` — session-start sanity check
- `py -3 tools/repo_intel/repo_query.py env --domain <shadow|terrain|...>` / `env --all` — which `MC2_*` vars exist, defaults, read sites
- `py -3 tools/repo_intel/repo_query.py binding --conflicts` / `binding <N>` — shader/GL binding-slot index
- `tools/repo_intel/grep_tool.py` — `grep_source()` / `symbol_lookup()` (import; lexical, not AST)
- `tools/repo_intel/gate_status.py`, `dead_gate_scan.py` — gate defaults / dead-gate candidates (gate amnesty)
- `py -3 tools/visual_verify/ab.py --bookmarks a,b (--install-a DIR --install-b DIR | --exe-a EXE --exe-b EXE [--install DIR]) [--crop x0,y0,x1,y1] [--env K=V]` — VK-only before/after (two builds; the engine has no GL arm): montage/crops/diff heatmap + MAD table (`--compare-only DIR_A DIR_B` reuses captures). Always A/B a day AND a night map (mc2_01_saved_205644, mc2_10_saved_204523). MAD is NOT a pass signal: mc2_10 with every building missing scored MAD 0.62 — look at the montage.
- graphify (AST-only, owned code; graph at `graphify-out/graph.json`, report `graphify-out/GRAPH_REPORT.md`): `graphify query "<question>"`, `graphify path "A" "B"`, `graphify explain "gosRenderer"` (run from repo root; community labels are generic, 1798 communities).
- Rebuild: file list in `graphify-out/.scoped_files.json` (GameOS mclib code RenderCore GuiRuntime shaders scripts tools); `graphify update` rescans whole root incl. 3rdparty, so re-run the scoped script instead.
- Use for call-graph/coupling questions (who calls X, cross-dir hubs); INFERRED edges are heuristic, verify with grep.
- Subagent briefs: name the relevant memory files + these tools up front.

## Code rules

- C++23 (`MC2_CXX_STANDARD=23`, MSVC `/std:c++latest`), RelWithDebInfo. Full relink after class-layout changes.
- Tracy zones: 100ns floor, none in per-element hot loops.
- No emoji in code or logs.
