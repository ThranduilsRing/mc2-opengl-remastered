# Industrial generator animation handoff

The approved `f57d04c6a` exports flatten the moving geometry into one mesh.
Both generators therefore deliberately keep their stock visuals and motion.
The art worktree was only read. Add the following nodes to **every LOD** before
replacing the stock render chain. Damaged/destroyed states remain stock.

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
