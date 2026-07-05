# POM-NEXT-SLICES — follow-on ladder after TERRAIN-CHUNK-POM-1 (for opus)

Base: `claude/pom-1` @ `2930a6a5` (TERRAIN-CHUNK-POM-1 shipped: real view-vector
POM behind `MC2_TERRAIN_POM`, default OFF, gate-OFF byte-identical incl. the
legacy faux shear). Recon: `.claude/TERRAIN-CHUNK-POM-1-RECON.md`.

## State snapshot (what POM-1 left behind)

- `shaders/terrain_lod_chunk.frag`: `chunkParallaxView()` (real vector, oracle-
  verbatim math) beside the untouched legacy `chunkParallax()` (faux, gate-OFF
  path). Gate uniform `u_pomView` (.x gate, .y NEAR, .z FAR); `cameraPos`
  (Stuff/MLR eye, MC2 = `(-x, z, y)` — swizzle PROVEN numerically, 6.7° mean
  err vs probe truth; all alternates 60–115° off). Diag bits: 4096 = pomOff
  heat, 8192 = view-vector oracle. u_diag bits 4096/8192 now taken.
- Driver `GameOS/gameos/gos_terrain_lod_chunk.cpp`: per-submit uploads beside
  pomParams; env knobs `MC2_TERRAIN_POM_SCALE/_STEPS/_NEAR/_FAR` (registered in
  `RenderCore/RendererFeatureRegistry.h` kAuxEnvVars + run_smoke allowlist).
- Validation assets: `tests/visual/bookmarks/mc2_24_pom_closeup.json` (desert
  close-up set, rot45/rot225 cross-launch byte-stable — the identity witnesses).

### Hard-won methodology facts (do NOT relearn these)

1. **mc2_01 is unusable for numeric pixel oracles** — the map center is ocean;
   water compositing (reflection of the terrain viz through the mirrored pass)
   silently corrupts decodes. Use mc2_24 (desert). This cost POM-1 several
   hours of false-FAIL analysis.
2. **run_smoke drops non-allowlisted MC2_* vars** — gate-ON smokes run inert
   without the allowlist entry; check `[ENV-DROP]` on stderr (POM vars now
   allowlisted).
3. **deploy_payload needs `--build-dir build64/RelWithDebInfo` in this
   worktree** — `build64` silently FAILs ("source exe not found") and a stale
   payload keeps running (deploy-fingerprint-stale-exe gotcha). Verify frag
   md5 repo-vs-deploy after every deploy.
4. **mc2_24 standard bookmarks are cross-launch NONdeterministic** (live units)
   — only the close-up rot45/rot225 framings are byte-stable identity
   witnesses; `rot45_graze` sees units on the horizon.
5. Bookmark JSON paths passed to `run_visual_capture.py --bookmarks` must be
   ABSOLUTE (engine cwd = deploy dir).

## Slice ladder (recommended order)

### POM-2 — Per-layer height sources from texture-remap (content slice)
The march reads `matNormalArray[layer].a` as displacement. Today only ROCK /
GRASS / CONCRETE alphas are summed (`chunkSampleDisplacement`), dirt/snow are
blank, and mc2_24-class desert missions are dirt/sand-dominant → POM-1 changes
only 2–5% of close-up pixels at stock scale.

- `terrain_materials.json` (TERRAIN-MATERIAL-LIB-1, `u_useMaterialLib`) already
  reserves a `height` field that is NOT wired — plumb it as a per-layer scale
  vec (rock/grass/dirt/snow) uploaded beside `matRoughness`/`matAO`, replacing
  the hardcoded `pomScaleMat = vec4(1,1,2.5,1)`.
- TERRAIN-MATERIAL-TEXTURE-REMAP-1 (recon in `.claude/`) is the seam for
  AUTHORING real height into the `.a` channels (rock cliff + dirt ripples).
  Ship height-in-alpha for the remapped set; validate with the 4096 heat viz
  (heat should light up on dirt once its alpha is real and it joins the sum).
- Landmine: `.a=1.0` white-alpha TGAs read as "full displacement everywhere"
  → `1.0 - d` = 0 = max depth constant → no visible shift but full march cost.
  Add a variance guard at load (skip layer if alpha is near-constant).

