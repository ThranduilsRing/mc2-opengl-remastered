# MC2 OpenGL/Vulkan — mc2-main (canonical, branch `claude/nifty-mendeleev`)

MechCommander 2 engine port: modern GL renderer transitioning to Vulkan, PBR terrain, shadows, ImGui UI, mod support.

## Operating mode (reset 2026-10-05 — read this first)

- **ONE lane.** All work happens here in `A:/Games/mc2-main`. No parallel worktrees, no parallel coding agents. Sessions may *launch* from `A:/Games/mc2-opengl-src` (root, stale) — never build or commit there.
- **Basics over process.** Every new rule, check or doc must replace one. A check red on nifty for more than a week gets fixed or deleted.
- **Features ship ON or not at all.** An `MC2_*` gate is a temporary kill-switch, not a parking spot. No new default-OFF feature gates. (Cleanup of the ~149 legacy default-OFF gates is in progress.)
- **Vulkan is the destination backend.** GL becomes removable once VK is complete.
- **Mods must keep working:** MC2X-CVE-G, MC2X-REAP3, Exodus, MagicExpansion, MechCommanderOmnitech, MCO-Carver5 (installs in `A:/Games/mc2-opengl/`).
- **Commit locally only.** Never push, open PRs, or upload unless the user explicitly says so.
- **Offline verification first** (below); launch the game only when an offline check can't answer the question.

## Paths

- Source + build: `A:/Games/mc2-main/` → `build64/RelWithDebInfo/mc2.exe`
- Dev install (deploy target): `A:/Games/mc2-opengl/mc2-win64-v0.4/` · Golden: `mc2-win64-v0.4c/` (don't overwrite)
- Archive of old lanes, installs, worktrees, ledgers, plans: `E:/mc2-archive-2026-10/` (also git history)
- CMake: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Commit → build → deploy → verify

```bash
# build (RelWithDebInfo only; fresh tree: py -3 scripts/bootstrap_worktree_build.py --build)
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 mc2_launcher
# deploy (sha256-verified; exe fingerprint embeds git sha, so COMMIT FIRST)
py -3 scripts/deploy_payload.py --target game
```

Offline checks (no game launch) — run these first, all should be green:

```bash
bash scripts/check-contracts.sh                       # ~150 static contract rows
py -3 tools/build_contract_harnesses.py --run --unit  # C++ harnesses + doctest tier
py -3 -B -m pytest scripts/tests/ -q -p no:cacheprovider
```

Smoke (launches the game off-screen, ~3 min):

```bash
MC2_DEBUG_STATE_DUMP=1 MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs --deploy 0.4
```

`tier1` = mc2_01/03/10/17/24. Evidence is the `[DEPLOY_FINGERPRINT] OK: exe sha=… matches worktree HEAD` line plus 5/5 PASS. Don't set `MC2_DIAGNOSTIC_TRACE_FILE` (the runner owns it and refuses the run).

## Where things are

- `docs/README.md` — docs index · `docs/tier1_env_vars.md` — `MC2_*` knobs · `docs/known_issues.md` · `docs/cxx17-coding-rules.md` · `docs/GLOSSARY.md`
- Registry of gates: `RenderCore/RendererFeatureRegistry.h` (generated view: `docs/flags.auto.yaml`)
- Design docs still live: `docs/superpowers/strategy/` (mod VFS, data ownership)
- Memory (maintainer machine): `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`

## Code rules

- C++17, MSVC RelWithDebInfo. Full relink after class-layout changes.
- Tracy zones: 100ns floor, none in per-element hot loops.
- No emoji in code or logs.
