# TERRAIN-CHUNK-POM-1 — RECON (design only; no code/build/launch)

Slice: give the **LIVE chunk terrain** (`shaders/terrain_lod_chunk.frag`, driver
`GameOS/gameos/gos_terrain_lod_chunk.cpp`) real view-dependent parallax so close/mid
rock+cliff surfaces get depth. `gos_terrain.frag` is the DEAD path — do NOT touch it
except as the reference oracle (its POM math is the template).

Worktree read at HEAD `7cf11031` (`A:/Games/mc2-controlmap-sample-1`).

---

## 0. HEADLINE — POM scaffold ALREADY EXISTS on the chunk path; it is just BLIND

The chunk frag already has the full POM machinery, ported earlier:
- `chunkSampleDisplacement()` frag:268-278 (weight-blended `.a` displacement, `textureLod(...,0)`).
- `chunkParallax()` frag:280-300 (16-layer ray-march, occlusion interp).
- POM branch in `chunkDetailNormal()` frag:334-340, gated `pomParams.x>0 && fwRock>0.4`.
- `sampleAntiTileArr()` frag:257-267 (textureGrad, UB2-safe).
- `pomParams` uniform frag:204, uploaded driver:1194 `glUniform4f(..., gos_GetTerrainPOMScale(), 8, 32, 0)`.
- `terrain_pom_scale_ = 0.02f` **DEFAULT NON-ZERO** (gameos_graphics.cpp:2455) — so the
  branch is *live today* but with a **hardcoded faux view dir** `viewDirTS = vec3(0.15,0.85,0.15)`
  (frag:281). That constant shear is why the user reports "no POM" on the Gaea mountain:
  every fragment gets the SAME tiny fixed UV offset regardless of where the camera is —
  no view-dependent depth, no cliff self-occlusion, invariant under camera motion.

**Therefore the slice is small and precise: replace the constant `viewDirTS` with a real
per-fragment tangent-space view vector derived from cameraPos, and put the whole thing
behind a proper default-OFF gate (there is currently NONE — see §5).** The ray-march loop,
displacement source, and UB2 discipline are already correct and stay.

---

## 1. View-vector plan (AXES — the trap)

### What the chunk frag/vert have TODAY
- vert (`terrain_lod_chunk.vert`): inputs `localOffset(ivec2)`, `isSkirtFlag`; uniforms
  `u_blockOriginX/Y`, `u_mapSide`, `u_halfMap`, `u_skirtDepth`, **`u_worldToClipGL`(mat4)**,
  stitch uniforms, `u_visualDisplace/Side`. Emits `v_worldPos(vec3)`, `v_terrainType(float)`.
  **NO cameraPos, NO view/inverse matrix** — confirms the deferral note "chunk has none".
- frag: has `v_worldPos` (world xyz), `terrainLightDir`, all material uniforms, height SSBO
  (binding 23), but **NO cameraPos and NO eye vector**.

### The cameraPos convention (Stuff/MLR trap — DO NOT get this wrong)
Authoritative reference is the DEAD frag `gos_terrain.frag:319-325`, verbatim:
```
// cameraPos is in Stuff/MLR space: .x=left/right, .y=elevation, .z=forward
// WorldPos   is in raw MC2 space:  .x=east, .y=north, .z=elevation
vec2 camGround = vec2(-cameraPos.x, cameraPos.z);   // Stuff(x,z) -> MC2(east,north)
float altBoost = max(cameraPos.y - WorldPos.z, 0.0) * 0.7;
```
So to reconstruct the MC2-world camera position from the `cameraPos` uniform:
```
camWorld.x (east)      = -cameraPos.x
camWorld.y (north)     =  cameraPos.z
camWorld.z (elevation) =  cameraPos.y
```
`v_worldPos` is already MC2 world (east,north,elev) — vert:141-143 `worldX=col*128-halfMap`,
`worldY=halfMap-row*128`, z=height. So:
```
vec3 camWorldMC2 = vec3(-cameraPos.x, cameraPos.z, cameraPos.y);
vec3 V_world     = normalize(camWorldMC2 - v_worldPos);   // fragment -> eye
```

