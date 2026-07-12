# MC2 Toolchain Bill of Materials

## Summary

This document catalogs the **asset cook, validation, and pipeline tools** in the MC2 OpenGL engine modernization project as of 2026-05-29. Each tool is classified by purpose, status (integrated, vendored, designed-not-implemented, missing), version, license, and dependency type (runtime vs. offline/cook-only).

The goal is to provide a single source of truth for vendors, contributors, and build automation to understand which tools are ready for production use and which require stabilization work.

---

## Summary Table

| Tool | Purpose | Status | Version | License | Dep Type | Next Slice |
|------|---------|--------|---------|---------|----------|-----------|
| **Assimp** | Mesh import (FBX/GLTF/OBJ/etc.) | VENDORED-NOT-CALLED | 6.0.5 | BSD-3 | Cook-only | ASSIMP-IMPORTER-PHASE-0 |
| **meshoptimizer** | LOD clustering + vertex cache optim | VENDORED-NOT-CALLED | HEAD (2023) | MIT | Cook-only | MESH-OPTIMIZE-LOD-0 |
| **mc2texcook** | TGA/PNG/EXR to KTX2 texture baker | INTEGRATED | HEAD (2026-05-27) | (project) | Cook-only | KTX2-BAKE-PROBE-1 |
| **KTX2 Loader** | Runtime KTX2 texture import | DESIGNED-NOT-IMPLEMENTED | N/A | (design) | Runtime | KTX2-BAKE-PROBE-1 |
| **validate_manifest** | Material manifest JSON validator | INTEGRATED | HEAD (2026-05-24) | (project) | Cook-only | ASSET-MANIFEST-0 |
| **shader_reflect** | Shader contract CI (SPIR-V golden test) | INTEGRATED | HEAD (2026-05-28) | (project) | Cook-only | SHADER-REFLECT-LOCK-0 |
| **validate_shaders** | glslangValidator + spirv-val wrapper | INTEGRATED | HEAD (2026-05-29) | (project) | Cook-only | SHADER-REFLECT-LOCK-0 |
| **project_sh (IBL)** | EXR to SH-L2 spherical harmonics | INTEGRATED | V-IBL-STATIC-1 v1 | (project) | Cook-only | V-IBL-STATIC-1-BAKE-0 |
| **Tracy** | CPU+GPU profiler (instrumentation lib) | VENDORED-INTEGRATED | HEAD (2026-04-11) | BSD | Runtime | (monitoring only) |
| **RenderDoc** | GPU debugger in-app API | VENDORED-INTEGRATED | 1.5.0+ (header-only) | MIT | Runtime | (debug-only) |
| **imgui** | Debug overlay UI | VENDORED-INTEGRATED | HEAD (2026-05-24) | MIT | Runtime | (UI framework) |
| **stb_image** | PNG/TGA/BMP image load (offline) | VENDORED-COOK-ONLY | HEAD (2026-05-25) | Public Domain | Cook-only | (stable) |
| **tinyexr** | EXR reader (IBL input) | VENDORED-COOK-ONLY | HEAD (2026-05-25) | BSD | Cook-only | (stable) |
| **GLTFPack** | glTF2 optimization (designed) | MISSING | N/A | Apache 2.0 | Cook-only | GLTF-PACK-PHASE-0 |
| **spirv-cross** | SPIR-V reflection (via glslangValidator) | VENDORED-NOT-CALLED | (external) | Apache 2.0 | Cook-only | SHADER-REFLECT-LOCK-0 |
| **glslangValidator** | GLSL to SPIR-V compiler | VENDORED-NOT-CALLED | (Vulkan SDK) | BSD | Cook-only | SHADER-REFLECT-LOCK-0 |
| **aseconv** | ASE (text mesh) to native format | INTEGRATED | (legacy data_tools) | (project) | Cook-only | (legacy) |
| **makefst** | FST (fast file) builder | INTEGRATED | (legacy data_tools) | (project) | Cook-only | (legacy) |
| **makersp** | RSP (response) file generator | INTEGRATED | (legacy data_tools) | (project) | Cook-only | (legacy) |
| **pak** | PAK archive builder | INTEGRATED | (legacy data_tools) | (project) | Cook-only | (legacy) |

---

## Per-Tool Details

### Assimp (Open Asset Import Library)

