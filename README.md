# MechCommander 2 -- OpenGL Remastered

![MC2 OpenGL Remastered](screenshots/hero.png)

A visual remaster of MechCommander 2, built on top of [alariq's OpenGL port](https://github.com/alariq/mc2). Rebuilds the rendering pipeline around PBR terrain splatting, real-time shadows, tessellation, and modern post-processing -- while leaving the original gameplay, missions, and systems alone.

**▶ [Download the latest release](https://github.com/ThranduilsRing/mc2-opengl-remastered/releases/latest)** — prebuilt engine + optional 4x upscaled textures.

MechCommander 2 was released by Microsoft/FASA Interactive in 2001 and its source code was later made public. alariq did the heavy lifting of getting that source running on modern Windows and Linux over OpenGL. This project picks up from there and focuses on the visuals.

## Features

### Terrain Rendering
- **PBR splatting** with per-material normal maps, parallax occlusion mapping, and roughness
- **Hardware tessellation** for smooth terrain geometry
- **Triplanar cliff mapping** -- rock texture on steep slopes
- **Cloud shadows** -- animated FBM noise
- **Height-based fog** (off by default; may need tuning)

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
- **Validation mode** (`--validate`) for autonomous build-test iteration
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
mc2.exe                                    # normal gameplay
mc2.exe -mission mis0101                   # skip menus, load directly
mc2.exe --validate --frames 60 --log out.json --screenshot out.tga  # validation mode
```

## Validation Mode

For autonomous development iteration. Loads a mission, renders N frames, writes telemetry JSON + optional screenshot, exits with status code.

```bash
mc2.exe --validate --frames 60 --log validate.json
```

Returns exit code 0 on success, 1 on shader compile errors or GL errors. See the [Modding Guide](docs/modding-guide.md) for the full autonomous development workflow.

## Improvements over vanilla

### Bug fixes
- Fixed per-frame sleep timer that was capping framerate
- Fixed intermittent color flickering

### Performance
- Moved significant rendering work from CPU to GPU (terrain, static props, tessellation)
- Runs at **4K with 30+ FPS zoomed out, ~90 FPS zoomed in** on mid-range hardware

(Plus alariq's extensive upstream bug fixes over the original engine.)

## Known Issues

- **Terrain tile overlap seams** visible at certain zooms and biome transitions
- **Pink color bleed** onto GUI elements from the post-process pipeline

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
- **OpenGL port, Linux support, and engine bug fixes**: [alariq/mc2](https://github.com/alariq/mc2) -- without this there is no remaster; all engine-level work is his
- **Visual remaster**: ThranduilsRing (this repo)
- **Development**: [Claude Code](https://claude.ai/code) (Anthropic)
