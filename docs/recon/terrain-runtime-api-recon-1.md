<!-- doc-status: stale | reviewed:2026-10-11 -->
# TERRAIN-RUNTIME-API-RECON-1 — master consumer map

Read-only recon. Goal: map every terrain-data consumer so a `TerrainRuntime`
compatibility API (the Terrain 2.0 spine) can be introduced with **no behavior
change**, then later split gameplay vs visual height.

Sub-docs (slice detail):
- `terrain-runtime-api-recon-1-datamodel.md` — data model + render-height path
- `terrain-runtime-api-recon-1-render-consumers.md` — shadows/decals/water/veg/props/picking
- `terrain-runtime-api-recon-1-pak-format.md` — .pak packets, load path, sidecar precedent
- (gameplay-consumer inventory inlined below)

---

## 1. The one fact that makes this tractable

**Height lives in exactly one place today.** `MapData::blocks[]` =
`PostcompVertex[]` (`mclib/vertex.h:33`); `float elevation` at byte 12
(`vertex.h:44`). Packet-0 of `<mission>.pak` is `memcpy`'d straight in
(`mapdata.cpp:205-209`). Grid is map-stable, indexed `blocks[mx+my*mapSide]`.

**Every GPU height buffer is a COPY of `blocks[].elevation`:**
- R32F height tex, unit 11 (`terrain.cpp:718`) — normals only.
- SSBO binding 23 `heights[]` (`gos_terrain_lod_chunk.h:16`, fill `mclib/terrain_init.cpp:193`) — LOD-chunk render geometry.
- Recipe SSBO `wz0..wz3` (`GameOS/gameos/gos_terrain_indirect_atlas.cpp:47-50`).

**RENDER height == GAMEPLAY height TODAY.** Gameplay = `Terrain::getTerrainElevation`
(`mclib/terrain_render.cpp:539`) → `mapData->terrainElevation` (`mapdata.cpp:1407/2189`) →
same `blocks[].elevation`. No numeric split exists. There is only a *documentary*
note (`terrain.cpp:711-716`) saying the height-tex is "visual-only, gameplay
authoritative" — but the bytes are identical.

**Material/overlay** = `PostcompVertex.textureData` DWORD (`vertex.h:46`):
hi16 = overlay TXM, lo16 = base TXM (`mapdata.cpp:279/540`). `terrainType`
(`vertex.h:50`) = material class. Cement-word is *derived* into recipe `_wp3`
(`GameOS/gameos/gos_terrain_indirect_cement.cpp:67`), not stored.

> **Consequence:** the compat API can wrap `getTerrainElevation` /
> texture / terrainType over `blocks[]` and cover everyone with zero reconcile.
> The risk is entirely in the *future* split, not the wrapping.

---

## 2. The divergence trap (the whole reason for the API)

Once `visualHeight != gameplayHeight`:
- everything that **GROUNDS** must keep using `gameplayHeight()` (`getTerrainElevation`)
- everything that **DRAWS** rides the visual mesh

The trap is silent default-equality. The API must expose **both**
`sampleGameplayHeight()` AND `sampleVisualHeight()` and force every call-site to
pick. No single `height()`.

---

## 3. Classified consumer map

Class ∈ {GPH=gameplay-height, VH=visual-height, MAT=material, FM=feature-mask, LDP=legacy-direct-packet}.
Break = risk if visual height diverges from gameplay height.

### Render / data-model (slice 1)
| consumer | file:line | class | break |
|---|---|---|---|
| `terrainElevation` (gameplay sample core) | mapdata.cpp:1407 | GPH | — (authoritative) |
| LOD-chunk vert (SSBO@23 + mat@24/@25) | terrain_lod_chunk.vert:45 | VH+MAT | self-consistent |
| indirect recipe `buildRecipeSlot` | GameOS/gameos/gos_terrain_indirect_atlas.cpp:47 | VH+MAT(_wp3) | self-consistent |
| thin records | — | MAT/FM | — |
| height R32F tex (normals) | terrain_height_normal.hglsl:59 | VH | low |
| water fast path | water_fast.vert:198 | LDP (own VBO) | — |
| quad setupTextures (CPU walk) | quad.cpp:~540 | LDP | — |

