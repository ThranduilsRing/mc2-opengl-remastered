# TERRAIN-CONTROLMAP-ALBEDO-1 — Recon

**Branch:** claude/controlmap-sample-1  **HEAD:** 251270f1  **Worktree:** `A:/Games/mc2-controlmap-sample-1`
**Scope:** RECON ONLY. No source changes, no build, no launch. Read-only reference to `terrain_lod_chunk.frag` / `mclib/terrain.cpp` (another agent is editing them for the overlay-V2 slice).

---

## Executive summary

The user is exactly right, and the frag confirms *why*. TERRAIN-CONTROLMAP-SAMPLE-1 already lets an authored RGBA map **replace `matWeights`** (`frag:613-628`, `u_useControlMap` branch, `texture(u_controlMap, uv)`). But `matWeights` only drives **detail normals** (`chunkDetailNormal`, `frag:687`) and a **weak tint pull** — it never repaints the base albedo, because the colormap always dominates the final colour. So an authored quadrant map reads as "normals/roughness shifted," not as four differently-coloured corners.

**Exactly where weight-driven colour STOPS today** (`frag:729-735`):
```
materialTint = tintRock*wR + tintGrass*wG + tintDirt*wD + tintConcrete*wC + tintSnow*snow;  // :729-731  (weight-driven, GOOD)
colLum       = dot(base, kLumaWeights);                                                       // :732  base = COLORMAP (9-tap blur, :421-429)
tintBase     = mix(0.18, 0.50, smoothstep(0.1,0.6,colLum));                                   // :733  << the chokepoint: 18–50% cap
tintStrength = mix(tintBase, 0.85, snowWeight) * tintStrengthScale;                           // :734
baseColor    = mix(base, materialTint, tintStrength);                                         // :735  << colormap keeps ≥50% weight always
```
`materialTint` IS weight-composed, but `tintStrength` is clamped to **0.18–0.50** (0.85 only under snow). So the burned-in BC7 colormap `base` retains **50–82% of the final albedo** everywhere. Repainting `matWeights` shifts `materialTint`, but that shift is diluted to a faint recolour — never a repaint. Downstream cliff darken (`:746-751`), triplanar (`:759-783`), breakup (`:784-790`), snow dampen (`:792`) all multiply `baseColor` and don't restore weight authority. **The colormap is the albedo; weights are a garnish.**

---

## Composition today (live chunk path), file:line

```
base   = 9-tap blur of u_colormap (BC7 KTX2 atlas, unit 0)          frag:421-429  <-- ALBEDO SOURCE
cement = u_cementAtlas override where cementWordF valid             frag:544-570  (base overwritten; separate axis)
matWeights = u_useControlMap ? texture(u_controlMap,uv)             frag:613-628  (SAMPLE-1 shipped; uv == colormap uv, :291)
           : chunkWeights(base,...)                                          (else = colormap-colour classifier)
  -> drives chunkDetailNormal (normals)                             frag:687      << weights' MAIN visible effect today
  -> drives materialTint (colour) but capped                        frag:729-735  << STOPS HERE (18–50% pull)
pureConcrete (v_terrainType/cement) forces weights->concrete        frag:661-667
cliff darken / triplanar / breakup / snow dampen                   frag:746-792  (all multiply baseColor)
```
Tints are uniforms (TERRAIN-MATERIAL-LIB-1 already promoted `tintConcrete`/`tintSnow` from literals — `frag:727-730`). Per-layer tint colours already live in `terrain_materials.json` (rock/grass/dirt/concrete `tint[3]`).

---

## Option matrix (weight-driven albedo)

| Opt | What | Cost | Weights? | North-star fit | Verdict |
|---|---|---|---|---|---|
| **(a)** matWeights × JSON per-layer **tint** colours, lerp OVER colormap by an authoring-strength scalar | **tiny** — reuse `materialTint` (:729) already built; add a strength that lets it reach 1.0 | YES | UE splat v1 (flat per-layer albedo) | **RECOMMENDED v1** |
| (b) per-layer albedo **TEXTURES** (sampler2DArray, tiled, sampled by weights) | medium — new texture array (unit taken? 5=normals), bind, 4 albedo TGAs, UV | YES | UE splat v2 (textured layers) | v2 follow-on |
| (c) `macro_color.png` sidecar replacing/blending colormap directly | small | **NO (bypasses weights)** | Gaea colour import | **ALREADY EXISTS — separate axis, see below** |

**(c) already exists and is orthogonal.** `tools/terrain_gen/import_gaea_height.py` `--color-file` (`:129-134`, `:223`) reads a 3-channel `.r32`, and `read_gaea` / `_read_image_height` (`:38-84`) accept EXR/PNG/TIFF colour → written straight to `{name}.burnin.tga` (the colormap, `:186-189`, `cmap_src="gaea color"`). That is macro-colour import: it authors `base` directly and **does not use matWeights at all**. It is the "paint the whole albedo raster" axis, complementary to (a)'s "paint per-layer weights." (c) is DONE for offline maps; nothing to build here. The user's problem is specifically the *live control-map → weights → albedo* path, which is (a).

