# MERGE-WAVE-2 LOG (2026-07-22/23)

Target: `claude/nifty-mendeleev` @ A:/Games/mc2-main (start HEAD `c4e7b663`,
wave-1 complete). Local only — NO push. Conventions per MERGE-WAVE-1-LOG.md
(union functionality, prefer origin-branch versions, EOL-normalize before
judging conflicts).

## 1. claude/deferred-sweep-1 (tip 110281e7, 65 commits, 85 files)
- Merge commit: `52f4e504`
- Conflicts (9 files) + resolutions:
  - `gameos_graphics.cpp` / `terrain_overlay.frag`: branch's ROAD-PBR
    asphalt/gravel uniforms + shading blocks DROPPED (ROAD-PBR-ABANDON-1
    retirement on nifty wins); new CEMENT-NIGHT-LIGHT-1 /
    OVERLAY-BIAS-LINEARIZE-1 / NIGHT-LIGHT-POOLS-2b..4 / PAD-AREA-GLOW-1
    uniforms + gos_Set/GetTerrainMissionFlatLight feed kept.
  - `gos_static_prop_batcher.cpp`: both branches added s_packetUvScaleX/Y
    (VK FENCE-UVSCALE-1 vs SHADOW-PROP-ALPHA-ATLAS-1) — HEAD superset kept
    (also carries VK-P11 s_typeAlbedoLayer/AlphaClass); comment notes the
    shadow-atlas bind as second consumer of the same sidecars.
  - `gos_terrain_water_mdi.frag`: WATER-ASPECT-CORRECT-2 (u_waterProjAspect
    runtime uniform) supersedes nifty's hardcoded 4:3 CORRECT-1 (337c2c1e);
    PORT-BEGIN:waterReflSetup marker preserved.
  - `camera.h`/`gamecam.h`: whole-file conflicts were EOL noise (base/branch
    CRLF vs nifty LF) — normalized 3-way remerge left 4 real hunks, all
    comment drift or branch-superset (CAMERA-PARK now also kills in-flight
    CAMERA-SMOOTHING-1 glides: scrollSmoothActive/rotSmoothActive = false).
  - `mech3d.cpp`: include union (tgl_load_guard.h + cone_glow.h).
  - `tier1_env_vars.md`: union (SHADOW-PROP-ALPHA-ATLAS + CAMERA-SMOOTHING-1
    sections; Road PBR RETIRED heading kept; MC2_CLOUD_SHADOW default ON
    matches gos_postprocess.cpp).
  - `flags.auto.yaml`: REGENERATED (scripts/flag-registry-auto-gen.py).
- Fix-forwards: `e98004f4` GpuBufferOwner glName->backendHandle in
  gos_terrain_lighting.cpp (VK L6.1 rename class, same as wave-1 lodchunk);
  `20f1efa7` orphan conflict marker in terrain_overlay.frag (caught by
  deploy_payload's shader-marker scan — my hunk-2 python resolution left the
  hunk-1 `>>>>>>>` line).
- Tier1 on 0.4d-rc1: 5/5 missions PASS at 20f1efa7 but the RUN was
  invalidated on DEPLOY_FINGERPRINT — the docs agent landed 8 docs-only
  commits mid-smoke (HEAD moved). Superseded by the final clean gate below.

## 2. Cherry-picks: modding tutorials (claude/modding-examples-1)
`git cherry-pick -x 095d24c9 6169a8e8 edbaef3b 437ca0ee 50c68973` →
`b41dcb3f..f16d9760`. Conflicts:
- `.gitignore` union (examples payload negation appended).
- `docs/visual-tuning-profiles.md` union (nifty showcase-grade section +
  branch modding/overlay section both kept).
- `tools/mech_import/README.md` modify/delete: nifty never had
  tools/mech_import — took the WHOLE directory from 50c68973 (7 files) so
  the mech-import tutorial references a complete tool.
- `tools/mc2mod/mc2mod.py`: union — nifty's pack_mod() refactor (shared by
  cmd_pack/cmd_publish) + branch's reproducible `_write_zip_member` pinned
  writing inside it.
Verified: 9 tutorials in docs/modding/examples/ (+ getting-started +
mechanism-map), tests/mc2mod 22/22 PASS.

