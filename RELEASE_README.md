# MechCommander 2 — OpenGL Remastered v0.2

A visual remaster of MechCommander 2: PBR terrain, real-time shadows, tessellation,
and modern post-processing. Original gameplay and missions are unchanged.

## Requirements

- Windows 10 or 11 (64-bit)
- GPU with OpenGL 4.3 support (any discrete GPU from 2012 or later)
- ~4 GB free disk space

## Install

Download all five zips and extract each into the **same folder**. Folder contents
merge — do not extract into subfolders.

| Zip | Contents |
|-----|----------|
| `mc2-remastered-engine.zip` | Executable, shaders, DLLs, tools |
| `mc2-gamedata.zip` | Game archives, maps, sounds, movies |
| `mc2-burnins-4x.zip` | 4× upscaled per-mission terrain lightmaps |
| `mc2-art.zip` | 4× upscaled UI and art textures |
| `mc2-tgl.zip` | 4× upscaled 3D model textures |

`mc2-burnins-4x.zip` is technically optional — the game falls back to stock-resolution
lightmaps inside the FST archives — but terrain colormaps will look noticeably blurry
without it.

No original MC2 install required.

**Optional:** Extract `mc2-load-points.zip` (53 KB) into
`%USERPROFILE%\.mechcommander2\savegame\` (create the folder if it doesn't exist).
Provides 24 pre-built save games — one per campaign mission — each with a full pilot
roster and 1,000,000 CBills. Load from the **Load Game** menu to jump to any mission.

## Run

Double-click `mc2.exe`, or run `run-with-log.bat` to capture a `stderr.log` next to
the executable if you hit a problem.

## Toggles (in-game)

These work at any time during gameplay:

| Key | Effect |
|-----|--------|
| RAlt+F1 | Toggle bloom |
| RAlt+F2 | Shadow debug overlay |
| RAlt+F3 | Toggle shadows |
| RAlt+F5 | Terrain draw killswitch |
| RAlt+5 | Toggle GPU grass |

## Known Issues

- **Shadow stutter** when the camera moves a large distance in one jump. Panning
  continuously is smooth; the re-render only triggers on large jumps.
- **Shadow banding** shifts slightly with camera rotation on terrain with steep
  geometry — a known limitation of view-dependent tessellation with a fixed shadow map.

## Troubleshooting

**Black screen / immediate crash:** your GPU may not support OpenGL 4.3. Check
`stderr.log` for a `[GPU]` line — if the version is below 4.3 the engine will not start.

**Wrong textures / scrambled icons:** do not replace `data/art/` files from third-party
mods that predate this release. Some icon atlas files are resolution-sensitive and must
stay at their original sizes.

**No sound:** SDL2_mixer requires audio to be available at startup. Check that your
default audio device is active before launching.

## Credits

- **Original game**: Microsoft / FASA Interactive (2001)
- **OpenGL port, Linux support, and engine bug fixes**: [alariq/mc2](https://github.com/alariq/mc2) — without this there is no remaster
- **D3F font loader, multi-monitor mouse grab, SDL window-event dispatch fix, FMV reference**: [Alexbeav](https://github.com/Alexbeav) — code cherry-picked from [MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)
- **Visual remaster**: ThranduilsRing
- **Development**: [Claude Code](https://claude.ai/code) (Anthropic)
