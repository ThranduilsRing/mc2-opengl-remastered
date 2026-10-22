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
