<!-- doc-status: current | reviewed:2026-10-11 -->
# MechCommander 2 -- OpenGL Remastered

MechCommander 2 was released by Microsoft/FASA Interactive in 2001 and its source code was later made public. alariq did the heavy lifting of getting that source running on modern Windows and Linux over OpenGL. This project picks up from there and modernizes the entirety of the engine.

![MC2 OpenGL Remastered](screenshots/hero.png)

A game engine rewrite and modernization of MechCommander 2, originally built on top of [alariq's OpenGL port](https://github.com/alariq/mc2). This repo modernizes nearly everything about the backend of the game. Nearly all per-frame CPU work has been eliminated. Most but not all systems live on the GPU now. As of 6/27/2026, the backend is "Vulkanized" - not a Vulkan renderer, but all draw passes, GL states, etc. are owned and explicit. I have reworked the editor to be more user friendly, added an asset viewer, and an explicit mod pathway that supports community campaigns and mods. As of today, 19 campaigns and nearly 400 missions are supported. 

**▶ [Download the latest release](https://github.com/ThranduilsRing/mc2-opengl-remastered/releases/latest)** — a self-contained install.

## Building

Requires Visual Studio 2022 Build Tools with MSVC v143. All third-party dependencies (SDL2, GLEW, zlib) are vendored as `3rdparty.zip` at the repo root (Git LFS). **Extract it before configuring:**

```bash
# one-time setup: extract vendored dependencies
cd <repo-root>
unzip 3rdparty.zip                # or right-click -> Extract Here
# you should now see 3rdparty/{cmake,include,lib,tracy}
```

Configure and build (note the **absolute** path to the 3rdparty folder -- relative paths won't resolve correctly at compile time):

```bash
cmake -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=C:/absolute/path/to/repo/3rdparty -DCMAKE_LIBRARY_ARCHITECTURE=x64 -DMC2_IMGUI=ON -B build64
cmake --build build64 --config RelWithDebInfo --target mc2
```

Output lands at `build64/RelWithDebInfo/mc2.exe`.

**Always use `RelWithDebInfo`**. Release builds crash due to GL debug callback registration.

For detailed build and dependency information, see [BUILD-WIN.md](BUILD-WIN.md).

## Running

```bash
mc2.exe                     # normal gameplay
mc2.exe -mission mc2_01     # skip menus, load directly into a mission
```

## Test configurations

Configurations we have run. This table is updated periodically, not on every build. Numbers come from fixed benchmark views (one day mission, one night mission, no battle) with VSync off, so busy battles will be slower. Fps is 1000 divided by frame time. Ranges run from median (p50) to 95th percentile (p95).

| Hardware | OS / backend | Resolution | Settings | Day | Night | Measured |
|---|---|---|---|---|---|---|
| Ryzen 7 5800X3D, GeForce RTX 2070 SUPER (driver 560.94), 64 GB | Windows 10, Vulkan | 1080p | High: Remastered kit, SMAA, dynamic shadows, vegetation, night local lights | CPU frame 3.5 to 4.7 ms (about 210 to 285 fps); GPU frame 2.0 to 2.3 ms | CPU frame 5.8 to 15.7 ms (about 64 to 172 fps); GPU frame 3.3 to 6.6 ms | 2026-10-24 |
| same | same | 4K | High | CPU 5.5 to 6.6 ms (about 150 to 180 fps); GPU 4.8 to 5.3 ms | CPU 8.3 to 20.2 ms (about 50 to 120 fps); GPU 7.1 to 12.2 ms | 2026-10-24 |
| same | same | 1080p | Medium: half-resolution scene, 8 shadow samples | GPU 1.3 to 1.8 ms; CPU pending | GPU 2.4 to 4.4 ms; CPU pending | 2026-10-25 |
| same | same | 4K | Medium | GPU 2.1 to 3.0 ms; CPU pending | GPU 3.2 to 5.4 ms; CPU pending | 2026-10-25 |
| same | same | 1080p | Low: half-resolution scene, shadows off | GPU 1.2 to 2.7 ms; CPU pending | GPU 2.2 to 4.4 ms; CPU pending | 2026-10-25 |
| same | same | 4K | Low | GPU 1.9 to 2.7 ms; CPU pending | GPU 2.8 to 4.8 ms; CPU pending | 2026-10-25 |
| Ryzen 7 5800X3D, Radeon RX 7900 XTX (driver 26.9.2), earlier host | Windows 10, Vulkan | 1080p | Classic art, default settings | CPU 3.1 ms, GPU 1.1 ms (medians) | CPU 7.4 ms, GPU 2.5 ms (medians) | 2026-10-24 |
| same | same | 4K | Classic art, default settings | CPU 3.6 ms, GPU 1.5 ms (medians) | CPU 8.3 ms, GPU 3.1 ms (medians) | 2026-10-24 |
| WSL Ubuntu 24.04, Mesa lavapipe (software Vulkan) under Xvfb | Linux, Vulkan | 320x180 | Potato | not a performance configuration | simulation determinism check only (passes) | 2026-10-25 |
| Laptop: i7-7700HQ, GeForce GTX 1050 Ti 4 GB (driver 582.78) | Windows, Vulkan | 720p, 1080p | Low / Potato | pending | pending | pending |
| Same laptop, Intel HD 630 (driver 31.0.101.2145) | Windows, Vulkan | 720p, 1080p | Low / Potato | pending | pending | pending |

Notes: the RTX 2070 SUPER High rows use the Remastered kit; Classic art measured the same in median cost, but Remastered has a heavier night tail (CPU p95 about 20 ms at 4K), so 60 fps is not guaranteed in large night battles. The Medium and Low rows are provisional (host was not fully idle) and show GPU time only. The RX 7900 XTX row is from an older build, kept for comparison.

To measure your own machine: `py -3 scripts/run_battle_bench.py` for frame times, or `py -3 scripts/run_smoke.py --lite --keep-logs` for a quick pass/fail run.

## Modding

- [docs/modding-guide.md](docs/modding-guide.md) — start here for modding
- `mods/` — four shipped example mods ([magic-ballistic-weapons](mods/magic-ballistic-weapons), [magic-gamesys-tweaks](mods/magic-gamesys-tweaks), [modern-tree-pack-v1](mods/modern-tree-pack-v1), [my-explosions](mods/my-explosions)) — see the "Example mods" section of the modding guide
- [docs/modding-effects.md](docs/modding-effects.md) — VFX/effects authoring
- [docs/modding-animations.md](docs/modding-animations.md) — animation authoring
- [tools/mc2weapon/README.md](tools/mc2weapon/README.md) — weapon data tool
- [docs/asset-pipeline.md](docs/asset-pipeline.md) — canonical asset inventory (geometry/texture/GLB import paths)

## License

- Original game: Shared Source Limited Permission License (see EULA.txt)
- OpenGL port base (alariq/mc2): GPL v3 (see license.txt)
- Third-party libraries use their own licenses
- Tracy profiler: BSD-3-Clause

## Credits

- **Original game**: Microsoft / FASA Interactive (2001)
- **Community Creators** - be sure to check out [MechCommander Omnitech by magic](https://www.moddb.com/mods/mechcommander-omnitech) and [MC2X by Wolfman](https://mc2x.net/) - they have been keeping this game active for 20+ years. 
- **Initial OpenGL port / GPL scaffolding**: [alariq/mc2](https://github.com/alariq/mc2) — the 2016–2021 GL/SDL port this project was originally forked from. His platform layer and port scaffolding remain under GPLv3 (see [LICENSES.md](LICENSES.md)); the remaster's renderer, tooling, and engine modernization are new work in this repo.
- **D3F font loader, multi-monitor mouse grab, SDL window-event dispatch fix, FMV reference**: [Alexbeav](https://github.com/Alexbeav) — code cherry-picked from [MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)
- **Mission Editor and UI Editor**: [@Methuselas](https://github.com/Methuselas)
- **Visual remaster**: ThranduilsRing (this repo)
- **Development**: [Claude Code](https://claude.ai/code) (Anthropic)
