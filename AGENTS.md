# AGENTS.md — harness guide for non-Claude agents (Codex, etc.)

**Read [CLAUDE.md](CLAUDE.md) in this directory FIRST — it is canonical for paths,
build, deploy, smoke, architecture, and rules.** This file only translates the
parts of CLAUDE.md that assume Claude Code machinery you don't have, and lists
the deterministic tools that replace it. Do not re-derive procedure that exists
here; do not invent alternatives to the canonical commands.

## Where you are

- Canonical source worktree: `A:/Games/mc2-main/`
  (branch `claude/nifty-mendeleev`; `main` is kept pointed at its tip).
- The ROOT repo checkout (`A:/Games/mc2-opengl-src/`) is a coordination shell —
  its `build64/` is STALE; do not build or edit there.
- Other `A:/Games/mc2-*` directories are parallel lane worktrees owned by other
  sessions. Stay out unless your task names one.
- Verify before editing: `git -C <worktree> branch --show-current` must match
  what your task expects. Never share a worktree with another concurrent agent.

## Translations (CLAUDE.md says → you do)

| CLAUDE.md says | You do |
|---|---|
| use `/mc2-build` skill | `py -3 tools/build_quiet.py --target mc2` (wraps the canonical VS CMake invocation; errors + warning-count + exit only). Full CMake path if you need it is in CLAUDE.md. |
| MCP `preflight()` / repo-intel tools | `py -3 tools/repo_intel/repo_query.py preflight --expect-branch <branch> --expect-root <abs-worktree>` (same verdicts, CLI). |
| MCP render-state tools | read `debug_state/latest_render_state.json` in the install dir after running with `MC2_DEBUG_STATE_DUMP=1`. |
| smoke gate (canonical command) | copy it VERBATIM from CLAUDE.md. Never `--kill-existing`, never `--with-menu-canary`, duration ≤ 30. Or use `witness_smoke.py` below, which wraps it correctly. |
| deploy via `deploy_payload.py` | same — it is a plain script. Never manual copies; shaders AND `data/defs/` ship in lockstep with the exe (deploy does this; do not deploy an exe any other way). |

## Deterministic toolbelt (prefer these over improvising)

Documented in `docs/disciplines.md` § "Agent-efficiency tools":

- `py -3 tools/build_quiet.py --target mc2` — quiet build verdict.
- `py -3 tools/witness_smoke.py --install <dir> [--deploy] --mission mc2_01 --expect "<regex>" [--forbid "<regex>"]` — deploy+smoke+log-grep → single PASS/FAIL.
- `py -3 tools/menu_oracle.py --exe <mc2.exe> --env MC2_NO_LAUNCHER=1` — is the main menu visibly rendering (JSON verdict). Sparse text menus can read mean<1 while visible.
- `py -3 tools/list_free_ordinals.py --compbas <csv-or-install-dir>` — free compbas equipment ordinals per install.
- `tools/mc2x_import/fst.py` — FST archive extraction (engine-faithful LZW + zlib).
- `py -3 tools/repo_intel/repo_query.py slice-preflight --slice NAME --symbols "a,b" --paths p1 --base SHA` — MANDATORY before coding a recon-derived fix; `verdict=STOP` means re-recon, do not code.

## Hard rules (subset of CLAUDE.md that agents most often break)

- RelWithDebInfo only. Full relink on class-layout changes.
- Every feature behind an `MC2_*` env gate, default OFF, gate-OFF byte-identical to stock.
- Offline doctest suite first: `cmake -S tests/unit -B build64-tests -G "Visual Studio 17 2022" -A x64`, build target `mc2_tests`, run `build64-tests/RelWithDebInfo/mc2_tests.exe` — must be 0 failed before engine work lands.
- Deploy lanes are owned. Ask/check before using any install dir; `A:/Games/mc2-opengl/mc2-win64-v0.4` is the user's personal install — read-only.
- No pushes. Local commits only, `Co-Authored-By` trailer per repo convention.
- Mod file changes in an install need `mods/<id>/.modindex-cache` deleted (or rely on the mtime signature, post `59f16541`).

## Memory / docs map

Session memory lives at `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md`
(read-only useful context; INDEX-*.md sub-indexes per topic). Repo docs topic tree is in CLAUDE.md.

<!-- sync-check: derived from CLAUDE.md; scripts/check-agents-md.sh verifies the
     pointer contract. If CLAUDE.md gains/renames canonical commands, update the
     Translations table here. Keep this file under 100 lines. -->
