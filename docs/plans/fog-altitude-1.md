# Fog altitude: invocation 1 / continuation record

Assignment: `claude/fog-altitude-1`, slot06, base `d77c0c803ecd1ad8d0cd66ae3c864fc2d41a38ad`.
Status: PARTIAL until invocation 2 proves the BEFORE overview repro and the final A/B.
Proof root: `E:/mc2-archive-2026-10/proofs/fog-altitude-1/`.
Assigned install: `E:/mc2-installs/fog-altitude-1` (not created in invocation 1).

## Contribution diagnosis

Opened both owner PNGs: 101157 has an almost opaque gray veil over all geometry;
101247 has clear airfield detail. This is a source diagnosis, not a reproduced runtime verdict.
The installed `mission.fst` mc2_01 camera block supplies FogStart=-1, FogFull=-2,
FogTransparency=1 and FogColor=14802644. Generic `camera.fst` defaults are 800/250;
the mission overrides them. Extracted inputs and all 13 canonical poses are archived
in `input-inventory.txt` and `inputs/`. Runtime overrides remain to be recorded.

| Contribution | At owner_101157 | At owner_101247 | Evidence / implication |
|---|---|---|---|
| Camera distance haze | 28000 alone is below 50000 | 5905.8447 alone is below 50000 | camera.cpp initialization and prefs.cpp override both use 50000/50000; no altitude-based saturation |
| Terrain/actor height haze | Zero for positive surface elevation with mission -1/-2 | Zero for positive surface elevation | FogStart/FogFull are surface elevations; camera altitude is not an input |
| EdgeFog range | Range ramp saturated at 28000, including after 1.5x | Central range approximately below the old 6000 start | edge_fog.frag independently used 6000/12000, with max opacity 0.92 |
| EdgeFog horizon multiplier | Must be checked with actual reconstructed ray | Must be checked with actual reconstructed ray | Existing signed horizon clamp remains unchanged; range saturation alone does not establish final pixel opacity |
| Overview coverage tint | Black veil <=0.30 where uncovered | Inactive outside overview | controlgui_render.cpp renderOverviewCoverageTint cannot directly whiten; unchanged |
| EnvironmentUBO | Lighting only | Lighting only | No fog fields; no unsupported W148 lighting claim |

The fix attenuates only the EdgeFog **range** term with spatial ray slope. It leaves
boundary concealment and outside-fill independent. Absolute slope makes this robust
to the reconstructed far endpoint's direction; sky exclusion and its signed horizon
profile are unchanged. A near-vertical 85-degree ray gives zero range haze. Low rays
with absolute vertical sine <=0.35 keep all range haze. No camera-altitude history,
volumetric architecture, temporal accumulation, queue, barrier or admission change.

The archived look-bisect folder had pinned identities and source diffs but no report
or classification line when inspected. Classification: **UNPROVEN / report missing**.
Verified git identities: B=8a39a0c9b27f9e662af297a9d47c14ea0ddb9300 is W138h
(pre-W147, not pre-W148); B2=d9a6dd0a3b6265073ad1f30be6add4588638039f;
C=1fdbb7c6c11420922680a43b52ee6038001be200 is W148;
D=798bedeaf2d0672bf145c143cf794c3c3f478cd4 is its archived current comparison.
No historical capture or bisect was repeated; golden identity is not asserted.

## Effective knobs (world units unless specified)

All distance derivation uses `shaders/include/fog_policy.hglsl`, compiled by C++, GLSL
and Slang. Source values are never multiplied in place. No serialized field changes.

| Knob / provenance | Old -> new effective value | Notes |
|---|---|---|
| Central distance scale / fixed policy | 1 -> 1.5 | Exactly once at the input boundary or source-distance use |
| Camera haze start / camera.cpp and prefs.cpp | 50000 -> 75000 | Custom configured start derives at x1.5 too |
| Camera fog full / source MaxClipDistance | 50000 -> 75000 | Snapshot's existing maxClip slot now means fog full only |
| Actual MaxClipDistance / init, prefs, setter | 50000 -> 50000 | All clipping/admission branches unchanged; getFarClipDistance and file values unchanged |
| Camera source DistanceFactor / compatibility field | init 1, static/setter infinity at equal endpoints -> 0 | Safe reciprocal of source span; active consumers use bounded fog policy instead |
| Effective snapshot reciprocal / derived | stale or infinite at equal endpoints -> 0 | Nondegenerate span uses 1/(1.5*(full-start)); packed layout unchanged |
| EdgeFog range / shader constants | 6000/12000 -> 9000/18000 | Smooth range ramp; scaling alone still saturates at 28000 |
| EdgeFog inward start / member default or existing env override | 50 -> 75, but effective minimum 768 -> 1152 | max(sourceStart,768)*1.5; wider band extends fog inward by 384 at default |
| Shared boundary offsets / edge_haze helper | 256/32 -> 384/48 inward from edge | Transition width 224 -> 336; wider band extends fog inward; terrain and water inherit it |
| Road/overlay exponential coefficient / shader constants | 0.00006 -> 0.00004 per world unit | Both overlay and terrain_cement_art active paths; cap remains 0.70 |
| Exponential surface-height falloff | 0.002 -> 0.002 per elevation unit | Modulates distance haze, not a separate height opacity; not multiplied by distance scale |
| Height elevation endpoints / mission file | mc2_01 -1/-2 -> -1/-2; generic defaults 800/250 -> 800/250 | Remain data-driven and serialized unchanged |
| Height opacity bound / shared policy | legacy up to 1 with 256-to-byte wrap -> <=0.05 | Packed opacity <=12/255=0.0470588; transmittance byte >=243 |
| EdgeFog view factor / shared spatial policy | 1 -> 1-smoothstep(0.35,0.85,abs(ray.z)) | Dimensionless normalized ray slope; low horizon=1, overview=0; range only |
| EdgeFog max / existing member or override | 0.92 -> 0.92 | Opacity, not distance |
| EdgeFog color | (0.80,0.81,0.83) unchanged | Existing override behavior retained |
| EdgeFog height member | 2000 unchanged / N/A | Declared/uploaded but not consumed in current fragment formula |
| Map half extent and outsideFill | Unchanged | Outside-fill still step at the real boundary; no map resizing |
| FogOob range | N/A -> N/A | No distance ramp; sky-only depth test, opacity and angular thresholds unchanged |
| Horizon fade | 0/5 degrees unchanged | Angular controls are not distances |
| Water near-gloss distances | 6000/12000 unchanged | Material response, not fog |

