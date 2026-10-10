<!-- doc-status: stale | reviewed:2026-10-11 -->
# Adversarial consistency review (2026-10-09, HEAD 3df9e4c2d, read-only)

Ranked by how badly each would mislead the next agent.

1. memory/project-next-tooling-modernization-plan.md ("URGENT next session" line): says ~560 false "(no longer exists)" tags exist and "Do NOT trust these tags". Tags were since stripped (my path+gate sweep found 0 false path tags, 1 false gate tag). Fix: delete the URGENT sentence about tags; keep "Sonnet adversarial review" pending note only if still wanted.
2. memory/visual_fidelity_roadmap.md:57: `MC2_STATIC_PROP_IBL_SH=0 (gate since removed)` is FALSE. Gate is live: RenderCore/RendererFeatureRegistry.h:124-128,501 (default ON, =0 uploads strength 0), debug_state_dump.cpp:491, GameOS/gameos/gos_static_prop_batcher_state.cpp:299 "AUTHORITATIVE GATE". Fix: drop the tag.
3. memory/MEMORY.md stale/format: "HEAD c030c3e0c" but HEAD is 3df9e4c2d (12 commits later; greens "at 95ad18b8a"). "~142 default-off Feature gates" is unverifiable (flags.auto.yaml: 295 default off, all kinds). The "Mission data" must-read bullet is glued onto the end of the "Build & deploy" line (missing newline) so it renders as one bullet. 3 body files are linked from 2 INDEX files (deploy-fingerprint-stale-exe-gotcha, feedback_smoke_duration, feedback_smoke_gate_no_menu_canary) violating the "exactly ONE" rule. 73 `[[wikilinks]]` resolve to nothing and 46 differ only by -/_ (pre-existing, not new).
4. .claude/VK-PARITY-GAPS.md (newly tracked in 073de8a9f) is a 2026-10-07 recon with no "superseded" banner; items 2,3,7 (mech shading, terrain ambient floor, water shore) have since landed per MEMORY (4f076ce91, 9d04c9a2a, dd68d7c5c). Unindexed. Fix: add a banner "recon 10-07; see HANDOFF-2026-10-08b for what landed".
5. docs/README.md:157 indexes docs/build-parallel-and-tooling.md (2026-06-30 six deploy dirs, multi-worktree map) with no stale marker; contradicts CLAUDE.md ONE lane. BUILD-WIN.md/CONTRIBUTING.md still describe PR flow and positional `deploy_payload.py <install-dir>` (works; --target game also works).
6. .claude/skills/mc2-deploy.md:19 says `--target editor` defaults to 0.4c; code (deploy_payload.py:1190-1199,1221) says editor = v0.4. Lines 85-93 do manual `cp` of exe/dll, contradicting BUILD-WIN.md "never copy by hand".
7. docs/flags.auto.yaml is stale: check-flag-registry-drift.sh reports DRIFT (406 changed lines, all `gameos_graphics.cpp:NNNN` line refs shifted by c47bc6892 deleting 9 lines). The script is advisory-only (always exit 0) so nothing fails. Fix: `py -3 scripts/flag-registry-auto-gen.py` and commit.

## Checkers (run now)
- check-gate-default-agreement: OK, 0 disagree. Blind spot: 109 of 362 registry env vars have no reader in scanned source (SCAN_DIRS C++ only; python/shader readers unseen) so they can never fail.
- cxx17_ratchet --check: OK. check-spirv-artifacts: PASS but covers only 16 pilot artifacts (none of the shaders edited in this pass).
- comment_tidy: report-only, no exit gate, not in check-contracts.sh; 2601 findings (CT002 dead-path 235, CT003 stale-line-ref 1409, CT009 lane-era 400, CT001 dead-env 0).
- flag-registry-drift: see 7.

## Commits since 7ce84ce8d
- No hidden code changes found in c47bc6892 / bf22d58a1 / 1722c8ede: only comments and one log-string edit (bf22d58a1 changes the "GPU particles" log message text; harmless, but a log string, not a comment). c47bc6892 edited shaders gpu_driven_terrain_solid.comp and terrain_lod_chunk.frag (comments only; neither is baked, check ok). mech.vert drift revert d27a64521 is correct.
- Meaning-changing comment: c47bc6892 rewrote `ambientProbeAtPassBegin ... // runs before order gate` dropping the gate name; fine.

## Clean areas
- MEMORY.md/must-read links all resolve; VK shadow stride c26b1eb2a and CSM unflipped 2fc06b5a9 facts match code (vulkan_shadow_island_live.cpp:1487) and HANDOFF-b.
- Runtime default GL is consistent (ARCHITECTURE-MAP.md:47, MEMORY.md); ONE-lane / "work in mc2-main" consistent across CLAUDE.md, AGENTS.md, ARCHITECTURE-MAP.md:95, MEMORY.md; worktree list matches.
- CLAUDE.md paths/tools exist; deploy_payload --target game, run_smoke --tier/--duration/--keep-logs/--deploy, ab.py --help all work. Remaining "(removed)" tags in .claude/agents and skills (fitini, fst, tglpp, widget, shapes/, water.vert) verified truly absent.
- C++23 plan exists in memory (project-cpp17-to-cpp23-goal) but CLAUDE.md "Code rules" still says C++17 only (consistent with the build, just not mentioning the plan).
