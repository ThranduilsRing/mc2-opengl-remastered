# AGENTS.md — guide for non-Claude agents (Codex, etc.)

**Read [CLAUDE.md](CLAUDE.md) first — it is canonical.** This file only maps
Claude-specific machinery to plain commands.

- Work only in `A:/Games/mc2-main` (branch `claude/nifty-mendeleev`). ONE lane: never run
  alongside another coding agent. The root checkout `A:/Games/mc2-opengl-src` is stale.
- Local commits only; never push.

| CLAUDE.md says | You do |
|---|---|
| build | `py -3 scripts/ninja_build.py mc2` |
| deploy | `py -3 scripts/deploy_payload.py --target game` (commit first) |
| smoke | copy the command from CLAUDE.md verbatim, or `py -3 tools/witness_smoke.py --install <dir> --mission mc2_01 --expect "<regex>"` |
| repo state | `py -3 tools/repo_intel/repo_query.py preflight --expect-branch claude/nifty-mendeleev --expect-root A:/Games/mc2-main` |
| render state | read `debug_state/latest_render_state.json` in the install after running with `MC2_DEBUG_STATE_DUMP=1` |

Other tools: `tools/menu_oracle.py` (needs the user's visible desktop), `tools/list_free_ordinals.py`
(free compbas ordinals), `tools/mc2x_import/fst.py` (FST extraction), `scripts/run_smoke.py`.

Rules: RelWithDebInfo only. Features ship default-ON or not at all (gates are temporary kill-switches).
Offline checks (`scripts/check-contracts.sh`, doctest tier) green before engine changes land.
Mod installs in `A:/Games/mc2-opengl/` must keep working.
