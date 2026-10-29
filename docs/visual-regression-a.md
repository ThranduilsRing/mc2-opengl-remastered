# Visual regression Phase A

Run from the repository root with Python, Pillow and NumPy installed. This is an
explicit review tool; the merge gate does not run captures or pixel comparisons.
The `visual_regression_schema` contract only validates `scripts/visual_regions.json`
and its bookmark references, without launching the engine or reading goldens.

```
py -3 scripts/run_visual.py --install E:/mc2-installs/claude-visual-regression-a --record-golden
py -3 scripts/run_visual.py --install E:/mc2-installs/claude-visual-regression-a --out E:/mc2-archive-2026-10/proofs/visual-regression-a/self
py -3 scripts/run_visual.py --install A:/Games/mc2-opengl/mc2-win64-v0.4 --out E:/mc2-archive-2026-10/proofs/visual-regression-a/v04
```

Default goldens: `E:/mc2-archive-2026-10/golden/visual-a/`. Recording requires a
new directory; existing goldens cannot be overwritten. Use `--golden DIR` to
select another version. PNGs, original engine sidecars, capture manifests,
bookmark recipes and logs stay outside git. Each set records the executable
SHA256 and the tolerance/camera configuration. Capture directories must be new
to prevent accidentally grading stale evidence. A failed recording is incomplete
and must be removed or replaced by a new directory before retrying.

Five fixed views use existing authored bookmarks: mc2_01 airport (day), mc2_10
runway (night), mc2_03 industrial base, mc2_24 fortress battlefield, and the
mc2_01 player Werewolf close-up. The battlefield view captures the stock mission;
it does not inject the separate battle benchmark's 48-mech fixture. Each capture
uses render trigger frame 240, 30 settle frames, the deterministic clock and
the runner's pinned render settings. The engine needs two poses, so the selected
pose is repeated. Only the first is graded. Launches use the existing background
runner with `--no-kill`, its host lease, and own-process cleanup.

The tolerance map partitions the complete normalized frame into world and HUD.
Each region specifies a maximum RGB channel delta and allowed fraction of
changed pixels per 32-pixel tile. A tile exceeding both thresholds is a changed
region, including partial tiles at the edges. Every such tile appears as a red
box on the candidate image. There is no whole-frame MAD pass threshold. The
initial tolerances are explicit conservative defaults, not calibrated quality
guarantees; inspecting the boxes and montage remains necessary.

`index.html` is a single responsive page with golden/candidate montages; tap an
image for full resolution. `report.json` lists every box in pixel coordinates,
its region and changed fraction, image hashes, build identities and MAD for
reference. To compare existing sets without launching:

```
py -3 scripts/run_visual.py --compare-only CAPTURE_DIR --golden GOLDEN_DIR --out REPORT_DIR
```

Exit codes: 0 clean, 1 changed regions, 2 invalid or missing evidence. Missing
PNGs/sidecars, non-deterministic captures, incompatible dimensions, camera poses,
render settings or recipes fail explicitly. A change in simulation timing is
recorded in the report and may explain boxed changes; this tool is not a sim
determinism verdict.