### Tangent space for POM
The chunk detail UVs are **top-down planar** (`worldXY * tiling`), i.e. tangent = +X(east),
bitangent = -Y(north) [because uv.y uses `-worldY`, matching frag:471/622], normal = +Z(up).
So the TS view vector is simply:
```
vec3 viewDirTS = vec3(V_world.x, -V_world.y, V_world.z);   // (tangent=+east, bitangent=-north, up)
```
and `viewDirTS.y` in the existing `chunkParallax` is the **.z(up)** component — which is
exactly what the current fixed `0.85` faux value stood in for. This makes the existing
`P = viewDirTS.xz / max(viewDirTS.y,0.001)` math correct with a real vector: `.y`=height
component (steepness), `.xz`→ note the swizzle: existing code uses `viewDirTS.xz` as the
GROUND plane and `.y` as up. So map real vector as `viewDirTS = vec3(east, up, -north)` to
match the existing `.xz`=ground / `.y`=up layout. **RULING R1 (below): pin the exact swizzle
against the dead-frag oracle by A/B, do not eyeball it.**

### Verdict
**Add `cameraPos` (vec4) to the CHUNK path.** Cheapest wiring: the accessor
**`gos_GetTerrainCameraPos(float*,float*,float*)` already exists** (gameos_graphics.cpp:9379,
reads `terrain_camera_pos_` — the SAME vec4 the legacy terrain frag uses at 6417/6896/7068).
The chunk driver already uses this exact `extern` accessor pattern (gos_terrain_lod_chunk.cpp:36,
186). Plan:
1. `extern void gos_GetTerrainCameraPos(float*,float*,float*);` in the chunk driver (not yet in
   `gameos.hpp` header — one-line extern like the others; optionally also add the decl to
   `gameos.hpp` near gos_GetTerrainMVPMat4 for cleanliness).
2. `uniform vec4 cameraPos;` in the frag (NAME MUST match the legacy `"cameraPos"` for zero
   confusion); cache loc `s_locCameraPos`, upload once/frame beside the mvp upload (driver:876).
3. Frag computes `V_world`/`viewDirTS` as above and feeds it to `chunkParallax`.
No vert change needed (frag already has `v_worldPos`; cameraPos is a uniform). No new SSBO.

---

## 2. Height-source matrix (what drives the parallax depth)

`chunkSampleDisplacement()` reads the **`.a` (alpha) channel of `matNormalArray`** per layer:
| Layer | idx | `.a` displacement today | POM v1? |
|---|---|---|---|
| ROCK | 0 | sampled (frag:274) | **YES** (primary target — Gaea rock/cliff) |
| GRASS | 1 | sampled (frag:275) | optional (low relief; keep but small scale) |
| DIRT | 2 | NOT summed (dead frag notes "dirt: blank alpha, no POM shift") | NO |
| CONCRETE | 3 | sampled (frag:276) | NO for v1 (runways are flat authored slabs; POM would fight the cement atlas + decals) |
| SNOW | 4 | — | NO |
| **MARBLE_CLIFF** | 5 | `mat5_normal.tga` `.a` = **displacement** (the "marble cliff" the prompt names); used by triplanar block frag:817-827 | **candidate** — but reached via `useTriplanarCliff` triplanar path, NOT the top-down `chunkParallax`. See R2. |

- **material-lib JSON height field:** `terrain_materials.json` reserves a height field but it is
  NOT yet wired to a per-layer POM scale uniform (only roughness/AO via `matRoughness`/`matAO`,
  `u_useMaterialLib`). v1 keeps displacement source = the array `.a` channel (already loaded);
  do NOT block on JSON plumbing.
