# TERRAIN-SHORELINE-MASK-1 — RECON

**Worktree:** `A:/Games/mc2-controlmap-sample-1` @ HEAD `380910d1` (prompt cited `251270f1`; overlay/controlmap agent has committed since — files below re-verified at 380910d1). **RECON ONLY** — no code, no build, no launch. One file.
**Advisor goal:** kill the mc2_17 land→water "giant blurry teal smear" via a generated shoreline distance/wetness mask (water level + terrain height) → live shader consumes: dry → damp band → wet reflective band → foam edge → shallow ramp → deep. Debug view. Gate default-OFF. Workbench contact sheet required.
**Cross-refs:** `.claude/TERRAIN-OVERLAY-V2-RECON-0.md` (this WT — the sidecar machinery this arc's terrain-side rides is ALREADY LANDED), `.claude/TERRAIN-CONTROLMAP-GENERATE-1-RECON.md`. Root `docs/superpowers/specs/2026-04-30-renderwater-shoreline-blend-handoff.md`.

> **KEY REFRAME — most of what this arc needs already exists, split across TWO subsystems that don't talk to each other today:**
> 1. **Water-SIDE shore is already sophisticated** — `gos_terrain_water_mdi.frag` (the armed MDI fast path) already computes a **world-space** Beer-Lambert depth ramp (DEEP↔SHALLOW), a shore smoothstep, camera-INDEPENDENT dual-fBm ripple/glint, and 10 debug views (mode 5 = "Shore ramp"). It is *not* the smear source. See §1.
> 2. **The smear IS the screen-space post pass** — `shaders/shoreline.frag` + `gosPostProcess::runShoreline()` (`gos_postprocess.cpp:1724`) detects "water" purely from **GBuffer1.a ∈ (0.15,0.35)** and multiplicatively brightens pixels with land neighbours in a 3-radius screen kernel. Screen-space + alpha-heuristic + no real land-distance = the blurry teal halo. See §1/§4-L1.
> 3. **The terrain-SIDE has zero shoreline treatment** and no water-level knowledge — `terrain_lod_chunk.frag:223` fakes `isWater` from colormap colour (`min(g,b)-r`), NOT from `[Water].Elevation`. The wet band / foam on the LAND side does not exist. **This is the actual gap and the v1 target.**
> 4. **The world-XY sidecar sampler this arc needs is already shipped twice** on this branch: control map (unit 12) and overlay-V2 sidecar (unit 1), both sampled by `v_worldPos` via `u_*Bounds` in the chunk frag. A shoreline mask is a **third consumer of the identical pattern** — wiring, not a new renderer. See §4.

---

## 1. Water / shoreline pipeline TODAY (file:line)

### 1a. Water render = SEPARATE overlay pass (NOT terrain splat) — confirmed
- Armed MDI fast path: VS `shaders/gos_terrain_water_fast_mdi.vert`, FS `shaders/gos_terrain_water_mdi.frag`; non-MDI twins `gos_terrain_water_fast.vert`. Driven by `Terrain::renderWaterFastPath` (`mclib/terrain.cpp`, also `quad.cpp`), compute feeder `shaders/gpu_driven_water.comp`. Distinct draw from the terrain chunk pass.
- **Authoritative water level** = `elev <= [Water].Elevation` from the mission `.fit`. Analyzer reads it at `tools/terrain_beautify/mission_terrain_analyzer.py:178-210` (`read_water_elevation` → `mclib/terrain.cpp` `waterElevation = readIdFloat("Elevation")`). VS uniform `waterElevation` + `alphaDepth` (`gos_terrain_water_fast_mdi.vert:68-69`). Per-vertex `PostcompVertex.water` byte is a **packed alpha, NOT a bool** (analyzer trap, comment at analyzer:180-186).

### 1b. Water FS already does depth ramp + shore + camera-independent detail (the good part)
`gos_terrain_water_mdi.frag`:
- Beer-Lambert transmittance `trans = exp(-WaterThickness*ABSORPTION_DENSITY)`; `waterCol = mix(DEEP_COLOR, SHALLOW_COLOR, trans)` (:146-147). SHALLOW default `(0.22,0.45,0.38)` teal, DEEP `(0.03,0.13,0.20)` — user-approved.
- Shore ramp `shore = smoothstep(-shoreBlend*0.5, shoreBlend, WaterThickness)` with `shoreBlend = max(alphaDepth,1.0)`; `if (shore<=0) discard` (:153-155). Final alpha `= shore * WATER_MAX_ALPHA` (:255).
- Camera-INDEPENDENT dual-fBm ripple (`RIPPLE_GAIN` brighten-only) + crest `GLINT` (:141-168) — explicitly f(WorldPos,time), NOT view angle.
- Camera-DEPENDENT sky reflection is **gated** (`u_waterReflStrength`, default 0) — the shelved S3 terrain reflection's math reused with SH-L2 sky source (:175-228). Comment (:178-183) records the **2026-05-18 rule supersession**: camera-dependence rejection was a *quality* verdict on the ground-colormap SOURCE, not a blanket principle. **For this arc, treat perceptible camera-dependence as still-forbidden by default** (advisor ruling stands unless user reopens); any wetness/foam MUST be f(WorldPos, time) only.
- **10 debug views** already present (`u_waterDebugMode` 0-9; 1=Tint 2=Alpha 4=Depth **5=Shore** 6=Lighting 7=SkyRefl). New water-side shoreline terms should extend this enum, not invent a parallel one.

### 1c. Beach-tile extension (shipped 2026-05-24) — confirmed, DON'T fight it
`gos_terrain_water_fast_mdi.vert:198-205`: above-water shore verts (`WaterThickness = waterElevation - velev < 0`) are seated ON the terrain surface, height **capped** `min(velev, waterElevation + shoreBlendVS)` (`shoreBlendVS = max(alphaDepth,1.0)`) so cliff corners don't tent the bilinear quad. FS shore smoothstep accepts negative `WaterThickness` and fades them out. This is why the water quad already creeps a tint onto the beach. **The "dead at alphaDepth=0" landmine:** `shoreBlend`/`shoreBlendVS` collapse when `alphaDepth==0` → shore band degenerates; the `max(...,1.0)`/`max(...,0.5)` guards are the fix already in place — any new band width MUST keep a floor.

### 1d. Screen-space shoreline foam pass = the smear (v1 should REPLACE/SUPERSEDE its role)
`gosPostProcess::runShoreline()` `gos_postprocess.cpp:1724-1765`, shader `shaders/shoreline.frag`:
- Water detected from `sceneNormalTex_` (GBuffer1) **alpha ∈ (0.15,0.35)** (:20-25); land = neighbour alpha>0.5 sampled at 3 radii × 8 dirs in **screen space** (:36-42).
- `shoreIntensity` from land-neighbour count, animated FBM foam + a `sin(time + length(TexCoord-0.5)*50)` **radial screen wave** (:59) → **multiplicative brighten** (`glBlendFunc(GL_DST_COLOR, GL_ZERO)`). No land-distance in world units, no wet darkening, no colour ramp; blurs across depth discontinuities. **This is the teal halo.**
- Gated on `shorelineEnabled_` (default true, :170) **and `sceneHasTerrain_`** — set by `markTerrainDrawn()` (:936 reset each frame). **Landmine:** the whole shoreline concept dies silently if the chunk draw ever early-returns past `markTerrainDrawn`.

### 1e. Depth / z-fight lockstep (load-bearing)
Water clip bias is **pre-divide, vertex-stage**: `clip.z += WATER_DEPTH_FUDGE_FAST * clip.w` (`gos_terrain_water_fast_mdi.vert:285`), single-sourced `shaders/include/terrain_depth_bias.hglsl`. Ordering `WATER < TERRAIN < OVERLAY`. A terrain-side wet band rides terrain depth (no new bias). Any water-side change must NOT touch the fudge.

### 1f. Analyzer already emits water + shoreline masks (offline)
`mission_terrain_analyzer.py`: `water = elev <= water_elev` (:206); `shoreline = land & dilate(water) & ~water` (:228) — a **1-cell binary land-ring, NOT a distance/wetness field**. Saved `masks/water.png`, `masks/shoreline.png` (:392-393). Cooked for mc2_01 & mc2_24 (`tests/terrain/beautify/*/masks/`); **mc2_17 not yet cooked** (must add). `WORLD_UNITS_PER_VERTEX` and `mapTopLeft` conventions live here for the world-XY raster.

---

## 2. Mask generation design

**Signal wanted:** signed distance (world units) to the water edge, on BOTH sides (land +, water −), so one field drives the whole ladder. From that: dry (`d > damp_w`), damp-darkened band (`0 < d < damp_w`), wet reflective band (`−wet_w < d < 0` land-adjacent shallow), foam edge (`|d| < foam_w`), shallow-water colour by `d` (or reuse water FS `WaterThickness`), deep.

**Recommendation: OFFLINE python EDT (primary), with a runtime-load fallback documented — do NOT compute per-frame.**
- Offline: extend `mission_terrain_analyzer.py` — from `water = elev<=water_elev`, compute a **Euclidean distance transform** of both `water` and `land` (`scipy.ndimage.distance_transform_edt`, already a dep of the beautify tool), combine to signed distance × `WORLD_UNITS_PER_VERTEX`. Emit a NEW cook `tools/terrain_beautify/cook_shoreline.py` → `data/missions/<stem>.beauty/shoreline_mask.png`.
- **Channel layout (single RGBA, mirrors control/overlay sidecars):** R = normalized signed distance (0.5 = waterline; <0.5 water, >0.5 land, scaled by a band-width const baked into the header/const); G = wet weight (0..1, land-side damp ramp); B = foam weight (edge lobe); A = valid/coverage (0 outside mission bounds). Single channel (distance only) is *insufficient* — the wet/foam ramps want independent widths per art tuning, and packing them offline is free. **Multi-channel RGBA recommended.**
- **Bounds header:** ship a `u_shorelineBounds vec4 (topLeftX, topLeftY, sizeX, sizeY)` exactly like `u_overlayBounds` (world-XY sampler, off-128wu-grid). Reuse the analyzer's `mapTopLeft`.
- **Runtime-load-only fallback (NOT per-frame compute):** the map is 120²-ish; an at-load EDT from `blocks[]`/`elev` in C++ is *cheap in principle* but re-implements scipy's EDT in C++ and duplicates the analyzer's water-level parse — **reject for v1**; offline cook is deterministic, testable (pytest byte-identical), and matches the two shipped sidecar precedents. Document runtime-compute as a later option only if per-mission cook friction appears.

---

## 3. Consumption split (terrain-side vs water-side)

**VERDICT: terrain-side-only v1 — and it plausibly kills the smear on its own.** The mc2_17 "smear" is the LAND↔water seam and the screen-space foam halo (§1d). The water FS already ramps colour by depth (§1b); it is not the primary offender. A world-space **wet-darkened + foam band on the LAND side of the seam**, sampled in the chunk frag, replaces the fake screen halo with a crisp, depth-correct, camera-independent edge. Ship that first; measure; only then decide if the water FS needs the mask.

- **Terrain-side (v1):** new block in `terrain_lod_chunk.frag` after the material/base composite, before final lighting output. Sample `shoreline_mask` by `v_worldPos` via `u_shorelineBounds` (copy the `u_useOverlaySidecar` block at :600-609 verbatim in shape). Apply: multiply base albedo by a wet-darken factor in G-band (wet sand/rock is darker + slightly more saturated), add procedural foam (B-band × noise, see §3-foam) as a bright rim, feather by R-distance. Gate `u_useShoreline`. **Also: retire/suppress `runShoreline()` when the terrain-side mask is active** (make `shorelineEnabled_` yield to the gate) so the screen halo and the new band don't double up.
- **Water-side (v2, deferred):** the water FS has `WorldPos` + `WaterThickness` already; it could sample the same `shoreline_mask` for a shallow foam lip on the water side of the seam. **Does the water fast path have per-pixel land-distance?** Not today — only `WaterThickness` (vertical column), which is a *proxy* for horizontal distance only on gentle slopes. The mask gives true horizontal distance → a cleaner water-side foam lip. Extend `u_waterDebugMode` enum. **Defer** until terrain-side is proven.

**Terrain frag lacks water level today** (`isWater` faked from colour, :223). v1 does NOT need to add `waterElevation` to the chunk frag — the mask *encodes* the water relationship offline. Keep the chunk frag water-level-agnostic; the mask is the only water input.

---

## 4. Foam

- **Procedural noise band in-frag** (not a texture): the chunk frag already `#include`s fbm/noise (used at :466-467) and the water FS proves the dual-counter-scroll fBm pattern. Reuse `fbm(v_worldPos.xy * freq + timeScroll)`.
- **TIME animation IS allowed and is distinct from camera-dependence.** The camera-independence ruling forbids f(view angle/position); it does NOT forbid f(time). The water FS animates ripples/glint by `time` while explicitly camera-independent (`gos_terrain_water_mdi.frag:55,141`). **Foam may scroll/pulse with `time`; it must NOT vary with camera.** (Flag for user confirmation only because it's a ruling boundary, but precedent is unambiguous.)
- Static noise texture rejected: extra unit + no animation + seams; procedural fBm on continuous `v_worldPos` is seam-free and free.

## 5. Binding / gate plan

- **Texture unit: chunk unit 2** (free after overlay-V2 took unit 1). Occupied: 0 colormap, 1 overlay sidecar, 3 cement, 5 matNormalArray, 9/10 shadows, 11 transition mask, 12 controlmap, 13 dyn map. Free: **2**, 4, 6, 7, 8. Add `constexpr int TERRAIN_SHORELINE_TEXUNIT = 2;` in `gos_terrain_lod_chunk.h` (mirror `TERRAIN_OVERLAY_SIDECAR_TEXUNIT` at :31). C++ owner: static `s_shorelineTex`/`s_shorelineSide` + `s_locUseShoreline`/`s_locShoreline`/`s_locShorelineBounds` (mirror `s_controlMapTex` block, `gos_terrain_lod_chunk.cpp:98-99,199-200,605-606,657-661`; bind block mirror :1046-1056).
- **Gate: `MC2_TERRAIN_SHORELINE` default OFF** — NONE exists today (grep clean). Read pattern = `MC2_TERRAIN_CONTROLMAP` static-lambda at `mclib/terrain.cpp:832`, `.beauty` sidecar load at :824-856. `MC2_TERRAIN_SHORELINE_FILE` override (precedent `MC2_TERRAIN_CONTROLMAP_FILE` :840). Register in flags yaml + `docs/tier1_env_vars.md` (`scripts/check-env-vars-documented.py` will flag it).
- **Byte-identity via uniform else-branch:** gate off / no sidecar → `s_shorelineTex==0` → `u_useShoreline=0` → whole frag block skipped → verbatim. NOT "bound-but-unsampled." Mirror the control-map identity comment (:1043-1045).
- **Debug viz:** add `u_diag` bit (chunk frag uses a `u_diag` bitmask, e.g. :615,624,642,670) → visualize R-distance / G-wet / B-foam / A-valid as color. For water-side later, extend `u_waterDebugMode`.

## 6. Interaction

- **Reuse the overlay-V2 world-XY sampler shape** (`terrain_lod_chunk.frag:600-609`): identical `ovUV = (v_worldPos - bounds.xy)/bounds.zw` math, in-bounds guard. Factor a shared `worldXYToSidecarUV(bounds)` helper OR just copy (three near-identical blocks now — a small helper is the clean move, but keep it additive/gated).
- **Displacement:** `MC2_TERRAIN_VISUAL_DISPLACE` (binding 26) bumps terrain Z; because the mask is sampled IN the chunk frag on `v_worldPos.xy`, the wet band **follows the displaced surface automatically** (same structural win the overlay recon notes, §3.3 there). No z-fight by construction (unlike the old flat overlay pass). The water quad's beach-extension (§1c) is Z-capped independently — compose, don't fight: mask wet-darkens the LAND texels; water quad handles the wet SURFACE. Verify they meet cleanly at the waterline.
- **Cement/mine/control overlays:** mask composites in albedo space AFTER base/cement/overlay-sidecar (:600-609) and BEFORE lighting. Cement pads at the shoreline should probably NOT get wet-sand darkening — gate the wet term by `!cementHit` (the overlay block sets `cementHit`, :607). Mines ride the separate overlay pass — untouched.
- **Beach-tile extension already shipped (§1c) — compose:** the mask's land-side band should visually continue the water quad's beach creep, not overlap-brighten it. Tune `damp_w`/`foam_w` so the seam is continuous.

## 7. Acceptance

- **Offline pytest** (`tools/terrain_beautify/`): `cook_shoreline.py` deterministic (seed → byte-identical PNG); signed-distance sign/zero-crossing correct on a synthetic pond fixture; mc2_17 + mc2_24 cook produces non-empty foam/wet bands. Analyzer water-level parse unchanged.
- **Workbench contact sheet** (`terrain_workbench.py`): shoreline_mask channels over `height.png` + `water.png` for mc2_17/mc2_24 — REQUIRED (advisor).
- **Gate-OFF:** `MC2_TERRAIN_SHORELINE` unset → canonical tier1 smoke (verbatim from CLAUDE.md; `tier1 = mc2_01 mc2_03 mc2_10 mc2_17 mc2_24` — **mc2_17 IS in tier1**, confirmed) exit 0, no new `crash_*`/GL errors, byte-identical.
- **Gate-ON static-cam:** fixed-position shot on mc2_17 (has water — the smear reference) land→water seam; before/after; debug-view screenshots of each channel. Deploy exe+shaders lockstep (hot-reload silent-fail — check console).
- **slice_gate:** one `--mission mc2_17` gate ON → exit 0; wet band + foam render; water quad + mines still render; no z-fight on the seam.

## 8. Landmines

1. **markTerrainDrawn (`gos_postprocess.cpp:936`, chunk `pp->markTerrainDrawn()`)** — gates the screen-space shoreline AND godrays/cloud-shadow via `sceneHasTerrain_`. Any early-return in a new chunk-frag/C++ branch that skips it silently kills those. Keep unconditional.
2. **Camera-independence ruling (advisor).** Wetness/foam MUST be f(WorldPos, time) ONLY — never view angle/position. TIME animation is explicitly OK (water FS precedent, `gos_terrain_water_mdi.frag:55,141`). The gated water-side SKY reflection (`u_waterReflStrength`) is the ONE sanctioned camera-dependent term and stays default-OFF — do not enable it as part of this arc.
3. **Depth-bias lockstep** (`WATER_DEPTH_FUDGE_FAST`, `terrain_depth_bias.hglsl`, pre-divide vertex-stage, `WATER<TERRAIN<OVERLAY`). Terrain-side mask rides terrain depth — introduce NO new bias. Don't touch the water fudge.
4. **"Dead at alphaDepth=0" / band-width floor.** All shore widths (`shoreBlend`,`shoreBlendVS` = `max(alphaDepth,1.0)`/`max(...,0.5)`) have floors for a reason (`gos_terrain_water_fast_mdi.vert:202`, frag:153). Any new `damp_w`/`wet_w`/`foam_w` derived from `alphaDepth` MUST keep a floor or the band degenerates on `alphaDepth==0` missions.
5. **Two water shader variants** — MDI (`*_mdi.vert/frag`, armed) AND non-MDI (`gos_terrain_water_fast.vert`). Any water-SIDE change (v2) must touch BOTH or gate to the armed path. (Terrain-side v1 avoids this entirely.)
6. **Double-shore.** If both the new terrain-side band and the legacy `runShoreline()` screen pass are active, the seam brightens twice. v1 MUST yield `shorelineEnabled_`/skip `runShoreline()` when `MC2_TERRAIN_SHORELINE` is on.
7. **Analyzer `shoreline` mask is a 1-cell dilate, NOT a distance field** (:228). Do not feed it directly as the mask — it has no band width. The new cook computes a real EDT.
8. **`quadList` = camera sliding window** — the per-frame narrow water set is not cacheable, but this is irrelevant to an offline-cooked, world-XY-addressed mask (mask is mission-static, sampled by world pos). Noted so no one tries to key the mask to the water quad set.
9. **PostcompVertex.water byte = packed alpha, not bool** (analyzer:180). Mask gen must use `elev <= [Water].Elevation`, never the vertex water byte.
10. **Concurrent overlay/controlmap agent** edits `terrain_lod_chunk.frag`/`terrain.cpp` on this branch (HEAD moved 251270f1→380910d1 during recon). Slice-preflight before coding; the sidecar bind blocks (:600-609, :1046-1078) are the churn zone — rebase-sensitive.

## 9. Open rulings (need user)

1. **Terrain-side-only v1 sufficient?** Recommend YES (ship terrain wet+foam band, retire screen `runShoreline()`, measure on mc2_17). Add water-side foam lip only if the seam still reads wrong.
2. **Foam time-animation** — confirm OK (precedent says yes; flagged only because it's a ruling boundary).
3. **Mask channels** — RGBA (dist/wet/foam/valid, recommended) vs single-channel distance + all ramps derived in-shader from consts?
4. **Retire vs coexist with `runShoreline()`** — recommend the terrain-side gate suppresses the screen pass (kills the smear at its source); or keep screen pass for the water-INTERIOR sparkle and only replace the land seam?
5. **Mask locus** — offline cook (recommended, deterministic/testable) vs at-load C++ EDT (cheaper per-mission friction, but duplicates analyzer + scipy)?
6. **Cement-at-shoreline** — wet-darken concrete pads on the beach, or exclude via `cementHit` (recommend exclude)?
7. **mc2_17 cook** — add mc2_17 to the beautify cook set (currently only mc2_01/mc2_24 cooked) as part of this slice.
