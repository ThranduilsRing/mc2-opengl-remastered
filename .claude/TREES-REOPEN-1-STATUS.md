# TREES-REOPEN-1 — STATUS

Lane `TREES-REOPEN-1` · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
No repo-content commits, **no build, no deploy, no game launch** (launches are
hard-blocked by `.claude/hooks/block_game_launch.py`; the block was respected).
Evidence dir: `A:/Games/mc2-main/scratch/treesreopen1/` (scripts + decoded crops).
Contracts measured this session: **89 passed / 1 failed** — see §7.

**Headline.** The prior lane's "TREES: CLOSED, NO DEFECT — the silhouettes are
baked colormap art from `burnInShadows`" is now **refuted by measurement**, not
merely unsupported. The baked colormap contains **no tree content at all**.
Whatever the user saw is produced **at runtime**. This document does *not* claim
a defect exists — it removes the false closure and ranks the live candidates
with the A/Bs that discriminate them.

---

## 1. META REVIEW — premises checked against READER SOURCE, not status docs

| Brief premise | Verdict from source |
|---|---|
| `burnInShadows` never consults prop/tree geometry | **CONFIRMED** — see §2 |
| The recipe path "never sets `kFlagIsAlphaTest` (bit 2)" | **STALE.** Both recipe sites now source bit 2 from `GetIsAlphaTest()` (`mclib/bdactor.cpp:4618`, `:7522`), gated on `propFoliageAlphaFlag()` (`bdactor.cpp:70-74`). The `GetIsSpotlight()` form is gone. The bit is still **0 by default** because the gate defaults OFF (`RenderCore/RendererFeatureRegistry.h:681-687`, field 4 = `false`). |
| `MC2_PROP_FOLIAGE_ALPHAFLAG` "fixes it at both sites and is DEFAULT-OFF" | Default-OFF **confirmed**. "Fixes it" is **wrong for trees** — both of its shader consumers are provably inert on a default launch. See §5, rank 1. |
| The prior lane's A/B rested on `MC2_SHADOW_ENABLE=0` | The gate **exists**, so the A/B was possible — but ⚠ **its registry entry disagrees with reader source**: `RendererFeatureRegistry.h:307` declares default `false` ("Opt-in `=1`"), while the actual readers implement **default ON** (`GameOS/gameos/gos_static_prop_batcher.cpp:8648-8649` and `gos_mech_batcher.cpp:1113-1114`: `!(v && v[0]=='0')`). A third reader treats it as default-OFF (`GameOS/gameos/vulkan_post_screenfx_native.cpp:760`, `envTriState(...) == 1`). Anyone grading that A/B off the registry graded the wrong arm. |
| H4 armed `needsFullBakeNextFrame` at `registerStatic` but not at the per-frame swap | **STALE.** The destructive per-frame swap the spec described is gone (`mclib/bdactor.cpp:6511-6525` records its deletion); spec Fixes 1, 2 and 3 have all landed. See §5, rank 2. |
| `submit_trees` is structurally blind | **CONFIRMED and already retracted in-tree** — commit `9c71dff5` (`FALSE-CONCLUSION-RETRACT-1`). Not used here. |
| Contracts last measured 89/1, failure = uncommitted in-flight gate | **CONFIRMED independently** — see §7. |

⚠ **Correction to the reopen brief itself.** The brief states bit 2 clear leaves
back-facing foliage with "`ndl = 0` … i.e. DRAWN BUT UNLIT = black". The
arithmetic is right but the conclusion does not follow for trees, because the
factor `ndl` multiplies is **identically zero for every tree already** (§5 rank
1). Flipping `MC2_PROP_FOLIAGE_ALPHAFLAG` cannot change a tree pixel on a
default launch. Building on that premise would have shipped a null fix.

---

## 2. Task 1 — does ANY baking stage consult tree/prop placement? No.

`TerrainColorMap::burnInShadows` (`mclib/terrtxm2.cpp:609`), read end to end:

- Builds `heightMap[pixelWidth²]` either from a `<name>.height.tga` sidecar
  (`terrtxm2.cpp:641-651`; **no `mc2_03.height.tga` exists** in
  `A:/Games/mc2-opengl/mc2srcdata-fresh/textures/`, so this branch is dead for
  mc2_03) or from `land->getTerrainElevation(pos)` (`terrtxm2.cpp:791`).