---

## Recommended design — TERRAIN-CONTROLMAP-ALBEDO-1 = option (a)

**One new strength scalar makes authored control maps visibly repaint terrain.** No new texture, no new binding, no new SSBO. Purely widens the existing `materialTint` pull so weights can dominate.

1. **New uniform `float u_controlAlbedoStrength` (default 0.0).** At `frag:734`, blend it in so authored weights can reach full albedo:
   `tintStrength = mix( mix(tintBase,0.85,snowWeight)*tintStrengthScale, 1.0, u_controlAlbedoStrength );`
   - `u_controlAlbedoStrength==0` → verbatim current expression → **byte-identical**.
   - `==1` → `tintStrength=1` → `baseColor=materialTint` → base colormap fully replaced by weight-composed per-layer tints → **four visibly different quadrants**.
   - Intermediate (e.g. 0.6) = colormap macro variation still mixed in (north-star "mix don't discard the BC7").
2. **Gate `MC2_TERRAIN_CONTROLMAP_ALBEDO` (default OFF).** C++ uploads `u_controlAlbedoStrength=0` when unset (member `terrain_control_albedo_strength_=0.0f`); when set, uploads the env/JSON value (recommend also a `terrain_materials.json` key `controlAlbedoStrength` so material-lib authors it — precedent: that reader already applies terrain uniforms). Env override `MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH` for iteration (0..1).
3. **Two-layer identity guard** (matches SAMPLE-1 / MATERIAL-LIB pattern): member default 0.0 *and* the `mix(...,1.0,0.0)` is algebraically the current value, so gate-OFF is provably verbatim even if the uniform is uploaded.

Optional refinement: gate the strength so it only lifts where `u_useControlMap!=0` (authored regions), leaving classifier-driven fragments at the current cap — but simplest v1 is a global scalar; the control map already zeroes non-authored areas via SAMPLE-1's passthrough.

**Why not touch `materialTint` itself:** it is already weight-composed and correct; the only defect is the `0.18–0.50` cap. Lifting the cap is the whole fix. Rebuilding the colour path would risk MATERIAL-LIB / cement / cliff interactions for no gain.

---

## Files to touch (FIX slice, later — NOT now)

- `shaders/terrain_lod_chunk.frag` — add `uniform float u_controlAlbedoStrength;`; edit the `tintStrength` line (`:734`) to the `mix(...,1.0,u_controlAlbedoStrength)` form. **Nothing else.**
- `GameOS/gameos/gameos_graphics.cpp` — new member `terrain_control_albedo_strength_` (default 0.0f, near the tint members ~`:2418-2425`); new `TerrainUniformLocs`/`ThinTerrainUniformLocs` loc + `glGetUniformLocation` (near the tint locs ~`:6856`); upload next to the tint uploads (~`:6858`, thin path ~`:6994`); gate read of `MC2_TERRAIN_CONTROLMAP_ALBEDO` (+ optional strength env).
- `GameOS/gameos/terrain_material_lib.cpp` + `data/terrain_materials.json` (optional) — add `controlAlbedoStrength` key + `gos_SetTerrainControlAlbedoStrength` setter (mirror existing tint setters) so the material-lib JSON authors it. Default 0.0 in the JSON preserves identity.
- `docs/` — one line in the terrain material / control-map doc; note the strength scalar and its 0..1 meaning.
- **Do NOT** touch `mclib/terrain.cpp` control-map upload (SAMPLE-1 owns it); this slice adds no texture. **Do NOT** touch `gos_terrain*indirect*` / `quad.cpp` / `gos_terrain.frag` (dead legacy path).

---

## Interactions

