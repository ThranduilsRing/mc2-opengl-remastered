# Codebase Structure

**Analysis Date:** 2026-05-14

## Directory Layout

```
mc2-opengl-src/                  # repo root
├── CMakeLists.txt               # top-level CMake; defines `mc2` target
├── CLAUDE.md                    # thin pointer to worktree CLAUDE.md
├── BUILD-WIN.md                 # Windows build instructions
├── README.md / EULA.txt / TODO  # project docs
├── MechCmd2.vcproj              # legacy VS project file (vestigial)
├── mc2res.cpp / mc2res.vcproj   # resource DLL source (strings, icons)
├── resource.h / strings.res.h   # resource IDs for mc2res
├── system.cfg                   # engine constants (loaded at startup)
│
├── code/                        # GAME layer — mission, mechs, GUI screens, MP
├── mclib/                       # ENGINE layer — terrain, textures, ABL, sound
│   ├── mlr/                     #   mesh renderer (templated mesh classes)
│   ├── gosfx/                   #   particle FX (clouds, debris, cards)
│   └── stuff/                   #   foundation: math, containers, streams
├── gui/                         # widget library (buttons/listboxes/edit)
├── GameOS/                      # PLATFORM layer — SDL2 + OpenGL back-end
│   ├── gameos/                  #   renderer, post-process, main loop, audio
│   │   └── utils/               #     shader builder, gl utils, image, vec
│   ├── include/                 #   public GOS API headers (gameos.hpp etc.)
│   └── src/                     #   POSIX shims for Win32 APIs
├── shaders/                     # GLSL 4.3 shaders (terrain, shadow, post)
│   └── include/                 #   shared GLSL headers (lighting/shadow)
├── Viewer/                      # standalone mech viewer tool (separate exe)
├── data_tools/                  # offline data tooling (.fst pack, .ase conv)
├── text_tool/                   # text-extraction tool (SDL2_ttf)
├── res/                         # resource compilation (CMake subdir)
├── gui/CMakeLists.txt etc.      # each subdir is a CMake module
│
├── data/                        # SHIPPING assets — textures used at runtime
├── mc2srcdata/                  # GIT SUBMODULE — game data (assets/missions)
├── 3rdparty/3rdparty/           # vendored SDL2, GLEW, ZLIB, etc.
├── cmake/ / cmake_overrides/    # CMake helpers (SDL2 finder, etc.)
├── build64/                     # CMake build output (RelWithDebInfo etc.)
├── deploy_*.py / upscale_*.py   # asset-pipeline scripts (peripheral)
├── docs/ / devdoc.txt / blog/   # documentation (mostly empty in root)
├── dump/ / misc/ / run/         # scratch / sample inputs / runtime data
└── .claude/worktrees/           # in-flight work-in-progress branches
                                 # (NOT part of the live codebase)
```

## Directory Purposes

### `code/` — game layer

- **Purpose:** Every gameplay class. The "front of house" — what the
  player experiences. This is the layer you modify for new mission types,
  new mech behaviours, new HUD widgets, new multiplayer features.
- **Contains:** ~150 `.cpp`/`.h` files. Mission orchestration, every
  unit/object type, every full-screen GUI (mainmenu, logistics, mechbay,
  mechlab, mechlopedia, missionselection, missionbriefing, missionresults,
  options, save/load, multiplayer setup), HUD overlays (control GUI,
  force group bar, tac map, info window, attribute meter, mech icon).
- **Key files:**
  - `code/mechcmd2.cpp` — entry callbacks (`InitializeGameEngine`,
    `DoGameLogic`, `UpdateRenderers`, `TerminateGameEngine`,
    `GetGameOSEnvironment`). 2770 lines.
  - `code/mission.cpp` + `mission2.cpp` — `Mission` class
    (init/update/render/destroy). 3549 lines.
  - `code/objmgr.cpp` — `ObjectManager` (3633 lines).
  - `code/mech.cpp` / `code/mover.cpp` — mech AI/state machine.
  - `code/gameobj.cpp` — `GameObject` base.
  - `code/gamecam.cpp` — `GameCamera` (oblique RTS).
  - `code/ablmc2.cpp` — bridges the ABL VM to game state.
  - `code/multplyr.cpp` / `mpdirecttcpip.cpp` — multiplayer session.
  - `code/saveload.cpp` — save game I/O.
  - `code/logistics*.cpp` / `mech*screen.cpp` / `mp*screen.cpp` —
    out-of-mission screens.
  - `code/d*.h` — small "data" headers (constants, IDs).
