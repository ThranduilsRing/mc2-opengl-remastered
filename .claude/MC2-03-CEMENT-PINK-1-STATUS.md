# MC2-03-CEMENT-PINK-1 — STATUS

**Lane:** MC2-03-CEMENT-PINK-1
**Worktree/branch:** `A:/Games/mc2-main` @ `claude/nifty-mendeleev`
**Commits (local only, NEVER pushed):** `7be02947`, `8d68684c`
**Verification install:** `A:/Games/mc2-opengl/mc2-win64-v0.4c` (canonical lane; abl-validate
and v0.3 were both held by live foreign mc2.exe at deploy time)
**Deployed artifact md5:** `2c50e7c52bc16df31464ec0f2ed769b4` — matches
`build64/RelWithDebInfo/mc2.exe` built at `8d68684c`
**Date:** 2026-07-25

---

## Scope

Three user-reported defects, all on the LIVE terrain LOD-chunk path:

1. **Concrete patchwork** — aprons/runways render as adjacent tiles at noticeably
   different brightness/tint with straight tile-boundary seams (mc2_03, mc2_24).
2. **Magenta/pink** — thin pink lines/patches near a road/decal edge (mc2_03).
3. **Missing road swaths** — long stretches of the road network simply not drawn
   (mc2_24): road, then a gap of bare terrain, then road.

## Repro poses

No mc2_03/mc2_24 bookmark existed in `tests/visual/bookmarks/`. Both were recovered
from install `saved_bookmarks` dirs and copied into the lane scratchpad:

