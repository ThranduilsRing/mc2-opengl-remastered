# MechCommander 2 — OpenGL Remastered (v0.5.2)

A modernized build of MechCommander 2: PBR terrain, real-time shadows,
tessellation, modern post-processing, a campaign/mod launcher, and a Mission
Editor. Original gameplay and missions are unchanged. Plays the stock campaign
plus a large library of community campaigns (MC2X / MCO) — 19 campaigns,
~398 missions.

For a guided download-to-playing walkthrough (including importing community
campaigns), see [docs/STARTER-GUIDE.md](docs/STARTER-GUIDE.md).

## Requirements

- Windows 10 or 11 (64-bit)
- GPU with OpenGL 4.3 support (NVIDIA GTX 900 series or later, AMD RX 400
  series or later, or Intel Iris Xe)
- ~6 GB free disk space

## Install

Create a new empty folder, then download the release zips and extract each
into that folder. Folder contents merge — do not extract into subfolders.

| Zip | Contents | Required? |
|-----|----------|-----------|
| `mc2-remastered-engine.zip` | `mc2.exe`, shaders, DLLs, Mission Editor, tools | yes |
| `mc2-gamedata.zip` | Game archives, maps, sounds, configs | yes |
| `mc2-movies.zip` | BIK cinematics | optional (gameplay works without intros) |
| `mc2-burnins-4x-pt1.zip` | 4× upscaled mission lightmaps (part 1) | optional but recommended |
| `mc2-burnins-4x-pt2.zip` | 4× upscaled mission lightmaps (part 2) | optional but recommended |
| `mc2-art.zip` | 4× upscaled UI and art textures | optional but recommended |
| `mc2-tgl.zip` | 4× upscaled 3D-model textures | optional but recommended |

The `burnins-4x` zips are technically optional — the game falls back to
stock-resolution lightmaps inside the FST archives — but terrain colormaps
look noticeably blurry without them.

No original MC2 install required to play the stock campaign. Community
campaigns are imported from an existing MC2X/MCO install via the launcher
(see below).

**Optional:** Extract `mc2-load-points.zip` (53 KB) into
`%USERPROFILE%\.mechcommander2\savegame\` (create the folder if it doesn't
exist). Provides pre-built save games — one per campaign mission — each with a
full pilot roster and 1,000,000 CBills. Load from the **Load Game** menu to
jump to any mission.

## Run

Run **`mc2.exe`**. The main menu's **Campaigns & mods** picker starts stock
or installed community campaigns; **Graphics** contains saved engine/display
options, presets, Remastered/Classic art and cheats. Content changes restart the
game and resume the selected campaign.

Import uses the portable Python 3 CLI shipped under `tools/` (standard library only).
From the installed game folder, probe then import your MC2, MC2X or MCO source:

```
py -3 tools/mc2_import/import_tool.py probe "SOURCE"
py -3 tools/mc2_import/import_tool.py auto "SOURCE" --deploy .
```

On Linux use `python3` instead of `py -3`. Add `--dry-run` to preview or `--force`
to replace an existing import. Ctrl+C cancels; check warnings/missing-file counts,
then press **Refresh** in the picker. Extract legacy installer EXEs into a separate
source folder first. The game migrates legacy launcher options into `options.cfg`
on startup and logs invalid/unknown entries with `MC2_LOG=1`.

If you hit a problem, `run-with-log.bat` captures a `stderr.log` next to the
executable.

## Mission Editor

> Mission Editor and UI Editor by [@Methuselas](https://github.com/Methuselas)

`Mission Editor.exe` ships alongside `mc2.exe`. Launch with `run-editor.bat`
(it must run with CWD set to the install folder). The editor runs on the same
GPU-driven rendering pipeline as the game and can open any campaign mission.
Treat as preview — save support and asset-import polish are still in progress.

## Toggles (in-game)

| Key | Effect |
|-----|--------|
| RAlt+F1 | Toggle bloom |
| RAlt+F2 | Shadow debug overlay |
| RAlt+F3 | Toggle shadows |
| RAlt+F5 | Terrain draw killswitch |
| RAlt+5 | Cycle HUD scale (1.0 → 0.90 → 0.85 → 0.80) |

## Known issues / limitations

- **Some static props pop in and out** — edge-case in the GPU visibility cull
  for small props near tile boundaries.
- **Widescreen**: the 3D scene renders 4:3 internally and stretches to fill
  the screen; HUD is fixed 800×600. A proper "hor+" widescreen mode is
  planned.
- Imported-campaign cosmetic edge cases (deploy-screen skill/rank icon
  scale on some campaigns, `<missing-string>` labels on a few MC2X
  campaigns) — see [docs/STARTER-GUIDE.md](docs/STARTER-GUIDE.md) §7 for the
  full honest list and workarounds (including the `MC2_ABL_SKIP_ERRORED_MODULES`
  flag needed only by MC2-Exodus).

### Tracked GitHub issues

Status of long-running issues (rows fixed during the v0.4→v0.5 cycle have
been dropped; see the release notes of each release for the fixed list):

| # | Title | Status |
|---|-------|--------|
| [#10](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/10) | Enemies missing scripted moves | Open — brain-stall diagnostics added (`MC2_BRAIN_FREEZE_TRACE`) |
| [#12](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/12) | Power generator glow upside-down | Open |
| [#14](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/14) | Pop-up turrets visible while underground | Open |
| [#17](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/17) | Mechs reluctant to fire weapons | Open — fire-decision diagnostics added (`MC2_FIRE_DECISION_TRACE`) |
| [#23](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/23) | Mission planner background turns gray | Open |
| [#26](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/26) | Cursor glitches in bottom 1/3 of screen | Open |
| [#37](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues/37) | Expanded terrain | Open (feature request) |

## Troubleshooting

**Black screen / immediate crash:** your GPU may not support OpenGL 4.3. Check
`stderr.log` for a `[GPU]` line — if the version is below 4.3 the engine will
not start.

**Black terrain on an imported campaign:** that campaign is missing its
colormaps — re-import it in the launcher from its original install.

**Wrong textures / scrambled icons:** do not replace `data/art/` files from
third-party mods that predate this release. Some icon atlas files are
resolution-sensitive and must stay at their original sizes.

**No sound:** SDL2_mixer requires audio to be available at startup. Check that
your default audio device is active before launching.

**Editor won't launch:** `Mission Editor.exe` must run with CWD set to the
install folder. Always launch via `run-editor.bat`.

**Stale after an update:** close the game fully before re-installing (a
running `mc2.exe` can't be overwritten).

## Credits

- **Original game**: Microsoft / FASA Interactive (2001)
- **OpenGL port, Linux support, and engine bug fixes**: [alariq/mc2](https://github.com/alariq/mc2) — without this there is no remaster
- **D3F font loader, multi-monitor mouse grab, SDL window-event dispatch fix, FMV reference**: [Alexbeav](https://github.com/Alexbeav) — code cherry-picked from [MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)
- **Mission Editor and UI Editor**: [@Methuselas](https://github.com/Methuselas)
- **Community Creators**: [MechCommander Omnitech by magic](https://www.moddb.com/mods/mechcommander-omnitech) and [MC2X by Wolfman](https://mc2x.net/)
- **Visual remaster + Mission Editor bring-up**: ThranduilsRing
- **Development**: [Claude Code](https://claude.ai/code) (Anthropic)
