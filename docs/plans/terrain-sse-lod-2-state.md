# terrain-sse-lod-2 state (lane 2, Astra review p35)

Terrain chunk LOD is chosen by screen-space error, not distance. `MC2_TERRAIN_LOD_CHUNK_DIST_SCALE` and the distance path are deleted.

- Error: `mc2lod::levelErrors` / `edgeErrors` (mclib/terrain_lod_rules.h) per block from the 4x visual bake when loaded, else coarse heights; cached per height version.
- Pick: `chooseSseLevel`, 1 px (0.25 px concrete), coarsen below 0.9 x tol, live FOV/viewport (terrain_update.cpp `chooseProjectedLod`).
- Morph: per-block weight toward level+1, capped so mix(L, L+1) error stays within tol; per-corner weights (`cornerMorph`: 0 next to culled/off-map/other-level blocks, else the shared min) packed in the patch `morph` word; VS `terrainMorphH`.
- Stitch error is in the bound: a pair whose lcm-stride shared edge projects over tol refines the coarser block (`stitchErrorLevel`).
- Shader stitch now clamps the partial-edge endpoint like `mc2lod::edgeHeight` (Astra finding 1).
- Proof: tests/unit/test_terrain_lod_rules.cpp (shader-equivalent shared-edge agreement over random levels/visibility/morph, 4 partial sizes, both orientations, morph sweep).
- Unchanged: RT terrain tiles (fixed LOD), picking/LOS (render only), GL path (VK-only engine).
- Residuals: shadow pass not morphed; level skips (>1 level in a frame) or a delta-clamp release can still pop; mode-2 errors ignore the edge fade and displace-far < 1.