- **Conspicuously absent:** the GUI logistics screen for the broader
  framework lives in `gui/logisticsscreen.cpp` — straddles the boundary.

### `mclib/` — engine library

- **Purpose:** Reusable engine code. Everything that is not game-specific
  rules — terrain, textures, animation, ABL VM, file I/O, sound, low-level
  3D shape primitives.
- **Contains:** 193 files. The largest subsystem is `mech3d.cpp` (5139
  lines) which lives here because mech *appearance* (rendering/animation)
  is engine, while mech *behaviour* lives in `code/mech.cpp`.
- **Key files:**
  - `mclib/txmmgr.cpp` — `MC_TextureManager`. Master vertex/shape node
    arrays, `renderLists()` flush. 2147 lines. **Center of the renderer.**
  - `mclib/terrain.cpp` — `Terrain` class. Heightfield, quadList window,
    geometry CPU pass.
  - `mclib/terrtxm.cpp` / `terrtxm2.cpp` — terrain texture atlas + LOD.
  - `mclib/quad.cpp` — `TerrainQuad::draw()` / `drawWater()` /
    `drawMine()` — submits per-quad vertices to MC_TextureManager.
  - `mclib/mech3d.cpp` — `Mech3DAppearance` (articulated mech rendering).
  - `mclib/bdactor.cpp` — buildings.
  - `mclib/gvactor.cpp` — ground vehicles.
  - `mclib/genactor.cpp` — generic terrain objects.
  - `mclib/appear.cpp` / `apprtype.cpp` — appearance base + factory.
  - `mclib/camera.cpp` — `Camera` base class.
  - `mclib/tgl.cpp` / `tglpp.cpp` — TGL pool primitives.
  - `mclib/abl*.cpp` — ABL VM (scanner, parser, executor, symbol table,
    debugger). Standalone scripting language.
  - `mclib/move.cpp` / `paths.cpp` / `pqueue.cpp` — pathfinding.
  - `mclib/fastfile.cpp` / `ffile.cpp` / `file.cpp` — `.fst` archive I/O.
  - `mclib/csvfile.cpp` / `inifile.cpp` — config formats.
  - `mclib/soundsys.cpp` — sound subsystem.
  - `mclib/userinput.cpp` — input abstraction.
  - `mclib/heap.cpp` — `UserHeap` allocator.
  - `mclib/lz.h` / `lzcomp.cpp` / `lzdecomp.cpp` — LZ compression for
    fastfiles.
  - `mclib/d*.h` — interface headers for engine-side `D`-prefixed types.

#### `mclib/mlr/` — mesh renderer

- **Purpose:** Templated mesh rendering classes for every combination of
  features (Indexed / Indirect, Colored, Lit, Detail-textured,
  Multi-textured, Polygon-mesh vs Triangle-mesh).
- **Naming convention:** `mlr_i_<features>_<pmesh|tmesh>.cpp` —
  - `i` = indexed
  - `c` = colored, `l` = lit, `dt` = detail texture,
    `det` = detail multi-texture, `mt` = multi-textured
  - `pmesh` = polygon mesh, `tmesh` = triangle mesh
  - e.g. `mlr_i_c_det_pmesh.cpp` = indexed/colored/detail multi-tex/poly-mesh.
- **Key files:**
  - `mclib/mlr/mlr.cpp` / `mlr.hpp` — top-level entry.
  - `mclib/mlr/gosvertex.cpp` / `gosvertexpool.hpp` — `gos_VERTEX` pool.
  - `mclib/mlr/gosimage.cpp` / `gosimagepool.cpp` — image (texture) pool.
  - `mclib/mlr/mlr_terrain.cpp` / `mlr_terrain2.cpp` — terrain-specific
    mesh variants.

