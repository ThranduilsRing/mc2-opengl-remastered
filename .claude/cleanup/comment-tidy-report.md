# comment-tidy report (report-only run)

Tool: `tools/comment_tidy/comment_tidy.py` (README alongside; tests: `test_comment_tidy.py`, 9 pass).
Run: 2025 files in owned dirs, 2680 findings, ~10 s. No source file was modified; `--apply` was not run (dry-run `--fix`: 1409 refs in 230 files).

## Counts per check per directory

| Check | GameOS | mclib | code | RenderCore | GuiRuntime | shaders | scripts | tools | Total |
|---|---|---|---|---|---|---|---|---|---|
| CT001 | 22 | 14 | 6 | 1 | 0 | 3 | 1 | 2 | 49 |
| CT002 | 82 | 60 | 18 | 5 | 0 | 38 | 15 | 17 | 235 |
| CT003 | 321 | 396 | 115 | 42 | 1 | 238 | 88 | 208 | 1409 |
| CT004 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| CT005 | 28 | 22 | 32 | 5 | 0 | 1 | 0 | 0 | 88 |
| CT006 | 29 | 125 | 170 | 0 | 0 | 4 | 0 | 2 | 330 |
| CT007 | 6 | 3 | 1 | 1 | 0 | 5 | 2 | 0 | 18 |
| CT008 | 37 | 29 | 24 | 0 | 0 | 2 | 0 | 0 | 92 |
| CT009 | 39 | 33 | 5 | 19 | 4 | 2 | 228 | 71 | 401 |
| CT010 | 27 | 6 | 10 | 3 | 0 | 2 | 6 | 4 | 58 |

## Examples (3 per check, spread across dirs)

