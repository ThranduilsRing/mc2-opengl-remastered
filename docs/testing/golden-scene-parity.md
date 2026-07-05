# Golden-Scene Parity Harness (GOLDEN-SCENE-PARITY-1)

The proof loop for validating a **render island** — a subsystem swapped behind a
gate (e.g. a Vulkan island replacing a GL pass) — against the GL baseline. It
answers, mechanically and without eyeballing: *does turning the island ON change
the picture or the render structure beyond the run-to-run noise floor?*

Driver: `scripts/run_golden_parity.py` (stdlib, Windows / py-3). It composes four
already-shipped pieces:

| Piece | Role |
|---|---|
| `scripts/run_visual_capture.py` | **PIXEL ORACLE** — bookmark-driven, glReadPixels FBO readback, byte-stable per-bookmark `sha256` under `MC2_SMOKE_FIXED_TIMESTEP=1`. |
| `scripts/golden_scene.py` | **STRUCTURAL manifest** — `registry_hash`, `pass_counters`, `render_health`, `exe_md5`, `gate_set`. |
| `scripts/golden_compare.py` | noise-floor builder + per-field WITHIN/BEYOND compare gate. |
| `scripts/golden_diff_report.py` | pixel-diff image + counter-delta culprit hints. |

## Which capture is the pixel oracle, and why

`run_visual_capture.py`, **not** `golden_scene.py`. `golden_scene.py`'s own
`pixel_hash` is grabbed at a fixed *early fly-through frame* and is documented as
best-effort — the fly-through camera is not frame-deterministic. `run_visual_capture`
is the fixed-camera path: **pinned bookmark poses** (no fly-through drift) +
**parked cursor** (kills RTS edge-scroll camera delta) + **fixed-step clock**
(`MC2_SMOKE_FIXED_TIMESTEP=1`). The engine stamps each capture sidecar
`deterministic:true` and the per-bookmark PNG `sha256` is byte-identical run to
run. A render island changes **pixels**, so the oracle must be the sharpest
deterministic pixel path we have.

The harness runs **both** per state and synthesizes one *combined manifest*:
structural fields from `golden_scene`, plus `pixel_shas` (per bookmark) and an
aggregate `pixel_sha_all` from `run_visual_capture`. The floor/compare runs over
that combined manifest.

## How sharp is the oracle? (mc2_01, measured)

The mc2_01 floor (`golden_floors/mc2_01.json`, N=3 OFF captures, v0.4 deploy)
classifies **195 fields EXACT**, **0 DRIFT**, 7 ignored (4 bookkeeping + the 3
frame-count-proportional `*_samples` counters, see below).
The 4 pixel fields — `pixel_sha_all` + `pixel_shas.{overview_center,ridge_lowangle,highangle_wide}`
— are all **EXACT**. So a change to any of the three bookmark framings flips an
exact `sha256` → BEYOND → parity FAIL, with the culprit bookmark named.

**Verdict: SHARP.** The oracle catches a *localized shading-only* island diff
(any pixel in a bookmark frame changing value), not just gross structural
regressions. The three bookmarks cover `terrain_splat`, `sky`,
`terrain_lod_chunk_skirts`, `shadow_cascade`, and `static_prop_pbr`. An island
touching pixels **outside** all three framings would not be caught — add a
bookmark that frames the island's output (see *Sharpening*).

### Frame-count-proportional counters are IGNORED, not DRIFT (GOLDEN-PARITY-FRAMECOUNT-NORMALIZE-1)

`render_health.frame_graph.{ambient_probe_samples, fbo_samples, viewport_probe_samples}`
are **cumulative per-frame occupancy-query counts**: each probe fires once per
pass per *frame*, so the absolute value is proportional to how many frames the run
reached inside the fixed 30 s window. **Every Vulkan island runs at a different
framerate** (an island adds per-frame cost), so a gate-ON run reaches fewer frames
than gate-OFF and all three counters scale down proportionally. That is pure
frame-count variance, **not** a render/layout signal.

