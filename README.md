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
cmake -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=C:/absolute/path/to/repo/3rdparty -DCMAKE_LIBRARY_ARCHITECTURE=x64 -B build64
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
