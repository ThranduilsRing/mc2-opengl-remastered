# MC2 OpenGL — mc2-main (canonical worktree, branch claude/nifty-mendeleev)

MC2 OpenGL port: tessellated terrain, PBR splatting, shadow maps, post-processing. Active branch `claude/nifty-mendeleev` (0.4 gpu-driven-rendering arc merged 2026-05-18). Root `terrain-pbr-mod` = STALE.

## Mandatory preflight (run before any edit)

```powershell
py -3 tools\repo_intel\repo_query.py preflight --expect-branch <branch> --expect-root <absolute-worktree-root>
```

- `branch_ok=false` or `root_ok=false`: **stop immediately**. Do not switch branches, stash, or repair.
- `safe_to_touch=false`: report the PRECHECK line and elevated dirty files, wait for explicit direction.
- Never share a physical worktree between parallel agents. A separate branch is not sufficient isolation.
- **Recon-derived fix slice?** Also run `repo_query.py slice-preflight --base <ref> --slice <NAME> --paths <files> --symbols <syms>` first. `verdict=STOP` (symbol changed on base..nifty) → re-recon, do NOT code. Mandatory for shared-recon-queue work. See `docs/disciplines.md`.

Lane → root map: canonical (branch claude/nifty-mendeleev)=`A:/Games/mc2-main` · renderpass-contract-3=`…/worktrees/renderpass-contract-3` · veg-schema-dense=`…/worktrees/veg-schema-dense`

## Topic tree

`docs/critical_inline_rules.md` — emoji ban, build/deploy, shaders/GL, discipline · `docs/tier1_env_vars.md` — MC2_* knobs · `docs/known_issues.md` · `docs/active_campaigns.md` — in-flight ledger · `docs/renderworld_arc_status.md` — M1–M6 · `docs/asset-pipeline.md` — canonical asset inventory · `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` → INDEX-{RENDERING,SHADERS,TERRAIN,MECH,BUILD-DEPLOY,MISSION-DATA,SMOKE-TEST,PROCESS}.md

## Orientation

- Project direction: `.planning/PROJECT.md` · Codebase maps: `.planning/codebase/ARCHITECTURE.md`
- Advisor routing: `.claude/agents/DOMAINS.md` (12 subagents) · Skills: `.claude/skills/`
- Steering: `A:/Games/mc2-opengl-src/.claude/STEERING.md` (`sh .claude/steer.sh "..."` blocks next action)
- MCP-first: `get_render_health()` → `get_latest_smoke_report()` → `get_diagnostic_events(tag,50)`. Requires engine running with `MC2_DEBUG_STATE_DUMP=1`.

## Key paths

- Source: `A:/Games/mc2-main/`
- Build: `A:/Games/mc2-main/build64/` — root `build64/` is STALE
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.4/`
- CMake: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`

## Codex build/deploy rails

- Build only with `/mc2-build`: explicit VS CMake, `--build build64 --config RelWithDebInfo --target mc2`.
- Deploy only with `scripts/deploy_payload.py` + explicit `--source-root --build-dir --exe-name`.
- Smoke only with `scripts/run_smoke.py`, `--keep-logs`, duration `<=30`, no `--kill-existing`.
- Never: direct MSBuild, random CMake, stamp touching, symlinks, manual release copies.

## Smoke gate

**Canonical invocation (verbatim; subagents must copy-paste):**

```powershell
$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-main\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs
```

`tier1` = mc2_01/03/10/17/24, 30s each. Inner-loop: `--mission mc2_01 --mission mc2_24`. Exit 0 = pass.

## Profiling

Tracy (`TRACY_ENABLE`). **100ns floor** — no per-element/per-vertex zones in hot loops. AMD RGP via Radeon Developer Panel.

## Memory & CLAUDE.md discipline

- No session narratives in CLAUDE.md. Root CLAUDE.md = thin pointer only (`scripts/check-claude-md-pointer.sh`).
- New finding → memory file + INDEX-TOPIC.md. Keep under 100 lines; growth → extract to `docs/`.

## graphify

Graph at `graphify-out/`. Use `graphify query/path/explain` before raw grep. `graphify update .` after code changes.