- **Material-lib tints (shipped):** `materialTint` consumes `tintRock/Grass/Dirt/Concrete/Snow` uniforms — already JSON-authored. This slice makes those tints *actually visible* at strength 1. Perfect composition, no conflict; they are the per-layer albedo (a) blends.
- **Snow HSV:** snow term stays HSV-derived (`frag:620-622` in the control-map branch; `snowWeight` feeds `materialTint` at `:731`). At strength 1, snow tint also becomes authoritative — desirable. The existing `mix(tintBase,0.85,snowWeight)` snow bump is subsumed by the strength `mix` (snow already lifts near 0.85; strength 1 lifts to 1.0). Verify snow maps (mc2_10) don't over-brighten — snow dampen (`:792`) still applies after.
- **Cement compositing:** cement overrides `base` (`:544-570`) and forces `matWeights`→concrete (`:667`), then `concreteColorBlend` restores the authored cement tone (`:739`). Because `concreteColorBlend` mixes `baseColor` back toward `base` AFTER our strength lift, **cement/runway colour is preserved** regardless of `u_controlAlbedoStrength`. Runways stay correct — good, do not reorder.
- **Colormap BC7 (macro variation):** at strength <1 the colormap still contributes (the point). Recommend authoring default around 0.6–0.8 for shipped maps so BC7 macro variation survives; 1.0 only for the quadrant test. "Mix, don't discard."
- **Overlay-V2 slice in flight (unit 13?):** that agent edits the SAME frag + terrain.cpp. This slice touches only `tintStrength` (`:734`) and adds one uniform — a narrow, non-overlapping region vs overlay/decal work near the cement/atlas block. Merge carefully; both add uniforms — keep additions in separate uniform-declaration lines.
- **Sand_M24 profile (`g_terrainMaterialProfile`):** widens the dirt/grass classifier in-shader (`frag:187-188`). Irrelevant when `u_useControlMap!=0` (classifier bypassed); with classifier on it only changes weights, which then feed `materialTint` — composes fine.

---

## Gate / byte-identity

- **Gate `MC2_TERRAIN_CONTROLMAP_ALBEDO` default OFF** ⇒ `terrain_control_albedo_strength_=0.0` uploaded ⇒ `tintStrength` line is algebraically `mix(X,1.0,0.0)==X` == current verbatim ⇒ byte-identical. Two-layer guard (member default + algebraic identity) as SAMPLE-1 / MATERIAL-LIB.
- The `terrain_materials.json` default (if the key is added) MUST be `controlAlbedoStrength: 0.0` so gate-ON-with-default-JSON is also identical.
- Shared uniform upload feeds the DEAD thin path too — add the loc/upload to `ThinTerrainUniformLocs` or it passes an unbound uniform.

## Debug viz

- Reuse SAMPLE-1's **DIAG bit 1024** (`frag:633-637`) = matWeights RGB. It already shows the authored weights directly — the *cause*. For the *effect*, DIAG 256 (`:605`, raw colormap) and DIAG 512 (`:736`, bypass tint) already exist as A/B toggles. No new debug mode needed; recommend documenting the trio (256 colormap / 1024 weights / 512 no-tint) as the albedo-repaint verification set.

## Acceptance

**Gate-OFF (byte-identical):** build RelWithDebInfo, deploy exe+shaders lockstep, canonical tier1 smoke (verbatim CLAUDE.md) with `MC2_TERRAIN_CONTROLMAP_ALBEDO` UNSET → exit 0, no new `crash_*`, no GL errors, shaders compile (check console — hot-reload silent-fail). Identity rests on the `mix(...,1.0,0.0)` algebraic-verbatim + member default 0.0.

**Gate-ON — MUST include static-cam human check (this is the whole point):**
- Author a **quadrant control map** (4 corners = pure rock / grass / dirt / concrete weights) via SAMPLE-1's path on mc2_24 (or a test map).
- `MC2_TERRAIN_CONTROLMAP=1 MC2_TERRAIN_CONTROLMAP_ALBEDO=1` strength 1.0 → **static-camera screenshot MUST show four visibly different colours** at the four corners (rock grey-blue / grass green / dirt brown / concrete grey). This is the human sign-off: if the corners still look the same colour, the fix failed.
- Compare against SAME map with `MC2_TERRAIN_CONTROLMAP_ALBEDO` OFF (corners look near-identical, colormap-dominated) — the before/after must be dramatic.
- Mid-strength (0.6): corners visibly tinted but BC7 macro variation still present.
- One tier1-mission smoke gate-ON (`--mission mc2_24`) → exit 0.

## Open rulings (need user)

1. **Strength authority:** single global `u_controlAlbedoStrength`, or per-layer strength (rock could repaint fully while grass stays colormap-blended)? Recommend global scalar v1; per-layer is v2 with textures (b).
2. **Default shipped strength for authored maps:** 1.0 (full repaint, ignores BC7) vs ~0.7 (blend, keep macro variation)? Recommend 0.7 shipped, 1.0 for the quadrant acceptance test only. Affects the "mix don't discard" north star.
3. **Lift globally or only in authored (`u_useControlMap!=0`) regions?** Global is simpler and SAMPLE-1 passthrough already neutralises non-authored areas; per-region gating is a refinement. Recommend global v1.
4. **JSON vs env authoring:** expose `controlAlbedoStrength` in `terrain_materials.json` (per-mission authoring) AND env override, or env-only for v1? Recommend both (JSON = author, env = iterate), matching MATERIAL-LIB precedence.
5. **Snow at strength 1:** allow the snow tint to also go fully authoritative, or clamp snow's contribution to the current 0.85 bump? Recommend let it lift (simpler, snow dampen still applies) and verify mc2_10.
