# Docs vs code consistency audit (2026-10-09, read-only)

Method: parsed RenderCore/RendererFeatureRegistry.h with scripts/registry_parse.py (361 env-var rows: Feature 60 ON / 142 OFF, Trace 3/153, Retired 1/2), grepped all docs/*.md, docs/*/*.md, CLAUDE.md, AGENTS.md, .claude/*.md for "MC2_X ... ON/OFF" claims, compared to registry and to the getenv reader. Ran scripts/check-gate-default-agreement.py (green: 229 agree, 0 disagree - but it only checks registry vs reader vs docs/tier1_env_vars.md + in-source comments, NOT other docs), scripts/check-flag-registry-drift.sh (STALE), scripts/check-doc-links.py (0 broken in live docs).

Ranking: P1 = an agent will act on a false default; P2 = wastes time; P3 = cosmetic.

## 0. Systemic findings

- P1 docs/flags.auto.yaml is STALE vs registry/source: `check-flag-registry-drift.sh` reports drift (118 changed lines: e.g. in_smoke_passthrough no->yes, shifted referenced_at line numbers, SHORE3 knobs). Fix: `py -3 scripts/flag-registry-auto-gen.py` and commit. Also for UNREGISTERED env vars the yaml says `default: unknown` (738 `registered: no`, e.g. MC2_GPU_PARTICLES, MC2_TERRAIN_INDIRECT, MC2_TERRAIN_SOLID_NARROW, MC2_DISABLE_GOSFX, MC2_SHADOW_MAP_SIZE, MC2_RENDER_SNAPSHOT, MC2_TERRAIN_INDIRECT_OVERLAY); `unknown` is not "off". Agents should read the reader (see truth table). 651 yaml rows `documented_in_tier1_env_vars: no`.
- P1 No checker covers default claims outside docs/tier1_env_vars.md. Fix (one change, prevents recurrence): extend check-gate-default-agreement.py scan list to docs/*.md and .claude/*.md, or delete the stale prose (below). Per CLAUDE.md "every new check replaces one" - prefer deleting prose.
- P2 Many docs cite `memory/<name>.md` (known_issues.md 38,76,88,107,126,130,132,136,140,144; ARCHITECTURE-MAP.md 73 `INDEX-MECH.md`, 91 `HANDOFF-2026-10-08b-vk-parity.md`). Repo `memory/` has only 3 HANDOFF files; the rest live on the maintainer machine (~/.claude/projects/...). Agents in a fresh checkout cannot open them. Fix: in docs/README.md add one line "memory/*.md refs = maintainer-machine memory, not in repo", or leave.

## 1. Truth table (code reader vs what docs say)

| Var | Truth (reader / registry) | Where |
|---|---|---|
| MC2_GPU_PARTICLES | OFF; unset => disabled. NOT in registry (allowlisted check-env-registry.sh:292 "promote-to-registry") | mclib/particles/batcher.cpp:80-90 |
| MC2_VFX_ORACLE_RENDER | OFF (`v && v[0]=='1'`); registry false | batcher.cpp:137-141 (comment at 127-133 above it is ALSO stale: says "flipped to default-ON", "Kill-switch =0") ; Registry.h:549-552 |
| MC2_TERRAIN_NORMALS_FROM_HEIGHT | OFF | Registry.h:516-520; tier1_env_vars.md:772 |
| MC2_SMART_LOAD | ON (`!v => true`) | code/logisticsdata.cpp:44-49; Registry.h ~3423 true |
| MC2_SHADOW_STATIC_BANDFIX_V2 | ON, `=0` kills (unregistered) | GameOS/gameos/gameos_graphics.cpp:261-265 |
| MC2_SHADOW_CSM, MC2_SHADOW_DYNAMIC_PROP_CASTERS, MC2_TERRAIN_LOD_CHUNK, MC2_COLORMAP_KTX2 | ON | registry |
| MC2_VFX_SOFT/LIT_PARTICLES, MC2_TERRAIN_LIGHTING_V1/V2, MC2_VK_VFX, MC2_VK_SHARED_GBUFFER | OFF | registry |

## 2. Findings by file (batchable fixes)

### docs/render-pipeline-map.md (P1, most-read doc)
- L99: `MC2_TERRAIN_NORMALS_FROM_HEIGHT` (ON) -> (OFF). Also `MC2_TERRAIN_INDIRECT`/`SOLID_NARROW` are unregistered; reader gos_terrain_indirect.cpp:106 - leave ON only if verified.
- L260: "**Status: GPU particle pipeline is DEFAULT** (`MC2_GPU_PARTICLES` ON)" -> "GPU particle pipeline is OPT-IN (`MC2_GPU_PARTICLES=1`); default is legacy CPU/MLR (reverted 2026-06-15 4ad31a9e)".
- L287: `MC2_GPU_PARTICLES` (ON), `MC2_VFX_ORACLE_RENDER` (ON) -> both (OFF).

### docs/vfx-oracle-coverage.md (P1)
- L11-14 table: "`MC2_VFX_ORACLE_RENDER` default-ON" -> "default-OFF (opt-in =1)".
- L37-38: "=0 disables ..." -> these are no-ops by default; ON requires =1.
- L67: "default-ON since VFX-WEAPON-FX-RESTORE-OPUS-1. Kill-switch =0 ... logs reason=default_on" -> "default-OFF (reverted); logs reason=default_off / opt_in". L71 "Gate-OFF byte-identical" is fine.
- Cleaner fix: add a 3-line banner at top "STATUS: all GPU oracle paths are default-OFF since 4ad31a9e; text below describes the opt-in path" instead of editing every row.

### docs/gosfx-animation-lifecycle-recon.md
- L52: "`MC2_VFX_ORACLE_RENDER` default-ON (`batcher.cpp:130`)" -> "default-OFF (`batcher.cpp:137-141`)". (P2; also the line number is off.)

### mclib/particles/batcher.cpp (code comment, P2 - misleads grep readers)
- L127-133: comment says default-ON/Kill-switch; code at 137-141 is opt-in. Fix comment to "Default OFF; =1 enables (reverted)".

### docs/active_campaigns.md
- L146: "Gate `MC2_SMART_LOAD=1` default OFF" -> "default ON (`=0` restores eager init); reader code/logisticsdata.cpp:44-49". P1 (wrong direction).
- L227: "Kill-switch MC2_STATIC_PROP_LIVE_BUILDER=1" - registry lists default false (consistent: =1 re-enables old live builder). OK, no change; just ambiguous wording "Kill-switch" -> "Re-enable switch".

### docs/engine-closure-audit.md
- L34, L202, L260: `MC2_GPU_PARTICLES=0` "master"/"Legacy CPU FX only" presented as the way to disable; default is already off. L245 MC2_SNAPSHOT_MECH_EXTRACT off matches registry (false); the false-positive in my sweep, no change. Fix L34/202/260: "master (default OFF; =1 enables GPU)".

### docs/known_issues.md (CLAUDE.md-linked; file header L62-64 says "remove fixed ones outright, don't append FIXED")
- L45: struck-through "Shadow stutter ... FIXED, stale entry" -> DELETE (dup of L74). L74 itself also "FIXED" -> DELETE (e9b9aea7 shipped 2026-05-16).
- L46 and L75: duplicates; both say `MC2_SHADOW_STATIC_BANDFIX_V2` "default-OFF, awaiting user field check". Code gameos_graphics.cpp:261-265 is default ON (killswitch =0), docs/tier1_env_vars.md:244 agrees. Fix: delete L75, change L46 to "banding fixed by BANDFIX_V2 (default ON, =0 reverts)" or delete both.
- L47 + L88 (water shoreline z-fight): duplicate; L88 cites a memory file not in repo. Git evidence of fixes after the entry: 37557c8b6 WATER-BIAS-LINEARIZE-1 (2026-07-18, "far z-fight cure"), 8ab474091 WATER-FAR-BAND-ATTRIB-1, a8ea67265 SHORELINE-STAIRCASE-1, 3af9c6737/0e701f763 shore mode 3 (2026-10-09). Needs a user field check before delete; merge to one bullet "mitigated, see commits".
- L48 + L121 (first-launch black terrain): L121 itself says NOT REPRODUCED (c5570f40d). Merge to one bullet, drop it from "Player-visible".
- L49 + L136 (options 4K resolution): duplicate, L136 says MITIGATED by HUD-RES-CLAMP. Delete L49 (keep L136).
- L51: GPU gosFX reverted default-off: ACCURATE (matches code). Keep; this is the doc the others contradict - cross-link from render-pipeline-map.
- L153: "gosFX dev-override broken ... `MC2_DISABLE_GOSFX=0`" cites `mlrclipper.cpp:206-209`; real path is mclib/mlr/mlrclipper.cpp. Warning still emitted at GameOS/gameos/gameosmain.cpp:1333-1342 so entry is live; fix the path only (P3).
- L74 ref `memory/...`, L38 `memory/debt_generated_map_pak_convergence.md`, L76/L88/L107/L126/L130/L132/L136/L140/L144 and L74 `.claude/DEFERRED-1-STATUS.md` (does not exist): remove the dangling `.claude/DEFERRED-1-STATUS.md` ref (L74), others per section 0.
- L78 (CSM default-ON) and L91 (LOD chunk default-ON) accurate.

### .claude/VK-ARC-STATE.md (P2/P3)
- L936 `MC2_VK_VFX` "default-ON under umbrella", L1106 `MC2_VK_SHARED_GBUFFER` sub-gates "default ON under" umbrella: registry has VK_VFX/SHARED_GBUFFER defaultOn=false because the true default is "ON only when umbrella=1"; consistent in intent, but a reader sees `false`. Add "(registry false = umbrella-dependent)" - optional.
- .claude/VK-PARITY-GAPS.md:24 `MC2_TERRAIN_POM` - table cell, registry false; verify wording ("PARTIAL") before editing; P3.
- .claude/LIGHTING-HEADROOM-AUDIT.md:317 historical ("was default-OFF"); ignore.

### docs/dev-velocity.md and docs/disciplines.md (P1: describe removed multi-lane system)
CLAUDE.md (2026-10-05 reset): ONE lane, no parallel worktrees/agents.
- docs/dev-velocity.md whole file (L1-83): "standing up agent lanes", `tools/spawn_lane.py` (still exists), `.claude/DEPLOY-DIR-LANE-MAP.md` (L62, DOES NOT EXIST), `.smoke_leases.json` (L74), "Never --no-lease" (L78). Contradicts CLAUDE.md. Fix: delete file (and its docs/README.md index entry if any) or prepend "SUPERSEDED by CLAUDE.md Operating mode; do not spawn lanes". Code still has the lease system (scripts/run_smoke.py:578-581 `--deploy`, `--no-lease`, scripts/smoke_lib/deploy_lease.py), so with one lane `--no-lease` is harmless; CLAUDE.md's smoke command uses `--deploy 0.4`, valid.
- docs/disciplines.md L9, L25-28, L115, L127-133: "No lane may report...", `list_lanes`, `check_conflict`, `commit-plan` (multiplexed dirty tree). Tools exist (tools/repo_intel/lane_registry.py, repo_query.py) but are moot with one lane. Fix: delete the lane-claim checklist lines (L25-26) and the "multi-lane dirty tree / commit-plan" section (~L125-135), keep bookend-review rule only if still wanted.
- docs/deploy-isolation.md:22 `.mc2_deploy_lane.json`, L35 "one lane destroyed mc2-win64-v0.4": historical rationale, mostly fine; docs/README.md:26 phrase "free lane dir" -> "free dir".
- docs/ARCHITECTURE-MAP.md:82 and .claude/skills/mc2-build.md:14, CLAUDE.md:30 reference `bootstrap_worktree_build.py` - script exists and CLAUDE.md uses it for "fresh tree"; fine.
- .claude/agents/*.md, .claude/agents/DOMAINS.md: not audited in depth (16 expert files); likely describe lanes - spot check recommended.

### CLAUDE.md
- L9 "~149 legacy default-OFF gates": registry has 142 Feature rows default-OFF (+153 Trace). Change to "~142" or drop number. P3.
- L7/L17-30 paths and commands: all verified present (scripts/deploy_payload.py --target game, bootstrap_worktree_build.py --build, run_smoke.py --tier/--duration/--keep-logs/--deploy, build_contract_harnesses.py --run --unit, tools/visual_verify/ab.py --bookmarks/--crop/--env/--compare-only, repo_query.py preflight/env --domain/--all/binding --conflicts, configure_analysis.py, check-contracts.sh, scripts/tests/). No broken command found.
- L28 `scripts/tests/` and L43 smoke ok. L57 `tools/repo_intel/gate_status.py`, `dead_gate_scan.py` exist.
- AGENTS.md:16 `debug_state/latest_render_state.json` is a runtime-generated path (not in repo) - fine.

### docs/README.md, docs/ARCHITECTURE-MAP.md
- All linked docs exist (check-doc-links: 0 broken live). ARCHITECTURE-MAP basenames (gos_*.cpp, vulkan_*_island.cpp, etc.) all exist under GameOS/gameos/ - no stale file refs found. Only dangling: L73 `INDEX-MECH.md` and L91 `HANDOFF-2026-10-08b-vk-parity.md` (memory, not in repo; see section 0). L68 ".tesc/.tese/.vert" is a fragment, not a path.

### docs/tier1_env_vars.md
- Defaults all agree with the registry per the checker (318 claims, 0 contradictions). Line 834 `MC2_VFX_ORACLE_RENDER` Default OFF and 772 NORMALS_FROM_HEIGHT Default OFF are the correct ones - this is the file other docs should defer to. MC2_GPU_PARTICLES has no entry in it; add one line "Default OFF; =1 enables GPU billboards" (P2).

## 3. Suggested batch order
1. Regenerate docs/flags.auto.yaml (one command).
2. One edit pass: render-pipeline-map (3 lines), vfx-oracle-coverage banner + L11-14/37/67, active_campaigns L146, gosfx-animation-lifecycle-recon L52, batcher.cpp comment L127-133, engine-closure-audit 3 lines.
3. known_issues.md dedupe (delete L45, L49, L74, L75, merge L47/L88 and L48/L121), fix L46, L153 path.
4. Delete or banner docs/dev-velocity.md; trim docs/disciplines.md lane sections.
5. Add MC2_GPU_PARTICLES to RendererFeatureRegistry.h (default false) so the yaml and checker cover it instead of `unknown`.
