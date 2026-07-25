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
- **★ SUBAGENT BOOKEND REVIEWS (standing user rule, mandatory).** Every dispatched subagent runs a **META REVIEW before starting** (read gate defaults from READER SOURCE, check what a DEFAULT LAUNCH executes, check for `-dirty` evidence / orphaned work / live lane claims / superseding commits, and **spot-check the brief's own premises** — report corrections, don't build on a wrong foundation) and an **ADVERSARIAL SELF-REVIEW before reporting done** (chiefly: *would the user see this on a default launch?* — a fix behind an unapproved default-OFF gate is **NOT delivered**; plus gate registered + smoke-allowlisted, clean-fingerprint md5-verified build, gate string actually present in the deployed binary, verified where the defect can manifest, no user approval regressed, and state residual risk + what you did NOT do). Full checklist: `docs/disciplines.md`.
- **★ STANDING CAPTURE RULE (visual grading).** The verdict shot is a **FULL-FRAME wide view at normal gameplay altitude**; crops are detail only. ≥3 distinct locations per map including a **control area the fix should not touch**; **day AND night**; across **mc2_01/mc2_03/mc2_10/mc2_24**. See `docs/visual-verify.md`.

Lane → root map (illustrative — ground truth is `git worktree list`): canonical (branch claude/nifty-mendeleev)=`A:/Games/mc2-main` · other lanes live under `A:/Games/mc2-opengl-src/.claude/worktrees/<name>`

## Topic tree

`docs/README.md` — docs index (start here) · `docs/critical_inline_rules.md` — emoji ban, build/deploy, shaders/GL, discipline · `docs/tier1_env_vars.md` — MC2_* knobs + gate-adding checklist · `docs/known_issues.md` · `docs/active_campaigns.md` — in-flight ledger · `docs/renderworld_arc_status.md` — M1–M6 steady-state ledger (arc closed 2026-05-24; GameAdapters seams remain live) · `docs/disciplines.md` — review/dispatch protocol · `docs/cxx17-coding-rules.md` · `docs/asset-pipeline.md` — canonical asset inventory · `docs/GLOSSARY.md` — jargon (nifty, lane, slice, …) · `~/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` (maintainer machine only) → INDEX-{RENDERING,SHADERS,TERRAIN,MECH,BUILD-DEPLOY,MISSION-DATA,SMOKE-TEST,PROCESS}.md

## Orientation

- Project direction: `.planning/PROJECT.md` · Codebase maps: `.planning/codebase/ARCHITECTURE.md`
- Advisor routing: `.claude/agents/DOMAINS.md` (12 subagents) · Skills: `.claude/skills/`
- Steering: `A:/Games/mc2-opengl-src/.claude/STEERING.md` (transient — created on demand by `steer.sh`, deleted on ack; `sh .claude/steer.sh "..."` blocks next action)
- MCP-first: `get_render_health()` → `get_latest_smoke_report()` → `get_diagnostic_events(tag,50)`. Requires engine running with `MC2_DEBUG_STATE_DUMP=1`.

## Key paths

- Source: `A:/Games/mc2-main/`
- Build: `A:/Games/mc2-main/build64/` — root `build64/` is STALE
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.4/` — user's personal install; agents deploy to a leased lane, not here
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