### Render-adjacent grounding/sampling (slice 2)
| consumer | file:line | class | break |
|---|---|---|---|
| baked self-shadow (colormap) | mclib/terrtxm2_bake.cpp:228 (samp 443/449/475) | VH/FM | LOW cosmetic |
| dynamic shadow map receiver | gameos_graphics.cpp:2490/2554 | VH | LOW (via grounding) |
| craters (4-corner getTerrainElevation) | crater.cpp:257-260 | GPH/LDP | **HIGH z-fight** |
| dynamic decal ring | dynamic_decal_ring.cpp:95 | GPH | **HIGH** |
| cement/road/runway (shader/colormap baked) | — | MAT | SAFE |
| mine overlay/decal (packet-4 state → terrain decal) | move.h:362-678; quad.cpp:455-921; code/objmgr_io.cpp:97 | FM(packet-4)+LDP-decal | **HIGH z-fight** (rides visual mesh; see landmines §5) |
| water plane + shore test + getWater | terrain.cpp:161/3539-3566/4074 | GPH-threshold+flat | **HIGH+SUBTLE** |
| vegetation grounding/water-floor/material/slope | VegetationAdapter.cpp:253-470 | GPH+MAT+FM | **HIGH** place / MOD gates |
| **prop/building/object grounding** | bldng.cpp:825-827; objmgr.cpp:4667/4695/4716/4743; gameobj.cpp:371/443/1866/2098 | GPH | **HIGH — LOAD-BEARING** (feeds shadow casters) |
| picking / screenToTerrainApprox / editor rect | camera.cpp:927/979/1388+; mclib/terrain_geometry.cpp:212 | GPH | MODERATE cursor offset |

### Gameplay (slice 3) — ALL must stay GPH
| category | representative file:line | hot? |
|---|---|---|
| **Unit grounding (per-frame Z)** | mech.cpp:6121; code/gvehicl_update.cpp:683; warrior.cpp:2878; turret.cpp:1660-1774; gate.cpp:359 (~21 sites) | **HOT** |
| Pathfinding / passability | mover.cpp:3109-4419; move.cpp:473-5252; `GameMap->getPassable` (~40 sites) | cold |
| LOS / sensor | mech.cpp:8107/8509-8645; gvehicl.cpp:3668/4933-5062 | cold |
| Weapon/projectile impact Z | mover.cpp:3501-4886; turret.cpp:1660-1774 | hot-in-combat |
| Path-step validation | mover.cpp:6657-6658; gvehicl.cpp:4012-4013 | cold |
| AI/brain terrain | none dedicated (delegates to getPassable) | — |

**Packet-4 MOVE axis** — `.pak` packet 4 holds per-cell state that is NOT in the
PostcompVertex grid: passability (derived from terrainType+slope) **and mine state**
(`MapCell::getMine`, drawn as a terrain decal — see landmines §5). This is its own
future API axis (`sampleCellState` / `FM_Mine`), independent of the height split and
sourced from `GameMap`, not `blocks[]`.

---

## 4. On-disk format & sidecar attach (slice 4)

- `<mission>.pak`: PacketFile, magic `0xFEEDFACE`. **Packet 0** = terrain land grid
  (raw `PostcompVertex[side*side]`, side = `sqrt(pkt0bytes/sizeof(PostcompVertex))`,
  not stored). **Packet 1** = objects/buildings. **Packet 3** = TacMap. **Packet 4**
  = MOVE/pathfinding. last = GUID. (`mission.cpp:2759-2797`, `editor/EditorData_save.cpp:441-478`)
- Dims/elevation-range/water are in the **`.fit`**, not the `.pak`
  (`mclib/terrain_geometry.cpp:136-160`). Spacing = 128 wu.
- **Sidecar precedent** = `<mission>.beauty/`: `sidecar.json` (baseHash = sha256 of
  `.pak`, grid_side, dtype) + `height_delta.r32` + optional `protected.r8`
  (`mission_sidecar.py:82-104`). Offline apply patches elevation@12+normal@0 into a
  NEW `.pak` only on baseHash match; live editor `BeautySidecarPreview` applies via
  HeightBrush, never writes.

