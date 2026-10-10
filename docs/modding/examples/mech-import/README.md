<!-- doc-status: current | reviewed:2026-10-11 -->
# Example Mod: Mech Mesh via glTF (`[Import]` Opt-In)

> **AVAILABILITY**
> - **Works on:** this checkout. Verified present: the `[Import]` opt-in probe (`mclib/mech3d.cpp:346-360`), the `.glb`→`.fbx`→`.ase` format probe (`mclib/msl_load.cpp:240-281`), and the Assimp-backed reader (`mclib/assimp_importer.cpp`).
> - **Build flag:** `ENABLE_ASSIMP_IMPORTER` (`CMakeLists.txt:204`), **default `ON`**. No rebuild needed on a normal build of this checkout.
> - **No runtime `MC2_*` gate controls whether this mechanism exists.** The opt-in is per-mech: a mech's own `.ini` either has an `[Import]` block (uses the modern importer for LOD0 only) or it doesn't (takes the original `.ase` path, byte-identical to stock). Diagnostics: `MC2_ASSIMP_TRACE=1` (probe/import trace), `MC2_GLTF_AXIS` / `MC2_GLTF_YOFF` / `MC2_GLTF_GROUND` (orientation tuning).
> - **★ MOD-OVERLAY CAVEAT (read before you install anything):** the `.ini` shadows correctly through `mods/<id>/data/tgl/`, but the paired `.glb` geometry file **does not** — Assimp's file reader bypasses the mod overlay entirely (see "Why this doesn't overlay cleanly" below, verified against the source and by exercising the shared resolver replica). **This tutorial instructs a direct drop-in to your own game install's `data/tgl/` folder, not a `mods/<id>/` install**, because that's the only way that reliably works today.
> - Confirmed live precedent: the stock `Atlas.ini`/`Atlas.glb` pair already ships this exact mechanism in the deploy tree (`data/tgl/atlas.ini` → `[Import] st Source = "Atlas"` → `data/tgl/Atlas.glb`) — this example mirrors that pattern on a different chassis.

This collection assumes you've read [`../getting-started.md`](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher, `mc2mod.py`, diagnostics) and skimmed [`../mechanism-map.md`](../mechanism-map.md) (the "Mechs" section, rows for the `[Import]` opt-in and the format probe). This doc only covers what's specific to *mesh-via-GLB*.

## What this example does

It swaps the stock **Warhammer**'s LOD0 mesh for a placeholder glTF box-mech, using the exact same `[Import]` mechanism the shipped `Atlas.ini` already uses in production. It's deliberately crude art (a 6-box "toy soldier": two legs, a torso, two arms, a head) — the point is proving out the *mechanism*, not modeling a mech. Warhammer's weapons, gestures (animation), footprint, and everything else about it are untouched.

## Prerequisites

