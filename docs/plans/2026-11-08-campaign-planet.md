# Campaign planet lane state

Assignment: mc2-slot08, claude/campaign-planet; base 8fc0c6c895.
Local commits only. Proof directory: E:/mc2-archive-2026-10/proofs/campaign-planet.

## Completed inspection

- MissionSelectionScreen::begin loads the campaign Group's OperationFile FIT.
  Its Static0 is a 256x256 globe composition, with the mission buttons over it.
- Shared marker atlas: mcl_cm_5. Source TGA black corner has alpha 209;
  installed PNG has alpha 255 everywhere. ImGui draws it with standard alpha
  blending; DrawUiImage's alphaTest argument currently does nothing.
- Marker cook keys black to alpha zero in both formats, retaining all other
  RGBA values and both button states. Source is art/source/campaign-planet.
- All campaign and generic/tutorial mission-select pages share this atlas.
  Briefing controls use mcl_mn_3; its numbered/lettered map glyph atlas mcl_mn_4
  also has opaque black padding, and receives the same alpha cook. mcl_mn_1/2/3
  contain panel backgrounds that must stay opaque. Tactical blips use blip.tga.
- gos_screenshot::deliver writes 24-bit RGB from the composed Vulkan present
  readback. engine_present_vk.cpp delivers the same pixel buffer to visual PNG
  capture and in-game TGA screenshots. It does not blend an RGBA UI texture
  against black at screenshot time.
- Stock source montage and extracted TGAs are in the proof directory.

## Stock campaign image mapping

| OperationFile / image stem | Missions |
|---|---|
| mcl_cm_op1_1 | mc2_01 |
| mcl_cm_op1_2 | mc2_02 |
| mcl_cm_op1_3 | mc2_03 |
| mcl_cm_op1_4 | mc2_04 |
| mcl_cm_op1_5 | mc2_05 |
| mcl_cm_op1_6 | mc2_06, 07, 08, 09 |
| mcl_cm_op2_1 | mc2_10 |
| mcl_cm_op2_2 | mc2_11, 12, 13; moon visible |
| mcl_cm_op2_3 | mc2_14, 15, 16, 17 |
| mcl_cm_op3_1 | mc2_18 |
| mcl_cm_op3_2 | mc2_19, 20, 21, 22 |
| mcl_cm_op3_3 | mc2_23 |
| mcl_cm_op3_4 | mc2_24 |

These are 13 entries, not 13 unique globes: op1_5, op3_3 and op3_4
are identical. mcl_cm_optutorials is a wireframe sphere. The campaign FIT
was extracted from mission.fst and is retained with proofs. The mission
briefing's mission map is separate from this campaign globe composition.

## In progress / exact next steps

1. Finish registering the eleven unique stock views to one sphere. Initial
   SIFT matches projected onto an orthographic sphere give strong overlapping
   landmark correspondences; reject spurious matches and solve loop closure
   before reconstructing continents. Assumed stock disc: (128,116), radius 104.
2. Reconstruct an equirectangular design map and separate cloud layer; regenerate
   surface detail, render all poses with their stock light, framing and moon.
   Preserve grid/name bar independently. Do not ship a plain upscale.
3. Keep source maps and a rerunnable cook; add only stock overrides, retaining
   custom mod art precedence and original fallback. Verify logical UV dimensions
   before shipping higher-resolution assets (FIT crops remain authored in 256px).
4. Commit final implementation, then box build/unit and one final PC Ninja build
   (mc2 + viewer), covering rows, and lint. Capture PC campaign selection before
   and after and paired PNG/TGA of the same presented frame. Record receipts.

## Hand-back scope and receipts

The marker fix, production-decode doctest, PNG/TGA pixel check, deployment
entries, mod atlas precedence, and campaign-select capture entry point are the
implementation checkpoint for this run. Full globe reconstruction remains a
separate substantial continuation: camera registration must be validated first;
several low-inlier matches imply conflicting orientations. Rendering those now
would move continents beneath mission markers. No planet replacement is shipped.

Initial offline pixel checks and deploy papercut tests: 6 passed. Final-tip
build/test/capture verdicts and exact SHA will be retained in the proof directory
(results.md), so proof bookkeeping does not change the tip being proved.

Research retained with proofs: campaign_planet_inventory.py,
register_campaign_planet.py, registration-initial.json and registration.log,
reconstruct_campaign_planet.py, surface-projection-DRAFT.png,
cloud-projection-DRAFT.png and surface-coverage-DRAFT.png. The first projection
covers 93.4% of the sphere, excluding ambiguous op2_3, but contains baked
lighting, clouds and seams. It is a registration diagnostic, NOT source art
ready for regeneration. Next: constrain all rotations to a consistent spin
axis, refine projection model / disc centre and radius, mask the screen grid,
and solve pose loop closure. Do not reuse bad match edges merely to connect
the graph. Separate illumination/clouds only after registration is sound.
