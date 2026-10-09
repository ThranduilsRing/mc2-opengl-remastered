# Split-path pair ledger (META-FIX design C)

Machine-checked by `scripts/check-split-path-contracts.py` (wired into
`scripts/check-contracts.sh` as `split_path_contracts` + `split_path_selftest`).
This ledger is the HUMAN side: what each pair is, why islands are islands, and
where the parity evidence lives. The checker's in-file tables
(`FS_CONVENTIONS`, `KNOWN_VS_WRITES`, `PAIRS`, `ISLANDS`,
`scripts/split-path-stamps.json`) are the enforcement ground truth — edit them
together with this file.

Protected failure classes: issue **#24** (untextured gos quad FS lost its
`.bgra` while the textured sibling kept it — R/B swap on one path only) and
issue **#34** (tess and non-tess terrain configs silently diverging on
shadow/near-plane behavior). Mirrors the static-prop family-ledger precedent.

## Re-stamp protocol (RULE 4)

A pair stamp records the sha1 of BOTH sides at last verified parity. Editing
either side turns `check-contracts.sh` red until you:

1. re-run the pair's parity evidence (below), and
2. `py -3 scripts/check-split-path-contracts.py --stamp <pair>`.

**Stamp fatigue warning (design residual risk):** running `--stamp`
mechanically without re-running the evidence degrades RULE 4 to a speed bump.
The stamp is a claim that parity was RE-VERIFIED.

## Pairs

| Pair | Members | Contract | Parity evidence |
|---|---|---|---|
| `edge_fog_frag` | `shaders/edge_fog.frag` ↔ `shaders/vulkan/edge_fog.frag` | same fog math on GL and VK paths | VK pixel-parity convention (RendererFeatureRegistry.h "pixel-parity-proven vs the GL path"); capture-baseline compare |
| `fog_oob_frag` | `shaders/fog_oob.frag` ↔ `shaders/vulkan/fog_oob.frag` | same out-of-bounds fog on GL and VK paths | same as above |
| `water_fast_vs` | `gos_terrain_water_fast.vert` ↔ `gos_terrain_water_fast_mdi.vert` | identical water surface math, MDI vs non-MDI submission | water A/B smoke + visual capture (INDEX-TERRAIN water-v2 slices) Re-stamped 2026-10-09 (3af9c6737): shore mode 3 depth lift (u_shoreFlat/u_shoreDepthK) is MDI-only by design; the non-MDI vert has no shore-flat uniform. |
| `shadow_terrain_paths` | `shadow_terrain.tesc` + `shadow_terrain.tese` ↔ `shadow_terrain.vert` | tess and non-tess shadow-terrain emit the same depth | nightly `MC2_TERRAIN_TESS=0/1` A/B leg (Probe-B recipe, ISSUE-FIXES-STATUS.md) with `MC2_SPLIT_PATH_PARITY_TRACE=1` |

### Re-stamp log

- **2026-07-30, BACKEND-DEFAULT-PARITY-1 — `edge_fog_frag` + `fog_oob_frag`.**
  Both GL members were re-stamped after a **comment-only** edit (the
  `MC2_SKYBOX_FOG_EXCLUDE` header block in each said the then-current default (OFF) long after
  the gate was flipped default-ON; the gate-default checker could not see the
  lie while the var had two contradicting C++ readers). The documented parity
  evidence for these two pairs is a **capture-baseline compare**, and engine
  launches were hard-blocked in that lane, so it could not be re-run. The
  evidence used instead is *stronger for this particular class of edit, and
  weaker for any other* — do not treat it as a precedent for a code change:
  - the VK members were not touched at all;
  - each GL member's comment-stripped, whitespace-normalized text is
    **byte-identical** to its pre-edit content (2019 chars for `edge_fog.frag`,
    2169 for `fog_oob.frag`), i.e. the pair's divergence surface did not move;
  - neither pair's four files contain `__LINE__` or `__FILE__`, so there is no
    construct by which comment lines could reach codegen.
  A re-stamp after any edit that changes a single token of GLSL **code** still
  requires the real capture compare.

## Island-only entries (stated reasons)

| File | Reason |
|---|---|
| `shaders/vulkan/edge_fog.vert` | VK fullscreen-triangle VS; the GL edge_fog pass has no dedicated `.vert` |
| `shaders/vulkan/fog_oob.vert` | VK fullscreen-triangle VS; the GL fog_oob pass has no dedicated `.vert` |

Every NEW file under `shaders/vulkan/` must be added to one of the two tables
or the checker fails (self-registration forcing function).

## Color-convention table notes (RULE 1)

Three legitimate conventions coexist (per-PROGRAM invariant: the packed-BGRA →
FragColor chain nets exactly one `.bgra`):

- **FS-swizzle** — FS reads `Color.bgra`; VS passes the packed attribute raw
  (`gos_vertex/gos_tex_vertex/gos_vertex_lighted/gos_terrain/water_mdi` frags).
- **VS-swizzle** — VS pre-swizzles (`terrain_overlay.vert: Color=colorIn.bgra`);
  the paired FS reads raw (`decal.frag`, program pairing at
  `gameos_graphics.cpp` `terrain_overlay.vert + decal.frag`).
- **Double-swizzle** — VS decodes SSBO data to logical RGBA then pre-swizzles
  `.bgra` so the shared FS-swizzle frag restores it (`gos_terrain_mask_water`,
  `gos_terrain_water_fast`, `gos_terrain_water_fast_mdi` verts).

**FROZEN-suspect entry (follow-up candidate, NOT certified for new code):**
`gos_terrain_thin.vert` writes `Color = unpackARGB(lrgb)` (logical RGBA, no
pre-swizzle) into `gos_terrain.frag`'s `.bgra` read — a latent net R/B swap on
the indirect-terrain vertex-light path. Invisible today because terrain vertex
light is near-greyscale and `terrainTextures2` forces `alphaOverride =
0xffffffff` on the modern path. Recorded in the checker's `KNOWN_VS_WRITES`
with a do-not-copy note; fixing it (pre-swizzle in the thin VS) would be a
visible-risk shader change and is deliberately NOT part of the freeze slice.

## Runtime tier

`MC2_SPLIT_PATH_PARITY_TRACE=1` (kAuxEnvVars, default OFF) emits
`[SPLIT_PATH_PARITY] site=... tess=<0|1> shadow=<0|1> match=<0|1>` at terrain
material load. `match=0` = the #34 decouple invariant broke (modern shadow
path inactive). The smoke marker-bucket row `split_path_parity`
(`scripts/smoke_lib/logparse.py`) makes any `match=0` a red mission verdict.
Nightly leg: `run_smoke --mission mc2_01 --mission mc2_24` with
`MC2_TERRAIN_TESS=0` + `MC2_SPLIT_PATH_PARITY_TRACE=1` (Probe-B recipe) —
THIS is the behavioral guard for the #34 class; RULE 3 only freezes tokens.
