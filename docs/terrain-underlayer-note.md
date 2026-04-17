# Terrain Underlayer Note

The dark "tiger stripe" terrain artifact came from the legacy non-water terrain
alpha/detail pass in `mclib/txmmgr.cpp`, not from the tessellated terrain shader.

What was happening:
- `Render.TerrainSolid` submitted the main terrain surface into the tessellation path.
- `Render.Overlays` still submitted the old non-water `MC2_ISTERRAIN | MC2_DRAWALPHA`
  detail layer on the original flat terrain plane.
- When tessellation/displacement pushed the visible terrain surface downward, that
  flat legacy layer showed through and matched the pattern visible with terrain
  rendering disabled.

Permanent fix:
- In `mclib/txmmgr.cpp`, skip non-water terrain alpha/detail batches.
- Keep `MC2_ISWATER` and `MC2_ISWATERDETAIL` passes intact.

If this ever needs to be revisited:
1. Verify the artifact still appears with terrain disabled (`RAlt+F5`).
2. Compare the terrain-off pattern against the terrain-on artifact.
3. Temporarily suppress only the non-water terrain alpha/detail pass.
4. If the artifact disappears while tess terrain remains, the underlayer is the culprit.

This avoids touching the tessellated solid path and prevents regressions where the
main terrain surface disappears entirely.
