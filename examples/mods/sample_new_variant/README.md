# sample_new_variant — Atlas AS7-X "Sampler" (legacy variant ADD, zero code)

Adds a genuinely NEW variant of the stock Atlas chassis by **whole-file-shadowing**
`data/objects/atlas.csv` — the legacy, works-today ADD path (S4 of
`.claude/ADD-PATH-RECON-1.md`). No gate, no engine change.

## What it does

The shadowed `atlas.csv` = stock file byte-identical, plus:

* one blank filler row (row 310, completing variant block 2's region), then
* a fourth 97-row variant block whose `VariantName` lands EXACTLY at
  row **314 = 23 + 97 × 3** — `VariantID 0003`, name `Atlas AS7-X Sampler`,
  loadout = Atlas Prime with two ER Lasers (compbas 143) swapped for an
  ER PPC (146) and an ER Large Laser (141).

`LogisticsData::initVariants` enumerates variant blocks open-endedly
(`while readString(23+97n, col 2) succeeds`), so the new block simply appears:
mech bay list, purchase (chassis is already in the mission's `purchase.fit
[Mechs]`; all component ids are stock-available), in-mission drop, and
post-mission save recovery (mod-variant recovery is already hardened in
`code/logisticsdata.cpp`).

## How to produce the shadowed `atlas.csv` (for YOUR variant)

The stock `atlas.csv` is NOT in this repo — it lives inside the game install
(loose under `data/objects/`, or extracted from the object FST). To build your
own shadowed copy:

1. **Get the stock file.** Copy `<install>/data/objects/atlas.csv` (or extract it
   from the FST with the game's asset tools). This is your base — keep it
   byte-identical below the append point.
2. **Find the append row.** Variant blocks are 97 rows; block `n`'s `VariantName`
   sits at 1-based row `23 + 97 × n`. The stock Atlas has 3 blocks (n=0,1,2), so
   the next name lands at row **314 = 23 + 97 × 3**. Pad with blank filler rows up
   to the block start if the stock file ends short (this example adds one filler
   at row 310).
3. **Append your 97-row block.** New `VariantID`, `VariantName`, and the loadout
   component ids (all must be stock-available compbas ids unless you also ship a
   `sample_equipment`-style ADD). NEVER insert between existing blocks — mission
   `.fit` files reference variants by row index (`VariantNumber`).
4. **Lint it** (next section). `mc2_modlint` computes the expected rows and points
   at the exact off-stride / truncated / duplicate-name row.

There is no dedicated CSV generator script — the file is authored by hand from
the stock base and validated by `mc2_modlint`. A one-off helper is left as a
follow-up; the lint step is the safety net.

## The layout is FRAGILE — lint it

Rows are 1-based; a block is 97 rows; the name MUST sit at `23 + 97n`; the
FIRST empty name cell stops enumeration (later blocks silently vanish); a
truncated block spawns a 0-armor mech. Validate before shipping:

```
mc2_modlint --mod examples/mods/sample_new_variant --stock-root <stock-data-root>
# or directly:
mc2_modlint --variant-csv examples/mods/sample_new_variant/data/objects/atlas.csv \
            --compbas <stock>/data/objects/compbas.csv
```

Exit 0 / `RESULT: clean` = the engine will see the block.

## Caveats (by design of the legacy path)

* Whole-file shadow = conflicts if two mods both touch `atlas.csv`
  (`mc2_modlint --mod a --mod b` reports it; engine is first-wins).
* Append only, NEVER insert: mission `.fit` files reference variants by
  row index (`VariantNumber`), so inserting a block renumbers stock variants.
* Missions authored against the mod need the mod active (missing variant
  falls back to variant 0 gracefully).
* Per-chassis variant index is masked to 8 bits in `getVariant()` — stay
  ≤256 blocks per chassis.