Equal/reversed distance endpoints explicitly step at start; non-finite inputs disable
the ramp. Degenerate/reversed/non-finite height endpoints disable height fog. Height
packing rounds opacity down, avoiding the old 256-to-zero wrap and preserving the
bound after quantization. CPU reference and GPU formula include the same function.

## Consumer coverage and merge coordination

- Terrain snapshot publishes derived haze endpoints and safe factor once, before native
  parameter packing. `gos_terrain_lighting.cpp` disabled parity diagnostics at 569 are
  not a production consumer and were left alone.
- Native terrain lighting and its CPU self-test reference use the shared height and
  distance functions. Water cull/pack forwards lighting fogRGB, water_geom.vert decodes
  alpha, and water_geom.frag mixes it; no water_geom edit.
- bdactor/tree, gvactor, genactor and mech CPU shapes use shared bounded height alpha.
  Existing distance-haze producers for gvactor/genactor/mechs/detached arms and editor
  links derive through Camera::fogHaze; their admission branches stay unchanged.
- Footprints use both shared functions; preview_draw inherits the shape fogRGB and
  supplied view.haze, without applying another distance scale.
- GPU mechs inherit corrected distance haze; the existing GPU mech body path does not
  consume shape elevation alpha (unchanged). GPU props rely on the shared screen-space
  fog rather than applying fogRGB.xyz (those bits carry team colors). No new fog pass.
- Terrain/road overlays and terrain road-art exponential coefficients are both covered;
  shared edge_haze automatically covers the terrain and water boundary consumers.
- No `water_geom.*`, `terrain_tier1.frag`, canonical bookmark or vegetation-look edits.
  Serialize vk_native_producers merges after frame-data/gpu telemetry. Fog input changes
  must invalidate any later cpu-avg lighting-output cache; no cache was introduced here.

## Proof and invocation 2

Targeted tests: `fog_policy/*` in mc2_tests (source-distance scale, repeated derivation,
equal/reversed/nonfinite/overflow/denormal inputs, alpha packing/bound, mc2_01 height
samples, 28000 overview range, low cross-map haze and boundary distances).
At 28000 the old and distance-only scaled range ramps are both 1; spatial factor at
85 degrees makes the range term 0. At range 15000 and low ray slope <=0.30, new range
haze is 0.7407407 before the unchanged max opacity. Runtime GPU self-test and pixel
agreement remain to be measured; sharing source is not a GPU execution receipt.

Receipts and final SHA are recorded in the archived report and slot state note.
Initial production shader check caught Slang's static-const requirement; shared numeric
policy constants now use preprocessor constants. Header uses .hglsl so the existing
production shader dependency globs and deploy checks include it. No ABI/layout change.

Invocation 2 (required, NOT RUN here):

1. Add the smallest capture-only route entering Tactical Overview for owner_101157;
   replaying the bookmark alone clamps to about 6400. BEFORE must reproduce the veil
   with cap_alt about 28000, or report PARTIAL without a fog verdict.
2. Read the archived bisect classification line if it becomes available; do not repeat
   history. Respect the telemetry and water-cliff merge order before final captures.
3. Commit capture route and final changes, lint final tip, ONE final RelWithDebInfo
   Windows Ninja compile via scripts/ninja_build.py -j4, matching exe and shaders.
4. Use only E:/mc2-installs/fog-altitude-1; before **each** A/B capture stage mirror
   v0.4 data/, assets/ and *.fst read-only into that stage. Archive missing/extra/size
   diffs for both arms excluding textures/_txm_cache_bak and txm_backup. No stock assets.
5. Pass A:/Games/mc2-ops/bookmarks/mc2_01_owner_2026-11-11.json explicitly. Validate
   membership and every pose value; all 13 on every mc2_01 capture. Include 101157,
   approved 101247, low 101056, cliff 101454, and corner 100525.
6. Approved offscreen harness + PC game lease, one game at a time; smoke-lite, mc2_10
   night, plus one additional verified daytime map and recorded pose. No --acceptance.
7. Record effective live camera/fog parameters, settings, payload/asset identities;
   open every montage. Judge terrain clarity, retained low horizon haze, actor/terrain/
   footprint/water agreement, approved-airfield fog-only changes and cliff control.
8. Archive all evidence here and hand back `proved at <final SHA>` with NOT RUN items
   explicit. No owner/golden/mod installs modified; local commits only, never push.
