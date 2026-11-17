# Native MW5 reader (parity PASS on the Mercs Timber Wolf, 2026-11-17)

This lane uses .NET 8 and exact NuGet pins CUE4Parse **1.2.2** and
CUE4Parse-Conversion **1.2.1**. Both are Apache-2.0:
[package](https://www.nuget.org/packages/CUE4Parse/1.2.2),
[source and licence](https://github.com/FabianFG/CUE4Parse/tree/v1.2.2).
The conversion 1.2.1 package requires reader 1.2.2; both packages identify
source commit `920dc1682278d70ee738383b6fbd3a1e9b7523ea` in their metadata.
No FModel assembly, Oodle binary or game asset is vendored. Transitive package
licences remain those of their respective authors. Newer CUE packages target
newer frameworks; updating the pins requires another API and synthetic check.

The player install is read only. Oodle is loaded at runtime only from the
install root, Engine/Binaries/ThirdParty/Oodle/Win64, or the game's
MW5Mercs/Binaries/Win64 or MW5Clans/Binaries/Win64 directory. The Mercs
install has no Oodle DLL (Oodle is inside the exe) and its pak reads without
one; a missing DLL only prints a note, and Oodle-compressed assets then stay on
the bridge. No download fallback exists. Real extraction requires Windows. The box must receive source and synthetic fixtures only.

`request.json` selects exact object paths (no guessing or whole-install exports):

```json
{
  "mesh": "MW5Mercs/Content/REPLACE/SK_MadCat.SK_MadCat",
  "clips": ["MW5Mercs/Content/REPLACE/Idle.Idle"],
  "materials": [],
  "loadouts": [],
  "textures": []
}
```

Replace the example paths with names from the player's install. List MTI/SKN
instances and loadout data assets explicitly, including non-mesh skin overrides.
Mesh materials, material parents and their referenced textures are followed.
`loadout.json` retains decoded UE property names and values, rather than
inventing hardpoint semantics. Material records retain parent references,
local scalar/vector/texture overrides, static switches and raw properties;
PNG names are stable SHA256 hashes of object paths, mapped in `textures.json`.
The glTF retains material/loadout metadata in extras; shader reconstruction is
not part of this lane.

```bash
# PC only; use your player install and scratch variables, never a repo output.
# MW5_SCRATCH must be G:/mc2-tmp/mw5-extractor-cue4parse-1 (or own lane install).
dotnet build tools/mw5_kit/extract/Mw5Extract.csproj -c RelWithDebInfo -p:RestoreLockedMode=true
dotnet tools/mw5_kit/extract/bin/RelWithDebInfo/net8.0/Mw5Extract.dll \
  --install "$MW5_INSTALL" --paks "$MW5_INSTALL/MW5Mercs/Content/Paks" \
  --ue 4.26 --request "$MW5_SCRATCH/request.json" --out "$MW5_SCRATCH/madcat-native"
py -3 -m tools.mw5_mechs.from_native "$MW5_SCRATCH/madcat-native" "$MW5_SCRATCH/madcat-native.gltf"
```

Use the actual engine version: 4.26 is a starting default, not a proven MW5
version claim. Clans selects `--ue 5.5 --usmap "$MW5_USMAP"`, with matching
player mappings. Optional `--aes HEX` mounts encrypted archives using the
player's key. UE5.5/usmap support is provisional and **NOT RUN** on real data.

Per-mech output: `skeleton.json`, `mesh.bin` (LOD0), `mesh.lodN.bin`,
`lods.json` (sections/material slots), `clips/*.bin` with name sidecars,
`sockets.json`, `materials.json`, `loadout.json`, `textures/*.png`, and
`bridge.json`. A nonempty bridge list returns exit 2; fatal setup returns 1.
Unsupported kinds/codecs, additive clips, mismatched skeletons, base-material
graphs, missing LOD0, zero ancestor scales and decoding failures stay on the
umodel bridge and are listed with reasons. The bridge stays for those
(base-material graphs, additive clips). Skeleton assets may hold more bones
than the mesh (Mech_SKL: 261 vs 121); clips bind to mesh bones by name. Do not treat partial output as complete extraction.

Native vectors are UE LH Z-up centimeters. Bone and socket transforms are
local, quaternion XYZW. CUE's ancestor-scale translation adjustment is undone
for rest and animation alike (including its animation adjustment threshold).
No C# axis flip, quaternion conjugation or centimeter conversion is applied.
Python alone changes `(x,y,z)` to `(x,z,y)*.01` for positions, swaps scale axes,
maps quaternions to `(-x,-z,-y,w)`, and reverses winding. Normals are unit vectors.
Additional LOD meshes are retained but only LOD0 is active in the default scene.

Binary format v1 is little endian. Mesh: `MW5M`, u32 version, vertex/index
counts; each vertex has 8 f32 (position/normal/uv), u32 influence count,
then (u32 skeleton bone index, f32 weight) pairs; final u32 triangle indices.
Clip: `MW5A`, u32 version, u32 frames, f32 fps, u32 track count; each track
has u32 bone then translation, rotation, scale channels. Each channel has
u32 key count then (f32 seconds, 3/4/3 f32 value) records. Empty channels use
rest values. Native frame positions become seconds once; keys are not resampled.

## Box synthetic validation

Run via `slot_sync.sh 0 <commit> <worktree>` and `slot.sh 0 bash ...`.
The engine build/unit commands are unnecessary for this standalone tool.
Use `/workspace/toolchains/dotnet8/dotnet` if dotnet is not on PATH:

```bash
dotnet build tools/mw5_kit/extract/Mw5Extract.csproj -c RelWithDebInfo -p:RestoreLockedMode=true
dotnet tools/mw5_kit/extract/bin/RelWithDebInfo/net8.0/Mw5Extract.dll --fixture "$TMPDIR/mw5-synthetic"
MW5_SYNTHETIC_FIXTURE="$TMPDIR/mw5-synthetic" python3 -B -m unittest tools.mw5_mechs.test_from_native -v
```

## PC parity step: PASS (Timber Wolf, Mercs, 2026-11-17)

`MW5Mercs-WindowsNoEditor.pak`, mesh `Timberwolf_SKM` plus `TBR_Stand`,
`TBR_WalkForward_Straight`, `TBR_RunForward_Straight`: 121 bones, 17488 LOD0
vertices, 35705 compared clip keys (translation, rotation; no clip has authored
scale). Max bone rest error 3.8e-5, vertex error 9.5e-7, rotation key error 0.0198 degrees. Receipt (numbers only):
`E:/mc2-archive-2026-10/proofs/mw5-native-parity-1/`.
`build_glb.py --skm <native.gltf>` takes this output directly (`--anim-dir`
is only for the umodel bridge) and yields the same 33 parts / 93 nodes.

Reference: umodel `-export -gltf` of the SKM (RH Y-up metres) plus its `.psa`
clips converted with `ue_basis.py` into glTF animations named like the native
clips. umodel writes zero/identity for bones a clip has no track for; those
channels are not data and are left out of the reference.
Supply weapon AABBs in glTF world meters for **every** socket in
`$MW5_SCRATCH/madcat-weapon-bounds.json`, keyed by socket name, each value
`{"min":[x,y,z],"max":[x,y,z]}` (the run above used the mesh extent plus 0.5 m,
a sanity bound, not measured hardpoints). Then:

```bash
py -3 -m tools.mw5_mechs.parity \
  --candidate "$MW5_SCRATCH/madcat-native.gltf" \
  --reference "$MW5_SCRATCH/madcat-bridge.gltf" \
  --weapon-bounds "$MW5_SCRATCH/madcat-weapon-bounds.json"
```

It requires equal bone sets, bone world rest error <1e-4, every candidate
channel present in the reference, equal clip-name sets, **every** rotation key <0.1 degree (sign
independent) and vector keys <1e-4 with the candidate sampled at each
reference key time (constant tracks are one key; untracked channels are rest),
LOD0 vertices (default-scene meshes) matched one-to-one within 1e-4 (needs scipy), and
every socket inside its bounds. Geometry/animation/socket bridge fallbacks
prevent a PASS. Counts/errors only. Not covered: normals, UVs, skin weights,
materials (`TimberWolf_Default_SKN` is not a material instance and stays on the
bridge), Clans/UE5.5/usmap (**NOT RUN**). A socket named like a bone becomes
node `<name>_socket`; a socket on a bone the mesh lacks (2 here) is a
`socket_skipped` fallback in the glTF extras, counted by parity. Scale tracks are
kept only where the compressed data has scale keys (authored zero scale stays);
non-UE animation codecs stay on the bridge.
