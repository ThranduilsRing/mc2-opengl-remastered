# TERRAIN-ROUND-METHOD-RECON-1 (shader angle)

Goal: cheapest shader path to ROUND blocky low-poly terrain (faceted pyramid -> dome)
WITHOUT moving original gameplay vertices. Read-only recon. nifty-mendeleev worktree.

## 1. EXISTING TESS PATH — Phong tessellation IS present and WIRED

`shaders/gos_terrain.tese:101-109` does textbook Phong tessellation:

    float alpha = tessDisplace.x;  // phongAlpha
    if (alpha > 0.0) {
        proj_i = worldPos - dot(worldPos - P_i, N_i) * N_i;   // project bary pt onto each corner tangent plane
        phongPos = bary.x*proj0 + bary.y*proj1 + bary.z*proj2;
        worldPos = mix(worldPos, phongPos, alpha);             // pull flat tri toward curved interpolant
    }

- Per-corner world pos `tcs_WorldPos` + per-corner world normal `tcs_WorldNorm` arrive
  from TCS (`gos_terrain.tesc:128-129`, both passthrough and SSBO-record paths). Phong
  tess REQUIRES per-vertex corner normals — present here.
- Wired end-to-end: uniform `tessDisplace.x` set from `terrain_phong_alpha_`
  (gameos_graphics.cpp:6262/6678/6976), default member **0.5f** (gameos_graphics.cpp:2254),
  ImGui slider 0..1 + reset 0.5 (GraphicsOptionsWindow.cpp:334-341).
- TES geometry only EXISTS where tess level >= 1 (tesc:72 distance LOD,
  `tessLevel` default 4.0, near/far 200/2000). Past ~2000wu tess collapses to 1 -> no rounding.

So: the tess path already rounds at alpha=0.5 within tess range. NOT broken — it is the
gos_terrain (thin/indirect patch-stream) path, not the live chunk raster path.

## 2. PHONG STRENGTH — alpha=1 = full dome, capped only by tess level

- alpha=0 -> flat facets; alpha=1 -> full Phong interpolant (max bulge toward vertex-normal
  curved surface). The mix is linear, uncapped 0..1. Cranking to 1 turns pyramids into domes.
- The REAL ceiling is **tessellation level**, not alpha. Phong only bulges the NEW interior
  micro-verts the tessellator inserts. tessLevel=1 (no subdivision) = no interior verts =
  alpha does NOTHING. At tessLevel 4 you get visible rounding; higher = smoother dome.
  Distance LOD (tesc:71-72) fades tess to 1 -> rounding fades with distance.

## 3. CHUNK PATH (the LIVE default raster path) — no tess stage, but a CPU-baked Phong twin EXISTS

`terrain_lod_chunk.vert` is a plain VBO VS (location0 ivec2 localOffset). No TCS/TES.

Per-VERTEX corner normals: NOT supplied as an attribute. The chunk path derives normals
PER-FRAGMENT in frag (`smoothTerrainNormal` terrain_lod_chunk.frag:104-111, central-difference
of the bilinear heightfield; also flat dFdx cross at :506). So adding a TES would need to
SUPPLY per-corner normals — they are computable from the height SSBO (binding 23) the same
way smoothTerrainNormal does, but are not currently emitted from the VS.

KEY FIND — the rounding already exists on the chunk path WITHOUT tessellation, via a
CPU-baked finer heightfield:
- `mclib/mapdata.cpp:2415-2483` runs the SAME Phong-Z projection on CPU (proj_i.z, mix by
  phongAlpha) inside the terrain HEIGHT query.
- `terrain_lod_chunk.vert:33-39,58-90` (`u_visualDisplace`) samples a 4x-finer visual
  heightfield SSBO (binding 26) for INTERIOR verts, chunk-edge/skirt verts pinned to the
  coarse line (crack-free). That fine field is the bake target. So the chunk path rounds by
  reading a denser pre-rounded heightfield, not by GPU tessellation.

## 4. NORMALS AFTER ROUNDING

- Chunk path: normals are recomputed per-fragment from the (rounded) heightfield every frame
  (smoothTerrainNormal central-difference). So if the fine field is rounded, lighting follows
  automatically — no separate normal recompute needed. This is the chunk path's big advantage.
- Tess path: `gos_terrain.tese:66-69` only barycentrically interpolates the ORIGINAL corner
  normals — it does NOT recompute a normal for the bulged surface. So a strongly domed tess
  surface gets the smooth interpolated normal (acceptable, Gouraud-ish) but not a normal
  exactly perpendicular to the new curved micro-face. Good enough visually; not exact.

## 5. ⚠ GAMEPLAY DESYNC TRAP (load-bearing)

mapdata.cpp:2415+ is inside the terrain HEIGHT-lookup used for placement/movement. The CPU
Phong block MUTATES the returned `result` height when phongAlpha>0
(mapdata.cpp:2482 `result = result + phongAlpha*(phongZ - result)`). That means the existing
Phong already shifts GAMEPLAY height, not just visuals — the "don't move vertices" constraint
is ALREADY violated on the CPU side wherever this query feeds gameplay. Verify whether this
codepath feeds gameplay collision/placement or is render-only before cranking alpha. (The GPU
TES Phong at tese:108 is render-only and safe; the CPU one at mapdata:2482 is the risk.)

## RECOMMENDATION (cheapest win, no gameplay desync)

1. CHEAPEST visual round on the LIVE path = the chunk visual-displace field already built:
   enable `u_visualDisplace`/`MC2_*` gate and bake the 4x fine field with Phong rounding on
   CPU but write it ONLY into the binding-26 VISUAL field (render-only), NOT into the gameplay
   height return at mapdata:2482. Normals follow free (per-frag recompute). No new shader stage.
2. Do NOT add a TCS/TES to the chunk pipeline first — it needs per-corner normal emission +
   crack-stitch at tess seams (the vert already fights T-junction cracks at :113-136; tess
   adds a second seam problem). More work than the bake it already has.
3. The gos_terrain TES Phong is the reference math and is render-only-safe, but it is not the
   live default path, so reviving it does not round what the player sees by default.

Net: round via the render-only 4x visual heightfield (Phong-baked into binding 26),
keep gameplay height query un-Phonged. Least work, no desync, normals auto-correct.