#### `mclib/gosfx/` — particle FX

- **Purpose:** Particle/effect system. Weapon trails, explosions, smoke,
  debris.
- **Key files:**
  - `effect.cpp` / `effectlibrary.cpp` — top-level effect registry.
  - `particlecloud.cpp` / `shapecloud.cpp` / `cardcloud.cpp` /
    `pointcloud.cpp` / `debriscloud.cpp` / `pertcloud.cpp` /
    `spinningcloud.cpp` / `shardcloud.cpp` — particle variants.
  - `card.cpp` / `shape.cpp` / `tube.cpp` — single emitters.
  - `pointlight.cpp` — light-emitting effects.
  - `fcurve.cpp` — animation curves.
  - `singleton.cpp` / `gosfxheaders.cpp` — bootstrap.

#### `mclib/stuff/` — foundation

- **Purpose:** Pre-STL math/container library. Pre-dates the OSS release.
  `Stuff::` namespace.
- **Key files:**
  - `vector3d.cpp` / `vector4d.cpp` / `point3d.cpp` / `normal.cpp` —
    vector math.
  - `matrix.cpp` / `linearmatrix.cpp` / `affinematrix.cpp` /
    `matrixstack.cpp` — matrix math.
  - `angle.cpp` / `motion.cpp` / `obb.cpp` — angles, motion, oriented
    bounding boxes.
  - `chain.cpp` / `hash.cpp` / `link.cpp` / `node.cpp` /
    `namelist.cpp` — intrusive containers.
  - `filestream.cpp` / `memorystream.cpp` / `notationfile.cpp` — I/O.
  - `*_test.cpp` files — unit tests (not compiled into main target).

### `GameOS/` — platform / renderer back-end

- **Purpose:** Every GL/SDL/OS call. Implements the GOS API
  (`gameos.hpp`) used by the rest of the codebase.
- **Subdirs:** `gameos/` (renderer + main loop), `include/` (headers),
  `src/` (POSIX shims).

#### `GameOS/gameos/` — implementation

- **Key files:**
  - `gameos/gameosmain.cpp` — `main()`. SDL window/context, GLEW init,
    OpenGL debug callback, frame loop (`draw_screen` →
    `Environment.DoGameLogic` + `UpdateRenderers`).
  - `gameos/gameos_graphics.cpp` — `gosRenderer` class (968+ lines of
    just the class). Render-state stack, draw dispatch, buffer/texture/
    material/font objects. 3166 lines total.
  - `gameos/gameos.cpp` — `Environment` global, memory heap stack,
    registry/user-data dir handling, system stubs.
  - `gameos/gos_render.cpp` — SDL2 window + GL context creation
    (`graphics::create_window`, `init_render_context`).
  - `gameos/gos_postprocess.cpp` — `gosPostProcess`. HDR scene FBO, bloom
    ping-pong, shadow map, FXAA, tonemap, procedural skybox.
  - `gameos/gameos_input.cpp` + `gos_input.cpp` — SDL → engine input
    translation.
  - `gameos/gameos_sound.cpp` — SDL2_mixer backend.
  - `gameos/gameos_fileio.cpp` — file I/O wrappers.
  - `gameos/gameos_res.cpp` — resource DLL (`mc2res`) loader.
  - `gameos/gameos_debugging.cpp` / `gameos_graphics_debug.cpp` —
    debug overlay / draw-call instrumentation.
  - `gameos/gos_font.cpp` — bitmap font renderer.
  - `gameos/gos_profiler.h` — Tracy zone macros (`ZoneScopedN`,
    `TracyGpuZone`).
  - `gameos/gos_validate.cpp` / `gos_validate.h` — debug validation
    layer (untracked in current branch).

#### `GameOS/gameos/utils/` — internal renderer utilities

