# Water planar reflection: scene preparation

The owner reports a canopy-shaped hole over the reflected cliff at mc2_01
saved_210809. The producer still uses SSR. This is a partial implementation;
it does not fix those pixels or claim visual acceptance.

The recording seam landed at 677cdee8d: `PropRecPlan` / `record_props_cmds`
in `GameOS/gameos/vulkan_color_prop_island.cpp`, alongside terrain's existing
`TerrainRecPlan` / `record_terrain_cmds`.

Continuation 1 adds independently owned scene preparation:

- `water_planar_view.h`: world-Z mirror composed before projection, winding
  parity and conservative native Vulkan frustum/above-water AABB admission.
  `shaders/include/water_planar.hglsl` shares the mirror/clip-distance functions
  with offline C++ tests; production shaders do not consume it yet.
- `water_planar_terrain.h`: `TerrainView` reads the whole-map height/bake fields,
  never the main patch list. It uses uniform finest topology (4x bake when
  present), retains partial edge patches, and caches bounds by field versions.
  Every prepare reselects for the current reflected matrix and water elevation.
  The eye is mirrored too; `cameraPosMC2` is misleadingly named Stuff/Y-up.
  `prepareTerrainView` obtains materials/fields through the load feed even when
  the main terrain list is empty. The view owns its patch vector; resource bytes
  are borrowed under the existing feed lifetime. The object cannot be copied.
- `water_planar_props.h`: `preparePropInstances` walks all generation-valid
  registry ranges, including non-shadow-casting alpha cards and offscreen actors.
  Known type bounds are transformed through Stuff matrices and the canonical
  axis conversion before reflected-view admission. Missing bounds retain the
  instance. Type grouping is stable and leaves the source registry unchanged.
  The caller must supply the unmirrored prop matrix to `View`, independently of
  terrain's matrix, and must not supply main HZB visibility. Reflection instances
  require their own GPU storage/compaction; they cannot overwrite main buffers.

Offline coverage lives in `tests/unit/test_water_planar.cpp`: plane, clipping,
matrix order, determinant/winding, six clip faces, independent terrain visibility,
fine-bake peaks, cache invalidation, view ownership, Stuff prop transforms,
missing bounds, stale/tombstoned recipes, stable grouping and alpha-card inclusion.
No GPU layout, barrier, pipeline or existing shader behavior changes here.

Remaining integration, in dependency order:

1. Stage separate reflected UBOs/descriptors and prop instance/compaction buffers.
   Use the new terrain and registry APIs above. Preserve main resources until
   their submission tickets retire. Source materials, alpha packet rules and
   day/night lighting must match the main pass. Do not reuse main HZB results.
2. Create attachment-compatible RGBA16F variants of the terrain and opaque /
   alpha-tested prop pipelines. Reverse prop CLOCKWISE winding once; terrain
   remains double-sided. Clip original world Z below Terrain::waterElevation
   in both shader paths, including terrain's hires early-return path. Shader
   clip-distance capability is not currently enabled in vulkan_context.cpp;
   fragment clipping is an available route without adding that device feature.
3. Add colour-attachment usage to the existing full-res reflection image and a
   dedicated D32 depth image (~31.6 MiB at 4K). Preserve water bindings 8/11.
   Reuse the recorders with reflected attachments; their current begin/end of
   command buffer and MRT pass setup need adaptation, not nested recording.
4. Replace record_reflection's SSR dispatch in vulkan_water_island_run.cpp:26
   only when both terrain and props are ready. Preserve the approved glint and
   cliff facet fix (33a581220 if a rebase is needed). No history or new gate.
5. Shader/layout + sync-val, production compile, commit-first isolated deploy
   to E:/mc2-installs/water-reflect-planar-1b; never retry a refused deletion.
   Then smoke-lite and 4K saved_210809/cliff/owner-pose/night captures, using
   the mirror-1 continuation-c recipe and a native-resolution deadline (six day
   poses permitted). Open sheets and report observations. Owner accepts.

All GPU tiers: cost NOT MEASURED. Reflected GPU draws, runtime and pixel proof
remain NOT RUN. The continuation state and proof receipts live outside the
worktree under the assigned slot/proof roots.