- A working MC2 install with `mc2.exe` built with `ENABLE_ASSIMP_IMPORTER` (default — almost certainly already true for any build of this checkout; confirm by checking your build log or just trying the steps below).
- Python 3 (`py -3`) only if you want to regenerate the placeholder `.glb` yourself — a pre-built one already ships in this folder.
- Your **own** copy of the game install (not the project's canonical dev deploy roots — see `../getting-started.md` §5).

## Why this doesn't overlay cleanly

The engine has two unrelated file-access paths, and only one of them goes through the mod overlay:

1. **The `.ini` itself** is opened by `FitIniFile`/`File::open` (`mclib/file.cpp:907`), which calls `TryModOpen()` (`mclib/file.cpp:592`) first — this is the normal mod-overlay path documented in `getting-started.md`. A `data/tgl/warhammer.ini` shipped in `mods/<id>/data/tgl/` **will** shadow the stock one correctly.
2. **The `.glb` geometry** is read by `TG_TypeMultiShape::LoadFromFile()` (`mclib/msl_load.cpp:301`), which:
   - first calls `fileExists(probePath, FILE_ON_DISK)` (`msl.cpp:457`) to decide whether to attempt the import at all. `fileExists()` (`mclib/file.cpp:935`) *does* consult the mod overlay (`TryModOpen`) before falling back to a plain `_stat()` — so this existence check can succeed purely because the file lives in `mods/<id>/data/tgl/`.
   - but then calls `ImportGeometryFromFile(probePath, this)` (`msl.cpp:463`), which constructs a bare `Assimp::Importer` and calls `imp.ReadFile(path, ...)` directly (`mclib/assimp_importer.cpp:480-482`) — **no `SetIOHandler()` is ever called**, so Assimp uses its own default OS-level file reader on the literal relative path string (`data/tgl/<base>.glb`), completely bypassing `TryModOpen`/`File::open`.

The practical effect: `fileExists()` reports "found" (via the mod overlay) even when the `.glb` only exists inside `mods/<id>/data/tgl/`, which is enough to make `LoadFromFile()` *attempt* the import — but the actual read then fails, because Assimp looks for the file relative to the process's current working directory, not inside `mods/`. On failure, `ImportGeometryFromFile` logs `Assimp ReadFile failed: ...` (`assimp_importer.cpp:492-493`) and `LoadFromFile` silently falls through to the stock `.ase` (`msl.cpp:467-471`, `471`) — so the mod would appear to do *nothing*, with no error dialog, just a log line you'd only see with `MC2_ASSIMP_TRACE=1`.

This was independently confirmed by running this repo's own offline resolver replica (`tools/mod_install/resolver.py`, the module `mc2mod check` uses, documented as mirroring `file.cpp` exactly) against a synthetic install with this example's mod active:

```
{'key': 'data/tgl/warhammer.ini',           'layer': 'mod:mech-import-example', ...}
{'key': 'data/tgl/warhammer_glb_demo.glb',  'layer': 'mod:mech-import-example', ...}
```

Both resolve to the `mod:` layer at the `File`/resolver level — confirming the trap: the resolver (and thus `fileExists()`) is telling the truth about the *file's* location, but that layer is never consulted by Assimp's reader for the second file, only the first.

**Bottom line:** ship the `.ini` and `.glb` as a matched pair, dropped **directly into your game install's own `data/tgl/`** (not `mods/<id>/data/tgl/`). If you only want the `.ini` to be moddable-and-shareable-via-overlay and are fine hand-placing the `.glb` once per install, that hybrid works too — but it's confusing to explain to players, so this tutorial just tells you to drop both files directly.

## Step 1 — Get the files

This example ships two payload files (already generated, ready to use) under [`mod/data/tgl/`](mod/data/tgl/):

| File | What it is |
|---|---|
| `warhammer.ini` | A full copy of the stock Warhammer `.ini` (weapon nodes, gestures, bounds — all unchanged) with one `[Import]` block added at the top, `st Source = "warhammer_glb_demo"` |
| `warhammer_glb_demo.glb` | The placeholder mesh — 144 verts / 72 tris, one untextured material, generated by [`generate_placeholder_glb.py`](generate_placeholder_glb.py) (pure stdlib, no dependencies) |

If you want to regenerate the `.glb` (e.g. after tweaking the generator script):

```powershell
cd docs\modding\examples\mech-import
py -3 generate_placeholder_glb.py mod\data\tgl\warhammer_glb_demo.glb
```

## Step 2 — Drop the files directly into your game install

Because of the overlay caveat above, **copy the files straight into your own game folder's `data/tgl/`** — do *not* put them under `mods/`:

```powershell
# Replace <your-game-folder> with your own MC2 install (not a project canonical
# deploy root — see ../getting-started.md §5)
$dst = "<your-game-folder>\data\tgl"
Copy-Item docs\modding\examples\mech-import\mod\data\tgl\warhammer.ini $dst -Force
Copy-Item docs\modding\examples\mech-import\mod\data\tgl\warhammer_glb_demo.glb $dst -Force
```

This overwrites the stock `warhammer.ini` in your install with the modded copy. **Back up your original `data/tgl/warhammer.ini` first** if you want an easy way back to stock (or just delete the two files you copied in — that also restores stock, since nothing else references `warhammer_glb_demo.glb`).

## Step 3 — Launch and verify

```powershell
cd <your-game-folder>
$env:MC2_ASSIMP_TRACE = "1"
.\mc2.exe
```

Load any mission or skirmish setup that fields a Warhammer (the mission editor's unit roster is the easiest way to check, or place one yourself if you're comfortable with the editor — this tutorial doesn't ship a new mission).

**What you should see:** where a normal, detailed Warhammer would stand, you instead see a crude gray box-mech silhouette (two-leg / torso / two-arm / head), correctly grounded (feet at terrain level, roughly 12.5 units tall) and using the same footprint, weapon-firing points, and walk/run animations as the stock Warhammer — only the LOD0 shape changed. It ships **untextured** (the placeholder material has no base-color image, so the importer leaves the `"NULLTXM"` sentinel in the texture slot — `mclib/assimp_importer.cpp:263-266`), so expect flat/undiffused shading rather than a painted surface.

**In the console/stderr**, with `MC2_ASSIMP_TRACE=1` you should see lines like:

```
[ASSIMP_TRACE] LoadFromFile baseName='warhammer_glb_demo'
[ASSIMP_TRACE]   probe 'data\tgl\warhammer_glb_demo.glb' exists=1
[ASSIMP_TRACE]   calling ImportGeometryFromFile...
[ASSIMP_TRACE] ImportGeometryFromFile path='data\tgl\warhammer_glb_demo.glb'
[ASSIMP_TRACE]   calling Assimp::Importer::ReadFile...
[ASSIMP_TRACE]   scene meshes=1 materials=1 animations=0
[ASSIMP_TRACE]   SUCCESS
```

If the mesh doesn't look right (floating, sunk, sideways, or upside-down), the placeholder's grounding/axis assumptions may not match your build's defaults — tune at runtime without rebuilding:

```powershell
$env:MC2_GLTF_AXIS = "1"    # try 0-3
$env:MC2_GLTF_YOFF = "2.0"  # nudge up/down
$env:MC2_GLTF_GROUND = "0"  # 0 disables auto-grounding, 1/2 pick which end grounds
```

## Troubleshooting

| Symptom | Likely cause | Check |
|---|---|---|
| Warhammer still looks like stock | You installed under `mods/<id>/` instead of a direct drop-in (see caveat above) — the `.glb` silently failed to load and the engine fell back to the stock `.ase` | Run with `MC2_ASSIMP_TRACE=1` and look for `Assimp ReadFile failed` in the log, or `MC2_LOG_FILE_RESOLVE=1` and check whether the `.ini`'s `[mod-hit]` line even fired |
| Nothing changed and no trace output at all | `warhammer.ini`'s `[Import]` block isn't being read, or you edited the wrong install's `data/tgl/` | Confirm the file you edited is actually the one `mc2.exe` loads from (its own `data/tgl/`, CWD-relative) |
| Mesh appears but in the wrong place/orientation | Axis/grounding mismatch between this placeholder's authoring convention and your runtime env | Use `MC2_GLTF_AXIS` / `MC2_GLTF_YOFF` / `MC2_GLTF_GROUND` as shown above |
| You changed the `.glb` and don't see the update | If you also tried a `mods/` install path: stale `.modindex-cache` (see `../getting-started.md` §6's stale-cache trap) — but doesn't apply to a direct drop-in, which is always read live | `$env:MC2_REBUILD_MOD_CACHE = "1"` only matters if you're using the (non-recommended) `mods/` layout |

## Packaging (optional, and only for the `.ini` half)

You *can* still pack this as a `.mc2mod` for distribution/consistency with other examples — `pack`/`verify-lite` only care about file integrity, not whether the payload will actually overlay correctly at runtime:

```powershell
py -3 tools\mc2mod\mc2mod.py pack docs\modding\examples\mech-import\mod --out <output-dir>
py -3 tools\mc2mod\mc2mod.py verify-lite <output-dir>\mech-import-example-0.0.0.mc2mod
```

Real output from running these against this example's payload:

```
pack: mech-import-example-0.0.0.mc2mod
  id=mech-import-example  version=0.0.0  files=3  sha256=506cbe7bf17705834362e971aac33623a844459b6b45c22869ce911ad7491075
verify-lite PASS: id=mech-import-example  version=0.0.0  files=3  pkg_sha256=506cbe7bf17705834362e971aac33623a844459b6b45c22869ce911ad7491075
```

**Note on the `sha256` value:** `pack` is reproducible on this checkout — zip member timestamps and permissions are pinned (`_write_zip_member` / `_REPRO_ZIP_DATE` in `tools/mc2mod/mc2mod.py`), so packing this exact unmodified payload always yields the hash above. If yours differs, your payload bytes differ — diff `mod/` against this folder. (Older checkouts of `mc2mod.py` stamped the generated `package.json` with wall-clock time, making the hash differ on every run; that was fixed by pinning the timestamps. On any checkout, `verify-lite PASS` with `files=3` remains the integrity verdict that matters.)

If you `install` this package the normal way (`mc2mod.py install ... --deploy <your-deploy-dir>`), it lands at `<deploy>/mods/mech-import-example/data/tgl/...` — which, per the caveat above, **will not actually change the mesh in-game** (the `.ini`'s `[Import]` opt-in will overlay fine, but its `.glb` won't be found by Assimp, so it silently falls back to stock). Treat `install` here as "get the files onto disk in a known place you can then manually copy from into `data/tgl/`", not as a working end-to-end install path for this specific mechanism.

## Animations stay legacy `Gestures0..N`

The imported `.glb`'s geometry replaces **shape only**. Mech animation in this checkout is still driven by the legacy `Gestures0..N` blocks in the same `.ini` (frame-indexed into whatever skeletal frames the *original* `.ase`-era rig baked) — this example's `warhammer.ini` keeps every stock `Gestures*` block byte-for-byte unchanged. A GLB's own skeletal animation clips (if it has any) are **not** wired up; per `../mechanism-map.md` "Mechs" table, that's an explicit known gap: *"GLB animation clips are not wired to the gesture system yet (future stage-2 work)"*. This is consistent with `tools/mech_import/README.md`'s note that Stage 1 is static mesh only and Stage 2 (animation) is future work.

## For a real (non-placeholder) mech import pipeline

This example's `.glb` is a hand-generated 6-box placeholder to keep the payload self-contained and dependency-free. For converting a real ripped/authored mech asset (e.g. a BattleTech-2018 rip) into a properly assembled, textured GLB suitable for this same `[Import]` mechanism, see [`tools/mech_import/README.md`](../../../../tools/mech_import/README.md) — it documents the full FBX2glTF → `fk_bake.py` pipeline, the deploy steps (`[Import]` block, texture placement), and the same tuning env vars referenced above.

## What's in this folder

```
mech-import/
  README.md                      <- this file
  generate_placeholder_glb.py    <- regenerates the placeholder .glb (pure stdlib)
  mod/
    mod.json                     <- optional metadata (id/name/description)
    data/
      tgl/
        warhammer.ini             <- stock Warhammer .ini + [Import] block
        warhammer_glb_demo.glb    <- placeholder mesh (144 verts / 72 tris, untextured)
```

`mod/` is laid out like a normal `mods/<id>/` folder (so `mc2mod pack`/`verify-lite` work against it, and it's structurally consistent with every other example in this collection) — but per the caveat above, the *installation instructions* for this specific mechanism tell you to copy `mod/data/tgl/*` directly into your game's own `data/tgl/`, not into `mods/<id>/data/tgl/`.

## Stock data reference

`mod/data/tgl/warhammer.ini`'s non-`[Import]` content (weapon nodes, bounds, gestures) is a verbatim copy of the stock `data/tgl/warhammer.ini` found in the deploy target `A:/Games/mc2-opengl/mc2-win64-v0.4/data/tgl/warhammer.ini` (this repo checkout's own `data/` tree does not ship loose `tgl/` assets). The `[Import]` block's placement/syntax mirrors the same deploy tree's `data/tgl/atlas.ini`, which already ships `[Import] st Source = "Atlas"` pointing at a real `Atlas.glb` in production — confirming this mechanism is live, not hypothetical.
