# TreeIsland S0: baseline, consumer audit, hash harness

Design: memory `decision-vegetation-treeisland-design-2026-11-03`. This slice changes no rendering.

## 1. Baseline

All runs on the AMD Radeon RX 7900 XTX (log: `[BENCH_DEVICE v1] device="AMD Radeon RX 7900 XTX"`, `[VK_ENGINE_CONTEXT] ... device_name="AMD Radeon RX 7900 XTX"`; `MC2_HARNESS_GPU=xtx`). Install E:/mc2-installs/lane8 at commit b19241244 (code identical to nifty 0d6326012). Evidence under `E:/mc2-archive-2026-10/proofs/treeisland-s0/`.

| Pose (stock) | Frame CPU total / p95 (ms) | DrawScreen p95 | GameCamera::render p95 | Notes |
|---|---|---|---|---|
| mc2_01 oblique_default (6052 frames) | 9094 / 2.10 | 1.08 | 0.92 | draws.props 32.0, terrain 21.8, shadow 6.7, mech 10.8 (PERF_COUNTERS); tree GPU scope `VKIsl.Props.Trees` ~0.000 ms in sampled frames, prop co-write tier-1 instCount=365 |
| mc2_10 wolfman (3359 frames) | 9020 / 4.91 | 2.36 | 2.18 | sim 52.5% / render 47.1% |

RGP captures (stock, XTX, `rgp_capture.py`): mc2_01 oblique_default `rgp_mc2_01_oblique_default_stock/mc2-20261103-190423683.rgp`; mc2_10 wolfman `rgp_mc2_10_wolfman_stock/mc2-20261103-190654719.rgp`; mc2_03 scout_1 `rgp_mc2_03_scout_1_stock/mc2-20261103-190856008.rgp` (earlier duplicates from aborted runs sit in the same folders).