| Map | Pose | Source |
|---|---|---|
| mc2_24 | `saved_192316` (the pose the user's Object Inspector screenshot names) | `releases/mc2-win64-issuefix-test/saved_bookmarks/mc2_24_saved_192316.json` |
| mc2_03 | `saved_220422` airbase apron (+ two derived lower/oblique variants) | `releases/mc2-win64-deferred2-test/saved_bookmarks/mc2_03_saved_220422.json` |
| mc2_01 | `saved_205644` airbase (regression) | `mc2-win64-abl-validate/saved_bookmarks/` |

All captures 3840x2160 (`MC2_WIDTH`/`MC2_HEIGHT`), deterministic
(`det=True` on every pose), `--no-kill`, own-PID reaping.

---

## New diagnostics (shipped, zero-cost off)

`MC2_TERRAIN_LOD_CHUNK_DIAG` bits added to `shaders/terrain_lod_chunk.frag`:

| Bit | View |
|---|---|
| `32768` | **cement class** — black = baked colormap only, red = SOLID cement-atlas replacement, green = TRANSITION cement-atlas, blue = diag-connect, yellow = runway-edge dilate, magenta = authored overlay sidecar |
| `65536` | **cement bypass** — skip every cement/overlay override so the raw baked colormap shows through |
| `131072` | **cement atlas layer-index hash** — adjacent tiles pulling different catalog cells become obvious |
| `262144` | **raw (unlit) cement atlas texel** |

These four views are what actually settled the diagnosis and are worth keeping.

---

## DEFECT 1 — concrete patchwork

### Suspect (a): this session's runway edge blend — CLEARED

`MC2_RUNWAY_EDGE_BLEND` is **default OFF** (`gate_status`: registered, default off,
2 readers) and was OFF in every arm that reproduces the patchwork. The `32768`
class view shows **zero** yellow (runway-edge-dilate) pixels on the mc2_24 pose
(21 px total, all sub-pixel). **Not the cause; no change made to that gate.**

### Actual root cause (measured)

1. `65536` (cement bypass) shows the baked colormap under the mc2_24 airbase is
   **bare dirt** — there is no concrete in the colormap at all. **All** apron,
   runway, taxiway and road paving comes from the **cement catalog atlas**.
   (This disproves the "solid is baked into the colormap" framing: on this map
   *both* classes are shader-sourced. The essential claim — two sources landing
   on different tones — is correct and is what is fixed.)
2. `262144` (raw atlas texel) vs the final beauty frame: cement pixels are
   **identical within 2/255**. Cement receives **no lighting**: the shader forces
   `normalLight = mix(normalLight, 1.0, pureConcrete)` — *"cement: fully flat-lit
   slab"* (`terrain_lod_chunk.frag`). So there is no shared lighting term to
   dominate and unify the tiles.
3. `131072` (layer hash) + per-family pixel statistics: the catalog packs
   **multiple authored concrete families with materially different base tone**.
   On mc2_24, family `0099cda0` renders `(98.8, 96.2, 87.9)` neutral grey while the
   majority families render `~(123, 121, 113)` warm tan.

Because cement is flat-lit, that raw authored tone difference is exposed at
**full contrast**, tile boundary to tile boundary. That is the patchwork.

The `32768` view also shows the visible darker ring around each apron is the
**TRANSITION** class (1 tile wide) pulling a different family than the SOLID
interior — the ring the user reads as a seam.

### Fix — CEMENT-ATLAS-TONE-MATCH-1 (`7be02947`)

CPU, once per mission load, in `BuildCementCatalogAtlas`
(`GameOS/gameos/gos_terrain_indirect.cpp`). **The baked art is left alone as the
authored ground truth in the sense that no source file is modified** — the fix
operates on the runtime-packed atlas buffer and moves the **shader-sampled side**
onto a single common tone:

- Per cell, compute the mean RGB over the **concrete band** — the `[P50,P90]`
  luminance window. This deliberately excludes dark asphalt and lane markings
  (below P50) and highlights (above P90), so runway cells are matched on their
  concrete, not their paint.
- Normalise every cell to the **component-wise median** across cells, **per
  channel**, so brightness *and* tint land together.
- Per-channel gain clamped to `[0.70, 1.40]`; cells with < 16 concrete-band
  texels or a near-black mean are skipped.
- Per-cell detail (slab lines, cracks, markings) is untouched — this is a gain,
  not a repaint.

**Killswitch `MC2_CEMENT_TONE_MATCH`, default OFF.** OFF, the atlas buffer is not
written at all ⇒ byte-identical.

### Verdict

| Check | Verdict | Evidence |
|---|---|---|
| Gate arms per arm | **PASS** | OFF: no `[CEMENT_TONE_MATCH v1]` line. ON: `event=applied mission=mc2_24 cells=110 adjusted=96 ref_rgb=149.0,146.4,138.4` |
| Between-family tone spread collapses | **PASS** | luminance stddev across the 7 largest catalog families: **9.03 → 3.95**; per-channel spread `[8.95 8.95 9.24] → [4.14 3.94 3.89]` |
| Worst offender closes the gap | **PASS** | family `0099cda0` `(98.8,96.2,87.9) → (112.0,110.3,103.4)` against a ~123 majority |
| Apron reads as one continuous surface | **PASS** | mc2_24 full-zoom apron crop: the darker transition ring around the pad is gone, pad + border + shoulder read as one concrete; runway asphalt and markings intact |
| Direction (adds light, never darkens) | **PASS** | mc2_01: 104168 px changed, **100.0% brighter**, mean dLum **+12.71** |
| mc2_01 regression | **PASS** | apron/runway/markings/props all intact; only cement pixels move; no new seams |
| mc2_03 | **PASS** | `cells=35 adjusted=30`; apron uniform on all three poses |
| Killswitch OFF = stock | **PASS** | by construction (the atlas post-pass is gated); OFF arm emits no `[CEMENT_TONE_MATCH]` line and no atlas write |

**Which side moved:** the **shader-sampled cement atlas** was corrected. No baked
colormap data, no `.txm`, and no art file was modified.

---

## DEFECT 2 — magenta/pink — NOT REPRODUCED; two mechanisms ruled out with instrumentation

**Not reproduced** in any capture: mc2_03 (9-pose map-wide scout + 3 apron poses at
4K), mc2_24, mc2_01 — **0 magenta pixels** under both a strict test
(`r>110 && b>110 && r>g+35 && b>g+35`) and a loose one (`r>g+18 && b>g+18`).

**No missing asset.** No fail-loud placeholder fired on mc2_03. The only asset
notes in the log are benign and pre-existing:
`[TERRAIN_SHORELINE v1] sidecar NOT FOUND … mc2_03.beauty/shoreline_mask.png`
(documented gate behaviour — falls back to pure elevation bands) and a
`[MECH_LOAD] WARN … mechType=0000000000000000` that also appears on mc2_24.

Mechanisms ruled out:

1. **Keyed transition tiles** — already fixed and shipped: `mclib/terrtxm.cpp`
   `textureFromMemoryAlpha` zeroes the whole pixel (RGB *and* alpha) on a chroma
   magenta test, landed `1d78f204` (2026-07-02), i.e. present in every install
   from v0.5.0 onward.
2. **The cement catalog atlas** — genuinely unguarded (solid cells reach the atlas
   via `textureFromMemory` / `gos_Texture_Solid`, which never honours the legacy
   `0xFF00FF` colourkey, and the atlas is packed **gutterless** and sampled
   `GL_LINEAR` — exactly the shape that produces a thin pink fringe). Added an
   **always-on census** so this can be judged per map without a gate:

   ```
   [CEMENT_ATLAS_MAGENTA v1] event=census mission=mc2_24 cells=110 magenta_cells=0 magenta_texels=0 keyed=0
   [CEMENT_ATLAS_MAGENTA v1] event=census mission=mc2_03 cells=35  magenta_cells=0 magenta_texels=0 keyed=0
   [CEMENT_ATLAS_MAGENTA v1] event=census mission=mc2_01 cells=65  magenta_cells=0 magenta_texels=0 keyed=0
   ```

   **Zero magenta texels on all three maps** ⇒ the cement atlas is *not* the source.

A gated zeroing pass **`MC2_CEMENT_ATLAS_MAGENTA_KEY`** (default OFF) ships anyway,
because the hole is real and a third-party/mod map could hit it — but on stock
mc2_03 it is a no-op.

**Disposition: OPEN, unreproduced.** Remaining unexcluded sources (neither
observed nor excludable from these poses) are the authored **overlay sidecar PNG**
path (already guarded by PNG-MAGENTA-KEY-1, default ON) and model/prop art via
`tgl_load_guard` fail-loud placeholders (no placeholder fired). **The next step is
the user's exact camera pose and mission time-of-day** — the report says *day* and
mc2_03 renders dusk-dark at the recovered apron bookmark, so the frame the user saw
is not one I could reconstruct. Ask for a saved bookmark on that frame.

---

## DEFECT 3 — missing road swaths (mc2_24)

### Root cause (quantified)

`PopulateRecipeCementWords` bakes a cement word for a **TRANSITION** tile
(`isCement && isAlpha`) only when **both**:

- at least one of the four corner `terrainType`s resolves to a **SOLID** cement
  catalog layer (`cementLayerIdx != 0xFFFF`), and
- `deriveMaskIdFromCorners() >= 0`.

A road tile running across open ground, away from any pad, has **no solid-cement
corner**, so it bakes **no word at all** and the fragment falls back to the bare
baked colormap. But `262144` proves the road/paving **art lives in the cement
catalog** — so those tiles render as bare dirt and the road visibly drops out.

Upstream of that: `BuildCementCatalogAtlas` explicitly skips alpha slots —
`if (tt->isAlpha(slot)) continue;  // pure-cement only` — so no transition/road
slot ever gets its own atlas layer to point at.

**Census (new, always on):**

```
[CEMENT_TRANSITION_DROP v1] event=census mission=mc2_24 transition_baked=512 dropped_no_layer=266 dropped_no_mask=0
```

**266 of 778 transition tiles — 34% — bake no cement word and render bare terrain.**
That is the "huge swaths of missing road", measured.

### Partial repair — CEMENT-TRANSITION-FALLBACK-1 (`8d68684c`), default OFF

A dropped transition tile adopts the cement layer of an orthogonally adjacent tile
that *did* bake a word (emitted as a transition word with the all-corners mask; the
shader re-derives its own coverage via CEMENT-HARD-EDGE-1, so pad/road **outline
shape is unaffected** — only missing paving is restored).

| Check | Verdict | Evidence |
|---|---|---|
| Gate arms | PASS | `event=fallback_applied mission=mc2_24 repaired=33` |
| Direction (adds paving) | PASS | 18982 px changed vs the tone-match-only arm, **96.0% brighter**, mean dLum **+82.89** |
| Covers the defect | **PARTIAL — 33 of 266 (12%)** | most dropped tiles form contiguous runs, so no orthogonal neighbour carries a word either |
| Killswitch OFF = stock | PASS | by construction; OFF changes no word |

**Honest residual.** This gate is a **partial** repair and is deliberately shipped
default OFF. The real fix is upstream: **admit alpha/transition cement slots into
the catalog atlas** (drop the `isAlpha` skip in `BuildCementCatalogAtlas`, give each
transition slot its own layer, and honour the cell's alpha in the frag so the road
art composites instead of replacing). That is a genuine slice — it changes atlas
size/packing and adds an alpha path to the cement branch — and is the recommended
follow-up. Do **not** read the 33-tile repair as "roads fixed".

### Parity counters requested (OFF vs ON)

`gos_push_overlay_calls` / `indirect_overlay_packed_quads` / `legacy_solid_setup_quads`
are counters on the **indirect** terrain path. The LIVE renderer for this defect is
the **LOD-chunk** path (`[TerrainLodChunk] shader loaded`, and the `32768` class view
resolves entirely on chunk-path fragments), so those three counters are unchanged
and non-diagnostic here. The equivalent chunk-path parity numbers, identical in both
arms, are:

```
[CEMENT_ATLAS v1] event=cement_words_baked cement_solid=921 cement_transition=512 total_vn=14400
[CEMENT_ATLAS v1] event=build_result N=110 diagTotal=110
```

with the new `[CEMENT_TRANSITION_DROP v1]` census supplying the number those
counters do not: `dropped_no_layer=266`.

---

## REVIEW PACK — before / after

Scratchpad:
`C:/Users/Joe/AppData/Local/Temp/claude/A--Games-mc2-opengl-src/f4bbf866-7ac2-41e3-b73b-ee6e568b2228/scratchpad/`

| Item | BEFORE (gate OFF) | AFTER (gate ON) |
|---|---|---|
| mc2_24 apron, full frame 4K | `A_off_24/mc2_24_saved_192316.png` | `B_on_24/mc2_24_saved_192316.png` |
| mc2_24 apron, full-zoom crop | `crop_apron_center.png` | `crop_apron_ON.png` |
| mc2_24 + road fallback | `B_on_24/…` | `C_roads_24/mc2_24_saved_192316.png` |
| mc2_01 airbase (regression) | `pad01_OFF.png` | `pad01_ON.png` |
| mc2_03 apron (3 poses) | `off2_mc2_03/` | `on_03/` |
| **Diagnosis views** (mc2_24) | cement class `diag_mc2_24/`; cement bypass `bypass_mc2_24/`; layer hash `diag131072/`; raw atlas `diag262144/` | — |

The single most legible pair for a human: **`crop_apron_center.png` (before)** vs
**`crop_apron_ON.png` (after)** — the darker transition ring around the pad, and the
tan-vs-grey slab blocks, resolve into one continuous concrete surface.

---

## Files touched

- `GameOS/gameos/gos_terrain_indirect.cpp` — 3 gates + 2 always-on census lines +
  the atlas tone-match and magenta passes + the transition fallback pass
- `shaders/terrain_lod_chunk.frag` — 4 new diagnostic views (`32768`/`65536`/`131072`/`262144`)
- `RenderCore/RendererFeatureRegistry.h` — 3 gate registrations
- `scripts/run_smoke.py` — 3 passthrough allowlist entries
- `docs/tier1_env_vars.md` — gate + diag-mode docs

## Gates (all Feature, all default OFF, all byte-identical when off)

| Gate | Effect |
|---|---|
| `MC2_CEMENT_TONE_MATCH` | atlas per-cell concrete-band tone normalisation — **cures the patchwork** |
| `MC2_CEMENT_ATLAS_MAGENTA_KEY` | zero magenta colourkey texels in solid cement atlas cells (no-op on stock maps; census always on) |
| `MC2_CEMENT_TRANSITION_FALLBACK` | partial repair of dropped transition/road tiles (33/266 on mc2_24) |

## Not done / handed off

- **Real road fix**: admit alpha cement slots into the catalog atlas + honour cell
  alpha in the cement branch. Recommended next slice.
- **Magenta**: needs the user's exact pose/mission-time to reproduce; two candidate
  mechanisms already excluded by instrumentation.
- **Default-ON decision** for `MC2_CEMENT_TONE_MATCH` is a taste/user call —
  the evidence supports it, but it changes every cement pixel on every map.
- The known non-deterministic mc2_03 0-frame load crash **did not reproduce**
  across ~8 mc2_03 launches this lane.