- Ray-marches shadows **through that height map only** (`terrtxm2.cpp:880-920`).
- Computes 8 face normals from height differences and takes `vertexNormal ·
  lightDir` (`terrtxm2.cpp:1064`).
- Modulates `ColorMap` by the result (`terrtxm2.cpp:1116+`).

The function body references `heightMap`, `shadowMap`, `land`, `eye->lightDirection`
and `ColorMap`. **It never touches an object list, an appearance, a `TG_Shape`,
or any prop/tree structure.** The whole burn-in path was checked the same way:
`mclib/burnin_jpeg_decode.cpp` (stb_image JPEG decode of the sidecar, no
geometry), `mclib/terrtxm2.cpp:107-200` (RGB→BGRA repack), and
`tools/terrain_gen/burnin_renderer.py` (mod-map cook tool; not in the stock
asset chain).

**Conclusion: no baking stage can produce tree-shaped silhouettes.** The prior
lane's mechanism is dead.

---

## 3. Task 2 — the decisive measurement: the silhouettes are NOT in the texels

### 3a. Method (reproducible, no game launch)

1. `mc2_03.pak` extracted from `mission.fst` — `scratch/treesreopen1/fst_extract.py`.
2. Packet 1 parsed as the flat object list (`code/objmgr.cpp:1276-1305`: `int32
   count`, then per object `int32 objTypeNum; float x,y,z; float rotation; int32
   damage, teamId, parentId; 2× pad` = 40 bytes). Type numbers resolved against
   `data/objects/object2.pak` `[ObjectClass] ObjectTypeNum` (`code/objtype.cpp:355-376`)
   → `ObjectTypeClass` (`code/dobjtype.h:31-58`, `TREE_TYPE == 0`).
   Script: `scratch/treesreopen1/objlist.py`. Same method that produced
   `tests/visual/bookmarks/mc2_03_building_faces.json`.
   **Result: 2560 objects — 2088 TREE, 241 TERRAINOBJECT, 219 BUILDING, 11 TURRET, 1 GATE.**
   Sanity: x ∈ [−6037, +6208], y ∈ [−5952, +6165], inside the map half-extent.
3. Map geometry from packet 0: 320000 bytes / `sizeof(PostcompVertex)` = 32
   (`mclib/vertex.h:33-54`) → 10000 verts → **side 100**, so
   `worldUnitsMapSide = 100 × 128 = 12800` (`mclib/terrain.cpp:645`,
   `worldUnitsPerVertex = 128`, `terrain.cpp:116`) and
   `mapTopLeft3d = (−6400, +6400)` (`terrain.cpp:726-727`).
   Stride check: parsing the packet as float32×8 gives normal magnitudes
   mean 0.977 / max 1.000 — the layout is right, not garbage.
4. Colormap → world mapping from `burnInShadows` itself (`terrtxm2.cpp:616-626,
   784-805`): `pixel(x,y) ↔ world(topLeft.x + x·wupp, topLeft.y − y·wupp)`.
   Deployed `mc2_03.burnin.jpg` is **5120×5120** ⇒ `wupp = 2.5` world units/px.

### 3b. The measurement, made mapping-INDEPENDENT on purpose

My first attempts to lock the texture orientation from terrain data were weak
(best |corr| 0.21, `mapcheck{,2,3}.py`), so the result is **not** made to depend
on getting the orientation right. Instead the core-vs-ring statistic is computed
under **all 8 orientations** of the dihedral group (`tree_probe8.py`).
Core = disc r ≤ 4 px (≤ 10 wu radius, tree-scale); ring = annulus 6–14 px
(15–35 wu). Units are 0–255 luminance; image std = 23.0.

Deployed 4× colormap `A:/Games/mc2-opengl/mc2-win64-v0.4c/data/textures/mc2_03.burnin.jpg`:

| orientation | TREE n | core−ring | ±SE | p(core darker) | CTRL core−ring | BLDG core−ring |
|---|---|---|---|---|---|---|
| rot0    | 2088 | −0.1150 | 0.179 | 0.515 | −0.108 | +0.419 |
| rot0+m  | 2088 | −0.2119 | 0.159 | 0.528 | +0.413 | −0.567 |
| rot90   | 2088 | −0.2404 | 0.168 | 0.527 | +0.101 | −0.025 |
| rot90+m | 2088 | −0.0302 | 0.155 | 0.533 | −0.078 | +0.618 |
| rot180  | 2088 | −0.0229 | 0.143 | 0.518 | +0.149 | +0.714 |
| rot180+m| 2088 | +0.1322 | 0.164 | 0.512 | −0.119 | +0.799 |
| rot270  | 2088 | −0.0913 | 0.157 | 0.513 | −0.118 | +0.779 |
| rot270+m| 2088 | −0.1829 | 0.166 | 0.541 | −0.335 | −0.068 |

