# REVIEW-ALLPOSES-1 — full-bookmark-set evidence rebuild

Lane `REVIEW-ALLPOSES-1`, worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`, HEAD `c3804cfe` (unchanged — this lane wrote **no**
engine source).

**Trigger.** User: *"why are you not using ALL of the bookmarks for the index
html? I have several top down views that make it much clearer."* Correct. The
0.5.2 review pack had been graded on ~8 hand-picked oblique poses while the
saved-bookmark collection held **19** poses across **5** maps, including **6
top-down views** that expose terrain, pavement and light-pool footprint defects
far more plainly than any oblique shot.

---

## 1. Build attribution (the precondition)

Prior evidence was unattributable: captured from `24f8daea-dirty`,
`f412423e-dirty`, `25aa6180-dirty`, and one install held a build whose manifest
sha was wrong.

Preconditions verified before capturing anything:

| Check | Result |
|---|---|
| Tracked source dirty at lane start | **none** (`git status --porcelain -uno` empty) |
| Build | `cmake --build build64 --target mc2` exit 0 |
| Fingerprint | `MC2_BUILD_GIT_SHA "c3804cfebb6b"`, **`MC2_BUILD_GIT_DIRTY 0`** |
| Deploy | `deploy_payload.py … --require-clean` → manifest `src_commit c3804cfebb6b`, 806 rows |
| Artifact | md5 `004ceea6f263662938303e5cfab9b00f`, identical build-dir vs install |
| Tree re-checked mid-capture | still clean, exe md5 unchanged |

**Every image in the new evidence blocks came from that one exe.** Older crops
are retained but demoted into collapsed "legacy crops" blocks and explicitly
labelled unattributable.

---

## 2. The bookmark inventory (19 poses, 5 maps)

`proj` is the camera projection angle: ~11–25° = oblique, 35–55° = tilted,
70–88° = **top-down**. Sources: the `saved_bookmarks/` directories of five
deploy installs (the Object Inspector's "Save Camera Bookmark" writes there).

| Pose | Map | Light | Proj | Alt | View | What is in frame |
|---|---|---|---|---|---|---|
| `saved_205644` | mc2_01 | day | 12.5 | 2599 | oblique | Coast oblique — waterline, beach, treeline |
| `saved_220655` | mc2_01 | day | **88.0** | 2880 | **TOP-DOWN** | Apron + runway + shoreline in one frame; the hard cement/terrain diagonal is unmissable |
| `saved_220422` | mc2_03 | **night** | **88.0** | 3649 | **TOP-DOWN** | Night compound: concrete apron, two road swaths, tents, pond; **no light pools at all** |
| `saved_205843` | mc2_05 | day | 11.4 | 5670 | oblique | Long coastal oblique, extended waterline |
| `lance_close` | mc2_10 | night | — | — | close | Lance bodies + ground contact |
| `saved_125552` | mc2_10 | night | 35.0 | 5434 | tilted | **USER-SAVED.** Open night terrain away from the airbase |
| `saved_192540` | mc2_10 | night | 37.5 | 6397 | tilted | Night open ground + horizon band (black-band pose) |
| `saved_204523` | mc2_10 | night | **85.8** | 4744 | **TOP-DOWN** | **Night coastline — blocky staircase water edge, the best shoreline diagnostic in the collection** |
| `saved_204611` | mc2_10 | night | 12.6 | 4954 | oblique | Night oblique toward the horizon |
| `saved_204753` | mc2_10 | night | 25.0 | 5434 | oblique | Airbase: beacons, apron, runway edge lights (the pose every light lane used) |
| `saved_204907` | mc2_10 | night | 12.6 | 5620 | oblique | Airbase from the side, lamp cones in profile |
| `saved_204937` | mc2_10 | night | 12.6 | 1257 | low | Low airbase pass, cone/ground intersection |
| `saved_204956` | mc2_10 | night | 12.6 | 1257 | low | Low apron pass, shifted east |
| `saved_204958` | mc2_10 | night | 12.6 | 1257 | low | Low apron pass (repeat) |
| `saved_205952` | mc2_10 | night | 10.9 | 6045 | corner | Map corner — edge fog / OOB band vs night sky |
| `saved_212019` | mc2_10 | night | 18.9 | 2113 | oblique | Road pose — ghost light wedge was found here |
| `saved_212140` | mc2_10 | night | **71.1** | 4008 | **TOP-DOWN** | Top-down over the lit area — pool footprints vs emitter positions |
| `saved_212258` | mc2_10 | night | **77.9** | 5261 | **TOP-DOWN** | **Full runway cross, lit compound, apron cement, shoreline — most informative single frame** |
| `saved_192316` | mc2_24 | day | **55.3** | 5516 | **TOP-DOWN** | **USER-SAVED.** Airbase concrete patchwork, dark blotches, road swaths, runway/apron tone split |

Note: the user's mc2_24 save is `mc2_24_saved_192316.json` (not `192916`).

### Which poses are diagnostic for which defect class

| Defect class | Best poses | Why |
|---|---|---|
| cement / runway / pavement | **mc2_24/192316**, **mc2_01/220655**, **mc2_10/212258**, mc2_03/220422 | plan view flattens the surface so tone breaks and missing swaths read as shape, not shading |
| night light pools + cones | **mc2_10/212140**, **mc2_10/212258**, mc2_10/204753, /204907, /204937 | top-down gives the pool *footprint* against its emitter; obliques give the cone *profile* |
| prop self-shadow | mc2_24/192316, mc2_01/220655, mc2_10/204937 | dense buildings with sun/no-sun contrast |
| shoreline | **mc2_10/204523**, mc2_01/220655, mc2_01/205644, mc2_05/205843 | the staircase is a plan-view artefact; obliques hide it |
| vehicles-unlit | mc2_10/204937, /204956, /204958, mc2_03/220422 | low night passes next to vehicles |
| letterbox | every pose | bars are visible in all captures at 3840×2160 |

**Top-down poses are the ones that were missing.** Six of nineteen, and they
carry the pavement, pool-footprint and shoreline verdicts.

---

## 3. Capture matrix

One run per (config, mission); a run captures every pose of that mission in a
single load (~55 s). `run_visual_capture.py … --no-kill`, fixed clock,
3840×2160, `MC2_LOG=1`.

| Config | Env | Missions |
|---|---|---|
| `BASE` | (defaults) | 01, 03, 05, 10, 24 |
| `S1_MEANDER` | `MC2_WATER_SHORE_MEANDER=1` | 01, 05, 10, 24 |
| `S2_RUNWAY_BLEND` | `MC2_RUNWAY_EDGE_BLEND=1` | 10, 24, 01 |
| `S3_CONTACT_SHAD` | `MC2_MOVER_CONTACT_SHADOW=1` | 10, 01 |
| `S4_WATERREFL_OFF` | `MC2_WATER_REFLECTION=0` | 01, 05 |
| `S5_NIGHT_FOG_DIM` | `MC2_NIGHT_FOG_DIM=1` | 10, **24 (day no-effect control)** |
| `S6_BB_FACING` | `MC2_LIGHT_BILLBOARD_FACING=1` | 10 |
| `S8_PROP_SHADOW` | `MC2_PROP_SHADOW_RECEIVE=1` | 24, 01, 10 |
| `S10_LR_A` | `MC2_LIGHT_REALISM=1` | 10 |
| `S10_LR_B` | variant B (opacity .40 / feather 2.5 / spill 2.2 / core 0 / beacon 70,40) | 10 |
| `S11_VEH_NIGHT` | `MC2_LEGACY_LIT_MC2_FRAME=1` + `MC2_GV_LIGHTSOUT_CLEAR=1` | 10 |
| `S12_CEMENT` | `TRANSITION_FALLBACK` + `ATLAS_MAGENTA_KEY` + `ALPHA_ATLAS` | 24, 10, 01 |

Standing capture rule honoured: the verdict shot is the **full frame** at
gameplay altitude (no crops); ≥3 locations per map wherever the map has them;
day (mc2_01, mc2_24) and night (mc2_03, mc2_10); a no-effect control in every
section.

**Deliberately NOT captured:** `MC2_CEMENT_TONE_MATCH` and
`MC2_RUNWAY_TONE_MATCH` — user-REJECTED. They stay default-OFF and are marked
rejected in §0 rather than presented as candidates.

---

## 4. Gate state reflected in the pack

Defaults re-read from the C++ readers today, because the 2026-07-24 inventory
doc had gone stale:

- `MC2_LIGHT_MOVER_VIS_GATE`, `MC2_LIGHT_CONE_VIS_GATE` — the doc says
  default-ON; the code says **PARKED default-OFF**
  (`mclib/cone_glow.cpp:171,185` → `return (v && v[0] == '1')`). The pack now
  shows OFF.
- `MC2_LIGHT_BILLBOARD_FACING` — default-OFF (`gos_static_prop_batcher.cpp:372`).
- `MC2_LIGHT_BILLBOARD_TWOSIDED` — still default-**ON** (`mclib/tgl.cpp:95`)
  and still contested.
- `MC2_CEMENT_ALPHA_ATLAS` — default-OFF (`gos_terrain_indirect.cpp:949`).
- Light realism **variant B is the user's chosen baseline** and is labelled so
  in §10.

---

## 5. INDEX.html rebuild

`release_assets/0.5.2/REVIEW/INDEX.html`, backup at `INDEX.html.pre-allposes.bak`.

- Section structure and the §0 gate-inventory table are kept.
- A gate-state correction block is prepended to §0.
- Each of §§1–12 gains a **FULL BOOKMARK SET** block: wide 1600px full-frame
  verdict shots (click through to 3840px), one row per pose, each row naming
  the pose, its altitude, whether it is top-down, and what is in frame.
- The older narrow crops are preserved inside a collapsed
  `<details>` "legacy crops" block, labelled unattributable.
- Every `<img src>` verified to resolve; tag balance checked.

---

## 6. Not done / limits

- No engine source was touched; no push; no commit of engine code.
- Sibling lanes (`VISIBILITY-SEMANTICS-1`, `VEHICLE-SELFLIT-4`,
  `CONE-SPILL-COUPLING-1`, `EDGE-LIGHT-EMISSION-1`,
  `CONTRACT-HARNESS-REPAIR-1`, `CEMENT-ALPHA-LAND-1`) share this worktree;
  the tree happened to be clean for the whole build+capture window, which is
  what made an attributable exe possible.
- `CEMENT-ALPHA-LAND-1` also claims `release_assets/0.5.2/REVIEW/`; a backup of
  the previous INDEX.html is in place in case that lane needs to reconcile.
