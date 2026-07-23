# BACKDROP-BLIT-ATTRIB-1 — status 2026-07-23

Branch `claude/nifty-mendeleev`, worktree A:/Games/mc2-main.
Lane BACKDROP-BLIT-ATTRIB-1. Commits **`16c2244a`** (fix + tooling, the VERIFIED
deploy) and **`d32a438f`** (empty-env-string hardening, behavior-identical for
all real inputs). Gate **`MC2_NIGHT_FOG_DIM`** (default-OFF).

## Headline

The mc2_10 night **"white horizon impostors + black band"** is the **EDGE-FOG /
OOB fog "cloud bank" pass** (`edge_fog.frag` / `fog_oob.frag`) painting a
**hardcoded bright-grey `u_fogColor` `{0.80,0.81,0.83}`** at the map horizon that
is **never dimmed for the mission night lighting**. On a night map the cloud bank
composites near-white and the map appears to float in a bright foggy void. Pinned
by RenderDoc **pixel-history on the pre-composite HDR scene FBO (231, not the
backbuffer)** — the two prior suspects (HDRI sky, static-prop batcher) are both
proven OFF-path. Landed a gated night-dim (`MC2_NIGHT_FOG_DIM`, default-OFF,
byte-identical) that scales the fog color to the measured night-sky shade;
A/B-verified on all 3 night poses.

## Attribution (source-grounded, RenderDoc-confirmed)

Tooling added to `tools/renderdoc/` (rd_worker `textures` / `target_resid`
attribute / `pixel` history / `disasm` modes + `scene_attrib.py` CLI) so
pre-composite scene-FBO writers can be pinned without the backbuffer tonemap
confound that stalled the prior two lanes.

Chain, capture `debug_state/fbl_rdc_frame432.rdc`, HDR scene FBO **231**
(3840×2160 RGBA16F), impostor/horizon region:

1. `scene_attrib.py --resid 231` region attribution + per-pixel color history at
   the horizon pixels showed the write order:
   - eid 101 `glClear` → sky-grey `(0.55,0.62,0.72)`
   - eid 126 `glDrawArrays` (VS/PS 253/254, samples **Texture 2159** = the
     16384×8192 BC6H equirect → this is the **HDRI skybox**) → brings the pixel
     **DOWN** to dark `(0.27,0.26,0.29)`. **The HDRI sky is NOT the culprit** —
     it is correctly night-dark.
   - eid 6770 (VS/PS 93/94, additive One→One, untextured vertex-color + fog =
     `gos_tex_vertex`/`gos_vertex` path) lifts localized "impostor" pixels.
   - **eid 7158** (VS/PS 277/278, alpha `SrcAlpha→InvSrcAlpha`, samples depth
     tex 233) lifts the **whole horizon band uniformly** `0.27 → 0.56`.
2. `disasm` of shader 278 = **`edge_fog.frag`** verbatim (uniforms
   `u_fogColor`, `u_fogOpacity`, `u_horizonClampEnabled`, `u_skyExcludeEnabled`,
   `invViewProj`, depth-reconstructed cloud bank). Output is `vec4(u_fogColor,
   fogFactor)` painted where depth==far (sky/horizon). The measured 0.56 grey =
   `u_fogColor` at the pass opacity.
3. C++ site: `GameOS/gameos/gos_postprocess.cpp::runEdgeFog()` uploads
   `edgeFogColor_`, and `edgeFogColor_` is a **compile-time constant**
   `{0.80,0.81,0.83}` (`gos_postprocess.h`) — its own comment tunes it for the
   **day** exposure/grade of mc2_10/mc2_24 and it is never scaled for night.

This matches the prior two lanes' disproofs (peer `73755184` floored the
untextured `gos_vertex` path = no-op on the band; static-prop exaggeration turned
buildings red while impostors stayed white; `[OBJBATCHER] submit_trees=0`) — all
consistent with the writer being the post-process fog pass, not geometry.

## Fix (commit `16c2244a`)

