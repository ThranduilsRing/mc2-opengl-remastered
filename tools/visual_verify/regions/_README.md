<!-- doc-status: current | reviewed:2026-10-11 -->
# Region manifests

One JSON per bookmark pose. Each names the artifact-prone rectangles the visual
verification fleet hand-cropped and graded, so `verify.py` can crop + diff the
same strips automatically.

Seeded from the 2026-07 11-agent verification session verdicts
(`VISUAL-VERIFY-PIPELINE-1`). Rect coordinates are the fleet's measured pixel
boxes where a verdict gave exact coordinates (`mode: "px"`, against a 3840×2160
reference), otherwise a normalized box (`mode: "norm"`) covering the described
feature. Boxes flagged `"approx": true` are placed from the prose description and
should be tightened against a live full-res capture the first time this pose is
blessed.

## Schema

```json
{
  "bookmark": "mc2_10_saved_204523",
  "mission": "mc2_10",
  "framing": "NIGHT near-top-down, N base, worst-water pose",
  "regions": [
    {
      "id": "shore_upperright_inlet",
      "rect": {"mode": "px", "x": 2340, "y": 650, "w": 190, "h": 280,
               "ref_w": 3840, "ref_h": 2160},
      "severity": "critical",
      "check": "Water outline must not snap to terrain tile grid (no ~140px axis-aligned steps).",
      "thresholds": {"mean_delta": 2.5, "changed_pct": 1.0, "struct": 6.0}
    }
  ]
}
```

- `rect.mode` = `"norm"` (0..1 fractions of the frame) or `"px"` (absolute pixels
  vs `ref_w`/`ref_h`, scaled to the real capture resolution at crop time).
- `severity` = `critical` | `high` | `medium` | `low` — advisory only.
- `check` = plain-English "what to look for" (carried into the report + golden).
- `thresholds` = candidate-vs-golden **drift budget**: a region FAILs if
  `mean_delta`, `changed_pct`, or `struct_delta` exceeds these. Captures are
  deterministic, so a true match is ~0; the budget absorbs dither and trips on a
  real geometry/lighting change. Omitted keys fall back to `verify.DEFAULT_THRESHOLDS`.