- **Key files:**
  - `shader_builder.cpp` — `glsl_program`, `makeProgram(prefix, vs, fs)`.
    **Shader `#version` is passed as the prefix here**, never in shader
    files.
  - `gl_utils.cpp` — `CHECK_GL_ERROR`, FBO helpers.
  - `Image.cpp` — TGA/PNG load helpers.
  - `camera.cpp` — utility camera (separate from engine `Camera`).
  - `matrix.cpp` / `vec.cpp` — small math (separate from `Stuff::`).
  - `gl_render_constants.cpp` — GL state constant tables.

#### `GameOS/include/` — public GOS API

- **Key files:**
  - `gameos.hpp` — the GOS API surface. `gos_*` C-style functions every
    other layer calls.
  - `toolos.hpp` — additional tools API.
  - `texture manager.hpp` (with space — yes, really).
  - `platform_*.h` — POSIX/Windows compat layer (`platform_str.h`,
    `platform_windows.h`, etc.).
  - `pch.hpp` — precompiled header.

#### `GameOS/src/` — POSIX shims

- **Purpose:** Implementations of Win32 APIs for cross-platform builds.
  Built as the `windows` static library.
- **Files:** `platform_eh.cpp`, `platform_io.cpp`, `platform_mbstring.cpp`,
  `platform_mmsystem.cpp`, `platform_stdlib.cpp`, `platform_str.cpp`,
  `platform_tchar.cpp`, `platform_winbase.cpp`, `platform_winnls.cpp`,
  `platform_winuser.cpp`.

### `gui/` — widget library

- **Purpose:** Small immediate-mode-ish widget set. Used by every
  `code/*screen.cpp`.
- **Key files:**
  - `gui/aSystem.cpp` / `aSystem.h` — `aSystem` (widget tree root).
  - `gui/abutton.cpp` — `aButton`.
  - `gui/aedit.cpp` — `aEdit` (text input).
  - `gui/alistbox.cpp` — `aListbox`.
  - `gui/ascroll.cpp` — scroll bar.
  - `gui/aanim.cpp` / `aanimobject.cpp` — animated widgets.
  - `gui/afont.cpp` — font wrapper.
  - `gui/logisticsscreen.cpp` — base class for logistics screens (lives
    here, not in `code/`, by historical accident).

### `shaders/` — GLSL

- **Purpose:** All shaders. Loaded at runtime via
  `shader_builder.cpp::makeProgram(prefix, vs, fs)`. Prefix is always
  `"#version 430\n"`.
- **Naming:** `gos_*` = engine-pipeline shaders, `shadow_*` = shadow
  pass, `bloom_*` / `postprocess.*` / `skybox.*` = post pass.
- **Key files:**
  - `gos_terrain.vert/tesc/tese/frag` — terrain with tessellation
    control + evaluation (POM, splat, shadow sampling, distance LOD).
  - `gos_tex_vertex(_lighted).vert/frag` — generic textured shape.
  - `gos_vertex(_lighted).vert/frag` — generic colored shape.
  - `gos_text.vert/frag` — bitmap text.
  - `object_tex.vert/frag` — modern object pipeline.
  - `shadow_depth.vert/frag` — shadow depth pass for shapes.
  - `shadow_terrain.vert/frag` — shadow depth pass for terrain.
  - `postprocess.vert/frag` — composite + tonemap + FXAA.
  - `bloom_threshold.frag` / `bloom_blur.frag` — bloom ping-pong.
  - `skybox.vert/frag` — procedural skybox.

#### `shaders/include/` — shared GLSL headers

- **Files:**
  - `lighting.hglsl` — light evaluation helpers.
  - `scene.hglsl` — `TG_HWSceneData` UBO layout (must match C++ struct in
    `txmmgr.cpp` — load-bearing lockstep, see
    `memory/cpp_glsl_ubo_struct_lockstep.md`).
  - `shadow.hglsl` — `calcShadow()` with variable-tap Poisson PCF.
  - `terrain_common.hglsl` — shared terrain helpers.

### `Viewer/` — standalone mech viewer

