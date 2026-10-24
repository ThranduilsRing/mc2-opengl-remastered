# Industrial generator animation handoff

Rebuild the approved package manually (art is read-only):
`py -3 tools/scifi_kit/industrial_package.py --out E:/mc2-archive-2026-10/proofs/kit-ingame-2/final/package --overwrite`.
This is an operator entry point; deployment consumes the precooked package.

The approved b7 exports (`1e3d3bca3`) now retain actual moving `Coil` geometry
in all three LODs. Both generators ship in Remastered; Classic and damaged/
destroyed appearances keep stock meshes. The art worktree was only read.
The historical stock pivots below describe the animation source, not a pivot
to impose on the new geometry: its authored glTF pivot is `(0, 6.9, 0)` metres,
before the parent's stock alignment transform and scale of four. The resulting
native pivots are `(-20.7206, 2.6720, -22.3358)` for powergenerator and
`(-21.4392, 2.6720, -22.3358)` for l_turretgenerator (the import probe records
all six LODs in `final/generator-import.json` under the lane proof directory).
The cooker preserves those origins and parents all four material splits under
the first `Coil` mesh. The stock rotation track binds by name to this parent;
the other sections inherit its transform rather than remaining stationary.

Coordinates below are exact stock ASE `TM_POS`, followed by the native TG node
centre `(-ASE.x, ASE.z, ASE.y)` in engine world units. These are absolute pivots,
not a replacement mesh's bounding-box centre.

| Asset | Node | Stock parent | ASE pivot | Native TG pivot |
|---|---|---|---|---|
| powergenerator | PowerGenerator | None | (0.0010, -0.4114, 0) | (-0.0010, 0, -0.4114) |
| powergenerator | Coil | PowerGenerator | (20.8182, -22.3358, 0) | (-20.8182, 0, -22.3358) |
| powergenerator | LOS_PwerSpike | PowerGenerator | (88.0502, -11.5444, 36.0387) | (-88.0502, 36.0387, -11.5444) |
| l_turretgenerator | PowerGenerator | None | (20.5224, -0.4114, 0) | (-20.5224, 0, -0.4114) |
| l_turretgenerator | Coil | PowerGenerator | (21.3772, -22.3358, 0) | (-21.3772, 0, -22.3358) |

The native Coil offsets relative to PowerGenerator are respectively
`(-20.8172, 0, -21.9244)` and `(-0.8548, 0, -21.9244)`.
`LOS_PwerSpike` is spelled that way in stock; its relative offset is
`(-88.0492, 36.0387, -11.1330)`.

Only **Coil** has an animation track: rotation, with stock ASE axis `(-1,0,0)`;
there is no translation track. Both clips run frames 1–61 at 30 fps, 160 ticks
per frame, and loop through the existing `Animation:0` base-shape path.
Sources: `A:/Games/mc2-srcdata/tgl/{powergenerator,l_turretgenerator}{,anim}.ase`.
The base and animation files agree on the pivots above.

Put the actual moving coil triangles under the exact short name `Coil`, with
its vertices local to the pivot. An empty helper named Coil would pass a name
check but leave the visible coil frozen. Preserve `mc2_pivot_tg_wu` on the
runtime node; the common cooker/importer preserves its rotation origin when
baking world transforms. Its runtime GLB translation is the native TG pivot
multiplied by `(-1,-1,1)` (the importer's axis-0 inverse). Keep the static base
and the Coil separate through material splitting and meshoptimizer LOD export.
The runtime binds stock tracks by node name and rejects the whole replacement
chain if an animated name is absent from any LOD.

## Post-reboot verification (2026-10-24)

Proof root: `E:/mc2-archive-2026-10/proofs/kit-ingame-2/final/`.
`coil-frame-180` and `coil-frame-197` capture the same mc2_03 generator view
at simulation frames 179 and 196, after the existing test command starts
stock gesture 0 on parts 126619 and 126621. The visible Coil sections rotate;
the static bases remain aligned. Bookmark sweeps freeze the simulation, so
two bookmarks within one sweep are not evidence of animation. The separate
`reboot-generator-damaged` capture drives both through `setObjStatus` and
shows the stock wrecks. `reboot-generator-classic` records the Classic view.
The retained native-import probe covers Coil and its children in all six
generator LODs. These game captures exercise L_TurretGenerator, not a second
in-game PowerGenerator fixture.

Night cost is not intrinsically a night-only regression: mc2_01 contains no
industrial-kit objects. On the same mc2_10 bookmarks, frame-481 Props samples
are 0.045-0.048 ms Classic / 0.173-0.175 ms Remastered at night, and
0.057 / 0.189 ms after setting daylight. Turning object pools off gives
0.031-0.042 / 0.166-0.167 ms: local pool walks explain only a small fraction.
The Props scope excludes the separately timed shadow passes. The remaining
cost tracks the replacement geometry/material workload; this does not isolate
geometry from normal/ORM shading or window emission. Reusing the pool result
removes a redundant walk, but these runs do not establish a shader-only speedup.
The old 0.126-0.127 ms result predates the animated generator replacements and
is not a matched shader baseline. All timings are provisional under shared
host load; CPU samples, full timing windows and foreign process observations
are in `reboot-cost-summary.json` and the raw measurement JSON files.