**terrain2 attach model:** mirror `.beauty/` → `<mission>.terrain2/` with
`terrain2.json` (baseHash) + loose row-major side² planes (gameplay_height,
visual_height, material_id, feature masks…). Engine reads it as an opt-in `MC2_*`
pass *after* `land->init`/`land->load`, applies only on baseHash match, degrades to
stock on miss. Stock `.pak` stays authoritative; sidecar additive/reversible/never
in save games.

---

## 5. TERRAIN-RUNTIME-API-1 — SHIPPED (2026-06-25)

`mclib/terrain_runtime.{h,cpp}` (`namespace TerrainRuntime`): pure pass-through
over `land`. `sampleGameplayHeight`/`sampleVisualHeight` (== `getTerrainElevation`
today, kept distinct on purpose), `sampleWaterLevel` (== `getWaterElevation`),
`sampleMaterialId` (== `getTerrainType`), `sampleFeatureMask` (FM_Cliff = slope,
FM_Shoreline = elev-vs-water live; FM_Cement / FM_ForestSuppress stub-0 per
landmines §6). No call-site migrated yet (foundation slice).

Parity gate `MC2_TERRAIN_RUNTIME_PARITY=1`: one-shot grid-cell walk in
`Terrain::update` (in-scope to inline `getCellPos`) comparing API vs legacy,
logs `[TERRAIN_RUNTIME v1]`. **Verified mc2_24: side=120 samples=4
mismatch{gameplay=0 visual=0 material=0 water=0}.** Gate-OFF = one-shot guard,
zero per-frame cost; smoke mc2_01 PASS, mc2_24 PASS. Deployed v0.5.0.

### Original intent (kept for the migration slices)

Introduce the API as a pure pass-through over `blocks[]`, no split yet:
```cpp
TerrainRuntime::sampleGameplayHeight(x,y)  // == getTerrainElevation today
TerrainRuntime::sampleVisualHeight(x,y)    // == same buffer today
TerrainRuntime::sampleWaterLevel(x,y)      // == waterElevation
TerrainRuntime::sampleMaterialId(x,y)      // == textureData/terrainType
TerrainRuntime::sampleFeatureMask(x,y,kind)// == legacy-derived (slope/overlay), stub masks
```
Parity gate: legacy-direct == API result, byte-identical, gate default-OFF.

**Migration priority (highest break-surface first):**
1. prop/building/object grounding (load-bearing, feeds shadow casters) + unit grounding
   — ✅ TERRAIN-RUNTIME-CONSUMER-GROUNDING-1 (2026-06-25). Added the spine grounding
     chokepoint `TerrainRuntime::groundElevation(pos)` (gate `MC2_TERRAIN_RUNTIME_GROUNDING`,
     default-OFF, byte-identical, single future flip-point). Migrated ~47 grounding sites
     onto it: `bldng.cpp` (Building::update) + `mech.cpp` (15) + `gvehicl.cpp` (15) +
     `warrior.cpp` (6) + `gate.cpp` (1) + `objmgr.cpp` (4) + `gameobj.cpp` (3) +
     `bdactor.cpp` (3, incl. null-guarded). Left untouched: `GameMap->getTerrainElevation`,
     tile-form `(r,c)`, and weapon-impact/path-waypoint vs grounding distinction is moot
     (all want gameplay height). Gate-OFF + gate-ON mc2_24 PASS, Δdestroys=0, visual-advisory
     (golden) PASS both states.
2. (remaining height consumers — UI markers, ablmc2 strike, team/tacordr/mover-path,
   weather, camera-grounding — can adopt `groundElevation` later; not hot grounding)
3. water threshold + shore (subtle straddle of one constant)
   — ✅ TERRAIN-RUNTIME-CONSUMER-WATER-1 (2026-06-25). Added `sampleWaterClass(pos)`
     (pure forward to `land->getWater`; single impl so no gate). Relocated the 3 gameplay
     water-class callers onto it: `mover.cpp:4429`, `move.cpp:1030`, `missiongui.cpp:4745`.
     Editor water callers left. Byte-identical; mc2_24 golden PASS. Keeps gameplay
     water-class on gameplay height, independent of the visual waterline mesh.
