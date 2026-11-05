<!-- doc-status: current | reviewed:2026-10-11 -->
# mech_import — BattleTech-2018 mech → MC2 importer pipeline

Converts ripped BattleTech 2018 (HBS) mech assets into MC2-ready GLB + textures.
Stage 1 = static mesh (proven: Marauder imported as `madcat`). Stage 2 = animation
(the converted GLB carries all skeletal anim clips; in-engine FK is future work).

## Why the indirect path

The dump's `MadCat/marauder_combined.glb` and per-part `Mesh/*.obj` are **not
assembled** — mech part vertices live in bone-local space, piled at the origin;
only the skeleton positions them. Naive geometry import → garbled pile / torso on
the ground. The authoritative assembly is the FBX **bind pose**.

Blender 5.1's FBX importer crashes on these ripped FBX (`KeyError: ...Model`), so
we convert with **FBX2glTF**, then bake the rest pose to static geometry with
`fk_bake.py` (pure numpy, no Blender).

## Pipeline

```
marauder.fbx
  │  FBX2glTF -i <fbx> -o _fbx/marauder -b      (skinned GLB: geom+skeleton+anims)
  ▼
_fbx/marauder.glb
  │  py -3 fk_bake.py --src _fbx/marauder.glb --out_glb _out/MadCat.glb \
  │      --albedo <...-Base-alb.png> [--normal <...-nrm.png>] [--emissive <...-ems.png>] \
  │      [--embed_max 2048] [--target_height 25.44] [--drop blip,uix,_dmg,_explode]
  ▼
_out/MadCat.glb   (assembled static mesh, UVs, embedded PBR maps, MC2-scaled)
```

`fk_bake.py` reads POSITION/JOINTS_0/WEIGHTS_0 + skin inverseBind + node hierarchy,
computes each joint's rest global transform via forward kinematics, linear-blend
skins every vertex once, drops `_dmg`/`_explode`/blip/UI meshes, scales to MC2 size,
and writes a single-material textured GLB. The embedded baseColor image is **named
after the source albedo** so MC2's importer derives the matching on-disk `.tga`.

## Helpers

- Blender discovery and headless runs use `tools/blender_pipeline/blender_runner.py`
  (`BLENDER_EXECUTABLE` env or standard paths).
- `inspect_fbx.py` — dump objects/materials/UVs of an FBX/GLB (recon).
- `render_glb.py` — headless WORKBENCH textured render to PNG for QA.

## Gotchas (learned the hard way)

- **Do NOT V-flip UVs** in the GLB writer — source glTF and output glTF share the
  same convention; an added `v = 1 - v` scrambles the texture.
- glTF **JSON chunk pads with spaces** (0x20), BIN chunk with nulls.
- 2048² TGAs need the bumped `MAX_LZ_BUFFER_SIZE` in `mclib/txmmgr.h` (engine).
- Base chassis uses only the Base atlas (no Weapons atlas); the dump's separate
  weapon-mount meshes are a later concern.

## Deploy (MC2 mech `[Import]` path)

1. `_out/MadCat.glb` → `<game>/data/tgl/MadCat.glb`
2. albedo `.tga` (name = importer-derived) → `<game>/data/tgl/` + `data/tgl/<ObjectTextureSize>/`
3. add `[Import]` `st Source = "Madcat"` to `data/tgl/madcat.ini` — this block **is**
   the opt-in (`[Import] Source=` read in `mclib/mech3d_type.cpp`); only LOD0 of a mech with it uses the importer.
   There is no runtime env-var gate — the build just needs `ENABLE_ASSIMP_IMPORTER`
   (CMake option, default ON). Any extension in `Source` is stripped; the engine
   probes `.glb` then `.fbx`, falling back to `.ase` (`TG_TypeMultiShape::LoadFromFile` in `mclib/msl_load.cpp`).
4. run the game; verify in `mc2_24` (no intro cinematic). Debug/tuning env vars:
   `MC2_GLTF_AXIS` / `MC2_GLTF_YOFF` /
   `MC2_GLTF_GROUND` (orientation / lift / grounding).