**Every TREE reading is ≤ 1.5 σ and inside the spread of the 400 random control
points. `p(core darker)` never exceeds 0.541 — a coin flip.** Repeated on the
**stock** 1280×1280 `A:/Games/mc2-opengl/mc2srcdata-fresh/textures/mc2_03.burnin.tga`:
same null, worst reading −0.485 ± 0.214 at rot90+m, controls of the same size.

A directional probe (dark lobe offset from tree centres, 12 azimuths at r = 10 px)
is flat: 60.72 – 61.19 luminance across all 12 (`burnin_probe.py`).

**Sensitivity bound.** With n = 2088 and SE 0.16, a silhouette of depth *D*
luminance present on a fraction *f* of trees shifts the mean by ≈ −*f·D*. A 3σ
detection needs *f·D* > 0.48, so this excludes **any silhouette of depth ≥ 20
luminance on more than ~2.4 % of the 2088 trees** (≥ 40 lum → 1.2 %). A
silhouette dark enough for a user to call out would be far above that.

### 3c. Eyes-on, at the densest tree concentration on the map

`scratch/treesreopen1/dense_marked.png` — the 1600×1600 world-unit window
`x ∈ [−3200,−1600], y ∈ [−4800,−3200]` containing **191 trees**, cropped 1:1 from
the deployed colormap with every tree position ringed in red. The texels are
smooth natural rock/grass/snow blend. **There is nothing repeating at tree
cadence, and no discrete dark blob under any marker.** (`dense_raw.png` is the
unannotated crop; `cluster0_trees_marked.png` is a second 4×-zoom sample.)

### 3d. Corroboration from an accepted prior lane

`.claude/VEHICLE-BAKED-TRUCK-3-STATUS.md` §1 decoded **both** mc2_10 colormaps
whole (stock 1536² and deployed 4× 6144²) and found "**ZERO man-made content
anywhere** — no runway, no tarmac, no cement pads, no buildings, no fences, no
roads, no vehicles". Same method, different map, same class of result. mc2_03
now adds trees to that list.

> **VERDICT ON TASK 2: the silhouettes are NOT baked art.** They are drawn at
> runtime. The prior lane's "no defect" verdict is not merely unsupported — its
> stated mechanism is refuted at source (§2) *and* its predicted artifact is
> absent from the asset (§3b, §3c).

---

## 4. Structural note: the stock colormap cannot encode a tree even in principle

The **stock** mc2_03 burn-in is 1280×1280 over 12800 world units = **10 world
units per pixel**. An MC2 tree card is a few tens of world units across, i.e.
**2–4 pixels**. The deployed 5120² sidecar is a 4× upscale of that same art
(downsampled it differs from the stock TGA by mean-abs 4.7 / 8.4 % of pixels
above 12 — recompression + upscale, no new structure). A recognisable
tree-shaped silhouette at gameplay zoom cannot come from this texture at any
resolution in the chain.

---

## 5. Task 3 — live candidate mechanisms, ranked

### Rank 1 — Trees carry NO directional (sun) light at all. Structural, and STOCK.

Chain, every link at source:

1. `mclib/camera.cpp:484` — `numLights = 2`; `:489` `worldLights[0]->init(TG_LIGHT_INFINITE)`
   (the sun), `:516` `worldLights[1]->init(TG_LIGHT_AMBIENT)`. There is exactly
   one directional light in the world list.
2. `mclib/bdactor.cpp:6924-6925` — `TreeAppearance::update` writes the terrain
   light sample into **slot 1 (AMBIENT)**: `eye->setLightColor(1,lightRGB); eye->setLightIntensity(1,1.0);`
3. `mclib/bdactor.cpp:6937-6938` — then **deactivates the sun**:
   `TG_LightPtr light = eye->getWorldLight(0); light->active = false;`
   Restored only at `bdactor.cpp:7054` (`light->active = true;`) — i.e. **after**
   the bake at `:7039` (`mc2CacheOrBakeStaticGpuLight(...)`).