### POM-3 — Marble-cliff / triplanar POM routing (ruling R2 follow-up)
POM-1 fades POM out over the same |Nz| 0.85→0.55 band where the triplanar
cliff blend fades in (cliff walls stay triplanar-owned, no double-relief).
MARBLE_CLIFF (layer 5) `.a` IS a displacement map but is only consumed by the
triplanar normal path.
- Option A (cheap): parallax the triplanar cliff UVs on the dominant axis only
  (the near-vertical projection), reusing `chunkParallaxView` with the axis-
  local tangent frame. Gate `MC2_TERRAIN_POM_CLIFF`, default OFF.
- Option B (defer): accept flat cliffs; revisit after POM-2 ships real heights.
- Do NOT run both top-down and triplanar POM in the blend band (double-shift
  seam) — keep the complementary fade, just move it to the cliff side.

### POM-4 — Cost tuning ladder (before any default-ON discussion)
Current shape: 4–16 `textureLod` per POM fragment, fwidth+distance+slope+LOD
quadruple-gated; FAR=600 A/B proved the governor zeroes the march.
1. Measure: Tracy frame delta gate-ON vs OFF at the mc2_24 close-up bookmark
   (coarse per-pass zone only — no per-fragment zones, 100ns floor rule) at
   default and at SCALE=0.06/STEPS=8.
2. `numLayers` currently interpolates to `pomParams.z` at grazing; consider
   capping effective grazing cost via `min(numLayers, 8 + 8*distFade)`.
3. Occlusion-interp sign audit: the oracle-verbatim epsilon guard
   (`after / max(abs(after-before),1e-6)`) flips the interp weight sign vs the
   legacy chunk expression (`after/(after-before)`, weight ∈ [0,1]). Verbatim-
   oracle was the POM-1 ruling; a tuning slice may fix the sign (deepens
   accuracy by up to one layer) — visual-only, gate-ON-only change, A/B with
   the close-up captures.
4. Retune default `_SCALE` (faux 0.85-up divided the old effective offset; a
   real grazing view yields larger P — stock 0.02 is conservative post-swap).

### POM-5 — Silhouette-POM assessment (assessment only, likely REJECT)
Real silhouette displacement (steep-parallax with depth write or shell/prism
extrusion) conflicts with two standing rulings:
- **NEVER `gl_FragDepth`** on this path (AMD early-Z landmine, decal tearing —
  frag header + `vulkan_aligned_depth_bias_ruling.md`). Depth-offset POM is
  therefore OFF the table entirely.
- Shadow lookups sample the true surface point; silhouette shift without depth
  breaks contact shadows at the offset rim.
Viable alternative if silhouettes are ever demanded: geometric displacement
via the existing TERRAIN-VISUAL-HEIGHT SSBO (vert-side, already corner-pinned)
at higher mesh density near camera — that is a tessellation/LOD slice, not a
POM slice. Recommendation: write the assessment, park it, spend the budget on
POM-2 content instead (biggest visible win per cost).

### POM-6 — Editor/inspector integration (quality-of-life, small)
Terrain Pass inspector mini-control: show effective gate/scale/steps/band and
the two diag-bit toggles (pattern: TERRAIN-NORMALS-FROM-HEIGHT-1 inspector
control). Zero render change; editor (EditRel) is GPU-path-only — no CPU
fallback code in editor TUs.

## Validation template (reuse verbatim for every slice above)

1. `slice-preflight` (symbols: `chunkParallaxView`, `pomParams`, `u_pomView`;
   paths: frag + driver) before coding.
2. Byte-identity OFF: `run_visual_capture` mc2_01 std set (5/5 deterministic)
   + mc2_24 close-up rot45/rot225 pre-vs-post — hashes must match exactly.
3. Gate-ON liveness + heat: close-up OFF≠ON, 4096 heat in-band vs FAR=600 blank.
4. tier1 smokes OFF + ON (gate var must be in the run_smoke allowlist).
5. env registry (`check-env-registry.sh`) green for any new MC2_* var.
