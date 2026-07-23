# MERGE-WAVE-1 LOG (2026-07-22)

Target: `claude/nifty-mendeleev` @ A:/Games/mc2-main (start HEAD `994c3776`).
Local merges only — NO push. Tier1 gate between merges.

Pre-merge housekeeping: 3 tracked doc edits stashed
(`MERGE-WAVE-1 pre-merge stash`: .claude/agents/DOMAINS.md,
.claude/archive-2026-07-05/HANDOFF-mech-preview-fbo.md,
docs/render-backend-seams/gated-layout-macros.md) — restore after wave.

## 1. claude/github-issue-fixes-1
- Base: 994c3776 (= nifty HEAD; branch was up-to-date, trivial merge)
- Merge commit: `794697a9`
- Conflicts: NONE
- Build: clean (mc2.exe linked, warnings only)
- Tier1: PASS 5/5 (mc2_01/03/10/17/24, deploy lane 0.4d-rc1, fresh deploy_payload)
- Note: run_smoke does NOT redeploy; had to `deploy_payload.py A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1 --build-dir build64/RelWithDebInfo --allow-new-target` (nifty's deploy allowlist lacks 0.4d-rc1 until unit-profile-seam merges). Stale lease from a killed first run released via smoke_lib.deploy_lease.release_lease().

## 2. claude/ui-phase1-integration
- Merge commit: `6b70818d`
- Conflicts (9 files) + resolutions:
  - `CMakeLists.txt` — union: hudimguipanel/hudtabregistry (nifty) + hud_imgui_font (branch)
  - `GameOS/gameos/gameosmain.cpp` — union: LOAD_RACE verdict/probe blocks (nifty) + CAMERA-PARK-1 cameraParkTick (branch); DPI block auto-deduped (identical both sides)
  - `code/infowindow.{h,cpp}` + `code/pausewindow.{h,cpp}` — POLICY CALL: nifty's IN-MISSION-HUD-IMGUI-SCOPE-1 slice B (MC2_HUD_IMGUI_PAUSE) + slice D (MC2_HUD_IMGUI_INFOWINDOW) are parallel earlier drafts of the SAME panels; ui-phase1's PAUSE/INFO-IMGUI slices are the ledger-documented, pixel-verified, field-tested versions → took ui-phase1, removed slice-B/D impls (decls+defs+call sites). KEPT: slice C tab registry (controlgui), hudimguipanel.{h,cpp}, infowindowmodel.h + unit test (standalone kernel). Gates MC2_HUD_IMGUI_PAUSE / MC2_HUD_IMGUI_INFOWINDOW no longer exist; MC2_UI_IMGUI_PAUSE / MC2_UI_IMGUI_INFO are the live ones.
  - `code/controlgui.h` — union: tab registry (nifty) + ObjectiveUiRow renderObjective signature + OBJ/TIMER/VIDEO ImGui decls (branch)
  - `code/mechicon.cpp`, `code/missionbegin.cpp` — whole-file conflicts were EOL noise (nifty=LF post issue-fixes, branch=CRLF); EOL-normalized 3-way remerge → zero real conflicts, result LF
  - `AGENTS.md` — add/add: kept nifty's canonical harness guide (branch had lane-pointer stub)
- Build: clean
- Tier1: PASS 5/5 (deploy fingerprint 6b70818d verified via deploy_status)

## 3. claude/vk-interop-bridge-1
- Merge commit: `c92c4740` (351 commits; base c18618c4)
- Conflicts: 28 files. Key resolutions (union of functionality):
  - EOL-noise dissolved by normalized 3-way remerge: weaponbolt.cpp, tgl.cpp, userinput.h (zero real hunks); mechcmd2/mission/gamecam/camera reduced to small real hunks. Result EOL = LF (nifty convention).
  - `GpuBufferOwner.glName -> backendHandle` (VK L6.1 rename) adopted repo-wide; nifty-era `.glName` users fixed (gos_terrain_lod_chunk.cpp 15 sites + light_ssbo). `RenderResourceDesc.glName` unchanged (still valid).
  - `RenderResourceRegistry.h` id collision: nifty TerrainVisualDampSsbo keeps 39; VK Mech/StaticProp ring + Water bufs renumbered 40-47; Count=48; static_asserts updated. Symbolic enum references make renumber safe.
  - `RendererFeatureRegistry.h`: both branches' entry blocks unioned; verified zero duplicate gate entries. flags.auto.yaml REGENERATED via scripts/flag-registry-auto-gen.py (not hand-merged — 379 pseudo-conflicts).
  - `gameos_graphics.cpp`: nifty's GAMEOS-GRAPHICS-SPLIT-1 layout kept; VK's in-moved-block changes ported into the split files via patch: gosTexture content-serial + rgba-mirror API (VK-HUD-MIRRORCOPY-1) -> gameos_graphics_internal.h; createHardwareTexture mirror hooks -> _texture.cpp; font-atlas mirror -> _font.cpp; light SSBO owner idiom + LIGHT-GROW-ONCE-PREFIX-SKIP-1 + upload stats + VK interop export -> _light_ssbo.cpp (1 hunk hand-applied). Water HDRI LOD hoist (mc2WaterHdriLodDefault) taken from VK.
  - OLD VK interop islands DELETED per VK branch: vulkan_edge_fog_island.cpp, vulkan_oob_fog_island.cpp, vulkan_postprocess_subgraph.cpp (modify/delete — deletion wins). Nifty's SKYBOX-FOG-EXCLUDE-VULKAN-PORT-1 (a3216945, targeted the deleted CPU stencil bridge) DROPPED for VK shaders — edge_fog/fog_oob.frag taken wholesale from VK (native fog consumer explicitly has no stencil binding). GL-side skybox fog exclude untouched. Docs bullets for the deleted gates pruned from tier1_env_vars.md with a merge note.
  - sim-speed: nifty's runtime-settable SIM-SPEED-BUTTON-1 (simSpeedGetMultiplier) kept over VK's older static-cached MC2_SIM_SPEED; no dangling VK symbol.
  - `gameos_input.cpp`: union — nifty UI-ASPECT-ANCHOR-1 canvas-box branch retained, coordinates switched to VK's HiDPI window-point normalize (winW/winH from SDL_GetWindowSize).
  - `GuiRuntime.cpp`: union — VK's GL-vs-Vulkan ImGui backend selection (s_openGlRendererInitialized, vkImguiRequested, VK shutdown/newframe paths) + nifty's context pinning + SEH-guarded shutdown; GL shutdown now conditional; InitEditorOpenGLOnly sets the GL flag.
  - `EditorInspector.cpp`: mc2_hzb_dump_camera_view now int (matches merged gamecam.cpp definition); nifty bookmark-save extern kept; both statics kept.
  - `gos_terrain_lod_chunk.cpp` / `terrain_lod_chunk.vert`: nifty's geomorph+reauth GL path kept (vert hunk = ours); VK tier-1 feed blocks + height/cement version bumps unioned in cpp; hunk-5 upload keeps nifty mip-append (renamed to backendHandle).
  - CAMERA-PARK: kept ui-phase1's refactored cameraParkTick (gos_visual_capture.cpp) over VK's inline original — same fixture, one implementation.
  - camera.h/gamecam.h: comment-only drift — took VK (origin) wording.
  - scripts: deploy_payload union (defs deploy + fingerprint guard + VK --require-clean + shader-marker scan); run_smoke allowlist union (MC2_FX_DEFS + MC2_VK_PICK_PROBE); runner union (marker_overrides + show_window, wShowWindow 5/4/7 precedence). All ast-parse clean.
  - tests/visual/bookmarks/mc2_01.json: VK's richer note + nifty's cliff_wall_sideon bookmark (JSON-validated).
  - visual_heightfield.py: nifty (superset: SHORE-CONTOUR-1 suppression + mips provenance sha).
- KNOWN FOLLOW-UPS: check-split-path-contracts.py stamps reference PORT-BEGIN markers dropped from terrain_lod_chunk.vert (GL side took nifty geomorph version) — checker may need re-stamp; gated-layout-macros.md taken from VK side, regenerate via its checker.

## 4. claude/road-pbr-abandon-1
- Merge commit: `bca04aaf`
- Conflicts: docs/flags.auto.yaml only — resolved by REGENERATING (scripts/flag-registry-auto-gen.py). MC2_ROAD_PBR verified absent from GameOS/mclib/code/RenderCore/shaders post-merge.
- Build: clean; Tier1: PASS 5/5 (exit 0)

## 5. claude/unit-profile-seam
- Merge commit: `01ec6bbd` (103 commits; carries exactly the documented S1-S4 arc + LAUNCHER-ENCODING-1 twin b3f089b8/d58be3c4 + MOD-INDEX-CACHE-MTIME-1 + modding-doc batches — matches ledger/memory, no undocumented feature work)
- Conflicts (6, small): code/infowindow.cpp (EOL remerge; union includes + EQUIP-ADD-UI-NAME-1 S3 fallback restored around nifty's cLoadString); mclib/mech3d.cpp (union: nifty tglLoadShapeChecked fail-loud kept + seam PRESENTATION-SOCKET-PROFILE-1 block); scripts/check-contracts.sh (nifty meta-fix checker blocks kept; seam's launcher_ascii/selftest auto-merged at line 162); scripts/deploy_payload.py (nifty fingerprint manifest write); tests/unit/test_rendercore.cpp (comment only); docs/tier1_env_vars.md (union — MC2_EQUIPMENT_ADD doc added)
- Build: clean; Tier1: PASS 5/5 (exit 0)

## Wave summary (2026-07-22/23)
- Final nifty tip after wave: merge 5 commit `01ec6bbd` + this log commit.
- Gates: merges 1,2,4,5 PASS first attempt; merge 3 PASS after fix-forwards (build) and two invalidated runs (deploy-tooling, fingerprint) — final clean-gate PASS.
- Release notes draft: `.claude/RELEASE-NOTES-DRAFT-v0.5.1.md` (do NOT cut until wave 2 / deferred-sweep-1 lands).
- Post-wave follow-ups: regenerate gated-layout-macros.md + re-stamp split-path-contracts; run full check-contracts.sh once before release cut; MC2_HUD_IMGUI_PAUSE/INFOWINDOW gates removed (superseded) — check no external docs reference them.

## DEFERRED
- claude/modding-examples-1 — NOT merged. The branch sits on the ROOT-repo docs lineage (contains 7cda2faa/e6aca67e etc., not ancestors of nifty) and conflicts on the repo-intel tooling (add/add: repo_query.py, env_index.py, grep_tool.py, dead_gate_scan.py, mission_smooth.py, mc2mod.py, render-state MCP, .gitignore, active_campaigns.md, visual-tuning-profiles.md). Merging would entangle the root-docs history into canonical nifty. Task rule was "merge only if clean" — it is not. Recommended path: cherry-pick the tutorial content commits (095d24c9, 6169a8e8, edbaef3b, 437ca0ee, 50c68973 — docs/modding/examples/* + tools/mc2mod) onto nifty in a follow-up, or graft without the root-lineage parents.
- claude/deferred-sweep-1 — wave 2 by design (agent still committing; lane tip moved 0418535b -> 84b63d16 during this wave).
- Fix-forward commits after merge: `<memory>/<vector>` include (internal.h); registry entry `},` terminators (2 union-seam drops); DrawCliffDecalPanel closing brace; Desc.glName restore in lodchunk (blanket rename over-hit, + cast); gos_profiler.h include in light TU; gosRendererGetTexture shim for font TU.
- Build: clean after 3 fix-forward rounds (+1 deploy_payload argparse dedup)
- Tier1: PASS 5/5 clean gate (exit 0) at HEAD 6727637b. (Two earlier runs: one invalidated by dup --require-clean crashing deploy — smoked stale exe; one 5/5 PASS but HARD-FAIL on fingerprint because the exe was built 1 script-only commit behind HEAD. Rebuilt+redeployed for the clean gate.)
