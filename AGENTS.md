# AGENTS.md — guide for non-Claude agents (Codex, etc.)

**Read [CLAUDE.md](CLAUDE.md) first — it is canonical.** This file only maps
Claude-specific machinery to plain commands.

- Work only in the worktree and branch your ASSIGNMENT names; one writer per worktree.
  The root checkout `A:/Games/mc2-opengl-src` is stale.
- Local commits only; never push.

| CLAUDE.md says | You do |
|---|---|
| build | `py -3 scripts/ninja_build.py` |
| deploy | `py -3 scripts/deploy_payload.py <assigned install> --ninja --allow-new-target` (commit first; the install is a positional argument, never v0.4) |
| smoke | `py -3 scripts/run_smoke.py --lite --keep-logs --exe <assigned install>/mc2.exe`, or `py -3 tools/witness_smoke.py --install <dir> --mission mc2_01 --expect "<regex>"` |
| repo state | `py -3 tools/repo_intel/repo_query.py preflight --expect-branch <assigned branch> --expect-root <assigned worktree>` |
| render state | read `debug_state/latest_render_state.json` in the install after running with `MC2_DEBUG_STATE_DUMP=1` |
| proof / expectations | what the assignment asks for, proved offline first (CLAUDE.md "Proof") |

Other tools: `tools/menu_oracle.py` (needs the user's visible desktop), `tools/list_free_ordinals.py`
(free compbas ordinals), `tools/mc2x_import/fst.py` (FST extraction).

Rules: RelWithDebInfo only. Features ship default-ON or not at all (gates are temporary kill-switches).
Offline checks (`scripts/check-contracts.sh`, doctest tier) green before engine changes land.
Mod installs in `A:/Games/mc2-opengl/` must keep working.
