<!-- doc-status: current | 2026-10-15 | lane 6 -->
# Lane 6 state (VK producers)

Worktree `A:/Games/mc2-lane6`, branch `claude/lane6-producers` (from nifty e6f9ac11e). Install: none used.
Goal: VK standalone on the data side so the GL producers can be deleted. Census: `vk-producer-census-2026-10-15.md`
(14 true GL dependencies: 11 lane 6, 3 lane 2; 13 placement-only).

## Done
- baa0b195a census (docs).
- 242b9582c census T1 (gos-handle textures), T4-T6 (terrain fields), T2 consumer half (colour-prop geometry):
  `texture_decode.cpp`, `terrain_field_store.cpp`, `mc2_static_prop_geometry_cpu`, `gos_GetTextureRgba`.
  Bug fixed on the way: `gos_TerrainLodChunk_UploadHeightPatch` never bumped the VK snapshot version, so the VK terrain kept
  pre-patch heights after a runtime height edit (BUGS-FOUND-FIXED row: orchestrator to add after the runtime proof).

## Proof
| check | result |
|---|---|
| ninja mc2 (build64-ninja) | PASS |
| build_all_targets --root-only | PASS (1673/1673) |
| doctest (build_contract_harnesses --run --unit-only) | PASS 3201 cases, incl. new test_texture_decode / test_terrain_field_store |
| check-contracts ratchets (cxx17, dup_lines, file_size, axis_swizzle) | OK, unchanged |
| gl_shader_variants | was STALE from a one-line shift; regenerated in 242b9582c |
| check-contracts other FAILs | container_callsites (editor/ObjectivesSummaryPanel.cpp), background-launch inventory, gated_layout_macros (fails on the base too), tools_tests row for the variants check: none in lane 6 files |
| GL tier1 + VK tier1 (MC2_VULKAN_VALIDATION=1) + [VRAM v1] before/after | NOT RUN: the host lease was held by lane 1 (batch, pid 616) on mc2-win64-v0.4 for the whole window |

## Open (next session, in order)
1. Runtime proof of 242b9582c: take the batch lease (`--tag lane6`) on mc2-win64-v0.4, deploy, GL tier1 + VK tier1 with
   validation (errors=0 warnings=0), compare `[VRAM v1]` against the e6f9ac11e exe. Expect CPU RSS up by the retained prop
   staging + terrain fields (a few MB), VRAM unchanged.
2. Pre-compressed (`.ktx2` BC7) gos textures still reach VK through a one-time GL readback inside
   `gosTexture::ensureRgbaMirrorFromGpu`: needs a CPU KTX2 path (keep the blocks, VK uploads BC7 directly).
3. T3 / T7 / T9 (prop albedo arrays, colormap atlas, terrain normal array): neutral array / atlas builders with CPU blobs.
4. T8 cement atlas, T10 / T11 water shore height + shoreline mask (field store).
5. Owners are still static-duration unique_ptr / file statics (terrain store lives Init..Destroy; prop staging is a file
   static): fold into a subsystem object with step 4 ownership.

## Seams needed from lane 2
- `vulkan_shadow_island_live.cpp:815-836` (shadow geometry upload): replace the `glGetBufferSubData` of `feed.glVbo/glIbo`
  with `mc2_static_prop_geometry_cpu` (declared in `vulkan_color_prop_island.h`), then drop `glVbo`/`glIbo` from
  `Mc2ShadowGeomFeed` (`vulkan_shadow_island_live.h:37`). Lane 6 then deletes nothing more there; the GL buffers stay for GL.
- T12-T14 (static / dynamic / CSM shadow maps, HDRI) are lane 2 producers.

## Scratch
Mine (session scratchpad, deleted by the harness): cfg/build/unit/cc logs. In the worktree: `build64-ninja/`,
`build-alltargets/`, the unit build tree (untracked build trees, lane 6 created them; delete at lane close).
