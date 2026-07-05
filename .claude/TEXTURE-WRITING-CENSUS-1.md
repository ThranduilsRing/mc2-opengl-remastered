# TEXTURE-WRITING-CENSUS-1

CENSUS ONLY (WHOLESALE-VECTORIZE-1 task 3) — no replacements built. Sweep of
the deployed texture surfaces for text/signage/marking-bearing art:

- `<deploy>/data/textures/` (3005 TGA; 807 top-level + mip/mask/overlay dirs)
- `<deploy>/data/tgl/128/` (2273 TGA model textures; 2728 total under data/tgl)
- `64Overlays/` road/runway tile markings covered separately by the
  WHOLESALE-VECTORIZE-1 vector-twin pipeline (`tools/terrain_beautify/cook_markings.py`)
  and are listed here only for completeness.

Deploy swept: `A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0`.
Method: filename keyword pass (sign/logo/decal/banner/billboard/arrow/…) +
visual contact-sheet sampling (96-tile broad sample + targeted zooms).
Sheets: `tools/terrain_beautify/_harness_out/markings/census_sheet1_keyword_hits.png`,
`census_sheet2_buildings.png`, `census_sheet3_broad_sample.png`,
`census_sheet4_zoom.png`.

Building/prop texture replacement is a DIFFERENT pipeline (Blender lane /
loose-file `data/tgl/` override per docs/modding-guide.md §5) — feasibility
notes below are advisory only.

## A. Literal writing (letters/numerals visible)

| File | Size | Text content | Vectorize feasibility |
|---|---|---|---|
| `data/tgl/128/billboard.tga` | 512x512 | Portrait + "ALVARADO" block caps | HIGH — flat billboard face; text is an isolated caption band; re-author as SVG/text layer over upscaled portrait. Already 512² (someone upscaled it); text still raster-fuzzy. |
| `data/tgl/128/billboard2.tga` | 512x512 | Hazard-triangle logo + "LIBERTY" script | HIGH — same billboard geometry; logo = 2 vector shapes + script text. |
| `data/tgl/128/school.tga` | 128x128 | "Public School" wall plaque | MEDIUM — small plaque region on a brick facade; vector text stamp into an upscaled facade is easy; UV region is a plain rect. |
| `data/textures/64Overlays/runway0010-0014.tga` | 64x64 | Runway designators "15W" / "72G" + threshold bars + arrows | DONE — covered by the vector-twin sidecar pipeline this slice (raster-fallback components for numerals, IoU-gated). |
| `data/tgl/128/gaspumps.tga` (also inside `lightrnflags.tga` atlas) | 128x128 | Red pump logo with a stylized "G" | LOW value — single letterform in a 16px sub-tile; contrast already OK. |

## B. Symbol signage (no letters, but sign/marking glyphs — prime vector candidates)

| File | Size | Content | Feasibility |
|---|---|---|---|
| `data/tgl/128/signs.tga` | 128x128 | Road-sign atlas: stop octagon, yield triangle, curve-warning (snake), black/yellow hazard bars | HIGH — four pure-geometry glyphs; ideal SVG re-author; used by roadside sign props. |
| `data/tgl/128/a_banners.tga` + 8 faction variants (`a_darkra_/a_dracon_/a_falcon_/a_mother_/a_rebels_/a_wolfma_/a_wolf_banners`, `a_bannersx`) | 128x128 | Cloth banners with faction iconography (skull+sabers, dragon, falcon, mech silhouette, X) | MEDIUM-HIGH — flat icon-on-cloth; icons trace cleanly; 9 files share one layout so one template covers all. |
| `data/tgl/128/lightrnflags.tga`, `gaspumps.tga` | 128x128 | Faction emblem atlas (sunburst, fist, crossed sabers, X) + hazard-stripe strips + color bars | MEDIUM — atlas sub-tiles are small (32px); emblems are shared faction art (same shapes as banners). |
| `data/tgl/128/fixarrowiv.tga` | 128x128 | Yellow/black hazard chevron strips | HIGH — pure stripes; trivially parametric. |
| `data/tgl/128/platesdecals.tga`(+`x`) | 128x128 | Dropship/eagle silhouette decal on wall plates | MEDIUM — one silhouette trace. |

## C. Painted markings on buildings (hazard striping, pads, emblems)

| File(s) | Content | Feasibility |
|---|---|---|
| `mechhanga1-8.tga`, `mechhangar.tga`, `mechhangas/_dam/x` | Circular hangar emblem (top-right disc) + hazard stripe bands + cyan light strips | MEDIUM — emblem+stripes are vector-friendly; the 8 camo variants share the emblem. |
| `largehangar0-2/_dam.tga` | Yellow/black hazard chevrons on door aprons | HIGH — parametric stripes. |
| `a_repairbay*.tga` (9 variants) | Hazard striping + green status rectangles | HIGH — same stripe primitive. |
| `a_landingmarker.tga` | Flat green landing square | Trivial — single flat quad (likely colorkeyed FX marker). |
| `turretbase*/bunk*/gate_*` | Sparse hazard stripes on trim | LOW priority — tiny screen footprint. |

## D. Not writing (checked and cleared)

- `data/textures/*.water*/*.detail/*.burnin/colormap` — terrain rasters, no text.
- City blocks (`a_cityblock*story*`), walls, doors, windows, rubble — plain art.
- Sky domes (`sky*.tga`), mech/vehicle skins (`*rgb.tga` colorize maps, blue-keyed) — no signage (mech skins carry faction decals via the RGB colorize path, out of scope).
- `s_face.tga` — carved stone face (art, not text).
- HUD/UI text lives under `data/art/` (mcl_* layout fits + UI TGAs) — UI pipeline, explicitly out of census scope.

## Priority shortlist (if a vectorize-writing slice is ever cut)

1. `signs.tga` — 4 pure-geometry road signs, highest clarity gain per pixel.
2. `billboard.tga` / `billboard2.tga` — the only literal prose in the world; text layer re-author.
3. `mechhangar emblem + hazard stripe` primitive shared across `mechhanga*/largehangar*/a_repairbay*` (one template, ~20 files).
4. `a_banners` faction icon set (9 files, one layout).
5. `school.tga` plaque text.

No files modified by this census.
