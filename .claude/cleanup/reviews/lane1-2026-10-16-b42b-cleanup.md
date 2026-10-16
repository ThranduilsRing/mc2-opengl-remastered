# Lane 1 cleanup review, 2026-10-16 second session (advanceCursor, BUGS 41-44, B4.2b, B4.6)

Pinned: e7e27ec75 e98f0e3be 6fd53a32e 785c03fd0 1cb2d8bc4 ecfa7fe74. Read-only; nothing built or run (proofs taken from lane1-state.md).
Result: 0 blockers, 1 should-fix, 6 nits. Digest attribution: the orchestrator's 06c4c08be hypothesis does NOT hold (section 4).

## Should-fix

S1. code/collsn.h (1cb2d8bc4) puts `#include<vector>` INSIDE `#ifndef MCLIB_H ... #endif`. When mclib.h was already included (the PCH
case) the include is skipped and the header relies on someone else having pulled in <vector>. Move it below the guard, as 6fd53a32e did
for terrtxm2.h.

## Nits

N1. advanceCursor (e7e27ec75): every map1 / map2 / special-area / footprint / return sum is replaced; `whichPacket + numMissionMapPackets`
(the map-0 start, move.cpp:588) stays a raw sum. Harmless (MissionMap::init returns a small count or 0, and 0 is Fatal), but say so or wrap it.
N2. The saturated value is unreachable as a cursor on the map-1/map-2 path (readPacket at 0x7fffffff fails -> existing Fatal). On the
map-0-refused path the return is mission + map0 + 2 and is NOT followed by a read: saveload_load.cpp:134 does `currentPacket += MOVE_readData(...)`
before its `badLoad` STOP, so a saturated return makes that `+=` overflow (UB, one line before a STOP; mission_init ignores the return).
Either test badLoad before the `+=` or note it. Row 41's "a refused first map never used its count" is true of the cursor, not of the return.
N3. The doctest covers advanceCursor (sum, saturation from either operand, chained saturation, negative clamp) but not the MOVE_readData
chain; acceptable since the chain is a straight substitution.
N4. resetBaseTexture's refusal (loadTga / tgaPixelsPresent, exactly one MC2_LOG line each, then releaseColorMapScratch) runs AFTER the
existing tile nodes were removed and set to 0xffffffff, so a truncated reset TGA leaves the map with no colour tiles. Better than reading
past the end; say it in the comment or the notes.
N5. cxx23-step4-b4-notes.md says the Terrain statics are kept because of "(no static-duration owners)"; lane1-state.md says "a vector there
would be a static-duration owner". Same reason, contradictory wording; use the state file's.
N6. CollisionSystem::collisionHeap (65,535 bytes) now only backs CollisionGridNode operator new; the grid and nodes no longer come from it.
Not a defect; a candidate for B4.6's follow-up.

## Checks that passed

- B4.2b: every former Free / free / delete of ColorMap, txmRAM[i].ourRAM, txmRAM, textures, the two UserHeaps and the JPG temp buffers is
  gone (destroy, recalcLight, resetBaseTexture, getScaledColorMap, init cleanup); COLOR_MAP_HEAP_SIZE / COLOR_TXM_HEAP_SIZE deleted; no
  remaining reference to colorMapHeap / colorMapRAMHeap. Tile copies: one contiguous block of count x 256x256x4 bytes, same per-tile size;
  handle arrays initialised to 0xffffffff (normal-map store too). The dead `usedJPG` / gos_Free branch removal is correct (it was never true).
  The never-freed malloc'd normalMapTextures is now owned (incidental leak fix, recorded in the state file). resetBaseTexture used to Malloc a
  second `textures` array over the live one (leak); `ownTextureHandles` replaces it. Allocation is load / bake / editor only, none per frame.
- MapData::terrainFaceCache: built on first buildTerrainFaceCache, cleared only by invalidateTerrainFaceCache; same lifetime as before.
- Kept rows have stated reasons (cpuColorMap / cpuDispAlpha freed by GameOS with free(), lane 2's file; Terrain statics; TerrainTextures arena).
- tryLoadBurninJpg: the new 256 MiB refusal logs one line; every early return leaves the vector empty, so the caller falls back to the .tga.
- B4.6: alert records / grid cells / nodes sized once while `!gridIsGo` (same counts; `?:1` keeps data() non-null); the per-frame
  CollisionGrid::init still only memsets gridSize / nodeSize bytes (0 when maxObjects is 0); destroy swaps the stores empty and re-inits.
  Index bounds unchanged (gosASSERT nextAvailableNode < maxObjects, pre-existing).
- 6fd53a32e is include-only (one line). Ratchet: every baseline.json count only falls; TOTAL 15,743 -> 15,709 matches the diff; journal
  baseline not tightened, as stated.
- BUGS 42-44 match lane 2's proposals (lane2-state.md task 2) word for word except: row 43 "not pinned" upper-cased and "row 20 per-tap
  counts" shortened to "per-tap counts"; row 44 status drops "orchestrator may keep it in the lane review instead". Faithful.
- Comments are WHAT+WHY, no journal tags added. CRLF: every touched source file is CRLF-consistent with its parent (CR count = line count).

## 4. mc2_10 WeaponBoltData digest attribution

06c4c08be changes only scripts/run_visual_capture.py (`build_capture_env` sets MC2_DETERMINISTIC_RNG=1 with the fixed clock) and its test.
scripts/run_smoke.py never imports or calls run_visual_capture. tools/saveload_roundtrip.py builds its env as `dict(sd.GATES, ...)`, where
sim_determinism.GATES already holds MC2_DETERMINISTIC_RNG=1, MC2_SMOKE_FIXED_TIMESTEP=1 and MC2_SIM_IDENTITY_TRACE=1, and has since the tool
was created (d6fcd12aa, 2026-10-11); it then runs run_smoke.py with that env. So saveload runs were seeded before and after 06c4c08be; the
commit cannot explain the shift. Lane 1's revert evidence stands: the only code between d2913b12e and ecfa7fe74 outside lane 1's own files
is GameOS (static-prop batcher, terrain atlas/field store, VK islands, gameos_graphics*), i.e. render-side. With sim_determinism passing on
mc2_10, the likeliest cause is another render-written field leaking into a bolt's saved state (the row-40 class), or a changed deployed
asset, not RNG. Recommend: diff the two saves' WeaponBoltData packets field by field (save_inspect.py) before re-recording the baseline.