4. `mclib/txmmgr.cpp:2488` — `GatherLightsParameters` skips inactive lights:
   `if ((listOfLights[iLight] != NULL) && (listOfLights[iLight]->active))`.
   ⇒ a tree's `TG_HWLightsData` row contains **one AMBIENT entry and no INFINITE entry**.
5. `shaders/static_prop.vert:715-728` — `sunColor` is taken from the first
   `TG_LIGHT_INFINITE`/`INFINITEWITHFALLOFF` row entry. With none present,
   `sunFound` stays false and **`sunColor = vec3(0.0)`**.
6. `shaders/static_prop.vert:823` — `lit = (seed + ambientTerm) * propDirFactor + sunColor * ndl;`
   The sun term is **identically zero for every tree**, whatever `ndl` is.

**The obvious escape hatch is closed.** `Camera::addWorldLight` (`mclib/camera.h:1315-1329`)
raises `numLights` to `MAX_LIGHTS_IN_WORLD`, so slots 2…31 *are* scanned — but
`worldLights[0]->init(TG_LIGHT_INFINITE)` at `mclib/camera.cpp:489` is the
**only** `TG_LIGHT_INFINITE` construction anywhere in `mclib/` or `code/`, and
`TG_LIGHT_INFINITEWITHFALLOFF` is **never constructed at all** (grep over both
trees returns only the three *comparison* sites: `bdactor.cpp:3235`,
`txmmgr.cpp:1922`, `static_prop.vert:726`). Every other world light is
POINT/SPOT (`bdactor.cpp:3435`, `:3673`, `carnage.cpp:742`, `turret.cpp:917`,
`weaponbolt.cpp:1451`). With slot 0 inactive there is nothing left for
`sunFound` to latch onto.

*Scope caveat:* this is the **registry/recipe** path — the steady-state path per
commit `9c71dff5` ("essentially all static props are registry-replayed"). A tree
that renders through the dynamic `submitMultiShape` path instead consumes the
same `lightData_` that `TransformMultiShape` filled inside the same sun-off
window (`bdactor.cpp:7030-7032`), so the conclusion carries — but that leg was
not traced end to end.

Compare `BldgAppearance::update`, which writes its sample to **slot 0** and
leaves the sun active (`mclib/bdactor.cpp:3856` `eye->setLightColor(0,lightRGB);`).
That asymmetry is real and was already recorded (ledger `PROP-LIGHT-CLASS-4`).

**Three consequences that matter here:**

- **`MC2_PROP_FOLIAGE_ALPHAFLAG` cannot fix black trees.** Consumer A
  (`static_prop.vert:730-735`, `abs(NdotL)` vs `max(NdotL,0)`) only changes
  `ndl`, which is multiplied by `sunColor = 0`. Consumer B
  (`static_prop.vert:798-804`, the wrap-term exclusion) is behind
  `u_propBaseLightDirStrength > 0.0`, and `MC2_PROP_BASE_LIGHT_DIR` defaults to
  0.0 (`RendererFeatureRegistry.h:674-678`). **On a default launch both
  consumers are inert for trees, so the gate is a guaranteed no-op.**
- This is a **better-supported explanation for `FOLIAGE-ALPHAFLAG-1` §4a's
  measured 0.000 %** than that lane's own reason ("these tree-card normals keep
  `NdotL >= 0` at the shipped sun elevations"), which is implausible across 2000
  leaf cards at arbitrary yaw. That lane's §4b (the exclusion moving foliage
  *brighter* when `MC2_PROP_BASE_LIGHT_DIR=0.55`) is fully consistent with this
  model: the wrap factor multiplies `ambientTerm`, which is all a tree has.
- **It is NOT a port regression.** Both lines are present verbatim in the first
  commit of `mclib/bdactor.cpp` (`305ad874`, the imported Microsoft source, at
  its `:4116-4117`). Trees have always been ambient-only. What *changed* is the
  consumer: `shaders/static_prop.vert:697-700` reads the row directly and has
  **no ambient floor and no protective early return**, unlike `calc_light`
  (`shaders/include/lighting.hglsl:222-225`, `numLights.x == 0 → return vec3(1)`).
  The in-tree statement of the same fact: `mclib/bdactor.cpp:4629-4630` — "faces
  pointing away from the sun otherwise render as a black void (static-prop
  lighting is `max(N·L,0)` with no ambient floor)". Seed is "effectively ZERO on
  these props" (`static_prop.vert:758-761`, `RendererFeatureRegistry.h:678`).