4. craters / decal ring (z-fight)
   — ✅ TERRAIN-RUNTIME-CONSUMER-DECALS-1 (2026-06-25). Added `decalElevation(pos)` →
     VISUAL height (gate `MC2_TERRAIN_RUNTIME_DECALS`, default-OFF, byte-identical). Decals
     intentionally route to VISUAL (they sit on the rendered surface) — the exact site where
     they must diverge from unit grounding (gameplay) to avoid z-fight. Migrated `crater.cpp`
     (4) + `dynamic_decal_ring.cpp` (1). Gate-OFF + gate-ON mc2_24 PASS, golden PASS.
5. vegetation placement + gates  (not started — separate consumer class)
6. picking  (not started — camera/editor class)
Pathfinding/passability + mines = separate axis (packet-4 MOVE / GameMap), defer.

**Do NOT** make visual height authoritative, change grounding before render proof,
require stock-map conversion, or treat colormap as the final material model.

---

## 7. ⏸ PAUSED 2026-06-25 + verification finding

**SHIPPED + committed (local nifty, NOT pushed):** obj 1 spine `76cb8449`, obj 2 grounding
`e0c4c0c3`+`5aa9aff4`, obj 3+4 water+decals `d8cc1b01`. Deployed v0.5.0 == HEAD.

**NOT done (paused):** obj 5 vegetation (was built+smoke-PASS, then **reverted** —
`VegetationAdapter.cpp` card-Z→visual / gates→gameplay split is the intended shape, re-apply
on resume), obj 6 picking (camera.cpp, never started). Mines/passability = deferred axis.

**VERIFICATION FINDING (blocks pixel-golden for these consumers):** `run_visual.py compare`
byte-compare is **non-deterministic on live missions**. Proof: mc2_24 captured with the SAME
binary, gate-OFF, twice → different sha256 per bookmark both times (and again gate-ON). Cause
= live combat (movers/AI/particles/weapon-spawned decals) isn't frame-reproducible even with
`fixed_clock`. So OFF-vs-ON sha diff = same noise as OFF-vs-OFF; it cannot prove byte-identity.
The smoke `[visual-advisory] ... PASS` line is a STALE cached result (fixed 2026-06-22 ts), NOT
a fresh compare — do not cite it as a golden.

**What these slices ARE verified by:** (1) byte-identity by construction — every migrated site
returns exactly the legacy value (pure forwarder); (2) smoke PASS + Δdestroys=0 + no crash,
gate-OFF and gate-ON. A deterministic pixel-golden for grounding/decals is infeasible (those
consumers only render on moving objects in non-deterministic combat). Resume options: accept
construction+smoke, OR build a frozen early-frame (pre-combat) A/B capture, OR diff-review the
subagent-done mech/gvehicl/warrior sweeps.

---

## 6. Known landmines (user field notes 2026-06-25 — will bite later)

These do NOT block the pure pass-through pass but constrain the MAT / FM / VH axes.

1. **Detail/normal layer is additive in a sampler2DArray** — the marble texture
   (mat5 cliff art) goes here. So the material axis already has a *layered additive*
   surface, not just the burned colormap. `sampleMaterialId()` / future material
   layering must coexist with this array, not replace it.
2. **Cement is burned into the colormap on load and is ALWAYS FLAT.** The
   cement→colormap **transition** is "the bane of our existence" — hard to put on the
   same layer as the terrain. Expect the feature-mask/material axis to fight this:
   cement is currently a *baked colormap* artifact, not a runtime material/feature, so
   a clean `sampleFeatureMask(cement)` will diverge from what's actually drawn until
   the transition is moved off the colormap.
3. **Prior heightmap-RESAMPLE attempts exist (resample, not displace).** The
   `sampleVisualHeight()` axis should reuse/align with these, not start fresh.
4. **Shoreline attempt exists but is currently NON-FUNCTIONAL.** `sampleWaterLevel()`
   + shoreline feature-mask must not assume the existing shoreline path works — treat
   it as a stub to be re-derived.

→ Exact file:line for each pinned in `terrain-runtime-api-recon-1-landmines.md`.
