# beauty-cook-all-maps state

Assignment: slot07, claude/beauty-cook-all-maps, requested base f269b312d;
checkout starts at 8fc0c6c89 (base plus Linux build fixes).

Audit so far: v0.4 has control_map + visual_height/mips on 24 maps;
overlay_v2 only mc2_01; shoreline_mask only 01/17/24; visual_damp only
01/10/17/24; shore_field absent on 13/21. Diagnostics/backups also leaked.
Runtime overlay_v2 gate defaults OFF (RendererFeatureRegistry.h:610), so
missing PNGs alone cannot explain default-play missing runway art.
overlay_extract.py is a flat placeholder classification oracle, not a
production road/runway bake; batch-enabling it would erase authored art.
Runtime PopulateRecipeCementWords replaces authored transition alpha with
corner-derived masks (gos_terrain_indirect_cement.cpp:588-603); investigate
coverage here. Stock runway-overlay maps: 01/02/05/08/10/13/14/19/21/23/24.

## Runtime sidecar audit (v0.4, 2026-11-08)

All mission numbers below are 01 through 24; bounds are companions, not textures.

| Sidecar | Shipped maps | Missing behavior / reader |
|---|---|---|
| control_map.png | all 24 | legacy classifier; terrain_init.cpp:335 |
| overlay_v2.png + bounds | 01 | runtime cement/road tile bake; :426; missing bounds uses full-map extent (:481) |
| shoreline_mask.png + bounds | 01,17,24 | elevation bands, no mask; :559; missing bounds uses full-map extent (:619) |
| shore_field.r16f | all except 13,21 | loader returns false; :674; water height/mask fallback |
| visual_height_4x.r32 | all 24 | coarse gameplay heights; :849; water mode 2 falls back to mask (:760) |
| visual_height_mips.r32 | all 24 | no authored mip pyramid; :893 |
| visual_damp.r32 | 01,10,17,24 | unity static damp, movers-only fade; :1075 |
| gameplay_height.sig | none | legacy height guard; :60; present mismatch rejects height/shore bakes |
| macro_color.tsf | none | native surface importer cooks resolved atlas/shore data; :1272 |
| height_delta.r32 | none | editor preview unavailable; BeautySidecarPreview.cpp:57 |
| protected.r8 | none | editor protection overlay unavailable; BeautySidecarPreview.cpp:184 |

The install contains diagnostic previews/reports/backups that are not runtime
inputs. No such byproducts should be added to deployment. The existing sidecar
checker omits macro_color.tsf from its inventory: its concatenated path is not
matched by the literal-format regex. This audit records it without regenerating
unrelated baselines.

## Implementation and remaining proof

TransitionCementWord now chooses the existing authored alpha atlas cell before
the analytic corner mask, except fully cement-covered interiors, which remain
opaque. This is the runtime fallback option in the brief: it applies to every
map, including mods with no beauty directory, without flattening road art into
the placeholder offline overlay oracle. The existing mission-load atlas cook
enumerates all alpha slots (gos_terrain_indirect_atlas.cpp:427-439).
The old mask-first precedence is present at base 8fc0c6c89:cement.cpp:599-609;
git blame traces that precedence to 2f24571474 and the narrowed alpha rescue
to 83856a88e0. A coverage doctest exercises all 16 corner configurations and
missing-art fallback through the production function (existing CMake slice).

Water coordination: slot05 owns shoreline/water for all maps; no changes here
to water producers, shaders, or deployment. Existing water cooks have batch
drivers but the payload does not automatically install their output.

Green so far: git diff --check and cxx17 ratchet. Pending: compiled doctest,
data validator, final compile/lint/box smoke, airfield identification and viewed
VK A/B. mc2_02 is only a candidate, not a confirmed screenshot match; bookmark
candidate is ignored/uncommitted until inspected. Seed creation in progress for
E:/mc2-installs/pc-lane-07. Proofs belong under
E:/mc2-archive-2026-10/proofs/beauty-cook-all-maps/.
