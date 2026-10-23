# Playable mission writer (2026-10-22)

`tools/mapgen/mission_writer.py` emits ordinary stock-load-path missions from
phase-1b/1c layout JSON. No engine changes, direct TGA terrain initialization,
stock mission edits, build, or deployment to a stock install are involved.

Two demos **load in game**, spawn their units with named pilots, and run enemy
brain callbacks. Both passed 30-second advisory `--allow-any-mission` smoke runs.
This is a playable flat-map prototype, not a campaign-ready generator.

| Mission | Cells / vertices | Objects | Units | Smoke |
|---|---|---:|---:|---|
| `mapgen_180_7` | 180 / 60 | 430 | 7 | PASS, 4,241 frames |
| `mapgen_720_23` | 720 / 240 | 6,053 | 57 | PASS, 4,025 frames |

The small mission was tested first. An additional 360/17 mission passed offline
emission, but has no runtime claim. Full map/layout/PAK/FIT/ABL artifacts live in
`G:/mapgen/missions/`; the isolated game copy is
`G:/mc2-installs/mc2-win64-mapgen/`. It is a real 13.041-GB copy of
`A:/Games/mc2-opengl/mc2-win64-v0.4`, not links or the golden v0.4c.
The copy log is `G:/mapgen-install-copy.log`.

## Reproduce

From the repository root:

```powershell
py -3 -m tools.mapgen.playable_demo --out G:/mapgen
# Or supply any supported flat, dry stock layout:
py -3 -m tools.mapgen.mission_writer layout.json --out G:/mapgen/missions --name my_mission
py -3 -B -m unittest discover -s tests/mapgen -v
```

The demo command writes flat 1,000-wu layouts, mission packets, a minimal ABL
scenario, pixel plans, checksums/validation reports, and generated inspection
bookmarks. Sizes are deliberately limited to square 180..720-cell grids divisible
by 30. Unsupported terrain and non-stock asset sets fail explicitly.

Copy only the generated `.pak`, `.fit`, and `.abl` files into the isolated
install's `data/missions` directory. Never copy over stock stems. Launch via the
harness in the background with `Start-Process -WindowStyle Hidden`; arguments:

```text
py -3 scripts/run_smoke.py --mission mapgen_180_7 --mission mapgen_720_23 --allow-any-mission --exe G:/mc2-installs/mc2-win64-mapgen/mc2.exe --ref-exe A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe --duration 30 --keep-logs
```

Evidence run environment: `MC2_DEBUG_STATE_DUMP=1`,
`MC2_BRAIN_FREEZE_TRACE=1`, `MC2_BRIDGE_MOVER_STATE=1`,
`MC2_MOD_HEALTH_DIAG=1`. The executable reference verifies byte identity with
the deliberately reused v0.4 binary; it does not claim this Python-only branch
was compiled. Source exe SHA-256:
`8088ac770b00b4a066478611410c27e03a2b9c21a500d6d6b4843c0d1d363d62`.

## Packets and validation

Packet 0 is a complete flat `PostcompVertex` array with actual stock mc2_01 grass
texture data, rebuilt extents and upward normals. Packet 1 uses the exact
`<i4f5i` object record plus count. World X/Y pivots snap to 1/256 wu and engine
float32; cell packing and both hops of parent chains are revalidated afterward.
The full existing layout validator checks native footprint overlaps, permitted
wall joins, clearances, and parent distances after snapping.

Packet 2 is a valid empty static tacmap-point list; Packet 3 is a generated
128-square TGA tacmap. MOVE begins at Packet 4, with local cells, three narrow
stock global graphs (25-byte areas, 57-byte doors, 3-byte door infos, 11-byte
links), and a zero special-area tail. Sector-local flood fills, contiguous edge
doors and actual within-area shortest-path costs are recomputed. Ground and
hover use the same dry-map graph; flying uses the unobstructed grid.

