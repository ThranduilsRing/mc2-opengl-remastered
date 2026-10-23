# Scale realism: phase 1c

## Diagnosis before changing generation

Baseline: `6c0b66726`, seed 42, stock style. Measurements use the same
12-cell single-link structural clustering as the corpus analyzer (at least
three structures or any turret), rather than treating generated parcel records
as comparable to authored settlements. These clusters can merge adjacent sites
and omit lone buildings. Parcel statistics are therefore reported separately.
All 320 extracted missions are included, including shared/mod-derived missions;
these are group medians, not 320 independent authors. No installs are modified.

| Metric | Stock (45) | Mod group medians (275) | Old 1024 | Old 3072 |
|---|---:|---:|---:|---:|
| Structural clusters | 7 | 7.5–15.5 | 55 | 370 |
| Nearest neighbour median, cells | 20.53 | 17.97–37.49 | 33.31 | 27.36 |
| Nearest neighbour CV | .505 | .528–.729 | .360 | .174 |
| Empty dry land fraction | .815 | .688–.844 | .608 | .579 |
| Tree density CV across 8×8 bins | 1.047 | 0–1.316 | .519 | .242 |
| Road components | 1 | 0–2 | 1 | 1 |
| Largest road component fraction | .914 | 0–.999 | 1 | 1 |

CV is standard deviation / mean. Empty land means a dry sample farther than
10% of the map side from every cluster centroid, on a fixed 64×64 sampling grid.
It measures regional gaps, not literal unoccupied pixels. It intentionally scales
with map size; absolute nearest-neighbour distances are also given because the
small authored missions cannot supply a target large-map population density.
Tree bins include zero-tree bins; REAP3's median zero reflects maps with no trees.
See [raw baseline](scale-realism/before-metrics.json) and
[corpus group measurements](scale-realism/corpus-metrics.json).

The concrete visual problems are:

* **Roadside ribbons.** All sites attach to six hub-and-spoke routes, independent
  of population. At 3072 this becomes a nearly continuous line of small parcels
  with huge featureless spaces between roads. The NN CV drops by half between
  1024 and 3072. This is not uniform density over the entire plane: density is
  implausibly uniform along the few developed corridors.
* **No scale hierarchy.** Old parcel area p10/median/p90 is 209/342/818 cells²
  at 1024 and 195/322/832 at 3072; maximum/minimum area ratios are only 7.22
  and 7.67. Structural object p10/median/p90 is 6/11/18.6 and 6/12/25.
  Increasing map area ninefold mostly replicates the same parcel size palette.
  The planner caps all parcel sides at 42 and buildings at 28; every outpost
  gets at least three requested buildings. Stock pooled observations instead
  include lone outposts and towns up to 160 structures, compound sides to 138.
* **Forest confetti.** Every tree cluster samples a Gaussian with sigma six cells;
  cluster count scales directly with area. At the full-map view the same fine
  stippling covers nearly every region; the decreasing bin CV measures this.
* **Flat background.** Five smooth hills, one exact elliptical lake, a straight
  escarpment and one grass type occupy every synthetic map. The geometry grows
  with the map, while all developed and forest details retain the same scale.
* **Connectivity already passes.** Both baseline maps have one road component.
  More connectivity alone cannot fix the lack of settlement-driven topology.
  Terrain avoidance also already exists: roads/parcels reject water and slopes
  >=10 degrees. Missing terrain *preference* and regional land use are the issues.

## Design and extrapolation

Small missions are curated slices of a larger world. Preserve their generator
path through 720 cells. For 1024+, use low-frequency fields conditioned on slope,
elevation and water to establish wilderness, farmland, industrial, military and
urban regions before parcels. Select separated population centres and clustered
satellites, leaving undeveloped regions. A truncated rank-size power law supplies
requested structure counts; parcel area follows population, with aspect and
layout variation. This is a design prior, not a claim that fictional military
bases empirically obey Zipf's law.

Roads should serve this population: a minimum spanning tree between major nodes,
some redundant major links, and short satellite branches. Routes favour gentle
low ground, and never bridge forbidden terrain. Regions should also control tree
presence and cluster radius, allowing woods, scrub, fields and clear military
approaches. Statistical regressions must check both hierarchy and clustering,
alongside the existing geometry and terrain validator.

All measurements and renders are offline. Full maps retain Quonset scale and
nearest-neighbour 4× previews; detailed regional crops use 1 wu/pixel.

## Implemented model

`macro.py` operates on conservative eight-cell blocks (every cell must be dry
and below the flatness limit). Smooth random fields, relative elevation, slope
and shore proximity select separated major centres. Region boundaries are
warped and rotated, with varied extents; neither the planning grid nor repeated
clearance ellipses should become visible in the full-map view. The renderer
interpolates blurred region membership, never integer category labels.

Major populations start at 150 structures and decay with rank exponent .86;
satellites use a steeper .95 law with a one-structure floor. This two-tier,
truncated rank-size prior replaces a minimum of three buildings in every site.
Counts grow sublinearly with area, capped at 220 requests. Parcel area follows
population with random aspect, constrained by actual corpus archetype maxima.
The native footprint pass can reject buildings that do not fit; reported size
statistics count the accepted structures. Region determines civilian/industrial/
military mix, and small sites omit unnecessary defensive systems.

