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
- **MechBay: DONE** — all 75 tiles (68 statics + 7 buttons) → one coherent 800x600
  logistics-screen panel (metal frames + yellow trim + mech-cell grid + loadout
  icon column; dynamic-content areas transparent). The earlier "3 FST-packed
  atlases missing" was a RESOLVER BUG, not an FST issue: some `FileName` fields
  carry the extension inline (`"mcl_mb_4.tga"`) so the tool tried `mcl_mb_4.tga.png`;
  the loose atlas is `MCL_MB_4.png`. Fixed by stripping any inline image extension
  before the ext search. The atlases are all LOOSE in `data/art/gui` — no FST
  extraction is required for the stock panels.

## Next steps

1. ~~**Wire the baked pause frame into the ImGui pause page**~~ **DONE** (`f3ed79c2`,
   PAUSE-IMGUI-SLICE-2): `code/pausewindow.cpp` `renderImGui` loads the baked frame
   (`loadTexture` → `get_gosTextureHandle` → `gos_GetGLTextureName`) and draws it to
   the WINDOW draw list (NOT the foreground list) under transparent buttons, inside
   the canvas-mapped rect (authored bbox origin 638,86 size 162×239). Agent-verified
   matching the legacy panel; tier1 smoke 5/5 +0 gate-off. User click-through pending.
2. ~~Extract FST-packed atlases~~ — NOT NEEDED (stock atlases are loose; the
   "missing" was the inline-extension resolver bug, now fixed).
3. **Info window** — procedural port (rects + scrollbar), no baked art.
4. ~~batch driver~~ **DONE** — see "Batch mode" below.

## Batch mode (`--all`)

Bakes EVERY tile-stitched panel across a legacy `game/` dir into one PNG each +
a `manifest.json` the ImGui layer consumes. In-mission panels appear in every
`buttonLayout<res>.fit`; they are baked ONCE from the canonical 800 fit.

```
py -3 tools/ui_art_reconstruct.py --all    --legacy-dir <install>/data/defs/ui/legacy/game    --art-dir <install>/data/art/gui    --out-dir <out>/ui_frames
```

Bakes all 14 stock tile-stitched panels with ZERO missing atlases:
Pause, MechBay(75), PilotReady(79), PilotReview, Promote{Left,Right}, SalvageArea,
CM, OptionsArea, PlayerParams, Obj, Video, Timer, Results. Parsing tolerates
malformed hand-authored fit values (e.g. a stray `129|`). Manifest per panel:
`{panel, fit, png, origin:[x,y], size:[w,h], authored_space:[800,600], tiles,
statics, buttons, missing_atlases:[]}`.

Data quirk: `PromoteGadgetLeft` reports size 400x1286 — its bbox includes the
authoring tool's offscreen slide-in element (same artifact as the AAR
`localHeight=1326`); the ImGui layer clips to the real 600 (use origin + the
600-clamped height, not the raw bbox height, for that panel).

## Wiring a baked panel into an ImGui page (the pattern)

`code/pausewindow.cpp` `renderImGui()` is the reference (PAUSE-IMGUI-SLICE-2):
load the baked PNG once via `mcTextureManager->loadTexture` →
`get_gosTextureHandle` → `gos_GetGLTextureName`, then
`ImGui::GetWindowDrawList()->AddImage(texID, pmin, pmax, uv0, uv1)` UNDER the
buttons (window draw list, transparent WindowBg), positioned at the manifest's
origin/size mapped through `gos_ComputeHudCanvasBox`.
