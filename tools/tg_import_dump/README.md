<!-- doc-status: current | reviewed:2026-10-11 -->
# tg_import_dump — engine-import inspector (MECH-IMPORT-TGDUMP-1)

Game-free CLI that runs the **real** mclib importer (`ImportGeometryFromFile`) on a
GLB/FBX and dumps the resulting `TG_TypeMultiShape` as JSON. **No GL, no game, no
deploy.** Closes the seam between `mech_import_harness` (which validates the
*source GLB* via Assimp) and the game: the bugs that cost the most time were in
engine **import → TG translation** (empty shapes → crash, texture slot 0 = blip →
black, merge result, bbox/scale, orientation) — all invisible until a 4-5 min
relink + deploy + smoke. This catches them in <1s.

## Build (standalone; never touches build64/)

```
cmake -S tools/tg_import_dump -B build64-tgdump -G "Visual Studio 17 2022" -A x64
cmake --build build64-tgdump --config RelWithDebInfo --target tg_import_dump
```

## Use

```
build64-tgdump/RelWithDebInfo/tg_import_dump.exe <model.glb|.fbx> [--no-ground]
```

JSON → stdout; importer traces (`MC2_MECH_SKEL_TRACE`) → stderr,
so stdout stays pure JSON. The skinned-mech bake respects `MC2_MECH_SKEL_HEIGHT`.

## Reports

shape/texture counts · per-shape node/verts/tris/slot-0 texture/bbox · empty-shape
count · aggregate bbox + dims · tallest axis + derived height · red-flag heuristics:

| flag | meaning (the bug it catches) |
|---|---|
| `slot0_blip` | texture slot 0 is a blip atlas → mech renders black |
| `slot0_missing` | slot 0 is NULLTXM/empty → untextured |
| `empty_shapes_present` | NULL-vertex shapes present → mech GPU/recipe crash |
| `orientation_suspect` | tallest axis ≠ Y → upside-down / sideways |

## How it links (game-free proof)

Links the real `assimp_importer.cpp` + `mech_skel_import.cpp` + `tgl/msl/stuff` TUs
+ the same CRT-backed `stubs.cpp` the `tgl_loader_standalone_spike` proved (gos_*/
heaps/files; `eye`/`land`/`mcTextureManager` are render-only, never hit on the
import path). `tgdump_seams.cpp` defines 5 tiny render/frame-jobs/txmmgr-trace
globals the import never executes. One zlib (Assimp's bundled `zlibstatic`, shared
with FastFile). If the import path ever pulled in a game-render symbol, the link
would fail — a clean link proves it stays game-free.

## Next infra (queued)

- `MECH-SHOT-SELFVERIFY-1` — auto-screenshot the in-game mech (final visual gate).
- `MECH-BONE-PARITY-GATE-1` — diff harness `gpu-bones` checksum vs engine trace (1B).
