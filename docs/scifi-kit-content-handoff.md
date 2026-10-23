# MC2 stock draft round: content handoff

Authoring: `tools/scifi_kit/art/`. Outputs: `E:/mc2-ref/scifi-kit/`.
Read against the parked `claude/content-pipeline` contract at `1132fb696`.
Original kit geometry and shaders; the existing CC0 Poly Haven concrete detail is
the only external raster detail sampled. Reference-game meshes, textures, UVs,
logos and traced images are never used as production data.

## Assets, states and materials

Eight stock selections: the six reviewed drafts, three stock-count barrels, and
the rusty quonset palette. Clean/rusty quonsets use identical geometry and UVs.
Each has pristine, worn, abandoned, partial and destroyed authoring exports, with
three LODs per export. The four building selections use full collapsed rubble for
destroyed; partial retains local damage. Fence destruction topples the complete
section. Wall destruction follows the stock shortened standing remnant.

The high source has denser physical bevels and authored wear/material shaders.
The game mesh is independently generated from the same kit parameters, retaining
the source fit transform. Selected-to-active Cycles baking creates a 2048 atlas:

* basecolor: sRGB, pure unlit base colour; no AO multiplication;
* normal: tangent-space OpenGL +Y; geometry is triangulated before baking;
* ORM: linear R=AO, G=roughness, B=metallic, A=coverage (opaque assets use 1).

Separate material slots share the atlas but carry canonical
`extras.mc2_material_class`: concrete, painted_metal, bare_metal, grating, glass,
wood, fabric, rubber, dielectric or emissive as applicable. Only bare_metal and
grating use metallic. Glass is dark opaque glazing under the current runtime
profile. Stock bounds and pivot measurements supply export placement; source
GLBs are standard Y-up metres (`frame=gltf`, `wu_per_unit=4`).

`content.py` reads the content-pipeline branch without editing it, imports the
complete state/condition/LOD chains into `mc2-content-ir/1`, validates source GLBs,
deduplicates repeated atlas/class slots, and writes a cooker recipe. It assigns
the original ORM map explicitly because Blender may repack a dielectric MR
texture separately from AO. Partial is an authoring/intermediate delivery:
the current IR/runtime only has intact/destroyed states, so partial is referenced
in frontend metadata, not mislabelled as a condition.

## Building light sockets: `mc2-building-light/1`

Source glTF contains named leaf nodes `LIGHT_door_01`, `LIGHT_window_left_01`,
etc. These are attachment points, not baked light cones or glTF punctual lamps.
Door points snap to the authored lamp lens; window points snap to glazing.
The source node translation is its position in standard glTF metres. Extras:

| Property | Meaning |
|---|---|
| `mc2_light_schema` | `mc2-building-light/1` |
| `mc2_light_type` | `point` or `spot` |
| `mc2_light_position_gltf_m` | Explicit source-frame position, including stock pivot |
| `mc2_light_direction_gltf` | Emission direction in the same source frame |
| `mc2_light_colour_linear` | Suggested linear RGB |
| `mc2_light_intensity_class` | `door` or `window`; R3 owns exposure/lumen calibration |
| `mc2_light_range_m` | Suggested maximum radius in author metres |
| `mc2_light_outer_cone_degrees` | Suggested spot cone full angle |
| `mc2_light_active` | True for the supplied intact pristine/worn socket |
| `mc2_light_lights_out_on_destroy` | Always true |

Convert source position/direction through the declared source-to-TG transform;
position and range scale by 4 WU/m. Mesh leaf nodes carry a local Z reflection (`mc2_stock_axis_alignment=TG-depth/1`) so author depth maps to TG +Z. The IR applies that negative-determinant transform with corrected winding and tangent handedness. Normalise the direction after transforming.
Do not interpret author metres as WU. The sidecar also retains author-frame
positions for review and explicit `position_tg_wu`, `direction_tg` and `range_wu`
for R3. Apply the building instance's model-to-world transform to these TG values.
No active sockets exist in partial, abandoned or destroyed
exports; rubble has no emitters. No beacon/floodlight is invented where the stock
building has no corresponding fixture.

The parked IR has only `Socket(kind="attach")`, with no light property payload.
It retains the named socket frames, while `frontend.light_sockets` and
`content/light-sockets.json` retain the full schema for R3. Its current runtime
GLB writer emits only mesh leaves and drops empty-node extras. Therefore source
GLBs and the sidecar are the authoritative light delivery; the cooker/R3 lane
must preserve/consume that metadata when integrating building spill. This round
does not claim real engine emitters are already implemented or verified.

Use `content.py` (optionally `--cook`) for the complete IR handoff. Its recipe is
a standard frontend entry point; the adapter additionally attaches light metadata
and complete destroyed LOD chains, which the parked recipe frontend cannot yet
express by itself. Worn/abandoned reuse the canonical destroyed chain.

## Counts and review

`mapping/counts/<stock-id>/counts.json` records source model/texture evidence,
primary feature counts and placements. Pristine primary counts must match stock;
damage may remove or collapse features. Fasteners, bevel strips, individual
glazing sub-panes and fine corrugation waves are secondary detail, not additional
building bays or crates. Render review includes all four sides and far LODs.
Unknown inventory counts are explicitly unknown, never inferred as zero.

No engine builds, deployments or game launches are part of this art round.
Pipeline validation proves the interchange contract, not final runtime lighting,
orientation, LOD transition quality or night exposure.
The parked runtime's destroyed override currently hooks Building objects only;
the delivered toppled fence still needs that consumer extended by its owning lane.