Not measured here (unproven): mc2_01 cross_map_view and mc2_24 wolfman (cross_map_view lives in bookmark file `mc2_01`, not `mc2_01_camera`; its first RGP try produced no .rgp, and `mission_cpu_profile.py` waited out the host game lease held by lane9 treebench); every tree-showcase mod run (lane9's treebench already measures mod vs stock at runway_overview/cliff_wall_sideon/cross_map_view, use its numbers); the per-zone CPU cost of the re-type loop at gos_static_prop_batcher_shadow.cpp:118-180 (no dedicated Tracy zone exists; the default zone list does not name it, so S1 step 0 is to add a zone "Props.TreeRetype" and read it). Vulkan enumeration: the engine selects the XTX; the RTX 2070 SUPER was not used.

## 2. Consumer audit (state a separate TreeIsland must keep in sync)

Trees are `TerrainObject`s (subType `TERROBJ_TREE`, `[TreeData]` block, code/terrobj.cpp:401-404) with a `TreeAppearance`
(mclib/bdactor_tree.cpp). Handles are assigned in mission-file order per object block (code/objmgr.cpp:292-295,
code/objmgr_io.cpp:57-125 fix-up of `objBlockInfo.firstHandle`).

| Consumer | Reads / writes | file:line |
|---|---|---|
| LOS forest bit (cells) | read from mission cell data, `GameMap->setForest` | mclib/move.cpp:857; accessors mclib/move.h:488,911-916 |
| Forest bit readers | LOS kernel snapshot (`cellForest`, `MaxTreeLOSCellBlock` limit), path goals, global-map doors, debug lines | code/los_kernel.cpp:49,135; code/team_los.cpp:126,161; mclib/move_movemap_goals.cpp:473,678; mclib/move_globalmap_doors.cpp:160; mclib/quad_lines.cpp:215,264 |
| `MaxTreeLOSBlock` | gamesys.fit, also re-read at load | code/mission_init.cpp:225; code/saveload.cpp:697; code/mission.cpp:685 |
| Per-tree LOS height (`markLOS`) | writes cell local height from the STOCK LOD0 (or damage) shape, never the draw LOD | mclib/bdactor_tree.cpp:643-700 (stock shape selection at 647-656); virtual mclib/appear.h:677 |
| `markLOS` on destroy | `markLOS(true)` then `markLOS()` after `setObjStatus(DESTROYED)` | code/terrobj.cpp:1329,1335 (damage), 1388,1394 (collision); building twin code/bldng.cpp:1539-1559, gate code/gate.cpp:553,799,810 |
| Tree fall start | mech/vehicle collision sets `OBJECT_FLAG_FALLING` | code/terrobj.cpp:486-503; carnage hook code/carnage.cpp:216 |
| Falling animation | `TerrainObject::update` runs the fall, sets `FALLEN` / clears `FALLING`, pushes `setShadowMoving` | code/terrobj.cpp:806-846 (FALLEN at 821-822), 729-730, 755; `TreeAppearance::update` mclib/bdactor_tree.cpp:380 |
| Flags FALLING / FALLEN | defined code/dgameobj.h:53 (FALLEN); static-registry eligibility forces dynamic while FALLING | code/terrobj.cpp:915-931 |
| Destroyed visual swap | `treeDmgShape`, shape swap deferred to presentation | mclib/bdactor_tree.cpp:768 (`destroy`), 643 (wrecked source); code/terrobj.cpp:1159-1200 |
| Static registry / batcher | per-instance re-type LOD every frame (CPU), unordered_map | GameOS/gameos/gos_static_prop_batcher_shadow.cpp:118-180 (`mc2_color_prop_island_get_frame_feed`); registration mclib/bdactor_tree_static.cpp |
| Binary tree LOD (800 wu) | `TreeAppearance::update` | mclib/bdactor_tree.cpp:730-765 (design memo) |
| Baked per-instance light | tree light bake gate | mclib/bdactor_lightbake.cpp (whole file), consumed by static prop registry gos_static_prop_registry.cpp:108,181,326 |
| Shadow bake | static D16 object map includes trees; no shadows on fallen trees | GameOS/gameos/vulkan_shadow_island_live.cpp:1406; code/terrobj.cpp:1110-1120; gos_static_prop_registry.cpp:781 (dynamic casters) |
| Saves | terrain object slots saved per handle (720-byte `TerrainObjectData`), flags/damage/status restored then appearance rebuilt | code/saveload.cpp:307-312; code/objmgr_io.cpp:929-933; code/save_layout_guard.h:41; code/object_slot_codec.h:72 |
| Minimap / tac map | no tree reference found in code/gametacmap.cpp or controlgui_update.cpp (trees do not draw); forest is shown only via debug lines quad_lines.cpp:215-264 | n/a |
| Editor | renders trees through its own `TreeAppearance` (toggle `renderTrees`), forest tool (Forest struct, TreeTypeN ids) | editor/EditorObjectMgr_render.cpp:41,171,284,362-367; editor/forest.cpp:132,182; editor/EditorObjectMgr.h:246-251 |
| Test fixtures | uicmd fixture reads `tree->treeShape`, forces rebake | code/missiongui_uicmd_fixture.cpp:269,286,305 |
| Mods (counts) | MCO 41,767 tree instances over 37 missions, Exodus 10,141 over 16, stock 52,836 over 33 (tools/tree_hash_baseline.json) | per-mod `object2.pak` `[TreeData]` types: 347 / 403 / 403 |

Key invariant for S1: LOS never reads draw state. `markLOS` already builds the stock shape on the sim side
(bdactor_tree.cpp:647-656), so the island may own all draw data as long as `OBJECT_FLAG_FALLING/FALLEN`, status and the
cell height writes stay on `TerrainObject`.

## 3. Hash harness

`tools/tree_hash.py` hashes packet-1 records (40 B: type, position, rotation, damage, team, parentId; file order = handle
order) of every tree type (type whose Object2.pak packet has `[TreeData]`) per mission. Baseline
`tools/tree_hash_baseline.json` covers 33 stock missions (all 24 campaign ones), MechCommanderOmnitech (37) and Exodus (16).
`py -3 tools/tree_hash.py check tools/tree_hash_baseline.json`; tests `tools/test_tree_hash.py`. Roots that are absent are
skipped. Limit: it guards the INPUT records the island must consume; the runtime check of S1 (live flag/handle parity) is a
separate S1 task.

## 4. Cheapest engine-side wins before the island

Ordered by cost/benefit; each is engine-side and needs no new content.

1. **Stop re-typing every instance every frame** (gos_static_prop_batcher_shadow.cpp:118-180). Today each frame walks all
   instances through an `unordered_map` and re-evaluates the LOD with the +-8% hysteresis. Tree LOD is binary at 800 wu
   (bdactor_tree.cpp:730-765), so cache the per-instance LOD bit and only re-evaluate when the camera moves out of its
   current cell or a distance band is crossed (a conservative bound of `|dcam| < hysteresis margin`). Replace the map with a
   flat array indexed by static-prop slot (handle order). This is the same data the island's TreeState SSBO needs, so it is
   S1 groundwork, not throwaway.
2. **Cutout (alpha-test) leaves for LOD0 only**: the material feed already declares alpha-test 0.5 for 667 types
   ([VK_COLOR_PROP_ISLAND] material feed active), so crown cards cost overdraw, not new code; measure at wolfman first.
3. **Per-type persistent LOD route**: keep one indirect-draw slice per (type, LOD) and move instances by writing the dirty
   list only (matches the S1 design), instead of rebuilding type runs per frame.
4. **Contract the black-tree class early**: add the zero-irradiance counter before S1 so the new path can assert it.

Not recommended before S1: touching `markLOS` or the forest bit (LOS never reads draw state; see Section 2).

## 5. S1 plan, refined

Changes to the S1 plan from the audit:

- **State parity is the S1 gate.** The island consumes handle order (hash harness guards the input records). Add a runtime
  check that, after load and after a scripted tree fall, `TreeState[handle]` equals the flags derived from
  `OBJECT_FLAG_FALLING/FALLEN` and object status (terrobj.cpp:806-846, 1329-1394).
- **Falling stays on the old path**: `TerrainObject::update` drives the fall and calls `setShadowMoving`; the island only
  reads the flag. While FALLING the instance leaves the island (static registry eligibility already forces dynamic,
  terrobj.cpp:915-931) and returns as FALLEN.
- **No save change**: slots stay 720 B (save_layout_guard.h:41); TreeState is rebuilt from flags in objmgr_io.cpp:929-933.
- **markLOS is untouched**: it uses the stock shape (bdactor_tree.cpp:647-656), so the island may swap draw meshes freely.
- **Editor must render the island or keep its own TreeAppearance** (EditorObjectMgr_render.cpp:41,171,284); recommended: keep
  the editor on its current path in S1 and add a toggle, since it has no sim flags.
- **Mods**: scale is MCO 41.8k / Exodus 10.1k / stock 52.8k trees over all missions, with up to 403 tree types; per-mod
  opt-out stays in the plan. Run `tools/tree_hash.py check` in every slice.
- Win 1 above lands first as the S1 data layout (flat handle-indexed LOD/state arrays), so S1 starts from measured numbers.
