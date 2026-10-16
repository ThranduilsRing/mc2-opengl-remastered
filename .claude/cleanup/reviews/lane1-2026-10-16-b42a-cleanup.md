# Lane 1 cleanup review, 2026-10-16 (B4.1c fixes, B4.2a, B4.3a)

Pinned: 16704d09d a4540d5a3 928a0291a b8a3028fe d2913b12e f5598116c. Read-only; nothing built or run (the lane's proofs are taken from lane1-state.md, not re-run).
Result: 0 blockers, 2 should-fix, 4 nits.

## Should-fix

S1. BUGS row 41 and the 16704d09d message overstate the reachable bug. `MOVE_readData` (mclib/move.cpp:589-640) reads maps 1 and 2 and the
special-area packet only inside `if (!GlobalMoveMap[0]->badLoad)`. A refused FIRST map therefore never uses its returned count; the wrong
cursor only mattered when map 1 (hover) was refused and map 2 / the special-area packet follow it. The fix is still right (every other
return is the layout size), but the row says "the first global map ... second and third maps ... read from the wrong packets". Reword the row to
the map-1 case, and say that a refused map 0 skips the rest of the movement load.
S2. `layoutPacketCount` saturates at 0x7fffffff, but move.cpp then sums `whichPacket + n0 + n1 + n2 + 1` in a 32-bit `long`: a hostile map-1 count of
0x7fffffff makes the next init cursor overflow (signed UB, in practice negative -> a Fatal from readPacket). The refusal is correct and the
outcome is a load failure either way; cap the returned value at the packet file's packet count (or document the saturation as "the next read fails")
and add the overflow case to the doctest.

## Checked and fine

- Layout size: `13 + numDoorInfos + (numDoors + NUM_DOOR_OFFSETS) * 2` is the same expression as the eight success returns (lines 155-377); computed from the
raw counts BEFORE they are zeroed (move_globalmap_build.cpp:144); negative infos or doors fall back to `whichPacket - startPacket`; long long intermediate, so no overflow in the helper.
- Ceilings: infos <= numAreas*255, links <= (numDoors+2)*2*257. Because numAreas/numDoors are tested first, the later checks see in-range values. Boundary and zero-area/zero-door cases are in test_globalmap_limits.cpp.
- Doctest through `GlobalMap::init` is in the `mc2_authored_order_seam` source list (CMakeLists.txt:932). Map A (links 1,000,000 > 26,728) reaches the numDoorLinks refusal, returns 124, map B is initialised at 124 and refuses on height. The log lines are checked by packet number. The second case reaches the negative-doors fallback (returns 10). The guard restores the heap and the log stream override.
- B4.2a: every touched loader scratch (colour map x3 paths, normal map, tile buffer, detail normal/displacement, nine material layers x2, baker height/shadow/fractal/output, .txm LZ read and write buffers) is a vector. Empty file, short read, header-less and body-short TGAs are refused with one `MC2_LOG(Io, Error)` and the decode is skipped (`loadTga` then `tgaPixelsPresent`; 24 bit = 3 bytes, anything else 4, overflow-safe size_t math, 1 GiB file ceiling). The old early-return and `break` leaks (`free` skipped on `allLoaded=false`) are gone because vectors own the data. `normalLayers`/`dispLayers` are views into `normalStore[9]`/`dispStore[9]` that outlive the upload block.
- The fractalPass noise buffer is value-initialised (zero); the lane's finding is correct and documented as offline-bake only. The other bake buffers keep their old zero fill by value-init.
- No per-frame allocation introduced: all of these run at mission load or in offline/editor bakes. `createTransition` read/compress buffers were malloc'd per call before and still are, now with RAII.
- Kept for B4.2b, listed in the commit message and notes: ColorMap, txmRAM, cpuColorMap, the third colour-map bake path, terrain.cpp tables.
- B4.3a: stores are sized once in `start()` (`assign`/`resize`), views are set from `.data()` right after, and nothing resizes a store while a view exists; `destroy()` nulls all four views before the swap-to-empty frees. The hardware-node block and the index table were never freed before and now are. Lookup order and values are unchanged (zero fill == `MC_VertexArrayNode{}` under the new static_assert; the texture nodes still get `init()`). The mc2.fx read buffer is local and lives until after `Load()`, the same lifetime as the old `Free` at function end.
- Include-only commits: a4540d5a3 touches the three terrtxm*.cpp files by one added include each (one blank line replaced); b8a3028fe replaces one blank line with `#include<vector>` in crater.h and txmmgr.h. tga_scratch.h has no users in that commit. Both are really include-only.
- Ratchet baseline: cxx17 LG05 +4 / LG06 +4 come from tga_scratch.* plus the loaders, and are named in lane1-state.md and the f5598116c subject; LG01 -3, LG09 -33 fall. `head` moved to d2913b12e.
- Comments are WHAT+WHY; no journal comments added (the "Left for B4.3b" and "kept views" notes live in commit messages / the notes file, not source).
- Line endings: for all 29 touched files, CR count == line count in both the parent and the commit (all CRLF, new files included). No CRLF damage and no mixed endings found; the two slips the lane admits are not in this range.

## Nits

N1. `loadTga` and `tgaPixelsPresent` (the two functions that log) have no test: test_tga_scratch.cpp covers `bodyFits` and `readAllBytes` only; `readHeader` is untested too. They need `File`, which is why they sit in the .cpp, but a refusal log-line case through a fake would close the claim "one log line".
N2. After a refusal in `TerrainColorMap::init` (`return -1` for the colour map) the later steps (normal map, detail, material arrays) are skipped; the caller's handling of -1 was not traced here. Confirm it is the same path as the existing `result != NO_ERR` returns.
N3. `getScaledColorMap` and `burnInShadows` now `return` silently after the log line with nothing written to the output buffer (editor-only tools); fine, but say so in the header comment.
N4. `#include<vector>` has no space, unlike the neighbouring includes.

## Efficiency retrospective
- One parent/child CR-count loop over the commit file lists settled the line-ending question for all 29 files in a single call.
- The most useful move was reading `MOVE_readData`'s caller before trusting the BUGS row; the returned count's reachability was not visible from the diff.
- Not re-run: build, doctests and captures; this pass trusted the lane's proof lines, so the render-proof claims are unverified here.