- **Purpose:** Separate executable for viewing/inspecting mech models.
  Has its own `CMakeLists.txt`.
- **Files:** `View.cpp`, `mission.fst` (sample input).

### `data_tools/` — offline asset tooling

- **Purpose:** Offline tools, not used at runtime.
- **Files:**
  - `aseconv.cpp` — .ASE (3ds Max ASCII Scene Export) converter.
  - `makefst.cpp` — pack assets into `.fst` archives.
  - `makersp.cpp` — make resource-string package.
  - `pak.cpp` — generic packer.

### `text_tool/` — text extraction

- **Purpose:** SDL2_ttf-based tool. CMake subdir.

### `data/` — runtime art assets

- **Subdirs:** `textures/` — shipping textures (upscaled / sidecars).
- **Used by:** the engine via `FastFile` overlay + loose-file fallback.

### `mc2srcdata/` — game data (submodule)

- **Purpose:** Git submodule containing campaign missions, mech CSVs,
  textures, audio, ABL scripts. Not part of the engine.
- **Treat as:** read-only data — do not map its internals as engine code.

### `3rdparty/3rdparty/` — vendored dependencies

- **Subdirs:** `cmake/`, `include/`, `lib/`. Holds SDL2, SDL2_mixer,
  SDL2_ttf, GLEW (pre-built). Picked up by `find_package` via
  `CMAKE_PREFIX_PATH`.

### `cmake/` / `cmake_overrides/`

- CMake helper modules (FFmpeg finder, build fingerprint, narrowing errors, standalone target, PCH). SDL2 comes from vcpkg; the vendored `cmake/sdl2` module tree was removed 2026-10-12.

### `build64/`

- CMake build directory. Always configure `--config RelWithDebInfo`
  (per worktree CLAUDE.md: Release crashes with `GL_INVALID_ENUM`).

### `docs/` / `devdoc.txt` / `blog/`

- Root `docs/` is empty on this branch. Authoritative docs live in the
  worktree (`.claude/worktrees/nifty-mendeleev/docs/`).

### `dump/` / `misc/` / `run/`

- Scratch directories: log dumps, sample inputs, runtime working dir.

### `__pycache__/` / `esrgan_*/` / `realesrgan*/` / `release_assets/`

- Python-script byproducts and upscaler model weights. **Not part of the
  C++ build.** Asset pipeline only.

### `.claude/`

- `.claude/skills/` — Claude Code skills (e.g. `mc2-build`, `mc2-deploy`).
- `.claude/worktrees/<branch>/` — git worktrees for in-flight branches.
  The **active development branch** lives at
  `.claude/worktrees/nifty-mendeleev/`. **Do not treat this as part of
  the live codebase** for mapping purposes — it is a working-copy of the
  whole repo under a feature branch.

## Key File Locations

**Entry Points:**
- `GameOS/gameos/gameosmain.cpp:282` — `main()`.
- `code/mechcmd2.cpp:825` — `InitializeGameEngine`.
- `code/mechcmd2.cpp:2024` — `DoGameLogic` (per-frame).
- `code/mechcmd2.cpp:676` — `UpdateRenderers` (per-frame render submit).
- `code/mechcmd2.cpp:1803` — `TerminateGameEngine`.

**Configuration:**
- `CMakeLists.txt` (root) — top-level build config; defines `LINUX_BUILD`
  globally.
- `system.cfg` — engine constants at install root.
- `<user>/.config/.mechcommander2/options.cfg` — runtime options.

**Renderer Core:**
- `GameOS/gameos/gameos_graphics.cpp` — `gosRenderer`.
- `GameOS/gameos/gos_postprocess.cpp` — FBOs, bloom, shadow, FXAA, sky.
- `mclib/txmmgr.cpp:910` — `renderLists()` flush.

**Per-frame hot paths:**
- `code/mission.cpp:318` — `Mission::update`.
- `code/mission.cpp:756` — `Mission::render`.
- `mclib/terrain.cpp:860` — `Terrain::render`.
- `mclib/terrain.cpp:943` — `Terrain::geometry`.