They are now classified **IGNORE** (`IGNORE_SUFFIXES` in `golden_compare.py`), not
DRIFT. Why not DRIFT: the drift tolerance is `[min − pad, max + pad]` over the
sampled OFF floor. When the N OFF captures happen to run at similar high
framerates the sampled range is *tight*, and a slower ON island's much lower count
falls outside the padded envelope → a **false BEYOND** even though pixels are
byte-identical. This actually happened on `mc2_01` / `MC2_VULKAN_EDGE_FOG_ISLAND`:
OFF ≈ 2400 frames vs ON ≈ 1735, samples `25267 → 14305` (fbo `29477 → 16688`),
identical fbo:probe ratio `1.167` both, **0 pixels changed**, yet the old floor
reported `within_floor=False culprits=frame_graph.{ambient_probe,fbo,viewport_probe}_samples`.

Why IGNORE and not per-frame normalization (option b): the combined manifest has
**no reliable total-frame denominator** — the frame count is itself the volatile
quantity, and `frame_graph.frame_graph_dryrun.frames` is 0 (dry-run disabled).
Reclassifying to IGNORE is the clean, principled choice and preserves the most
signal, because the **correctness** member of this same probe family is
`*_mismatches` — `ambient_probe_mismatches`, `fbo_mismatches`,
`viewport_probe_mismatches` — which stay **EXACT** and still gate: a nonzero
mismatch means a pass ran under the wrong ambient/FBO/viewport state, a real
regression. We drop ONLY the frame-proportional *counts*, never the *mismatches*.

Everything load-bearing remains EXACT: `registry_hash`, `exe_md5`, every
`pass_counters.*`, every `render_health` flag + `*_mismatches` counter, and every
pixel `sha`. `golden_compare.py self-test` regression-guards this: frame-count-only
divergence PASSES; a pixel-sha diff or a `*_mismatches` bump FAILS.

## The one-liner

```powershell
py -3 scripts\run_golden_parity.py mc2_01 MC2_MY_ISLAND_GATE --noise-floor golden_floors\mc2_01.json
```

- `scene` = mission stem; a bookmark file `tests/visual/bookmarks/<scene>.json`
  must exist (mc2_01/03/10/17/24 ship). **Use a ≥2-pose bookmark** — the engine
  capture iterator silently fails to fire on a single-pose bookmark.
- `gate_name` = the island env-var toggled OFF (state A) → ON (state B,
  `--gate-value`, default `1`).
- Omit `--noise-floor` to build a fresh floor from `--n-floor` (default 3) OFF
  captures instead of reusing the committed one.
- `--fixed-timestep` is the default (max pixel determinism); `--no-fixed-timestep`
  is debug-only and makes pixels drift.
- **Exit 0** iff A(OFF) vs B(ON) is within the floor for every field *including*
  every exact pixel `sha`. Nonzero otherwise, printing culprit fields + report path.

Each capture warps the mouse and takes ~30–45 s; a full run (floor build + A + B)
is ~7–8 min, or ~3 min reusing a committed floor. The harness **never**
`--kill-existing`; `run_visual_capture` reaps only its own children and restores
the cursor at exit.

## Noise-floor setup (one-time per scene)

A floor is exe-specific: `exe_md5` is an EXACT field, so a floor built on one
deploy will fail parity against a different exe. Rebuild when the deployed exe
changes.

```powershell
:: builds golden_floors\<scene>.json from 3 OFF captures of the current deploy
py -3 scripts\run_golden_parity.py <scene> MC2_NOOP --n-floor 3
:: (writes the floor under <exe_dir>\debug_state\golden_parity\<scene>\noise_floor.json;
::  promote it to golden_floors\<scene>.json to commit — strip absolute sample_sources)
```

