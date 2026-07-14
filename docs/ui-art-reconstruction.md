# UI art reconstruction (UI-ART-RECONSTRUCT-1)

The legacy MC2 UI panels are **stitched at runtime** from many small UV-crops of a
few 128×128 shared atlases (`data/art/gui/MCUI_Universal{1,2,3}.png` and per-screen
atlases like `mcl_mb_*`). Each `[<Panel>Static N]` block in a legacy
`data/defs/ui/legacy/game/*.fit` names an atlas + a UV rect (`UNormal`/`VNormal` +
`UWidth`/`VHeight`) + a screen rect (`XLocation`/`YLocation`/`Width`/`Height`),
blitted 1:1 onto the 800×600 authored canvas. They tiled because of period
texture-size limits — corners, edge bars, and fills were separate little pieces.

**We have no such limit.** For the ImGui menu recreations we compose those tiles
**offline** into ONE flattened RGBA PNG per panel (the exact pixels the game
assembles) and sample it as a single texture, instead of re-stitching at runtime.

## The tool

`tools/ui_art_reconstruct.py` — reads a legacy `.fit` + the art dir, resolves a
panel's `Static`/`Button` tiles, composes them into one PNG. Engine-parity: PNG
magenta key (`FF00FF` → transparent, PNG-MAGENTA-KEY-1); top-left UV origin; 1:1
blit (honors a scale if a panel ever differs). Auto-discovers tile count when a
panel has no `<Panel>Window` count header. Missing atlases (packed in an FST) are
skipped and **reported by name** so you know exactly what to extract — no crash.

```
py -3 tools/ui_art_reconstruct.py \
   --fit  <install>/data/defs/ui/legacy/game/buttonLayout800.fit \
   --art-dir <install>/data/art/gui \
   --panel Pause --out data/ui_reconstructed/pause_frame.png --crop
```

## Panel taxonomy (which panels this serves)

- **Tile-stitched (this tool bakes them):** `Pause`, `MechBay`, `Obj`, `OptionsArea`,
  `PilotReady`, `PilotReview`, `PlayerParams`, `PromoteGadgetLeft/Right`, `CM` … —
  the `<Panel>Static N` pattern across `buttonLayout*.fit` + `MCL_*_Layout.fit`.
- **Procedural (NOT tile-stitched — a different ImGui port style):** `InfoWindow`
  draws colored boxes (`InfoHeaderBoxColor`, `ScrollContentBox*`) + a hand-rolled
  scrollbar in code, not atlas tiles. Its ImGui port reproduces the procedural
  draw (rects + scrollbar), no baked frame.

## Status (2026-07-14)

- **Pause: DONE** — 17 tiles (10 frame statics + 7 button-normal) → one 162×239 PNG
  at authored origin (638,86). Baked reference: `data/ui_reconstructed/pause_frame.png`.
  Matches the legacy in-game panel (hazard-stripe frame + 7 blue button slots).
- **MechBay: partial** — 68 tiles composed; 3 atlases (`mcl_mb_4/5/6.tga`) are
  FST-packed and need extraction before a full bake.

## Next steps

1. **Wire the baked pause frame into the ImGui pause page** (`code/pausewindow.cpp`
   `renderImGui`): draw `pause_frame.png` via `GuiRuntime::DrawUiImage` inside the
   `gos_ComputeHudCanvasBox` rect (authored 162×239 at origin 638,86 → scaled), then
   the 7 buttons + labels on top — replacing the flat ImGui window styling with the
   real MC2 chrome. This is the visual-parity upgrade the port was gated for.
2. **Extract FST-packed atlases** (`mcl_mb_*` etc.) so the logistics panels bake fully.
3. **Info window** — procedural port (rects + scrollbar), no baked art.
4. Consider a batch driver (`--all`) that bakes every tile-stitched panel + emits a
   manifest of frame PNGs + their authored origins for the ImGui layer to consume.