Objects stamp conservative rotated model rectangles into MOVE; unit placement
requires a clear 3x3 neighborhood. All unit spawns are connected to the player.
FIT includes the three-mech player lance, two stock UrbanMech guards per enemy
site, valid `pmwhacksaw` / `PMWRegular` profiles, `PBrain` / `SBGuard`, commander
groups, team objective blocks and a marked `DestroySpecificStructure` objective.
The minimal scenario ABL returns zero while native objective/defeat evaluation
handles mission termination.

Every emitted packet is reread using the repository PacketFile decoder. MOVE
headers, dimensions, area indices, reciprocal references, link capacities and
cursor termination are checked. The normal corpus extractor rereads the PAK/FIT
with stock dependencies and rejects unresolved parents or type IDs. All **48**
mapgen tests passed, including deliberate dimension, link-pool, parent, duplicate
cell and non-flat-input corruptions.

## Runtime and viewed captures

Final smoke artifacts:
`tests/smoke/artifacts/2026-10-22T07-28-49-736504-5604/`.
Both runs passed deploy identity, mission heartbeat and render-state schema
checks. Mover telemetry records 7 and 57 actors and named pilots. Enemy callbacks
report `hasBrain=1`; the large mission reaches
`[MOD_HEALTH v1] event=brain_runs count=100 scenarioTime=5.60`.
This proves executing AI, not a combat or victory playthrough.

Viewed comparison sheets:

- [180-cell plan and game](mapgen_180_7_comparison.png)
- [720-cell plan and game](mapgen_720_23_comparison.png)

Original 1920x1080 captures, sidecars, capture logs and manifests:
`G:/mapgen/captures/180_wide/` and `G:/mapgen/captures/720_wide/`.
Earlier closer captures, including the viewed player lance, are in
`G:/mapgen/captures/180/` and `G:/mapgen/captures/720/`.
Sidecars identify `backend=vulkan`, `readback_source=vk_present` and the actual
camera poses. Every capture call used `--no-kill`, ran offscreen, and declared
`MC2_SCREENSHOT_SOURCE=backbuffer` and `MC2_FORCE_ALL_VISIBLE=1`. Visibility
override is for inspection, not required for missions to load or AI to run.

Capture arguments (substitute mission, bookmark and output directory for 720):

```text
py -3 scripts/run_visual_capture.py --mission mapgen_180_7 --bookmarks G:/mapgen/bookmarks/mapgen_180_7.json --exe G:/mc2-installs/mc2-win64-mapgen/mc2.exe --out-dir G:/mapgen/captures/180_wide --duration 30 --no-kill --env-declare MC2_SCREENSHOT_SOURCE=backbuffer --env-declare MC2_FORCE_ALL_VISIBLE=1
py -3 -m tools.mapgen.comparison G:/mapgen/missions/mapgen_180_7.layout.json --captures G:/mapgen/captures/180_wide --out docs/mapgen/phase2/mapgen_180_7_comparison.png
```

## Still missing for campaign use

- Bake road overlays, cement and terrain transitions. The viewed sheets make
  the difference explicit: the current playable surface is uniform stock grass.
- Replace conservative visual rectangles with the native collision-template
  baker, including LOS heights, tree/forest behavior, gate ownership and
  destructible special areas. Current blocked cells remain blocked after their
  structures are destroyed; gate apertures start passable. This is a material
  gameplay limitation, not full stock MOVE parity.
- Author multiple objectives, AI patrols/attacks and difficulty, varied units
  and pilots, failure rules, reinforcement scripts and briefings. Guards execute,
  but successful movement/combat and objective completion need a playthrough.
- Campaign/drop-zone integration, progression/rewards/salvage and save/load
  verification. Only the quick-start harness path has been tested.
- Non-flat heightfields/water, mod catalogs, arbitrary dimensions and larger
  editor maps need separate terrain/collision and capacity validation.

Local commits only. No hook or launch guard was bypassed. The repo-query
ambiguity-only result was handled under the explicit owner ruling.