The committed `golden_floors/mc2_01.json` was built this way (N=3, v0.4,
`exe_md5=b1597c01e2602a0fc97e0a37066a215e`).

## Reading a failure

`golden_compare` prints every diff as `ok` (within floor) or `FAIL` (beyond):

- `FAIL ... pixel_shas.<bookmark> ... (exact-field-changed)` → **that bookmark's
  frame changed pixels**. The strongest signal. `golden_diff_report` writes
  `report/golden_diff.(png|tga)` (changed pixels highlighted) + a heatmap +
  the changed-region bounding box.
- `FAIL ... pass_counters.<pass>.<field> ...` → a draw/state count changed in a
  named pass; `golden_diff_report` names the likely culprit pass.
- `FAIL ... registry_hash ...` → the GPU-resource ownership graph changed
  (a resource added/dropped or a lifetime drifted).
- `FAIL ... exe_md5 ...` → you are running a different exe than the floor was
  built on. Rebuild the floor.

The `RESULT:` line summarizes: verdict, within_floor, `pixel_oracle=SHARP/BLUNT`,
culprit fields, report path.

## Determinism caveats / residual drift envelope

- Always capture with `MC2_SMOKE_FIXED_TIMESTEP=1` (the default). Do **not** pass
  `--no-fixed-timestep` for a real parity check.
- **First-run warmup noise** (shader compile, texture residency, static-prop
  streaming, PBR first-use cache) can perturb the first capture. The floor is
  built from N≥2 captures precisely to absorb this; `run_visual_capture`'s own
  bless flow recommends `--runs 3 --warmup 1`. In the mc2_01 measurement the
  three bookmark `sha256`s were byte-stable post-warmup.
- A pixel field that **drifts OFF-vs-OFF** lands non-EXACT in the floor and then
  **cannot** fail an ON candidate. The harness prints a `NOTE:` listing any such
  fields, and the `RESULT:` line reads `pixel_oracle=BLUNT` if *zero* pixel
  fields came out exact (the oracle is too noisy for that scene — treat a PASS as
  structural-only and sharpen before trusting it).

## Sharpening (if the oracle is too blunt for a scene / island)

- **Frame the island**: add a ≥2-pose bookmark to
  `tests/visual/bookmarks/<scene>.json` whose `covers` names the island's output,
  so its pixels land in an EXACT `sha`.
- Pin the camera / fixed frame (already done via bookmarks + `MC2_SMOKE_FIXED_TIMESTEP`).
- If particles/animation phase leak into the frame and drift the `sha`, capture a
  scene/framing that minimizes them, or extend the engine's deterministic-settle
  before readback (`MC2_VISUAL_SETTLE`).

## Self-test (proves the harness before any island exists)

Run the one-liner with a **no-op gate** — OFF vs a gate the engine ignores is
within the floor, so it must exit 0:

```powershell
py -3 scripts\run_golden_parity.py mc2_01 MC2_GOLDEN_PARITY_NOOP_SELFTEST --noise-floor golden_floors\mc2_01.json
```

Verified result (v0.4 deploy):
`RESULT: PASS scene=mc2_01 gate=MC2_GOLDEN_PARITY_NOOP_SELFTEST=1 within_floor=True pixel_oracle=SHARP(4 exact px fields) culprits=(none)` — exit 0.

### Compare-layer self-test (no GPU, no game run)

`golden_compare.py self-test` exercises the classifier directly on synthetic
manifests — no deploy required — and regression-guards
GOLDEN-PARITY-FRAMECOUNT-NORMALIZE-1:

```powershell
py -3 scripts\golden_compare.py self-test
```

Asserts, against a deliberately *tight* OFF floor: (1) frame-count-only divergence
(the `*_samples` counters scaled down as a slower island would, pixels identical)
→ WITHIN; (2) a pixel-`sha` diff → BEYOND; (3) a `*_mismatches` correctness bump →
BEYOND. Exit 0 iff all three behave.