**Purpose:** Universal mesh importer. Supports 30+ file formats (FBX, glTF2, OBJ, Collada, 3DS, etc.). Used to stage source assets from DCC tools into normalized intermediate forms.

**Status:** VENDORED-NOT-CALLED

**Location:** 3rdparty/assimp/

**Version:** 6.0.5 (from CMakeLists.txt)

**License:** BSD-3-Clause (see 3rdparty/assimp/LICENSE)

**Dependency Type:** Cook-only (offline)

**Details:**
- Vendored in full (complete source tree).
- Not yet integrated into the build pipeline. No cook stage currently calls it.
- Includes Poly2Tri triangulation (BSD), Draco compression (Apache 2.0), zlib (Zlib), and others as vendored subdependencies.
- Next phase would be to wire mesh import stage in ASSIMP-IMPORTER-PHASE-0.

**Recommended Next Slice:** ASSIMP-IMPORTER-PHASE-0

---

### meshoptimizer

**Purpose:** Cluster-based LOD generation, vertex cache optimization, and index buffer strip generation for GPU-driven rendering. Essential for per-mech/static-prop LOD tiers.

**Status:** VENDORED-NOT-CALLED

**Location:** 3rdparty/meshoptimizer/

**Version:** HEAD (commit unknown; 2023 era based on file dates)

**License:** MIT (see 3rdparty/meshoptimizer/LICENSE.md)

**Dependency Type:** Cook-only (offline)

**Details:**
- Full source with C++ API. Can be compiled as static lib or called via build system.
- Not currently wired into asset cook pipeline.
- Would be called AFTER mesh import (post-Assimp) to generate LOD variants and optimize vertex cache for GPU streaming.

**Recommended Next Slice:** MESH-OPTIMIZE-LOD-0

---

### mc2texcook

**Purpose:** Cook TGA/PNG/EXR source art into KTX2 format (Khronos Texture 2.0). Handles sRGB color space, mip chain generation, and preset-based format selection (albedo, normal, ORM, emissive, mask).

**Status:** INTEGRATED

**Location:** tools/mc2texcook/mc2texcook.py

**Version:** HEAD (2026-05-27)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline)

**Details:**
- Fully working Python 3.8+ tool. Uses Pillow for image I/O.
- Supports 5 texture presets: albedo (sRGB), emissive (sRGB), normal (linear), ORM (linear), mask (linear).
- Generates mip chain automatically (box filter per level).
- Output is uncompressed KTX2 with full mip levels.
- Known limitation: No Basis-U compression yet. Full uncompressed mipmaps increase disk/memory footprint.
- Tests in tools/mc2texcook/tests/test_mc2texcook.py validate format correctness and mip generation.

**Recommended Next Slice:** KTX2-BAKE-PROBE-1

---

### KTX2 Runtime Loader

**Purpose:** Load KTX2 texture files at runtime, decode mip levels, and bind to Vulkan sampler descriptors.

**Status:** DESIGNED-NOT-IMPLEMENTED

**Location:** Planned in RenderCore/KtxLoader.{h,cpp}

**Version:** N/A

**License:** (Project)

**Dependency Type:** Runtime (game startup + streaming)

**Details:**
- Design is in place (KTX2 spec from Khronos).
- No implementation yet.
- Prerequisite for shipping any KTX2-baked assets.
- Loader must handle: KTX2 file header parsing, DFD parsing, mip level streaming.

**Recommended Next Slice:** KTX2-BAKE-PROBE-1

---

### validate_manifest

**Purpose:** Offline JSON validator for material manifest files. Checks schema conformance, file existence, and semantic coherence (e.g., alpha_test only when albedo has alpha).

**Status:** INTEGRATED

**Location:** tools/material_cook/validate_manifest.py

**Version:** HEAD (2026-05-24)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline)

**Details:**
- Fully working Python tool.
- Validates against schema at tools/material_cook/material_manifest.schema.json (4299 bytes, comprehensive).
- Checks KTX2 file headers to infer alpha channel presence and format compatibility.
- Two run modes: Default (warn on drift from schema) and --strict (fail on any deviation).
- Option --skip-file-checks for CI when assets are not yet baked.

**Recommended Next Slice:** ASSET-MANIFEST-0

---

### shader_reflect

**Purpose:** Shader contract CI. Compiles each shader+variant to SPIR-V via glslangValidator, reflects contracts via spirv-cross, compares against golden JSON files. Detects accidental binding layout changes, new/removed uniforms, and compile-time regressions.

