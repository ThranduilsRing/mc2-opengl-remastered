# TERRAIN-ROUND-METHOD-RECON-1

Read-only. Goal: find a method that turns a blocky faceted "5-point pyramid" stock
hill into a real ROUNDED hill. Corner-pinned 4x displacement (Stages 1-2) added
lumpiness but kept the pyramidal silhouette (it pins corners/apex). All file:line
grep-verified 2026-06-26.

## Live-path facts (the cutover)

- Live default terrain = **terrain_lod_chunk** (vert+frag ONLY, NO tess stage).
  makeProgram("terrain_lod_chunk", .vert, .frag) gos_terrain_lod_chunk.cpp:373-377.
  Vertex Z from SSBO@23 flat/faceted (terrain_lod_chunk.vert:25,45). Frag smooths
  NORMALS via bilinear central-diff (lighting only) -> silhouette stays faceted.
- Default-ON since Phase 10 v1b. Gate mc2TerrainLodChunkEnabled() terrain.cpp:139.
  chunk-OFF path (routed to tess gos_terrain bridge) declared **vestigial /
  "has no production renderer"** terrain.cpp:144-148. Tess path superseded for
  perf/scaling, NOT buggy.
- Tess path gos_terrain.{tesc,tese} IS fully implemented + Phong-capable.
  TESE Phong block gos_terrain.tese:101-109; alpha = tessDisplace.x.
- **Phong alpha defaults to 0.5** (terrain_phong_alpha_, gameos_graphics.cpp:2254),
  wired into tess draw uniforms (gameos_graphics.cpp:6675). Production-tuned, not token.
- **KEY: tess Phong is ALSO mirrored into GAMEPLAY height.**
  MapData::terrainElevation (mapdata.cpp:2189) re-runs the identical Phong projection
  on the CPU (mapdata.cpp:2456-2482) via gos_GetTerrainPhongAlpha(). On the tess path
  visual==gameplay by construction -> **zero desync, by design.**

## Method assessment

### 1. PHONG / PN-TRIANGLE TESSELLATION (rounds faces, keeps vertices)
- Round-ability GOOD. Curves triangle interior toward per-vertex normal tangent
  planes while KEEPING apex/corner vertices. Does NOT need apex to move -> rounds the
  FACES between the pyramid's 5 points, converting flat facets into convex bulges =
  exactly "de-faceting a pyramid into a dome." Scales with alpha(0..1)+tessLevel.
  0.5 conservative; pushable ~0.7-0.85 for rounder hills.
- Desync NONE. Apex stays at gameplay height; CPU mirror (mapdata.cpp:2456) already
  locks gameplay==Phong visual. No bake, no visual!=gameplay buffer, no decal/shadow/
  pick divergence to plumb.
- Effort: math already exists CPU+GPU. Two sub-options:
  (a) add TCS/TES to the LIVE chunk path -> keep production renderer; draw GL_PATCHES,
      port the 8-line Phong block (tese:101-109) into a new chunk TESE, supply vertex
      normals to TCS + tess-factor LOD wiring. Core recon called chunk-tess high-risk
      for DISPLACEMENT (factors vs LOD bands + seams) -- but Phong rounding is far more
      seam-forgiving: shared edge verts/normals -> neighbor patches agree on the edge
      curve -> no cracks if normals shared. Medium effort, contained risk.
  (b) productionize the existing tess path, raise alpha ~0.7-0.85 -> Phong proven, but
      un-deprecates a renderer retired for perf; re-opens the cutover's perf question.
  LESS WORK = (a) chunk-TES Phong IF seam/normal-sharing handled (keeps winning path).

### 2. WHY CHUNK ISN'T TESSELLATED (the cutover)
- Not buggy-deprecated -> superseded for perf/scaling. Chunk = bounded draw
  (<=64 glDrawElements, cache-friendly SSBO, LOD bands, skirts), GPU-indirect-friendly.
  chunk-OFF "has no production renderer" (terrain.cpp:144-148) = explicit retirement.
- Rounding CAN live on chunk via an added TES stage (1a); reviving tess path NOT
  mandatory. Blocker = engineering TES+LOD+seam wiring chunk never had, not a tess bug.

### 3. CORNER-MOVING VISUAL RESHAPE + VISUAL GROUNDING (heavy real de-pyramid)
- Round-ability GOOD (real silhouette change) -- but requires UNpinning corners, then
  visual/gameplay diverge for real. This is the road the user already half-walked.
- Spine EXISTS but INERT: terrain_runtime.cpp groundElevation(:28)/sampleVisualHeight
  (:42)/decalElevation(:50) all byte-identical to gameplay today; every branch returns
  getTerrainElevation; gates (MC2_TERRAIN_RUNTIME_GROUNDING/DECALS) plumbed but UNFED.
- Residual desync to build/feed: decals (cheap, seam built; desync doc §2); object
  foot-contact (~47 sites; §3); picking (camera.cpp converge to gameplay; §4); **shadow
  CASTER stays coarse** (separate CPU path mapdata.cpp:1294; shader recon §3 = ugliest);
  PLUS a visual-height bake + loader (none exists; core recon §7). HIGH effort, multi-
  slice, permanent desync management. Cost >> Phong; REINTRODUCES the split Phong avoids.

### 4. SUBDIVISION SURFACES (Loop / Catmull-Clark)
- Overkill. Needs full mesh topology/connectivity (chunk streams independent patches,
  no shared topology), iterative refinement, harder gameplay-height mirror than Phong's
  per-vertex projection. Phong tess is the single-pass GPU-native subset that already
  rounds facets. Abandon.

## VERDICT -- ranked (best = rounds + lowest risk + least desync)

1. **PHONG TESSELLATION -- recommended. THE answer.** Gameplay-safe rounding, NO bake,
   NO visual!=gameplay split (CPU mirror mapdata.cpp:2456 locks them). De-facets the
   pyramid FACES = the exact failure corner-pinning could not fix. Cheapest correct path.
   Prefer 1a (add TES to live chunk path); fallback 1b (productionize tess path, alpha
   ~0.7-0.85) if chunk-TES seam wiring proves costly (1b re-opens perf).
2. CORNER-MOVING VISUAL RESHAPE -- only if Phong's silhouette change judged insufficient.
   High effort, permanent desync, partially-built spine.
3. SUBDIVISION SURFACES -- abandon (overkill, topology-hostile to patch streaming).

## ABANDON corner-pinned 4x displacement? -- YES for rounding.
Corner-pinning is structurally incapable of de-pyramiding: pins apex+corners so the
silhouette stays pyramidal, only the interior gets noisier (sometimes worse). Phong
rounds the FACES while keeping those pinned verts at gameplay height -- strictly the
better tool for "blocky hill -> rounded hill," shipping gameplay-lockstep for free.
Keep corner-pinned only for a separate micro-detail/lumpiness goal; for ROUNDING, drop
it for Phong.