Gate `MC2_NIGHT_FOG_DIM` (RendererFeature #47, default-OFF). When ON, a shared
`gosPostProcess::effectiveFogColor()` helper scales `edgeFogColor_` **and** the
sibling `oobFogColor_` by `MC2_NIGHT_FOG_DIM_SCALE` (default **0.35**, tuned to
the measured night HDRI-sky value ~0.27/0.80) at BOTH upload sites: the legacy
`runEdgeFog`/`runFogOob` uploads and the frame-graph executor context
(`PostprocessFogRegionContext`). Gate OFF → scale 1.0 → `out = in * 1.0f` (exact
in IEEE float) → **byte-identical**.

Files: `GameOS/gameos/gos_postprocess.{cpp,h}`,
`RenderCore/RendererFeatureRegistry.h` (enum + kFeatureTable +
`MC2_NIGHT_FOG_DIM_SCALE` aux tunable), `scripts/run_smoke.py` (allowlist),
`docs/tier1_env_vars.md`, `tools/renderdoc/*`. `gate_status(MC2_NIGHT_FOG_DIM)` =
**OK** (registered, documented, smoke-allowlisted, default-OFF, 3 readers).

Build clean (RelWithDebInfo, mc2 relinked). Deployed to lane **0.4**, manifest
`src_commit 16c2244af599`.

## Verification (build `16c2244a`, install mc2-win64-v0.4, frame 431)

Night poses `mc2_10_saved_204611 / 204907 / 205952` via
`tools/visual_verify/capture.py`, three arms. Engine logs confirm gate state per
arm (`[NIGHT_FOG_DIM] enabled=0/1`).

- **OFF vs ON(0.35), pose 205952 & 204611 (visual, `scratch/vv_nfd/cmp_*.png`):**
  OFF = map floating in a **bright white-grey fog void / washed-out horizon** (the
  reported defect). ON = fog dimmed to a **plausible night grey**; terrain, water,
  airbase lights, trees, and unit blips **unchanged**. Clean, targeted effect.
- **Exaggeration (`MC2_NIGHT_FOG_DIM_SCALE=0.0`, pose 205952):** fog void goes
  **pure black** — proves the horizon band is fully owned by this fog color path.
  Per-row luma is monotonic OFF→ON→EXAGG = **178 → 62 → 0** (exactly ×0.35, ×0.0).
- **Day-pose / gate-OFF byte-identity:** by construction (scale 1.0f, exact). Day
  missions are untouched in shipping because the gate is default-OFF.
- **Cinematic letterbox:** unaffected — the letterbox bars are a UI/movie overlay
  path, not the depth-gated fog pass (which only writes where depth==far). The
  black bottom seen under exaggeration is the fog void, not a bar.

## Residual risk / handoff

- **Manual killswitch, not auto night-detect.** No mission night-light color is
  plumbed into `gos_postprocess`, so `MC2_NIGHT_FOG_DIM` is a per-run/per-mission
  toggle rather than an automatic day/night term. A DAY mission run with the gate
  forced ON would also dim its fog. Follow-up (optional): plumb the mission
  terrain-light RGB into the postprocess so the scale derives from actual scene
  lighting (higher blast radius — a new signal into gos_postprocess). For now the
  correct default is OFF; enable for night maps.
- The additive `gos_vertex` glow (eid 6770) adds a small localized lift on top of
  the fog band; it is a minor secondary contributor, not the dominant white band,
  and is left untouched (peer `73755184`'s floor of that path was already an
  A/B-proven no-op on the band).
- Deployed/verified exe = `16c2244a`. `d32a438f` (empty-env-string hardening) is
  committed but behavior-identical for all non-empty inputs; no redeploy needed.
- Fingerprint carries `MC2_BUILD_GIT_DIRTY=1` from pre-existing unrelated dirty
  scratch dirs (debug_state/, scratchpad/); the fix source IS committed at
  `16c2244a`.

## Status: attribution CONFIRMED (RenderDoc scene-FBO pixel-history), fix SHIPPED + A/B-VERIFIED, gate default-OFF.