**Bifurcation worth logging:** `TreeAppearance::prewarmStaticLightBake`
(`mclib/bdactor.cpp:7742-7773`) calls `SetLightList` + `mc2CacheOrBakeStaticGpuLight`
**without** deactivating the sun — so a prewarm-baked tree would carry a sun row
while an update-baked one would not. In-game this is behind
`MC2_STATIC_REG_PREWARM`, **default OFF** (`code/objmgr.cpp:1684`), but the
**editor calls it unconditionally** (`editor/EditorObjectMgr.cpp:1209`, `:1515`).
Editor and game therefore light trees differently.

### Rank 2 — All-zero / stale `ObjectLights` row ⇒ genuinely black trees

- Symptom documented as *black trees* in
  `docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md:78-87`.
- `RecipeRange::lightDataIndex` init `0xFFFFFFFF` (`GameOS/gameos/gos_static_prop_registry.cpp:628`),
  written by `markVisible` (`:700`, `:720`, `:725`), reset on invalidate (`:788`),
  fallback at flush (`:1146-1147`); `numLights == 0` note at `:1153-1155`.
- Detection counter already exists: `[STATIC_LIGHT_ZERO v1] … zeroLightSlots`
  at `gos_static_prop_registry.cpp:1647-1659`. **This — not `submit_trees` — is
  the instrument for this class.**
