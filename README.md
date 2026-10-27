<!-- doc-status: current | reviewed:2026-10-25 -->
# MechCommander 2 Remastered

A rebuilt engine for MechCommander 2 (Microsoft / FASA Interactive, 2001). The missions, campaigns and game rules are the ones you remember. Underneath them sit a Vulkan renderer, a GPU-driven frame, a modern C++23 code base, and a mod pipeline that runs the big community campaigns.

![MechCommander 2 Remastered](screenshots/hero.png)

**[Download the latest release](https://github.com/ThranduilsRing/mc2-opengl-remastered/releases/latest)**: a self-contained install. Run `mc2-launcher.exe`.

## At a glance

| | |
|---|---|
| Renderer | Vulkan only. The OpenGL backend and GLEW were deleted in October 2026. |
| Code | C++23 (MSVC `/std:c++latest`; Clang 20 with libc++ on Linux) |
| Platforms | Windows 10/11 x64; native Linux build (Vulkan through SDL) |
| Content | Stock campaign plus community campaigns: 19 campaigns, about 400 missions |
| Art | Remastered building kit by default; one checkbox switches back to Classic art |
| Tests | About 3,500 doctest cases in 179 test files, plus static contract checks, smoke runs, simulation-determinism and save/load checks |

## What's new: legacy subsystems replaced

Every row below is in the code today, not on a roadmap.

| Legacy subsystem | Replaced with |
|---|---|
| OpenGL renderer (GL contexts, GLEW, GL debug layer) | A Vulkan backend on volk and VMA. GL was deleted, not just switched off. |
| CPU-transformed TGL shapes drawn one at a time | GPU-instanced batchers for buildings, props and trees; mechs skinned on the GPU from a bone buffer; GPU occlusion culling with a hierarchical Z-buffer (HZB) |
| CPU terrain submission | GPU-driven terrain: compute culling and indirect draws, chunked LOD, PBR material splatting, a BC7 KTX2 colour-map atlas |
| Static, CPU-lit look | Cascaded shadow maps, a baked terrain shadow map (PCSS), cloud shadows, HDRI sky, SH-L2 image-based ambient on props, SMAA/FXAA, impact decals, GPU vegetation culling |
| Hand-rolled `Stuff::` math library (vectors, matrices, rotations, planes, rays, spheres, OBBs) | glm. 59 Stuff source files and about 29,700 lines deleted. A bit-identity harness checked every op before simulation code moved, and save formats stay byte-exact. |
| Hand-rolled containers and strings (`EString`, `EList`, `SortList`, File-family tables) | `std::string`, `std::vector` and the other standard containers |
| Custom game heaps (TGL heap, appearance heap, object caches) | The CRT allocator, with `std::unique_ptr` ownership |
| `printf` / `SPEW` / `OutputDebugString` logging | One logging facade (`mc2::log`): `std::format`-based, with categories and levels |
| About 1,400 raw `getenv` reads of `MC2_*` switches | A typed, compile-time-checked gate table of 801 named gates. A misspelled gate name does not compile. |
| TGA textures loaded and converted at runtime | An offline cooker (`tools/content_pipeline`): glTF, ASE or FBX sources go into one IR, then out as KTX2 BC7 / BC5 textures with full mip chains and a validated material schema |
| GLSL compiled by the driver at startup | 71 Vulkan shaders compiled offline to SPIR-V (glslang, plus Slang for new compute shaders) and checked against their reflection |
| Simulation tied to the render frame rate | A fixed 30 Hz simulation tick, independent of frame rate and camera, with pinned determinism checks on Windows and Linux |
| Loose override folders | A layered mod filesystem (active mod, its dependencies, base data, FST archives), with one resolver shared by the engine, editor and installer |
| Bink cinematics that did not play | Bink 1 cinematics play through FFmpeg (LGPL) |
| Ad-hoc debug overlays | Dear ImGui: graphics options, render-pass inspector, editor inspector |
| Crashes with no trail | A crash reporter that writes a report bundle and tells you where it is |

## Modern C++: more than 24,000 legacy idioms retired

The code was C++98-era when the project started. Since 9 October 2026 a ratchet (`tools/cxx17_ratchet`) has counted 20 legacy idioms in every source directory, and its counts may only go down. From each idiom's peak to today:

| Legacy idiom | Modern replacement | Peak | Now |
|---|---|---:|---:|
| C-style casts | `static_cast` / `reinterpret_cast` / `std::bit_cast` | 7,963 | 87 |
| `NULL` | `nullptr` | 4,971 | 33 |
| `#define` constants | `inline constexpr` | 2,907 | 451 |
| Raw `new` / `delete` | `std::unique_ptr`, `make_unique`, containers | 2,668 | 1,113 |
| `sprintf` / `snprintf` buffers | `std::format` / `std::format_to` | 1,529 | 354 |
| `printf` / `SPEW` logging | the `std::format` logging facade | 1,473 | 263 |
| Raw `MC2_*` environment reads | typed gate reads | 1,391 | 3 |
| Gate-name string literals | names checked against the gate table | 1,647 | 121 |
| `const char*` / `const std::string&` read-only parameters | `std::string_view` | 1,860 | 1,358 |
| `typedef` | `using` aliases | 815 | 54 |
| `.first` / `.second` | structured bindings | 366 | 225 |
| Unscoped `enum` | `enum class` | 334 | 203 |
| Win32 path APIs and backslash concatenation | `std::filesystem` | 289 | 213 |
| Manual min/max clamp chains | `std::clamp` | 141 | 37 |
| Out-params and sentinel returns | `std::optional` / `std::expected` | 519 | 455 |
| Manual 2D index math | `std::mdspan` | 181 | 130 |
| Pointer + count parameters | `std::span` | 243 | 216 |
| Type punning | `std::bit_cast` | 92 | 65 |

That is 24,017 sites retired across all 20 idioms, or 21,103 without the two gate idioms. A site counts as retired when it was rewritten, or when it was deleted along with dead code (never-compiled MLR files, unused Stuff I/O and containers, dead heap branches and similar).

The tree has grown far beyond the code it started from. Counting substantive lines of C and C++ (30 or more characters, matched against Microsoft's 2006 shared-source release and alariq's port), about 67% of today's lines are new in this project, about 31% are Microsoft's original game code still present verbatim, and about 2% are lines from alariq's port that are not in Microsoft's release. The Microsoft share is concentrated in the game logic (`code/` and `mclib/`, each about half original). The renderer, the content pipeline, the gate system, the platform layer (`GameOS/`, 92% new) and the test suites are almost entirely new. Line matching understates edits to existing lines, so these figures measure what was written fresh, not what was touched.

## Building

To run what you build you need the game data: the release zips, or an existing install.

### Windows

Prerequisites:

- Visual Studio 2022 Build Tools with the MSVC v143 workload
- The LunarG Vulkan SDK 1.4.350 or later (it provides `glslangValidator`, `spirv-cross` and `slangc`)
- Git with Git LFS. Run `git lfs install` once; without it, `3rdparty.zip` clones as a small pointer file.
- Python 3.10 or later

1. Clone the repo and extract `3rdparty.zip` at the repo root. You should get `3rdparty/{cmake,include,lib,tracy}`.
2. Build. **Ninja (recommended)** tracks every header, so it never leaves a stale binary behind:

   ```bat
   py -3 scripts/configure_ninja.py --no-compile-cache --build
   py -3 scripts/ninja_build.py
   ```

   Output: `build64-ninja/mc2.exe`.

   **Visual Studio generator**: use absolute paths.

   ```bat
   cmake -S . -B build64 -G "Visual Studio 17 2022" -A x64 -DMC2_IMGUI=ON -DMC2_VULKAN=ON ^
     -DCMAKE_PREFIX_PATH=C:/path/to/repo/3rdparty/cmake ^
     -DZLIB_INCLUDE_DIR=C:/path/to/repo/3rdparty/include ^
     -DZLIB_LIBRARY=C:/path/to/repo/3rdparty/lib/x64/zlib.lib
   cmake --build build64 --config RelWithDebInfo --target mc2 mc2_launcher
   ```

   Output: `build64/RelWithDebInfo/mc2.exe`. RelWithDebInfo is the supported configuration.
3. Deploy into a game install. The exe, shaders and payload must stay in lockstep, so never copy them by hand. Commit first: the exe embeds the git SHA it was built from.

   ```bat
   py -3 scripts/deploy_payload.py C:/Games/MC2R --target game --ninja --allow-new-target
   ```

   Leave out `--ninja` if you built with the Visual Studio generator.

[BUILD-WIN.md](BUILD-WIN.md) has more detail, including how to rebuild the third-party libraries by hand.

### Linux (native)

Build on a Linux filesystem (under WSL, use ext4, not `/mnt/c`). The tested toolchain is Clang/libc++ 20, lld, Vulkan-Headers 1.4.350, DXC 1.9 and Slang 2026.20. You also need the SDL2, SDL2_mixer, SDL2_ttf, zlib and FFmpeg development packages, plus CMake, Ninja, glslang-tools and spirv-cross.

```sh
cmake -S . -B build64-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  -DVulkan_INCLUDE_DIR="$HOME/toolchains/vulkan/include" \
  -DMC2_DXC="$HOME/toolchains/dxc/bin/dxc" \
  -DMC2_SLANGC="$HOME/toolchains/slang/bin/slangc"
ninja -C build64-ninja mc2
python3 scripts/deploy_payload.py "$INSTALL" --target game --ninja --allow-new-target --classic-assets
```

The launcher, the crash dialog and the dev shell are Windows-only. [docs/linux-build.md](docs/linux-build.md) covers the unit tests and the software-Vulkan (lavapipe) determinism run.

### Offline checks

None of these launch the game:

```bash
bash scripts/check-contracts.sh                       # static contract rows
py -3 tools/build_contract_harnesses.py --run --unit  # C++ harnesses + doctest
py -3 -B -m pytest scripts/tests/ -q -p no:cacheprovider
```

## Running

```bash
mc2-launcher.exe            # campaign/mod picker, Import, Engine Options, Graphics
mc2.exe                     # brings the launcher up first
mc2.exe -mission mc2_01     # skip the menus and load a mission directly
mc2.exe --classic-assets    # Classic building art for this run only (--remastered-assets is the reverse)
```

If you need to report a problem, `run-with-log.bat` writes `stderr.log` next to the exe. Multiplayer is compiled out for now.

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
| Laptop: i7-7700HQ, GeForce GTX 1050 Ti 4 GB (driver 582.78) | Windows, Vulkan | 720p | Low / Potato (same numbers within noise) | 181 fps average, 1% low 122 fps (CPU bound, GPU about 1 ms) | 101 fps average, 1% low 41 fps (CPU bound, GPU about 1.5 ms) | 2026-10-27 |
| Same laptop, Intel HD 630 (driver 31.0.101.2145) | Windows, Vulkan | 720p | Low / Potato | 88 fps average, 1% low 69 fps (both presets) | Low 54 fps average, 1% low 35 fps; Potato 57 fps average, 1% low 38 fps | 2026-10-27 |

Notes: the RTX 2070 SUPER High rows use the Remastered kit; Classic art measured the same in median cost, but Remastered has a heavier night tail (CPU p95 about 20 ms at 4K), so 60 fps is not guaranteed in large night battles. The Medium and Low rows are provisional (host was not fully idle) and show GPU time only. The RX 7900 XTX row is from an older build, kept for comparison.

To measure your own machine: `py -3 scripts/run_battle_bench.py` for frame times, or `py -3 scripts/run_smoke.py --lite --keep-logs` for a quick pass/fail run.

## Modding

The launcher imports community campaigns from an existing MC2, MC2X or MCO install. These fan mods are support targets, and an automated mod battery loads all six:

- MC2X-CVE-G
- MC2X-REAP3
- Exodus
- MagicExpansion
- MechCommander Omnitech
- MCO-Carver5

Bring your own copies: this project supports these mods but does not ship them. Your own mods, with their model, INI and shape overrides, take priority over the Remastered kit in both art modes.

- [docs/modding-guide.md](docs/modding-guide.md): start here
- `mods/`: four example mods ([magic-ballistic-weapons](mods/magic-ballistic-weapons), [magic-gamesys-tweaks](mods/magic-gamesys-tweaks), [modern-tree-pack-v1](mods/modern-tree-pack-v1), [my-explosions](mods/my-explosions))
- [docs/content-pipeline.md](docs/content-pipeline.md): importing or replacing models (IR, cooker, material schema, packages)
- [docs/modding-effects.md](docs/modding-effects.md) and [docs/modding-animations.md](docs/modding-animations.md): VFX and animation authoring
- [docs/modding-weapons.md](docs/modding-weapons.md) and [tools/mc2weapon/README.md](tools/mc2weapon/README.md): weapon data
- [docs/asset-pipeline.md](docs/asset-pipeline.md): asset inventory and import paths

## License

The tree combines code from several origins. [LICENSES.md](LICENSES.md) has the full provenance.

- Original game code and assets: Microsoft Shared Source Limited Permissive License ([EULA.txt](EULA.txt)). The art may only be distributed non-commercially, so this project and its releases are free.
- Platform-layer code inherited from alariq's port: GPLv3 ([license.txt](license.txt))
- New remaster code: MIT where marked
- Third-party libraries (SDL2, FFmpeg, Dear ImGui, glm, volk, VMA, Assimp, SMAA, Tracy and others): their own licenses, under `3rdparty/` and in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## Credits

- **Original game:** Microsoft and FASA Interactive (2001), and Microsoft for the shared-source release that makes all of this possible.
- **alariq** ([alariq/mc2](https://github.com/alariq/mc2)), **the patron saint of MechCommander 2, with our highest praise.** Microsoft released the game code but not GameOS, the platform layer it all runs on. alariq rebuilt that layer by hand from headers and the game's own expectations, on SDL and OpenGL, and carried Microsoft's 32-bit Windows-only code to 64-bit Windows and Linux (2016 to 2021). That is the work that took MechCommander 2 from a source dump to a game that builds and plays on modern machines. Every line of this remaster stands on that foundation. Most of the platform layer has since been rewritten for Vulkan (about 43% of its lines survive), but none of this exists without alariq's port. Code inherited from it remains GPLv3. Thank you, alariq.
- **Community creators:** [MechCommander Omnitech by magic](https://www.moddb.com/mods/mechcommander-omnitech) and [MC2X by Wolfman](https://mc2x.net/), who have kept this game alive for more than 20 years.
- **[Alexbeav](https://github.com/Alexbeav)** ([MechCommander2-Restoration-Project](https://github.com/Alexbeav/MechCommander2-Restoration-Project)): D3F font loader, multi-monitor mouse grab, SDL window-event dispatch fix, FMV reference.
- **[Methuselas](https://github.com/Methuselas):** Mission Editor and UI Editor.
- **Remaster and engine work:** ThranduilsRing.
- **Development:** built with [Claude Code](https://claude.ai/code) (Anthropic).
