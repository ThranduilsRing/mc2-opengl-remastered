# MechCommander 2 -- OpenGL Remastered


<img width="2141" height="997" alt="image" src="https://github.com/user-attachments/assets/724aa51d-b9ee-44f9-a958-dbf7a7875405" />


A visual remaster of MechCommander 2, built on top of [alariq's OpenGL port](https://github.com/alariq/mc2). Rebuilds the rendering pipeline around PBR terrain splatting, real-time shadows, tessellation, and modern post-processing -- while leaving the original gameplay, missions, and systems alone.

**▶ [Download the latest release](https://github.com/ThranduilsRing/mc2-opengl-remastered/releases/latest)** — a self-contained install across five zips.

### Install (from release)

Download all five zips and extract each into the **same folder** (contents merge without overwriting):

1. **`mc2-remastered-engine.zip`** — engine, shaders, runtime DLLs, asset tools
2. **`mc2-gamedata.zip`** — `.fst` archives + `data/sound`, `data/movies`, `data/objects`, stock-resolution loose files
3. **`mc2-burnins-4x.zip`** — 4× upscaled per-mission lightmaps (`data/textures/*.burnin.tga`); without this, terrain colormaps are blurry. The FST archives carry stock-resolution fallbacks, so the game runs without it but looks much worse.
4. **`mc2-art.zip`** — 4× upscaled PBR art overrides (`data/art/`)
5. **`mc2-tgl.zip`** — 4× upscaled PBR terrain model overrides (`data/tgl/`)

Run `mc2.exe`. No original MC2 install required.

**Optional:** **`mc2-load-points.zip`** (53 KB) — 24 pre-built save games, one per campaign mission, each with a full pilot roster and 1,000,000 CBills. Extract into `%USERPROFILE%\.mechcommander2\savegame\` (create the folder if it doesn't exist). Then pick one from the **Load Game** menu to jump straight to any mission.

MechCommander 2 was released by Microsoft/FASA Interactive in 2001 and its source code was later made public. alariq did the heavy lifting of getting that source running on modern Windows and Linux over OpenGL. This project picks up from there and focuses on the visuals.

## Features

### Cinematics (new in v0.2)
- **Full-motion video playback** via FFmpeg (LGPL) -- intros, mission briefings, and debriefs all play in-engine. Audio-mastered A/V clock with wall-clock fallback; letterboxed screen-space quad.

### Terrain Rendering
- **PBR splatting** with per-material normal maps, parallax occlusion mapping, and roughness
- **Hardware tessellation** for smooth terrain geometry; seam expansion stitches cliff face discontinuities via tangent-plane projection
- **Triplanar cliff mapping** -- rock texture on steep slopes
- **Cloud shadows** -- animated FBM noise, amplitude normalized per biome
- **Height-based fog** (off by default; may need tuning)
- **Terrain grain** fades by projected screen frequency (no grain noise at high zoom)

### Lighting and Shadows
- **Static terrain shadow map** (8192x8192), rendered once on the first frame
- **Dynamic mech shadows** (4096x4096) with Poisson disk PCF (16-sample)
- **Post-process shadow pass** -- shadows on all geometry via depth reconstruction
- **G-buffer MRT** -- normal buffer for deferred shadow/lighting decisions

### Post-Processing (infrastructure)

The post-process pipeline is built and running but most effects are **off by default** — the intent is to have the plumbing in place so effects can be dialed in later without re-wiring the renderer.

- **Procedural skybox** with sun disc (on by default)
- **Bloom** (off by default) -- threshold extraction and two-pass Gaussian blur
- **FXAA** (off by default) -- anti-aliasing
- **ACES Filmic tonemapping** (off by default) -- tonemap and gamma correction

### Tools
- **AI texture upscaling** pipeline (4x via realesrgan-ncnn-vulkan)
- **Tracy profiler** integration (18 CPU+GPU zones, always-on)
- **Debug hotkeys** for toggling every visual feature live
- **Loose file overrides** -- drop textures in `data/` to override archive contents

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
mc2.exe -mission mis0101    # skip menus, load directly into a mission
```

## Improvements over vanilla

### Bug fixes (in upstream code)
- Removed the 10 ms per-frame `nanosleep` in the main loop that capped framerate at ~100 FPS (alariq port, 2016)
- Fixed a base-game pathfinding crash: `GlobalMap`'s no-arg ctor left `pathExistsTable` uninitialized, surfacing on mod content
- Guarded `Turret::update` against `teamId == -1` (neutral/environmental turrets), an out-of-bounds read on `turretsEnabled[]`

### Performance
- Moved significant rendering work from CPU to GPU (terrain, static props, tessellation)
- Configurable FPS cap via `MC2_FPS_CAP` env var (default 165 in-mission, 90 in menus)
- Runs at **4K with 30+ FPS zoomed out, ~90 FPS zoomed in** on mid-range hardware

(Plus alariq's extensive upstream bug fixes over the original engine.)

## Known Issues

- **Shadow map stutter** when the camera moves more than ~500 units; a world-fixed static shadow design is ready but not yet implemented
- **Shadow banding** shifts with camera rotation due to view-dependent terrain geometry

## Documentation

- **[Modding Guide](docs/modding-guide.md)** -- rendering pipeline, shader editing, texture upscaling, debug hotkeys, autonomous development
- **[Changelog](CHANGELOG.md)** -- all rendering features added over vanilla MC2

## License

- Original game: Shared Source Limited Permission License (see EULA.txt)
- OpenGL port base (alariq/mc2): GPL v3 (see license.txt)
- Third-party libraries use their own licenses
- Tracy profiler: BSD-3-Clause

## Credits

- **Original game**: Microsoft / FASA Interactive (2001)
- **Community Creators** - be sure to check out [MechCommander Omnitech by magic](https://www.moddb.com/mods/mechcommander-omnitech) and [MC2X by Wolfman](https://mc2x.net/) - they have been keeping this game active for 20+ years. 
- **OpenGL port, Linux support, and engine bug fixes**: [alariq/mc2](https://github.com/alariq/mc2) -- without this there is no remaster; all engine-level work is his
- **D3F font loader, multi-monitor mouse grab, SDL window-event dispatch fix, FMV reference**: [Alexbeav](https://github.com/Alexbeav) — code cherry-picked from [MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)
- **Visual remaster**: ThranduilsRing (this repo)
- **Development**: [Claude Code](https://claude.ai/code) (Anthropic)