- **Fix status (the brief's premise here is stale):** Fix 1 slot-recycle landed
  default-ON (`gos_static_prop_registry.cpp:286-300`, `MC2_STATIC_RECIPE_RECYCLE=0`
  to disable); Fix 2 armed at the tree re-register site (`mclib/bdactor.cpp:6679`,
  comment `:6674-6678` cites the spec by name); Fix 3 per-LOD pre-registration
  landed (`bdactor.cpp:7595-7692`, LOD0 light slot shared to higher LODs at
  `:7645-7646`). The old destructive per-frame swap is **gone**
  (`bdactor.cpp:6511-6525`).
- **Residual hole:** the live per-frame LOD site `TreeAppearance::selectActiveLOD`
  (`bdactor.cpp:7203-7245`) arms `needsFullBakeNextFrame` **only** when the target
  LOD isn't already registered (`:7242-7243`). That is correct *given* Fix 3 —
  but if per-LOD registration ever fails (`bdactor.cpp:7530-7538` marks a LOD
  unavailable), a swap into it takes no re-bake. Hysteresis at `:7218-7231`.

### Rank 3 — Runtime ground shadow with missing or black geometry above it

This is the mechanism whose *shape* matches "tree-shaped dark silhouettes on
open ground" most directly.

- Prop casters feed the dynamic/CSM map by default: the replay at
  `mclib/txmmgr.cpp:3213-3248` is gated on `gos_IsModernShadowPathActive()`
  (`GameOS/gameos/gameos_graphics.cpp:10097` — true whenever the shadow-terrain
  material loaded, **not** on any shadow env var) and on
  `MC2_SHADOW_DYNAMIC_PROP_CASTERS`, **default-ON** (`txmmgr.cpp:3212-3214`,
  registry `RendererFeatureRegistry.h:1981`).
- Far-LOD trees are suppressed from the full-registry replay by
  `setRecipeNoShadow` (`mclib/bdactor.cpp:7679`) under `MC2_PROP_LOD_SHADOW_CAST`
  default-OFF — but `flushShadow`'s camera-visible loop has **no** `noShadow`
  filter (`bdactor.cpp:7671-7673`), so **visible LOD1 trees already cast**.
- Independent sighting of exactly this symptom shape, on **this map**:
  `.claude/BUILDING-FACE-POSE-1-STATUS.md` rejected the mc2_03 village site
  `(546, 3676)` because "the housing renders as ground shadows with **no
  building geometry above them**".

### Rank 4 — Far-LOD tree textures: an in-tree LATENT HAZARD that is now live

`TreeAppearance::selectActiveLOD` (`mclib/bdactor.cpp:7203-7245`) **does** switch
trees to LOD ≥ 1 past `MC2_IMPOSTOR_DIST` (default **800** world units, ±8 %
hysteresis, `:7215-7232`). But `mclib/bdactor.cpp:6519-6525` warns, verbatim:

> "LATENT HAZARD: the deleted block also held the per-LOD-swap texture
> (re)loader, dead today under the 2026-05-12 TEMP LOD-0 pin (selectLOD forced 0
> in this function). If that pin is reverted (when the LOD-1 invisibility root
> cause is fixed), LOD selection + the per-LOD texture loader MUST be re-homed
> BEFORE the revert lands — TreeAppearance::init loads LOD-0 textures ONLY;
> LOD-1+ would be unloaded after any LOD swap."

`TREE-OVERRIDE-LOD-MVP-1` reintroduced per-frame LOD selection **through a
different function** (`selectActiveLOD`, not the pinned `selectLOD`), so the
condition the hazard note guards against may now hold without the note's own
trigger ("if that pin is reverted") ever firing. ⚠ **Unverified.** The modern
path registers per-LOD recipes with packet-indexed atlas sidecars in
`finalizeGeometry()` (`bdactor.cpp:7595-7692`), which may supply far-LOD textures
independently of `TreeAppearance::init` — so this may be a non-issue. It is
recorded here because it is a documented, unresolved, tree-specific hazard whose
precondition changed, and nothing in the tree says it was re-checked. Note also
that far LODs **share LOD0's light slot** (`bdactor.cpp:7645-7646`), so §5 rank 1
applies to them identically.

### Rank 5 — Alpha discard removing cards (absent, not black)

`shaders/static_prop.frag:290-306` — `tex_color.a < 0.5 → discard` (or the Bayer
dither path). Produces *missing* foliage, not dark foliage. Ranked below 1–4
because it changes the symptom from "dark" to "gone".

### Rank 6 — The recipe-vs-submit bit-2 disagreement

Real and worth closing on hygiene grounds: `mclib/bdactor.cpp:4618` / `:7522`
emit bit 2 only under a default-OFF gate, while
`GameOS/gameos/gos_static_prop_batcher.cpp:5574-5576` sets it unconditionally
from the same `alphaTestOn`. Two live paths disagree about one bit's meaning.
**Demoted to last** because §5 rank 1 shows neither shader consumer of that bit
can move a tree pixel at default settings.

---

## 6. Task 4 — the A/Bs that discriminate these, for when launching is re-enabled

None of these were run. `submit_trees` is not used by any of them.
All need a tree-dense pose; the densest on mc2_03 is world
`x ∈ [−3200,−1600], y ∈ [−4800,−3200]` (**191 trees**, §3c) — no bookmark set
covers it, so one should be authored from mission data the same way
`mc2_03_building_faces.json` was.

| # | Discriminates | Command | Prediction that decides it |
|---|---|---|---|
| **D1** | rank 1 vs rank 2 | `MC2_LIGHTING_DEBUG_VIEW=lightcount`, then `=sun`, then `=ambient` (registry `RendererFeatureRegistry.h:2199-2202`; views wired at `gos_static_prop_batcher.cpp:6798-6803`, `:8028`; shader emits `v_dbgLightMeta.y = numLights.x` at `static_prop.vert:719`) | **lightcount == 1 and `=sun` black ⇒ rank 1** (ambient-only by construction; rank 2 excluded). **lightcount == 0 ⇒ rank 2** (row never baked / zeroed). Anything else falsifies both. |
| **D2** | rank 2 live? | read `[STATIC_LIGHT_ZERO v1] … zeroLightSlots` (`gos_static_prop_registry.cpp:1647-1659`) at the tree pose | non-zero ⇒ rank 2 is live and is the first thing to fix |
| **D3** | rank 1 bake-window, and the prewarm split | `MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE=1 MC2_DIAG_VERBOSE=1` → `[BAKEORD v1]` lines carry `sunActive=` per bake (`mclib/bdactor.cpp:3210-3220`) | every **tree** bake should read `sunActive=0`. Any tree baking at `sunActive=1` means a second bake path is live and trees are inconsistent with each other. |
| **D4** | the editor/game prewarm asymmetry | `MC2_STATIC_REG_PREWARM=1` vs default, same pose | any tree-pixel change ⇒ the asymmetry at `bdactor.cpp:7742` is user-visible and the two bake paths must be reconciled |
| **D5** | rank 3 — are the silhouettes shadows? | `MC2_SHADOW_DYNAMIC_PROP_CASTERS=0` (kill-switch, default-ON) at the pose showing them | silhouettes **vanish or thin out ⇒ they are runtime prop shadows**. Unchanged ⇒ rank 3 is out. ⚠ Use *this* gate, **not** `MC2_SHADOW_ENABLE` — that one's registry default and its reader default disagree (§1) and the modern CSM replay does not consult it at all. |
| **D6** | rank 4 (far-LOD textures) | `MC2_FORCE_LOD=1` vs `MC2_FORCE_LOD=0` at a **near** tree cluster (`bdactor.cpp:7211-7213`), and `MC2_IMPOSTOR_DIST=100000` (pins LOD0 everywhere) vs default at a **far** one | far trees correct only with the impostor distance pinned out ⇒ the far-LOD texture hazard is live |
| **D7** | rank 5 | compare frames for *missing* vs *dark* cards at the same pose | dark-but-present ⇒ ranks 1/2. Present at one distance and gone at another ⇒ alpha discard / LOD. |

**Cheapest ordering: D1 → D2 → D5.** D1 alone separates the top two ranks in one
frame, and it needs no new gate and no code change.

---

## 7. Contracts — measured, not inherited

`bash scripts/check-contracts.sh` in `A:/Games/mc2-main`: **89 passed, 1 failed.**
The failure is `env_registry`: unregistered `MC2_WINDOW_OFFSCREEN`. Verified to
be **uncommitted in-flight work, not a committed regression** — the symbol
appears only in the dirty working tree (`GameOS/gameos/gos_render.cpp:139,162,420,735`
and `gameos_graphics.cpp:6071`, both `M` in `git status`), and
`git log -S "MC2_WINDOW_OFFSCREEN"` returns **no commit**. Matches the brief.

---

## 8. ADVERSARIAL SELF-REVIEW — what this does NOT establish

- **I never saw the user's frame.** "The trees are off" is not a symptom
  statement; I do not know the mission, pose, time of day, or whether the
  complaint is *black*, *missing*, *flat*, or *wrongly shadowed*. Ranking
  candidates is the most that can honestly be done without it. **The single
  highest-value next step is asking the user which frame.**
- **No defect is claimed.** §5 rank 1 is a *structural* fact that is also
  **stock behaviour** (`305ad874`) — trees have always been ambient-only. It is
  a strong explanation for *flat/dark* trees but it is not, by itself, evidence
  that anything regressed.
- **Nothing here was run.** Every §5/§6 claim is source-derived. Launches are
  blocked and the block was respected; no harness, no renamed binary, no wrapper.
- **The asset measurement is mc2_03 only** (2088 trees), plus the prior lane's
  whole-texture inspection of mc2_10. It is not a survey of all 24 colormaps.
  It is, however, decisive for the map the complaint chain has been graded on.
- **Could the mapping be wrong and hide a real signal?** No: the statistic was
  computed under all 8 orientations (§3b) and the densest window was inspected
  by eye at 1:1 (§3c). A silhouette would have to be invisible under every
  orientation *and* invisible to the eye.
- **If the user launched the game right now with default settings**, nothing in
  this document changes what they see. This lane wrote **no code** and flipped
  **no gate**. `MC2_PROP_FOLIAGE_ALPHAFLAG` remains default-OFF and — per §5
  rank 1 — turning it on would change nothing for trees anyway.
- **The one thing I would still like and do not have:** confirmation that a
  tree's `ObjectLights` row really reads `numLights == 1` at runtime. The source
  chain (§5 rank 1, six links) is tight, but D1 is one frame and would settle it.

---

## 9. Notes for the next lane

- Reproduce §3 with: `py -3 scratch/treesreopen1/fst_extract.py <deploy>/mission.fst mc2_03 <out>`
  → `objlist.py <mc2_03.pak> <object2.pak> <objs.json>`
  → `tree_probe8.py <burnin image> <objs.json>`. No engine, no build.
- The method generalises to any map: swap the stem. Use it **before** writing
  shader-lighting code, the way `VEHICLE-BAKED-TRUCK-3` did.
- `mc2_03` has **no `.height.tga`**, so its burn-in used terrain elevation, not a
  hi-res sidecar. Check this per map before reasoning about burn-in detail.
- ⚠ **Do not read `MC2_SHADOW_ENABLE`'s default from the registry.** Registry
  says OFF, the two GL readers implement ON, the VK reader implements OFF (§1).
  That inconsistency deserves its own hygiene fix and is out of scope here.