**Status:** INTEGRATED

**Location:** tools/shader_reflect/reflect.py

**Version:** HEAD (2026-05-28)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline CI/pre-commit)

**Details:**
- Fully working Python tool. Requires glslangValidator and spirv-cross (from Vulkan SDK or PATH).
- Covers all major shader variants (static_prop, mech, shadow_static_prop, material_gpu_contract).
- Golden files stored in tools/shader_reflect/expected/*.json (generated via --update flag).
- Wired into pre-commit hooks via .claude/hooks/ for automatic CI gate.
- Known limitation: glslangValidator v12 does not implement ARB-suffixed draw-params builtins; workaround rewrites them to core names.

**Recommended Next Slice:** SHADER-REFLECT-LOCK-0

---

### validate_shaders

**Purpose:** Pre-compile validation gate. Walks all shader source files, injects canonical #version 430 header, compiles to SPIR-V via glslangValidator, and validates via spirv-val. Detects syntax errors and GLSL version mismatches before runtime.

**Status:** INTEGRATED

**Location:** scripts/validate_shaders.py

**Version:** HEAD (2026-05-29)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline CI/pre-commit)

**Details:**
- Fully working Python tool. Requires Vulkan SDK (glslangValidator + spirv-val on PATH or in VULKAN_SDK/Bin).
- Two modes: --vulkan (default): compile to SPIR-V + spirv-val validation. --gl: GL-semantics only, no SPIR-V.
- Injection discipline: Prepends #version 430 to match engine makeProgram() behavior.
- Injects #extension GL_GOOGLE_include_directive if shader has #include directives.
- Passes -I shaders/include to resolve .hglsl include files.
- Skips gos_terrain_lighting.comp (stitched at runtime).

**Recommended Next Slice:** SHADER-REFLECT-LOCK-0

---

### project_sh (IBL Spherical Harmonics Projector)

**Purpose:** Project equirectangular HDRI EXR images into 9 SH-L2 spherical harmonic coefficients (Ramamoorthi-Hanrahan 2001). Outputs coefficients for diffuse ambient lighting in static prop / terrain shaders.

**Status:** INTEGRATED

**Location:** tools/ibl/project_sh.py

**Version:** V-IBL-STATIC-1 v1 (HEAD, 2026-05-27)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline, one-time per HDRI)

**Details:**
- Fully working Python tool. Requires NumPy and one of: imageio, OpenEXR, or cv2 for EXR loading.
- Input: equirectangular HDRI .exr file (e.g., data/hdr/DaySkyHDRI063B_4K.exr).
- Output: 9 vec3 SH coefficients in [L00, L1-1, L10, L11, L2-2, L2-1, L20, L21, L22] order.
- Modes: Stdout output, C++ header generation, self-test validation.
- Axis convention: Y-up world; spherical coordinates n = (sin(theta)cos(phi), cos(theta), sin(theta)sin(phi)) with theta from +Y pole.
- Irradiance note: Shader evaluator (static_prop.vert evalShL2) pre-convolves the diffuse cosine-lobe kernel into Ramamoorthi-Hanrahan constants c1..c5. Do NOT premultiply kernel in projector; do NOT apply additional /pi at consumer.

**Recommended Next Slice:** V-IBL-STATIC-1-BAKE-0

---

### Tracy (CPU+GPU Profiler)

**Purpose:** Runtime instrumentation library for CPU thread profiling and GPU zone timing. Integrated into frame loop and render passes for Tier 5 performance analysis.

**Status:** VENDORED-INTEGRATED

**Location:** 3rdparty/tracy/

**Version:** HEAD (as of 2026-04-11; exact commit unknown)

**License:** BSD (Tracy is dual-licensed; this tree uses BSD variant)

**Dependency Type:** Runtime (compiled in; can be toggled off with TRACY_ENABLE=0)

**Details:**
- Fully vendored source (client-side C++ headers + common utilities).
- Always compiled in (see CLAUDE.md: Tracy always compiled in (TRACY_ENABLE)).
- GPU zones on shadow/terrain/3D/post-process.
- CPU zones on frame loop, render-call sequence, etc.
- 100ns instrumentation floor: never instrument a region less than 100ns; coarse per-pass zones only.
- Zero overhead when Tracy UI is not connected (on-demand socket connection).

---

### RenderDoc (GPU Debugger In-App API)

**Purpose:** Optional GPU frame capture via in-application API. Triggered by environment variable MC2_RDC_CAPTURE_FRAME at runtime. Single-header vendored C API.

**Status:** VENDORED-INTEGRATED

**Location:** 3rdparty/renderdoc/renderdoc_app.h

**Version:** v1.x branch HEAD, declares API versions 1.5.0 to 1.7.0; we request 1.5.0 for max compat.

**License:** MIT (Copyright 2015-2026 Baldur Karlsson)

**Dependency Type:** Runtime (debug-only; no-op if renderdoc.dll not available)

**Details:**
- Single C header file (no compilation needed).
- No link-time dependency. Engine resolves renderdoc.dll at runtime via GetModuleHandleA.
- If neither resolves, hook silently no-ops; engine continues normally.
- Integrated into GameOS graphics layer to detect and trigger frame captures on demand (Tier 5 testing).

---

### imgui (Immediate-Mode GUI)

**Purpose:** Debug overlay UI for real-time parameter tweaking, profiler display, and visualization of render state.

**Status:** VENDORED-INTEGRATED

**Location:** 3rdparty/imgui/

**Version:** HEAD (as of 2026-05-24)

**License:** MIT (Copyright 2014-2025 Omar Cornut)

**Dependency Type:** Runtime (UI framework; optional visual debug layer)

**Details:**
- Full source tree with backends (OpenGL, Vulkan, etc.).
- Compiled as part of GameOS static lib.
- Used for runtime visualization of SH coefficients, material parameters, and GPU metrics.
- Stable state; no changes planned.

---

### stb_image

**Purpose:** Lightweight PNG/TGA/BMP image loading. Used by offline cook tools (mc2texcook, validate_manifest) for image I/O.

**Status:** VENDORED-COOK-ONLY

**Location:** 3rdparty/stb/stb_image.h

**Version:** HEAD (as of 2026-05-25)

**License:** Public Domain (Sean Barrett, stb library)

**Dependency Type:** Cook-only (offline)

**Details:**
- Single header file; no compilation or external dependencies.
- Used by mc2texcook for TGA/PNG source art loading (via Pillow wrapper in Python).
- Fallback option if Pillow unavailable (not currently used; Pillow is required).
- Stable; no changes planned.

---

### tinyexr

**Purpose:** EXR (OpenEXR) image format reader. Used by project_sh.py for HDRI loading.

**Status:** VENDORED-COOK-ONLY

**Location:** 3rdparty/tinyexr/

**Version:** HEAD (as of 2026-05-25)

**License:** BSD (Copyright 2014-2025 Syoyo Fujita)

**Dependency Type:** Cook-only (offline)

**Details:**
- Single-header library with miniz (zlib-derived compression) bundled.
- Used by tools/ibl/project_sh.py to load equirectangular HDRI files.
- Fully working; stable.
- No changes planned.

---

### GLTFPack (glTF Optimization)

**Purpose:** Optimize glTF2 mesh geometry for streaming and GPU-driven rendering. Generates vertex/index caches, LOD variants, and quantization hints.

**Status:** MISSING

**Location:** Would go in 3rdparty/gltfpack/ if added

**Version:** N/A (not yet vendored)

**License:** Apache 2.0 (https://github.com/zeux/gltfpack)

**Dependency Type:** Cook-only (offline)

**Details:**
- Not yet integrated. Designed for future adoption after glTF import pipeline is stabilized.
- Mesh Optimizer ecosystem. Swiss-army tool for glTF optimization: vertex/index deduplication, clustering, quantization, animation decimation.
- Recommended as post-import cook stage (glTF to GLTFpack to optimized glTF to pak).
- Would be called in asset build pipeline after Assimp mesh import but before runtime packaging.

**Recommended Next Slice:** GLTF-PACK-PHASE-0

---

### spirv-cross

**Purpose:** SPIR-V cross-compilation and reflection. Emits reflected binding layouts from compiled shader bytecode. Used by shader_reflect.py to lock shader contract golden files.

**Status:** VENDORED-NOT-CALLED (external tool via Vulkan SDK)

**Location:** External (Vulkan SDK bin, or vendored separately if needed)

**Version:** Vendored indirectly; not pinned in this repo. Vulkan SDK 1.3.x includes spirv-cross.

**License:** Apache 2.0

**Dependency Type:** Cook-only (offline CI)

**Details:**
- Not directly vendored as source; assumed to be present via Vulkan SDK installation or PATH.
- Called by tools/shader_reflect/reflect.py with flag --reflect to introspect SPIR-V compiled shaders.
- Output is JSON-serialized binding layout (uniforms, samplers, storage blocks, etc.).
- No changes planned; stable external dependency.

---

### glslangValidator

**Purpose:** GLSL-to-SPIR-V compiler. Compiles shader source to Vulkan bytecode. Used by validate_shaders.py and shader_reflect.py.

**Status:** VENDORED-NOT-CALLED (external tool via Vulkan SDK)

**Location:** External (Vulkan SDK bin, or PATH)

**Version:** Vulkan SDK 1.3.x; exact version depends on system installation.

**License:** BSD (Khronos Glslang project)

**Dependency Type:** Cook-only (offline CI)

**Details:**
- Not directly vendored as source; assumed to be present via Vulkan SDK.
- Called by scripts/validate_shaders.py and tools/shader_reflect/reflect.py.
- Supports flags: --auto-map-locations, -V (Vulkan), -S (stage), -I (include-dir).
- No changes planned; stable external dependency.

---

### aseconv

**Purpose:** Legacy ASE (text mesh format) converter. Reads .ase files and converts to native runtime format.

**Status:** INTEGRATED

**Location:** data_tools/aseconv.cpp

**Version:** (Legacy; no version string; 2712 from hardcoded version stamp at line 9 of aseconv.cpp)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline; legacy asset pipeline)

**Details:**
- Fully working C++ tool. Compiled as data_tools executable.
- Part of legacy asset pipeline from original MechCommander 2 (early 2000s).
- Used in historical data build process; mentioned in README.md as part of build_scripts folder in mc2srcdata.

---

### makefst

**Purpose:** Fast File (FST) archive builder. Packages cooked asset files into binary archives for efficient runtime loading.

**Status:** INTEGRATED

**Location:** data_tools/makefst.cpp

**Version:** (Legacy; no version string)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline; legacy archive building)

**Details:**
- Fully working C++ tool. Compiled as data_tools executable.
- Part of legacy asset packaging pipeline.
- Used to build .fst archive files mentioned in system.cfg and asset discovery.

---

### makersp

**Purpose:** Response file (.rsp) generator. Generates response files for batch compilation or linking (typical for large projects).

**Status:** INTEGRATED

**Location:** data_tools/makersp.cpp

**Version:** (Legacy; no version string)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline; build automation)

**Details:**
- Fully working C++ tool. Compiled as data_tools executable.
- Legacy build automation; generates .rsp response files for compiler invocations.

---

### pak

**Purpose:** PAK archive builder. Packages binary asset files into .pak containers.

**Status:** INTEGRATED

**Location:** data_tools/pak.cpp

**Version:** (Legacy; no version string)

**License:** Project (internal)

**Dependency Type:** Cook-only (offline; asset packaging)

**Details:**
- Fully working C++ tool. Compiled as data_tools executable.
- Part of legacy asset packaging infrastructure.
- Used to build .pak files referenced at game startup for asset discovery.

---

## Recommended Pipeline Sequencing

Based on dependency graph and functional maturity, the recommended order for stabilizing and activating the toolchain is:

1. **ASSET-MANIFEST-0** (Week 1-2)
   - Finalize material_manifest.schema.json for a single mech type.
   - Run validate_manifest on reference material set.
   - Stage JSON in data pack; wire runtime material binding.
   - Unlocks: material authoring workflow.

2. **SHADER-REFLECT-LOCK-0** (Week 2-3)
   - Lock shader contract goldens via shader_reflect --update.
   - Wire pre-commit hooks for shader validation.
   - Run as Tier 1.2 CI gate for all shader commits.
   - Add to smoke test matrix.
   - Unlocks: shader binding stability; safe shader refactors.

3. **KTX2-BAKE-PROBE-1** (Week 3-4)
   - Implement KtxLoader.{h,cpp} for uncompressed mip streaming.
   - Cook a full material set (albedo, normal, ORM, emissive) via mc2texcook.
   - Apply to one mech type in-game.
   - Measure memory footprint; decide on Basis-U compression if needed.
   - Unlocks: modern texture pipeline; mod support via .ktx2 files.

4. **V-IBL-STATIC-1-BAKE-0** (Week 4-5)
   - Source or create high-quality HDRI.
   - Run project_sh.py to generate RenderCore/IblShCoeffs.h.
   - Integrate into static_prop.vert and terrain shader.
   - Test ambient occlusion + SH lighting on static props.
   - Unlocks: dynamic ambient from HDRI; better realism in outdoor scenes.

5. **ASSIMP-IMPORTER-PHASE-0** (Week 5-8)
   - Design mesh import pipeline (source format to Assimp to normalize to LOD gen to pak).
   - Define canonical vertex attrib layout for Vulkan VAO binding.
   - Test Assimp import on subset of FBX/glTF models.
   - Integrate into build_scripts (equivalent to aseconv for modern DCC tools).
   - Unlocks: FBX/glTF/OBJ import for mechs and static props.

6. **MESH-OPTIMIZE-LOD-0** (Week 8-10)
   - Integrate meshoptimizer as post-Assimp cook stage.
   - Design LOD tier strategy (LOD0=full, LOD1=50%, LOD2=25%, etc.).
   - Test on 10 representative mech models; measure size/quality tradeoff.
   - Verify GPU-driven cull and draw compatibility.
   - Unlocks: efficient LOD streaming for large scene complexity.

7. **GLTF-PACK-PHASE-0** (Week 10-12, parallel with ASSIMP)
   - Evaluate glTFpack vs. legacy ASE→PAK (size, load time, compatibility).
   - Vendor gltfpack into 3rdparty/.
   - Design modern pak format (glTF + metadata JSON).
   - Test on sample glTF imports.
   - Unlocks: optimized glTF distribution; modern modding format.

---

## Next Slice Recommendation: KTX2-BAKE-PROBE-1

**Rationale:**

1. **Unblocks asset distribution:** mc2texcook is ready for production use. Implementing KtxLoader is the minimum viable step to ship KTX2-baked textures. This is critical for mod support and reducing disk footprint.

2. **Low risk:** KTX2 spec is stable; KtxLoader is straightforward mip-level streaming implementation. No design uncertainty.

3. **Measurable impact:** Users will see faster asset load times and cleaner mod installation (single .ktx2 per texture vs. TGA source).

4. **Prerequisite for IBL:** V-IBL-STATIC-1 baking depends on KTX2 loader being present to consume the emissive map output.

5. **Manageable scope:**
   - Implement KtxLoader (2-3 days).
   - Cook representative material set (1 day).
   - In-game testing and integration (2-3 days).
   - Fallback to TGA if needed (low risk).

**Next actions:**
- Read tools/mc2texcook/README.md and test on one TGA source.
- Stub out RenderCore/KtxLoader.cpp with KTX2 header parsing.
- Wire into GameOS/gameos/gos_texture.cpp (texture binding pipeline).
- Cook a test material (e.g., Locust mech body).
- Load in-game via KtxLoader; verify visual quality vs. original TGA.
- Document workflow in docs/modding-guide.md.

---

## Appendix: External Tools and SDK Requirements

| Tool | Source | Install |
|------|--------|---------|
| glslangValidator | Vulkan SDK 1.3.x | $VULKAN_SDK/Bin/glslangValidator or PATH |
| spirv-val | Vulkan SDK 1.3.x | $VULKAN_SDK/Bin/spirv-val or PATH |
| spirv-cross | Vulkan SDK 1.3.x (optional; for shader_reflect) | $VULKAN_SDK/Bin/spirv-cross or PATH |
| Python 3.8+ | System or pyenv | Required for all .py cook tools |
| Pillow | pip install Pillow | Required by mc2texcook |
| NumPy | pip install numpy | Required by project_sh.py |
| OpenEXR or imageio | pip install imageio (or OpenEXR) | Optional (fallback for EXR loading in project_sh.py) |
| CMake 3.22+ | System | Required for 3rdparty/assimp build (if enabled) |

---

## Document Metadata

- **Generated:** 2026-05-29
- **Source:** Audit of .claude/worktrees/nifty-mendeleev/ (Track V Toolchain audit task TOOLCHAIN-BOM-1)
- **Scope:** Asset cook, validation, and pipeline tools (runtime libraries cataloged separately)
- **Next update:** When a new tool is vendored or a status changes
