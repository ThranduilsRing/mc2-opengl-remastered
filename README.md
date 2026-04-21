# MechCommander 2 -- OpenGL Remastered

A visual remaster of MechCommander 2, built on top of [alariq's OpenGL port](https://github.com/alariq/mc2). Rebuilds the rendering pipeline around PBR terrain splatting, real-time shadows, tessellation, and modern post-processing -- while leaving the original gameplay, missions, and systems alone.

MechCommander 2 was released by Microsoft/FASA Interactive in 2001 and its source code was later made public. alariq did the heavy lifting of getting that source running on modern Windows and Linux over OpenGL. This project picks up from there and focuses on the visuals.

## Features

### Terrain Rendering
- **PBR splatting** with per-material normal maps, parallax occlusion mapping, and roughness
- **Hardware tessellation** for smooth terrain geometry
- **Triplanar cliff mapping** -- rock texture on steep slopes
- **Cloud shadows** -- animated FBM noise
- **Height-based fog** -- thicker in valleys

### Lighting and Shadows
- **Static terrain shadow map** (4096x4096) with multi-frame accumulation
- **Dynamic mech shadows** (2048x2048) with Poisson disk PCF (16-sample)
- **Post-process shadow pass** -- shadows on all geometry via depth reconstruction
- **G-buffer MRT** -- normal buffer for deferred shadow/lighting decisions

### Post-Processing
- **Bloom** with threshold extraction and two-pass Gaussian blur
- **FXAA** anti-aliasing
- **ACES Filmic tonemapping** with gamma correction
- **Procedural skybox** with sun disc

### Tools
- **Validation mode** (`--validate`) for autonomous build-test iteration
- **AI texture upscaling** pipeline (4x via realesrgan-ncnn-vulkan)
- **Tracy profiler** integration (18 CPU+GPU zones, always-on)
- **Debug hotkeys** for toggling every visual feature live
- **Loose file overrides** -- drop textures in `data/` to override archive contents

## Building

Requires Visual Studio 2022 Build Tools with MSVC v143. All third-party dependencies (SDL2, GLEW, zlib) are included in `3rdparty/`.

```bash
cmake -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=./3rdparty/3rdparty -DCMAKE_LIBRARY_ARCHITECTURE=x64 -B build64
cmake --build build64 --config RelWithDebInfo --target mc2
```

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
- **Visual remaster**: Joe Mathews (this repo)
- **Development**: [Claude Code](https://claude.ai/code) (Anthropic)