- **v1 layer scope: ROCK only (+ tiny grass), driven by weight `w.x`.** This matches the user's
  ask ("close/mid rock+detail get depth"). The existing `pomScaleMat = vec4(1,1,2.5,1)` (frag:335)
  already biases dirt; keep rock-dominant. Concrete/cement explicitly EXCLUDED (`pureConcrete`
  path already suppresses detail; POM must stay off there or runways/decals tear).

---

## 3. Ray-march budget + distance cutoff

Context: this frag ALREADY samples colormap(9-tap blur)+detail-normal(5 layers, some anti-tile
3-tap)+transition+cement(up to 8 neighbor SSBO reads)+control+overlay+shoreline+fbm×2. It is a
heavy frag. POM adds an N-iteration loop each doing a weighted `textureLod` (rock weight branch).

- **Loop bound:** existing `chunkParallax` caps at **16 layers** (frag:290), `numLayers` from
  `mix(pomParams.y=8, pomParams.z=32, up)` clamped 4..16. Keep 16 hard cap. On 7900 XTX @1440p
  this is fine for a NEAR-only band; the risk is applying it map-wide.
- **Distance cutoff (the real cost governor):** the branch is already `fwidth`-gated
  (`fwRock>0.4`, frag:334) — sub-pixel tiling → skip. This is a *screen-space* proxy for
  distance and is the cheap early-out. **ADD an explicit world-distance cutoff** using the new
  `cameraPos`: compute `camDist = distance(v_worldPos.xy, vec2(-cameraPos.x, cameraPos.z))`
  (+altBoost per dead frag:325) and **fade POM strength 1→0 across [POM_NEAR .. POM_FAR]**
  (suggest ~1500 .. 3500 wu; 1 tile=384wu). Multiply `pomOff` by both `fwRock` AND `distFade`.
  Beyond POM_FAR: numLayers→0 / skip loop entirely (uniform-flow early-out on the fade scalar
  being 0 — but keep the loop bound compile-constant; gate by `if(distFade>0.001)`).
- **Early-out on flat weights:** already present (`if (w.x>0.01)` etc. + `fwRock>0.4`). Add
  `pomParams.x>0 && distFade>0` so gate-OFF and far both skip the loop with zero samples.
- **LOD-step cutoff:** `u_lodStep` is available (1=LOD0..20=LOD5). Cheap belt-and-suspenders:
  only run POM for `u_lodStep <= 2` (LOD0/1), matching the existing detail-normal LOD fade
  (`detailNormalStrength.z` path frag:754-759). Coarse chunks are far → no POM.

**Budget verdict:** ~4–16 extra textureLod/frag in a NEAR ring only (fwidth + world-dist +
lodStep triple-gated). Expected cost bounded to the close rock band; measure (see §6). If
over budget, drop grass POM and cap numLayers at 8 near-only.

---

## 4. Landmines

- **UB2 (non-uniform-branch texture fetch):** ALREADY handled correctly. `chunkSampleDisplacement`
  uses `textureLod(...,0.0)` (frag:274-276) inside the data-dependent march loop — valid in any
  control flow. `sampleAntiTileArr` uses explicit-gradient `textureGrad` (frag:257-267). The POM
  *offset* is computed in `chunkDetailNormal` and then applied to per-layer UVs whose sampling is
  `textureGrad` with uniform-scope gradients (frag:342-352, 365-388). **DO NOT introduce a plain
  `texture()` inside the weight/POM branch** — keep textureLod/textureGrad.
- **gl_FragDepth (AMD early-Z landmine):** **DO NOT write `gl_FragDepth`.** POM is SHADING-only
  parallax — the silhouette/geometry stays on the real displaced triangle. The frag header
  (frag:112-115, 394-398) explicitly documents that writing `gl_FragDepth` disabled early-Z/Hi-Z
  on AMD and caused decal tearing at the cement boundary (greybeard META-FIX,
  `vulkan_aligned_depth_bias_ruling.md`). Terrain depth bias is applied PRE-DIVIDE in the vert
  (`clip.z += 2*FUDGE*clip.w`, currently FUDGE=0). POM must not touch depth.