**Testing:**
- `mclib/stuff/*_test.cpp` — unit tests for foundation math (e.g.
  `affinematrix_test.cpp`, `matrix_test.cpp`, `hash_test.cpp`,
  `memoryblock_test.cpp`, `notationfile_test.cpp`). Not built into
  `mc2.exe`.
- No higher-level test harness in-tree. Runtime smoke is the
  `mc2-check` / `mc2-deploy` skill flow.

## Naming Conventions

**Files (C++):**
- `lowercasename.cpp` / `lowercasename.h` — engine + game source.
- `d<name>.h` — "declarations" header (forward decls + constants for
  the matching `<name>.cpp`).
- `<feature>_<variant>.cpp` — variant explosion (e.g. `mlr/`).
- `*_test.cpp` — unit test (not compiled into `mc2`).

**Files (shaders):**
- `gos_<thing>.{vert,frag,tesc,tese}` — pipeline shader.
- `<feature>.{vert,frag}` — post-process / utility shader.
- `<feature>.hglsl` — shared GLSL include.

**Classes:**
- Engine classes: PascalCase (`Mission`, `Terrain`, `Camera`,
  `MC_TextureManager`, `TG_Shape`, `Mech3DAppearance`).
- Pre-STL containers: `Stuff::` namespace, snake_case + PascalCase mix
  (`Stuff::Vector3D`, `Stuff::Matrix4x4`, `Stuff::FileStream`).
- GUI widgets: lowercase `a` prefix (`aButton`, `aListbox`, `aEdit`,
  `aSystem`).

**Functions:**
- `gos_<verb><Noun>` — GOS API (`gos_RenderIndexedArray`,
  `gos_SetRenderState`, `gos_GetTextureHandle`).
- Engine member functions: camelCase (`update`, `render`, `geometry`,
  `clearArrays`).
- Old DX-era functions: PascalCase (`InitializeGameEngine`,
  `TerminateGameEngine`, `UpdateRenderers`, `DoGameLogic`,
  `TransformMultiShape`).

**Globals (load-bearing singletons):**
- `eye` — Camera.
- `land` — Terrain.
- `mcTextureManager` — MC_TextureManager.
- `ObjectManager` — ObjectManager singleton.
- `userInput` — UserInput.
- `soundSystem` — SoundSystem.
- `PathManager` / `SensorManager` / `craterManager` / `weather`.
- `MPlayer` — multiplayer session (NULL when SP).
- `Commander::home` / `Team::home` — local player anchors.
- `Environment` — GOS environment struct (callbacks + screen size).

**Env vars:**
- `MC2_*` — modernization + instrumentation knobs. See worktree
  CLAUDE.md "Tier-1 Instrumentation Env Vars" section.

## Where to Add New Code

**New gameplay feature (object type, weapon, AI behaviour):**
- Class: `code/<feature>.cpp` / `<feature>.h`.
- Add the `.cpp` to `SOURCES` in the root `CMakeLists.txt` (lines
  91–181).
- If it's a new `GameObject` subclass, register it in `objtype.cpp` /
  `objmgr.cpp` so `ObjectManager` can spawn/iterate it.

**New full-screen GUI:**
- Class: `code/<name>screen.cpp`/`.h`.
- Use widgets from `gui/`.
- Add to `SOURCES` in `CMakeLists.txt`.
- Wire into `Logistics` state machine or `DoGameLogic`'s screen
  dispatcher.

**New HUD widget:**
- Class: `code/<name>.cpp`. Tie into `MissionInterface` (see
  `code/missiongui.cpp`).

**New mech / vehicle / building appearance variant:**
- Modify or subclass `Mech3DAppearance` (`mclib/mech3d.cpp`),
  `GVAppearance` (`gvactor.cpp`), or `BdgAppearance` (`bdactor.cpp`).
- Honor the cull-gate contract — read
  `memory/cull_gates_are_load_bearing.md` first.