Prim's MST connects major centres; one to three additional major links provide
loops. Satellites branch to nearby earlier nodes. Eight-way A* minimizes length
plus slope, relative elevation and a smooth terrain cost, forbidding corner cuts.
Fine routes stay inside wholly traversable coarse blocks. Existing edge access,
parcel gates and access spurs remain connected to that network. The native
validator sweeps diagonal widths in short sections and checks their centrelines
against reserved roads. All sites that actually place retain an access road;
unsuitable parcels may be skipped, leaving some unused network nodes.

Woods occupy suitable wilderness moisture bands, with cluster radius
`4 + 36 * moisture²` cells. Fields and developed regions stay open; cluster
locations and density no longer repeat across the whole map. Large synthetic
maps also gain broad drainage relief and irregular lake shores. Supplied terrain
is still preserved. Tree canopies resolve to native-sized ellipses in 1 wu/pixel
detail crops instead of remaining three-pixel symbols.

The original 180/360/720 generation paths are exactly preserved, verified against
the original generator at `6c0b66726` (not merely against visually similar PNGs).
The stored historical small JSONs differ from that generator only in size
metadata; their original images and files are retained.

This remains an offline layout model. Rank-size sizes and valley preference are
design priors; a routed graph is not proof of engine MOVE passability. Sparse
fictional military/civilian populations do not establish real-world density
calibration at 3072 cells.

## Final seed-42 results

| Metric | 1024 before → after | 3072 before → after |
|---|---:|---:|
| Actual parcels/sites | 57 → 41 | 478 → 174 |
| All-site nearest-neighbour CV | .340 → .504 | .176 → .901 |
| Parcel area max/min | 7.22 → 40.80 | 7.67 → 30.55 |
| Largest accepted structural population | 25 → 150 | 27 → 83 |
| Sites with <=3 structures | 0% → 88% | 0% → 88% |
| Empty regional dry-land fraction | 60.8% → 86.4% | 57.9% → 62.3% |
| Tree density CV (8×8 bins) | .519 → 1.200 | .242 → 1.036 |
| Road components / largest fraction | 1 / 100% → 1 / 100% | 1 / 100% → 1 / 100% |

All-site nearest neighbours use parcel centres, including lone outposts. The
corpus-comparable structural clustering remains in the raw reports: at 1024 it
finds only five groups, with NN CV .207 (old .360); at 3072 it finds 26, with
NN CV .694 (old .174). The 1024 cluster-CV decrease is not hidden or treated as a
failure of the site hierarchy: the clustering rule excludes the many new lone
outposts. The regression therefore measures all-site spacing, while retaining
both definitions for review. Empty land continues to use structural clusters
for corpus comparability, so it excludes the proximity of lone buildings.

These results remove the measured roadside regularity, narrow site palette and
uniform forest statistics. The 3072 empty-land proxy improves only moderately;
the full-map distinction is that population occupies separated regions rather
than repeating continuously along the same six corridors. Not every statistic
is expected to equal the small authored-map medians.

The final 42-test offline suite passes, including three regional planning seeds,
valley-cost routing, no diagonal corner cutting, deterministic macro output,
hierarchy/gaps/forest statistics, deliberately unrouted diagonals, native tree
canopy dimensions, the existing 24-seed small-map sweep and exact small-map
baseline hashes. Both large bakes pass the native geometry/corpus validator;
the 3072 compressed JSON passes again after disk readback. No engine files,
game processes or install contents were changed.

## Review artifacts and reproduction

* [1024 before/after sheet](scale-realism/before_after_1024.png)
* [3072 before/after sheet](scale-realism/before_after_3072.png)
* [1024 raw metrics](scale-realism/metrics_1024.json) and
  [3072 raw metrics](scale-realism/metrics_3072.json)
* [1024 crop locations](scale-realism/crops_1024.json) and
  [3072 crop locations](scale-realism/crops_3072.json)

Full-map originals and exact 4× previews remain under `raster/generated/`.
Each sheet includes matched world-coordinate crops for urban core, industrial
and wilderness regions. The original crop pairs are
`scale-realism/{before,after}_{1024,3072}_{urban_core,industrial,wilderness}_1wu.png`,
each 1024×1024 at 1 wu/pixel with an exact `_4x.png` viewing copy. A crop's region
label describes its role in the new layout; the old location may be undeveloped.
The 3072 sheet reduces the full map to a 1536-pixel panel; use the separate
3115-pixel full original or 12460-pixel preview for full-map detail.

```
py -3 -B tools/mapgen/generate.py --size 1024 --seed 42 --cells-per-char 8 --out docs/mapgen/raster/generated
py -3 -B tools/mapgen/generate.py --size 3072 --seed 42 --cells-per-char 24 --out docs/mapgen/raster/generated
py -3 -B -m tools.mapgen.scale_review
py -3 -B -m tools.mapgen.scale_metrics
py -3 -B -m unittest discover -s tests/mapgen -v
py -3 -B tools/mapgen/validate.py docs/mapgen/raster/generated/generated_3072_42.json.gz
```

The comparison tool retrieves the baseline JSON and full PNGs from local git
revision `6c0b66726`; it needs no old worktree or installed game. The scalar
metrics and all crop bounds are saved alongside the images.