- **Shadow interaction:** terrain uses `calcShadow`(static) + `calcDynamicShadow`(dynamic) sampled
  at `v_worldPos` (frag:872-874). Because POM does NOT move geometry or depth, the shadow lookup
  stays at the true surface point — **no self-shadow mismatch from POM depth offset** (this is the
  benefit of not writing FragDepth). POM-self-shadowing (marching the light ray) is OUT of scope
  for v1; only the shading UV is parallaxed.
- **Skirts:** `u_skirtDepth>0` fragments already force `N=vec3(0,0,1)` and take the else-branch;
  POM lives in `chunkDetailNormal` which is only called in the non-skirt path (frag:729-731). Skirts
  are untouched — good.
- **Concrete/cement:** POM must stay suppressed where `pureConcrete>0` (detail already ×`(1-pureConcrete)`
  frag:731). Confirm the new distFade/viewdir path does not sneak POM onto cement tiles (runway/decal
  tearing risk). Keep the existing `pomScaleMat`/weight gating rock-dominant.
- **Byte-identity:** `terrain_pom_scale_` is 0.02 by DEFAULT (not 0) — so simply wiring a real
  view vector would CHANGE stock pixels immediately (not byte-identical). This is why a NEW
  default-OFF gate is mandatory (§5): the real-view-vector path must be behind `MC2_TERRAIN_POM`,
  and with the gate OFF the frag must reproduce EXACTLY today's output (fixed faux viewDir, or —
  cleaner — POM fully off). **RULING R3.**

---

## 5. Gates / knobs / debug viz

- **NEW gate `MC2_TERRAIN_POM` (default OFF).** There is currently **NO** such env gate anywhere
  (grep: `terrain_pom_scale_` is a plain `0.02f` member set only via `setTerrainPOMParams`, no env).
  The driver reads it once (static bool) and uploads `pomParams.x = gate ? gos_GetTerrainPOMScale()
  : 0.0`. With `pomParams.x==0` the frag's `if (pomParams.x>0.0 ...)` branch (frag:334) never
  runs → **byte-identical to no-POM**. This also cleanly resolves R3: gate OFF = POM off (the
  0.02 default that ships today produces only the blind faux-shear that the user calls "no POM"
  anyway, so turning it fully off on the gate-OFF path is a safe/defensible identity baseline —
  confirm with maintainer, R3).
- **Strength knob:** `MC2_TERRAIN_POM_SCALE` (float) → overrides `gos_GetTerrainPOMScale()`/
  `terrain_pom_scale_`, feeds `pomParams.x`. Default 0.02 (or retune for real view vector — the
  faux-0.85 up meant the old scale was effectively divided by ~0.85; a real grazing view makes
  `P` larger, so the visible offset scale changes — expect to RETUNE, R1).
- **Steps knob:** `MC2_TERRAIN_POM_STEPS` (int) → `pomParams.y/.z` (min/max layers), clamp 4..16.
- **Distance knob:** `MC2_TERRAIN_POM_NEAR` / `_FAR` (wu) for the fade band (§3). Uploadable as a
  new `vec2 u_pomDist` uniform, or fold into `pomParams` if a 4th slot frees up (`.w` is currently
  unused=0.0).
- **Debug viz:** add a `u_diag` bit (the frag already has a bitmask: 1,2,4,8,16,32,64,128,256,512,1024).
  Suggest **bit 2048 = visualize POM UV offset** (`pomOff` magnitude as heat) and/or **viewDirTS as
  color** — early-return like the existing diag blocks (frag:621-636, 648-652). Also reuse
  `u_lightingDebugView==41` (normal) to confirm the parallaxed normal reads.

---

## 6. Acceptance