**New shader:**
- File: `shaders/<name>.{vert,frag}` (+ `.hglsl` if shared).
- Load via `makeProgram(\"#version 430\\n\", ...)` from
  `gameos_graphics.cpp` or `gos_postprocess.cpp`.
- Never put `#version` in the file itself.
- For UBO structs, keep C++ and GLSL in lockstep — see
  `memory/cpp_glsl_ubo_struct_lockstep.md`.

**New post-process pass:**
- Add to `gosPostProcess` in `GameOS/gameos/gos_postprocess.cpp` (FBO
  + shader pair).
- Toggle via `bloomEnabled_`-style bool + `RAlt+F<N>` keybind in
  `gameosmain.cpp:37` `handle_key_down`.

**New engine subsystem (terrain feature, audio effect, file format):**
- File: `mclib/<feature>.cpp`/`.h`.
- Add to `mclib/CMakeLists.txt`.
- Init from `InitializeGameEngine` in `code/mechcmd2.cpp:825`. Note
  the constraint: **widget-loading subsystems must init BEFORE
  `InitializeGameEngine` runs widget queries** (see
  `memory/mc2_init_order_widgets_before_subsystems.md`).

**New GOS API function:**
- Declare in `GameOS/include/gameos.hpp`.
- Implement in `GameOS/gameos/gameos_graphics.cpp` (or the appropriate
  `gameos_*.cpp`).

**New low-level renderer change (state cache, draw path):**
- Most edits land in `mclib/txmmgr.cpp` (queue layer) or
  `GameOS/gameos/gameos_graphics.cpp` (GL state layer).
- Read the "GPU-direct renderer bring-up checklist"
  (`memory/gpu_direct_renderer_bringup_checklist.md`) BEFORE writing a
  fast path that bypasses `mcTextureManager`.

**New particle effect:**
- Subclass one of `mclib/gosfx/*cloud.cpp` (`particlecloud`,
  `shapecloud`, `cardcloud`, etc.) and register with `EffectLibrary`.

**New ABL builtin:**
- `mclib/abl*std.cpp` (`ablstd.cpp`, `ablxstd.cpp`). Bridge from game
  state in `code/ablmc2.cpp`.

**New unit test for foundation math:**
- `mclib/stuff/<feature>_test.cpp`. These are standalone — they are not
  linked into `mc2.exe`.

## Special Directories

**`build64/`:**
- Purpose: CMake build output.
- Generated: Yes.
- Committed: No.
- Configs: `RelWithDebInfo` is the canonical one (worktree CLAUDE.md
  rule: Release crashes with `GL_INVALID_ENUM`).

**`.claude/worktrees/`:**
- Purpose: Git worktrees for in-flight feature branches. Currently ~25
  WIP branches live here (`bindless-textures`, `gpu-driven-rendering`,
  `pre-bake-terrain`, `mech-skinning-import`, etc.). The active one is
  `nifty-mendeleev`.
- Generated: Yes (by `git worktree add`).
- Committed: No.
- **Mapping rule:** do NOT treat as part of the live codebase. Each
  worktree is a full repo checkout on a different branch.

**`mc2srcdata/`:**
- Purpose: Game assets (campaigns, missions, mech CSVs, sounds, art).
- Generated: No.
- Committed: As a git submodule.
- **Mapping rule:** data, not code. Do not map internals.

**`__pycache__/`, `esrgan_models/`, `esrgan_out/`, `esrgan_in/`,
`realesrgan-ncnn-vulkan/`, `release_assets/`:**
- Purpose: Asset upscaling pipeline (Real-ESRGAN, Stable Diffusion
  terrain upscale). Driven by `upscale_*.py` / `deploy_*.py` /
  `pack_mat_normal.py`. Outputs land in `data/textures/` as 4× upscaled
  sidecar TGAs.
- Generated: Yes (`esrgan_out`, `__pycache__`).
- Committed: Mixed (models yes, outputs no).
- **Mapping rule:** peripheral. Not part of the C++ engine.

**`dump/`, `misc/`, `run/`:**
- Scratch / log / sample-input directories. Not load-bearing.

---

*Structure analysis: 2026-05-14*
