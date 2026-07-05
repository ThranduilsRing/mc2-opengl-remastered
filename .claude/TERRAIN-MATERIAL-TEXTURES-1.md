# TERRAIN-MATERIAL-TEXTURES-1 — per-layer PBR albedo array (albedo v1)

Lane `texremap` (A:/Games/mc2-texremap, branch `claude/texremap-1`, base `9f4a00e7`).
Recon: `.claude/worktrees/nifty-mendeleev/.claude/TERRAIN-MATERIAL-TEXTURE-REMAP-1-RECON.md` (v2 design).
Slice-preflight vs recon base `7cf11031` flagged STOP on `cook_png_to_bc7` — the drift IS the
prep commit `62d3d3c0` this slice builds on; re-recon'd against current HEAD inline (all cited
code re-read at `9f4a00e7`) before coding.

## What shipped

- **Gate `MC2_TERRAIN_MATERIAL_TEXTURES` (default OFF, byte-identical).** OFF -> binder uploads
  `u_useMatAlbedo=0` -> `terrain_lod_chunk.frag` colormap-tint composition runs VERBATIM
  (verbatim-else); no member writes on the OFF path.
- **Array:** 6-layer `GL_TEXTURE_2D_ARRAY`, `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` (BC7 sRGB —
  sampler does the decode, per the audit finding), layers = MAT_LAYER_* order
  rock/grass/dirt/concrete/snow + cliff(5), 2048², 12 mips, uploaded verbatim via
  `glCompressedTexSubImage3D`. VRAM ≈ 32 MiB. Loader is self-contained in the LIVE chunk binder
  (`GameOS/gameos/gos_terrain_lod_chunk.cpp`, `tglc_EnsureMatAlbedoArrayLoaded`, unit 4), lazy at
  first gate-ON bind, ALL-or-nothing fail-soft (`[TERRAIN_MAT_TEX]` log lines).
- **Frag composition** (`shaders/terrain_lod_chunk.frag`): gate-ON block after the tint mix —
  per-layer albedo sampled world-space with material-lib tiling (`matTiling`, rock /3 = same UVs
  as the detail-normal path), weighted by matWeights (classifier OR control map) + HSV snow;
  colormap stays MACRO tint (`texAlb * base * 2.0`); `u_matAlbedoStrength` mixes legacy vs
  textured (env `MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH` > JSON `matAlbedoStrength` > 0.7).
  Cliff block: steep slopes sample CLIFF layer triplanar (LOD hoisted to uniform flow — UB2) =
  "cliffs look like rock" payoff. Cement/overlay restores still win (block runs before them).
- **TinyJson nested-layers extension** (`GameOS/gameos/terrain_material_lib.cpp`): root parser
  collects top-level strings (`textureRoot`) + descends `layers{ "<ch>": { "albedo": path } }`;
  v1 flat files parse identically. Own killswitch (controlAlbedo precedent), applied at mission
  load; overrides land before the lazy first-draw load.
- **Cook fix** (`tools/mc2texcook/cook_pbr_maps.py` + `cook_terrain_layers.py`): 62d3d3c0 cooked
  ALL terrain_layers KTX2s mip-0-only (no `--generate-mipmap`). Added opt-in `mips=` param
  (default False = old behavior for other callers); 6 albedo layers re-cooked levels=12.
  Normal KTX2s untouched (no consumer yet); their path also requests mips on future re-cooks.
  KTX2s remain loose gitignored sidecars; `scripts/deploy_payload.py` gained
  `TERRAIN_LAYER_PAYLOAD` (optional, fail-soft skip when absent).
- **Registered:** `RenderCore/RendererFeatureRegistry.h` kAuxEnvVars (Feature + Trace knob),
  `scripts/run_smoke.py` allowlist, `docs/tier1_env_vars.md`, `docs/flags.auto.yaml` regenerated.
  check-env-registry PASS / check-new-gates clean / mc2texcook pytest 58 pass.

## Not in this slice (recon rulings)

- Normal-source remap (R3: reuse existing `mat<N>_normal.tga` array) — untouched.
- ORM array (R2: scalars stay authoritative), POM-offset albedo alignment (POM default-OFF),
  shore_sand/wet_rock channels (no MAT_LAYER slot).

## Validation

- Build RelWithDebInfo clean (`aa94df7a`).
- slice_gate on v0.4 spare lane (NO 0.5-testing): baseline (gate none) + gate-ON
  (`MC2_TERRAIN_MATERIAL_TEXTURES=1`) on mc2_24 (mesa) + gaea_peaks_01 (rock showcase).
  See main branch commits + smoke artifacts under `tests/smoke/artifacts/`.
