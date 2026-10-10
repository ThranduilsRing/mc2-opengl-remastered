<!-- doc-status: current | reviewed:2026-10-11 -->
# mc2weapon_gui

Dear ImGui (SDL2 + OpenGL3) weapon editor for MechCommander 2. A **foolproof
front-end** over the same weapon model as the [`mc2weapon`](../mc2weapon/) CLI:
enum fields are dropdowns (you can't typo a Type / Range / FX), numbers are
validated live, and **Save is disabled while anything is invalid**. Saving writes a
loose `mods/<id>/data/objects/compbas.csv` overlay (+ `mod.json`) that the game
resolves first via `MC2_ACTIVE_MOD` — no `.pak` repack. No engine/game code linked.

## Build

```sh
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" build64 -DENABLE_MC2WEAPON_GUI=ON
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2weapon_gui
# -> build64/out/tools/mc2weapon_gui/RelWithDebInfo/mc2weapon_gui.exe
```

## Run

Needs `SDL2.dll` + `glew32.dll` (copy them next to the exe from the game dir, or run
from the game install root). `effects.csv` is loose (`data/objects/effects.csv`);
`compbas.csv` is packed in v0.4, so point `--compbas` at a loose copy (source data
or a mod):

```sh
mc2weapon_gui.exe --compbas path/to/compbas.csv --effects path/to/effects.csv
# (both auto-detected from common locations if omitted)
```

## Workflow

1. Pick a weapon from the **Weapons** list (filter box; `all components` to show the rest).
2. Edit in the **Editor** panel: `type` / `range` / `FX` are dropdowns; `damage / heat /
   recycle / tons / slots` are validated (invalid = red, with the reason). The **FX**
   picker lists every effects.csv entry (id + trail name; shows hit/miss).
3. **Create** a new weapon: type an unused masterID, click *Create* — it seeds a weapon
   you then edit.
4. Set `mod id` (and optionally `mod root`), click **Save overlay**. Then launch the game
   with `MC2_ACTIVE_MOD=<id>`.

## Relationship to the CLI

Same data model + validation rules as `tools/mc2weapon/mc2weapon.py`. Use the **GUI**
for foolproof point-and-click editing; use the **CLI** for scripting, batch edits, CI
validation (`mc2weapon.py validate ...`), and headless work.

## Flags

- `--headless-smoke [--frames N]` — load + render N frames then exit 0 (build/run check;
  falls back to a core-only load if there's no display).
- `--selftest` — load → edit → write overlay → reload → verify round-trip (no display).
- `--compbas <path>` / `--effects <path>` — override the auto-detected CSVs.

## Not yet

Bolt *visual* edits (texture/color, in `object2.pak`) and a 3D visualizer are later /
deferred — see [docs/weapon-tool-design.md](../../docs/weapon-tool-design.md).