### CT001 (49)
- `code/weapon_presentation_proof.cpp:24` env/gate MC2_EQUIP_PRESENTATION_PROOF not found in code or registry (comment say -- `weapon. (The throwaway MC2_EQUIP_PRESENTATION_PROOF parity-proof gate was`
- `RenderCore/ambient_contract.h:46` env/gate MC2_AMBIENT_PROBE not found in code or registry -- `*** blend is NOT a per-pass axis in THIS engine: the runtime probe (MC2_AMBIENT_PROBE)`
- `mclib/mech3d.cpp:4582` env/gate MC2_GPU_MECH_SHADOW_SKIP not found in code or registry (comment says it -- `MECH-KILLSWITCH-SHADOW-PAIR-RETIRE-1: former MC2_GPU_MECH_SHADOW_SKIP retired.`

### CT002 (235)
- `tools/mod_install/parity_smoke.py:199` path data/tgl/foo.ini does not exist -- `records (e.g. "data/tgl/foo.ini") while the resolver emits absolute`
- `scripts/check-vfx-no-objectid.sh:21` path docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md -- `Resolutions: docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md (Q1 confirms)`
- `GameOS/gameos/gameos_graphics.cpp:7698` path memory/terrain_mvp_gl_false.md does not exist -- `GL_FALSE is correct for terrainMVP (memory/terrain_mvp_gl_false.md) —`

### CT003 (1409)
- `code/missiongui.cpp:1523` line-number ref objmgr.cpp:1348-1360 will rot -- `arrays -- objmgr.cpp:1348-1360 increments numTerrainObjects /`
- `GameOS/gameos/gos_terrain_lod_chunk.cpp:386` line-number ref GpuBufferOwner.h:15 will rot -- `(the "LATER slice" flagged in GpuBufferOwner.h:15). The wrapper's ops mirror`
- `shaders/gos_terrain_lighting.comp:227` line-number ref quad.cpp:1317-1320 will rot -- `quad.cpp:1317-1320: lightning`

### CT004 (0)

### CT005 (88)
- `RenderCore/frame_executor.h:12` function _validation_failures() not found in code -- `extern "C" mc2_framegraph_executor_owned_passes() / _validation_failures().`
- `GameOS/gameos/gl_state_guard.h:151` function endXxxPass() not found in code -- `change clip semantics, or defensively in endXxxPass() to re-assert the scene`
- `code/missiongui.cpp:1430` function ForCapture() not found in code -- `ForCapture() deliberately clears lookTargetObject.`

### CT006 (330)
- `code/warrior.cpp:1379` 5 consecutive lines look like commented-out code -- `moveOrders[which].zone.angle[0] = 45.0;`
- `GameOS/gameos/gos_render.cpp:738` 6 consecutive lines look like commented-out code -- `status = SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &value);`
- `mclib/ablrtn.cpp:878` 5 consecutive lines look like commented-out code -- `bufferOffset = 0;`

### CT007 (18)
- `RenderCore/postprocess_subgraph.h:2` 32-line comment, 17/28 lines history/ticket-heavy; condense to 1-2 lines + doc p -- `POSTPROCESS-SUBGRAPH-1: declarative sub-stage inventory for the PostProcess pass.`
- `GameOS/gameos/gos_terrain_surface_schema.h:3` 25-line comment, 11/20 lines history/ticket-heavy; condense to 1-2 lines + doc p -- ``
- `shaders/include/mech_shading.hglsl:4` 17-line comment, 6/15 lines history/ticket-heavy; condense to 1-2 lines + doc po -- `VK-PARITY-MECH-1: the GL DEFAULT mech per-fragment shading terms, shared`

### CT008 (92)
- `GameOS/src/platform_winuser.cpp:14` TODO untouched for 3671d (since 2016-09-19) -- `TODO: implememt`
- `mclib/tgl.cpp:3475` FIXME untouched for 3192d (since 2018-01-12) -- `FIXME: this is not correct if model has more than 1 texture!`
- `code/team.cpp:1069` HACK untouched for 3671d (since 2016-09-19) -- `TILE HACK...`

### CT009 (401)
- `RenderCore/render_state_desc.h:204` worktree/lane-era reference 'lane' -- `the representative PipelineId is Invalid, but the lane does route applyPipeline).`
- `GameOS/gameos/gos_static_prop_batcher.cpp:884` worktree/lane-era reference 'lane' -- `this lane: "edge light is just doing the wrong thing entirely. the actual`
- `mclib/terrain.h:387` worktree/lane-era reference 'lane' -- `build+upload at terrain.cpp ttype[] loop. Additive editor-lane helper.`

### CT010 (58)
- `GameOS/gameos/vulkan_post_hzb_native.cpp:614` possible tombstone: 'was deleted' -- `VK-RUNG8 item-4: MC2_VK_POST_HZB_DIRECT_CHAIN is RETIRED. The GL<->VK handoff arm was deleted; native HZB now `
- `scripts/check-gate-default-agreement.py:1406` possible tombstone: 'used to be' -- `7. ALL KINDS ARE COMPARED (ENV-DROP-CLOSE-1). A numeric Trace knob is still skipped -- but because atol() make`
- `tools/class_light_band_harness/class_light_band_harness.cpp:2755` possible tombstone: '...gone' -- `...gone on every mission`

## False-positive notes
- CT001: 32 of 49 are comments that *deliberately* name a retired gate ("X retired to constant", "was never defined"). Tagged `explains_retirement`; the name is dead but the sentence is a tombstone, so condense, don't just delete the token. Only ~17 are plain stale mentions.
- CT002: 84 are `memory/*.md` (agent-memory files not in the repo; `category: agent-memory`) and 95 are `docs/superpowers/{specs,plans}/...` (docs dir pruned). `data/...` paths (9) may exist only in a populated game tree. Bare file names (no `/`) are not checked. Illustrative paths ("data/tgl/foo.ini") hit.
- CT003: line refs inside tool output/log quotes are also matched; the fix only edits comment text, is idempotent, and handles `:12-20`, `:6155/6311/6400`, `:12:5`. Strings are never touched.
- CT004: 0 findings: every sha-looking token in comments resolves (all 7+ char shas I spot-checked exist). Detection is context-gated (word "commit/sha/..." or >=12 hex), so bare short shas are missed by design.
- CT005: names defined only in comments, external API names (`_validation_failures()`), pseudo-names (`endXxxPass()`), and fragments (`ForCapture()` is a member suffix) show up. Review-only.
- CT006: mostly true (old MS-era dead code in code/ and mclib/). Pseudo-code and example snippets in doc comments also match; the prose guard is crude.
- CT007: 18 only because the 40% history-density bar is strict; also catches CMake rationale blocks that are still useful. Ticket-ID regex treats `UTF-8`-style tokens as IDs rarely.
- CT008: blame is ~all 2016-2018 (original MS import) so age is a weak signal; 92/92 are >8y old. `-w` blame; uncommitted lines get age unknown and are dropped.
- CT009: noisy. 228 hits in scripts/ are the *current* deploy-lane / `--lane` / `--no-lease` tooling (real concepts), and RenderCore "lane" = pipeline route. Needs per-hit human review; do not auto-fix.
- CT010: "used to be", "has been deleted" often describe live data semantics (mclib/stuff/database.cpp). Roughly half are true tombstones.

## Recommended rollout
1. **Auto-fix now: CT003** (safe, comment-only, idempotent). Apply per directory in separate commits (GuiRuntime+RenderCore first as canary, then shaders, code, scripts, tools, mclib, GameOS) with a build check between, since the repo is edited concurrently. After applying, re-run to confirm 0 CT003.
2. **Per-directory review batches (human/agent, one dir at a time):**
   - CT002 + CT001: drop or repoint dead paths; rewrite the 32 retirement-explaining gate comments into one-liners. Do `memory/` refs as one scripted rewrite (point to docs or delete).
   - CT007 then CT010 then CT009 together (all "provenance" cleanup): start with the 18 CT007 essays; condense to 1-2 lines + doc pointer. CT009 is scripts/ and tools/ heavy and mostly legitimate; review GameOS/mclib/RenderCore hits only.
   - CT006: code/ (170) and mclib/ (125) - delete commented-out code (git has history); safe-fix candidate later once a stricter detector (brace/semicolon ratio) exists.
3. **Report-only:** CT005, CT008 (feed into issue backlog), CT004 (keep as CI gate: it should stay at 0).
4. **CI/gate idea:** run `--checks CT001,CT002,CT004` on changed files only (`--paths`) as a non-blocking warning to stop new dead gates/paths accumulating.