- **Static-cam close-up rock shot:** load a Gaea high-relief map (see `TERRAIN-GAEA-RELIEF-1`,
  commit 077526e3) or mc2_24/mc2_01; park camera close+low on a rock/cliff face; capture
  gate-OFF vs `MC2_TERRAIN_POM=1`. Expect visible depth/self-occlusion that MOVES with camera
  orbit (the current fixed-viewDir shear does NOT move — that's the pass/fail tell). Grazing
  angle should deepen the parallax; top-down should flatten it.
- **slice_gate / byte-identity proof:** run the tier1 smoke gate VERBATIM (CLAUDE.md canonical
  command) gate-OFF → must be byte-identical to pre-slice (pixel-diff 0, since `pomParams.x=0`
  short-circuits the branch). Use `slice-preflight` before coding (symbols: `chunkParallax`,
  `pomParams`, `gos_GetTerrainCameraPos`, paths `shaders/terrain_lod_chunk.frag`,
  `GameOS/gameos/gos_terrain_lod_chunk.cpp`).
- **fps delta measurement:** Tracy is always compiled (`TRACY_ENABLE`). The chunk draw is inside
  the terrain-solid pass zone (coarse per-pass, per the 100ns-floor rule — do NOT add per-fragment
  zones). Measure frame time gate-OFF vs gate-ON at the SAME static close-up cam (Tracy frame
  histogram, or the smoke fps log). Also A/B `MC2_TERRAIN_POM_NEAR/_FAR` to confirm the far band
  is truly skipping the loop (fps should recover when the rock leaves the near ring).

---

## 7. Open rulings (need maintainer sign-off before coding)

- **R1 — exact TS swizzle + scale retune.** Pin `viewDirTS = vec3(?, up, ?)` against the DEAD
  `gos_terrain.frag` POM as oracle by A/B on the same map/cam; the real grazing view will change
  the effective offset magnitude vs the old fixed 0.85 — expect to retune `pomParams.x`. Do NOT
  eyeball the axis mapping; the east/north/up ↔ Stuff(left/elev/fwd) swap is the memory trap.
- **R2 — cliff layer routing.** Rock POM via the top-down `chunkParallax` (uv=worldXY) STRETCHES
  on near-vertical cliff faces (same reason `useTriplanarCliff` exists for the normal). Ruling:
  v1 = top-down rock POM only (accept stretch on the steepest faces); OR gate POM behind
  `macroNz` (skip POM where the triplanar cliff path is active, to avoid double-relief).
  Recommend: **run POM on the sloped-but-not-vertical band, let triplanar own the cliff wall.**
- **R3 — gate-OFF identity baseline.** Confirm gate OFF = POM fully off (pomParams.x=0) is the
  accepted "byte-identical" reference, given stock currently ships the blind faux-shear at 0.02.
  (Recommended: yes — the faux path is the "no POM" the user is complaining about.)
- **R4 — put `MC2_TERRAIN_POM_*` in `docs/tier1_env_vars.md`** and the invariant scripts? (gate
  is default-OFF cosmetic; likely tier-N not tier1, but log it.)
- **R5 — header decl.** Add `gos_GetTerrainCameraPos` to `GameOS/include/gameos.hpp` (next to
  `gos_GetTerrainMVPMat4`, line ~2375) or keep as a local `extern` in the chunk driver like the
  other 20+ terrain accessors? (Consistency: local extern is the established pattern here.)

---
### Files
- `shaders/terrain_lod_chunk.frag` (POM scaffold: 204,257-300,334-340; view-vec + gate goes here)
- `shaders/terrain_lod_chunk.vert` (no change needed)
- `GameOS/gameos/gos_terrain_lod_chunk.cpp` (uniform upload: mvp 876, pomParams 1194; add cameraPos + gate)
- `GameOS/gameos/gameos_graphics.cpp` (accessor `gos_GetTerrainCameraPos` :9379, `terrain_pom_scale_` :2455)
- `shaders/gos_terrain.frag` (DEAD — ORACLE ONLY: cameraPos convention 319-325, parallaxMapping 284-306)
- `shaders/include/terrain_mat_layers.hglsl` (MARBLE_CLIFF=5 displacement layer)
