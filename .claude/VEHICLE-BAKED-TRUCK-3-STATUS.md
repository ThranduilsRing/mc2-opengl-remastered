# VEHICLE-BAKED-TRUCK-3 — STATUS

**Phase 1 verdict: the "trucks are BAKED INTO THE TERRAIN BURN-IN / COLORMAP"
hypothesis is DEAD.** Killed on direct inspection of the texture, zero build,
zero deploy. Phase 2 is therefore NOT entered as specified; what replaces the
hypothesis is recorded below.

- Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`
- Lane `VEHICLE-BAKED-TRUCK-3`, no repo-content commits, no build, no deploy
- Evidence dir: `scratch/vehbaked3/`

---

## 1. The kill — what the burn-in actually contains

Two independent copies of the mc2_10 colormap were decoded and looked at, full
frame, no cropping guesswork:

| texture | size | path |
|---|---|---|
| **stock** burn-in | 1536 x 1536 RGB | `A:/Games/mc2-srcdata/textures/mc2_10.burnin.tga` |
| deployed **4x** burn-in | 6144 x 6144 RGB | `A:/Games/mc2-opengl/mc2-win64-v0.3/data/textures/mc2_10.burnin.tga` (also `.ktx2`, `.jpg` sidecars) |

Crops:
- `scratch/vehbaked3/stock_burnin.png` — the whole stock 1536² colormap
- `scratch/vehbaked3/burnin_small.png` — the whole deployed 4x colormap
- `scratch/vehbaked3/burnin_ne_full.png` — full-res 2244x1600 crop of the NE
  quadrant containing the camera position `(3746.6, 5151.2)` under **both**
  candidate map sizes (side 100 → px ~4888,575; side 120 → px ~4582,994)

**Both textures contain ZERO man-made content anywhere in the frame.** No
runway, no tarmac, no cement pads, no buildings, no fences, no roads, no
vehicles. They are pure natural terrain colormaps (rock / grass / dirt / snow
blends and soft terrain shading). A truck cannot be baked into a texture that
contains no baked objects of any kind.

This is a stronger statement than a crop test: it is not "the trucks aren't in
this region", it is "**nothing man-made is in this texture at all**".

## 2. What the terrain program is actually sampling for those pixels

- `u_colormap` (= the burn-in above) supplies only the natural ground tone.
- The compound's grey tarmac / runway / road surface comes from the **legacy
  cement-word + 128Overlays overlay-tile path**, composited inside
  `shaders/terrain_lod_chunk.frag` (cement catalog override at ~line 871, cement
  word bit layout `bit31=VALID`, `bit30=IS_TRANSITION`).
- The modern **authored overlay sidecar (`TERRAIN-OVERLAY-V2-PARITY-1`,
  `data/missions/<stem>.beauty/overlay_v2.png`) is INERT for mc2_10** — the
  deployed `data/missions/mc2_10.beauty/` holds `control_map.png` only
  (confirmed via `mission_facts` across all six deploy lanes). So
  `u_useOverlaySidecar` is 0 and that branch is never taken on this mission.
- The 128Overlays families available are: `2lanedirt, bridge, bridgedam,
  cementpattern, damagedroad, dirtroad, droad2proad, droad2proadcross,
  pavedroad, rough, runway, scorch, x_*`. The `runway` / `pavedroad` families
  are **dark asphalt** art with white markings
  (`scratch/vehbaked3/tiles_runway.png`, `tiles_scorch.png`); the
  `cementpattern` family is the yellow taxiway dashes visible in the render
  (`scratch/vehbaked3/cement_tiles.png`) — a direct art-to-pixel match that
  confirms this path is the one painting the compound floor.

**Measured:** `runway0000.tga` mean RGB = `[53.5, 50.8, 44.4]`. The dark band
measured in the night render at the reported location = `[56.7, 52.9, 46.4]`.
That is essentially a 1:1 match to unlit-value dark asphalt art.

## 3. Replacement hypothesis (stated as unproven, same discipline as lane 2)

> The dark elongated object at screen `(458-483, 300-307)` in the
> `mc2_10_saved_204753` pose is most likely **dark-asphalt terrain overlay
> pavement (runway / paved-road family) and/or ground sitting at the edge of the
> compound light pool — not a vehicle at all.**

Supporting, all from prior lanes plus this one:
1. Per-pixel history names terrain as the sole final writer (lane 2).
2. Four object-program magenta exaggerations leave it untouched (lane 2).
3. Its colour matches the runway overlay art almost exactly (this lane).
4. **Lane 2's own static_prop probe turned "the small parked trucks in the
   bottom row" magenta** — i.e. the genuine trucks in this scene ARE static
   props and DO receive light correctly. Something that is not painted by
   static_prop is, by that same evidence, not one of those trucks.
5. At 20x (`scratch/vehbaked3/darkobj_12x.png`) the object is ~20x5 screen
   pixels with no volumetric shading, no wheels, no roof highlight — a flat
   patch. Prior lanes located it by **local-contrast search**, which is a soft
   identification and is the most likely error in the whole chain.

Honest counter-evidence: at that resolution a genuinely unlit truck would also
look like a flat dark blob, so item 5 is suggestive, not decisive. The decisive
test is item 1 of section 5.

## 4. Runs performed (all against the existing v0.3 install, no build/deploy)

Harness: `scripts/run_visual_capture.py --mission mc2_10 --bookmarks
A:/Games/mc2-opengl/mc2-win64-v0.3/saved_bookmarks/mc2_10_saved_204753.json`.

| run | out dir | result |
|---|---|---|
| baseline | `scratch/vehbaked3/base/` | reference render at the pose |
| `MC2_TERRAIN_DEBUG_MODE=2` (RawColormap) | `scratch/vehbaked3/raw_colormap/` | **only 25,985 / 480,000 px changed — the mode did NOT do a full raw-colormap replacement on this exe/path. Do not trust its output.** |
| `MC2_TERRAIN_LOD_CHUNK_DIAG=32768` (CEMENT-CLASS-DIAG) | `scratch/vehbaked3/diag32768/` | **no effect** — no classification colours appeared |
| `MC2_TERRAIN_LOD_CHUNK_DIAG=65536` (CEMENT-BYPASS-DIAG) | `scratch/vehbaked3/diag65536/` | **no effect** |

The v0.3 exe (`b01a7a96`) predates these diag bits. **They need a current build
to be usable** — which is blocked behind WORKTREE-HYGIENE-1 (the ~189-file dirty
tree still makes `deploy_payload.py` refuse on a dirty fingerprint).

Bookmark gotcha worth recording: a single-pose bookmark JSON silently captures
nothing (`bookmarks=0`, `engine_capture_fired=True`, no PNG written). Use the
2-entry `saved_bookmarks/*.json` from the install.

## 5. What to chase next, in order

1. **Re-confirm the complaint object before any more shader work.** Enumerate
   mc2_10's vehicle entities from `A:/Games/mc2-srcdata/missions/mc2_10.fit`
   (or `MC2_MOVER_COHERENCE_AUDIT`) and project them to screen at the 204753
   pose. If no vehicle entity lands within a few pixels of `(470, 303)`, the
   object is terrain/scenery and the "vehicle lighting bug" framing is wrong —
   and three lanes of vehicle-lighting work have been chasing a mislabelled
   pixel. If a vehicle IS there, that is a hard contradiction with lane 2's
   attribution and the RenderDoc work needs redoing. Either way this is the
   cheapest, highest-information next step. **Ideally: ask the user to point at
   the object they mean.**
2. **Terrain-side exaggeration probe** — the mirror of the four object-side
   probes already done, and the one nobody has run. Either use the diag bits
   already present in `shaders/terrain_lod_chunk.frag`
   (`u_diag` 32768 = cement class, 65536 = cement bypass, 262144 = raw cement
   atlas texel) **on a current build**, or REPLACE the `fragColor` write with
   magenta gated on `cementHit` / overlay coverage. Terrain shaders are NOT in
   the `shaders/spv/` package cache, so a source edit takes effect directly.
3. Only if 1 and 2 show a real unlit vehicle: resume object lighting work.
4. If 1/2 confirm overlay pavement: this is **authored-content behaviour** —
   dark asphalt next to bright cement — and the design question becomes
   "should terrain overlay pavement receive the compound's night point-lights
   the same way the base terrain does", which is a terrain-lighting slice, not
   a vehicle slice.

## 6. Methodology traps (carried forward, both still live)

1. A magenta probe **inserted before** the existing `FragColor = c;` is a no-op.
   **Replace** the write.
2. `shaders/spv/*.spv` is a package cache that silently defeats source edits for
   `mech`, `postprocess`, `cloud`, `edge_fog`, `shoreline`, `ssao`,
   `hzb_reduce`, `fog_oob`. Move it aside. (Terrain, static_prop and
   gos_tex_vertex are NOT cached.)
3. **New:** `MC2_TERRAIN_DEBUG_MODE` and `MC2_TERRAIN_LOD_CHUNK_DIAG` are
   silently inert on the v0.3 install. Always A/B a diag run against a baseline
   run and confirm a large pixel delta before believing a diag image.
4. **New:** single-pose bookmark JSONs capture nothing while still reporting
   `engine_capture_fired=True`.

## 7. Lane coordination

- Repo-content edits: **none**. Only `.claude/VEHICLE-BAKED-TRUCK-3-STATUS.md`
  and untracked `scratch/vehbaked3/`. Both were clear in the lane registry.
- No build, no deploy, no push, no amend. WORKTREE-HYGIENE-1 unaffected.