## 3. Post-wave contracts (65/65 PASS — `402068b3`,`61ba5fd1` parts 1-3)
Original run: 51 passed / 14 failed — ALL failures were wave-merge reconciles,
none functional:
- split-path: 46 native VK tier-1 island shaders declared island-only
  (runtime pixel-parity per VK arc evidence); water_geom.frag FS_CONVENTIONS
  row; edge_fog/fog_oob/water_fast_vs pairs re-stamped; LOCKSTEP-MIRROR
  markers on terrain_tier1{,_skirt}.vert.
- gated-layout-macros.md regenerated (108 guards clean).
- light_abi_lockstep: stride literal retargeted to the split light-ssbo TU.
- pipeline_desc: PipelineDesc gained drawBuffers — field list updated.
- shader_injectors: 12 governance rows added to shader-permutation-inventory.
- raw_gl_depth{func,mask}: tools/vk_gl_interop_spike/ allowlisted.
- texture_owners: TEX-CLASS tags on 7 sites.
- spirv_artifacts: SPIR-V rebaked (build_variants.py) + occupancy regenerated
  (ObjectLightPool binding 28 from night-light-pool mech consumer).
- doc_links: examples README links + vulkan-island-template ref fixed.
- path_normalization: MC2_UI_COMMAND_FILE reader tagged loose-only.
- env_registry: 27 wave-landed vars allowlisted (VK diags, seam sources,
  stats, fixtures).
- include_firewall: symbol scan made string-literal/trailing-comment aware;
  RenderResourceLifetime::Mission (RenderCore's own enumerator) exempted.
- no_raw_gl_from_game: simplecamera.cpp (env-gated preview diags) +
  bdactor.cpp (building-PBR SSBO, v1 freeze w/ follow-up note) allowlisted.
- render_contract_gbuf1: terrain_lod_chunk.frag's 18 raw GBuffer1 writes
  routed through rc_gbuffer1_shadowHandled_flatUp(). LESSON: the include had
  to FOLLOW the `#define PREC` line — first attempt compiled-failed all 5
  tier1 missions (shader_error bucket), fixed in `091246ef`.
- deploy_data_parity: PASS (ran, not skipped).

## 4. RELEASE v0.5.1
- Release lane: `A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1` — seeded
  from canonical 0.5.0 install (robocopy), then scripts/deploy_payload.py
  (exe+shaders+manifest at `091246ef`); lane registered in
  smoke_lib/deploy_lease.py DEPLOY_FOLDERS as "0.5.1".
- Packaging (coordinator-directed, release-blocking): build_release.sh now
  REQUIRES mc2-launcher.exe in the engine zip and stages the 4 example mods
  from the REPO mods/ tree (never the deploy's mods/ dir — that accumulates
  local/third-party mods); test_release_zip_install.py requires
  mc2-launcher.exe + mods/.
- FINAL CLEAN TIER1 GATE: 5/5 PASS, exit 0, DEPLOY_FINGERPRINT OK
  (exe 091246ef == HEAD) on lane 0.5.1.
- Zips: `A:/Games/mc2-main/release_assets/0.5.1/` — engine 608M (incl.
  launcher + mods/), gamedata 4.2G, burnins-4x pt1 1.5G + pt2 861M,
  art 89M, tgl, movies 161M.
- ZIP GATE: PASS (tree 28459 files OK; zips-only install smoke mc2_01 PASS;
  `release_install_manifest.csv` + `release_install_report.json`
  verdict=PASS archived alongside the zips).
- Release notes finalized: `release_assets/0.5.1/RELEASE_NOTES.md`
  (canonical, ships with zips) = `.claude/RELEASE-NOTES-DRAFT-v0.5.1.md`
  (tracked mirror). RELEASE_README.md header bumped to v0.5.1 (+ copied
  into the lane with INSTALL.txt, which the 0.5.0 seed lacked at root).

## USER-GATED (not done, by design)
- git tag v0.5.1 + push (branch NEVER pushed this session).
- GitHub Release creation + zip upload.
- Click-throughs (ImGui panels, launcher, night-lighting eyeball at user
  bookmarks — the COMPLAINT-SHAPE verification pass from the ledger).
- Issue replies (.claude/issue-reply-drafts.md, still unposted).
