> **REVIEW-GATE STATUS (2026-07-10): CLEARED-FOR-PHASE-0** (adversarial round embedded; meta review passed).
> Three required revisions for the Phase-0 agent to fold in before/at first commit:
> 1. §2 framing: "strict stack proven correctness-clean" → annotate: proof being REGENERATED under a hardened forbidden-readback checker after a realized false-pass (vulkan_terrain_island.cpp:125-133 counter never incremented); 8.7-delete + fail-hard gates re-derive independently.
> 2. ABI-STRUCT-AUDIT-1: add a COMPLETENESS deliverable — mechanical enumeration of all struct-typed fread/File::read/memcpy sinks cross-referenced vs the long/wchar_t/enum/bitfield field inventory, as a CI check-* that fails on un-annotated on-disk structs. Oracle = byte-diff + proof the subset is exhaustive.
> 3. Phase-0 actionability: state worktree + lane-registration (register_lane) + branch convention for each Phase-0 slice (shared-build64 + lane-overlap hazards).
> Meta-review adjudications binding on implementers: VK macOS-critical path = plain vkCmdDrawIndexed only (compute/tess/MDI = GL-only, compiled out); genuine Phase-2 risk = terrain sampler-count vs Metal's 16-sampler cap (MSL-LIMIT-AUDIT-1 may be L, not M); mech asset-feed cost extrapolation rests on a parity bar never met even on Windows.

# MechCommander 2 — macOS (Apple Silicon / MoltenVK) Port Roadmap

**Synthesis of 8 subsystem recon reports, hardened against adversarial review · Target: arm64-only, VK-only renderer via MoltenVK, macOS 13+ · 2026-07-10**

---

## 1. Executive Summary

**Total shape.** This is a *moderate-to-large*, well-bounded port, not a rewrite — but it has **two co-equal long poles** that dominate the schedule and one architectural inversion that gates everything downstream. The recon consensus across all 8 subsystems is unusually favorable *for the windowing/WSI layer*: the engine already carries a **latent Linux-port heritage** (SDL2 owns windowing/input/entry-point, `-DLINUX_BUILD` is defined globally even on Windows, a `GameOS/src/platform_win*` Win32-emulation shim exists, all x86 inline asm is dead C-ported, `int main` not `WinMain`, no SSE intrinsics). The windowing "SDL2 vs native Cocoa" question is **already answered — SDL2, years ago**; a Cocoa shim would be strictly more code for zero gain.

**But the favorable tone is earned only for WSI.** Two layers the recon under-weighted carry hard, silent-corruption risk and must be priced as first-class work:

- **The data/ABI layer.** Win64 is LLP64 (`long`=4, `wchar_t`=2); macOS arm64 is LP64 (`long`=8, `wchar_t`=4). This codebase reads `.pak`/`.fit`/save binaries by `fread`-into-struct + pointer-cast. There are **~193 `long <field>;` declarations across 30 mclib headers alone** (`mech3d.h` 40, `ablenv.h` 13, `gvactor.h` 12, `bdactor.h` 12, `heap.h` 11, `cmponent.h`/`bitflag.h` 10 each…). Any that back an on-disk record read at the **wrong offset/size on arm64** — silent save-game and asset corruption, not a bisectable crash. This is the **second long pole after asset-feed** and it must land before First-Pixel.
- **The toolchain/ABI layer.** `GameOS/include/pch.hpp:9` carries a bare file-scope `#pragma pack(4)` with no visible `push`/`pop`. The PCH is included in nearly every TU; on macOS, MoltenVK/SDL/Foundation system headers would be parsed **under pack(4)**, mis-laying-out `NSObject`/`CAMetalLayer`/`dispatch` structs → memory corruption, not warnings. This is a boot-blocking BLOCKER, not an "honorable mention."

**The critical path** runs through three serial gates, now with the ABI audit as a co-pole:

1. **A GL-free boot must exist** (Windows-side arch work) — MoltenVK has no GL 4.3, and the VK backend today *boots GL first and fail-softs to GL everywhere*. Every downstream subsystem (packaging, signing, smoke) is blocked until a GL-free boot is real. Distribution recon states it bluntly: *"no point building the bundle/signing pipeline until a GL-free boot exists."*
2. **The GL asset-feed rewrite** (long pole #1, XL) — terrain/prop/mech VK islands source their pixels via `glGetTexImage`/`glGetCompressedTexImage` readback from the **GL** texture manager. On a VK-only build that texture manager does not exist. This is asset-pipeline work, not WSI work, and is *the dominant cost of "VK-only on macOS."* The **water island is the proven template** (disk `.ktx2` BC6H sidecar + device-feature check, zero GL dependency).
3. **The LP64 on-disk struct audit** (long pole #2, L–XL) — every struct `fread`/`memcpy`'d from a file or pak buffer must be audited for `long`/`unsigned long`/`wchar_t`/enum-width/bitfield fields and converted to fixed-width types. Runs mostly on Windows, before any Mac exists, and must land before a mission scene is trusted.

**First pixel** (main menu + intro video) additionally depends on a VK-native path for `gos_DrawQuads`/`gos_LockTexture` (the 2D/immediate pipeline that video and some UI ride), which **does not exist on the VK branch** (its VK files are scene islands + post + present only).

**Rough phase count: 9** (Phase 0 Windows-prerequisites → Phase 8 packaging).

**What parallelizes.** Substantial work can proceed off the critical path on the Windows box *before any Mac exists*: the LP64 struct audit, the 8.7 interop deletion, the P0 terrain-staging perf fix, the branch merge, the CMake platform-conditionalization, the POSIX shims (QPC, `snprintf`, atomic-rename, heap asm), the `pch.hpp` pack fix, and the `run_smoke.py`/`deploy_payload.py` de-Windowsing are all authorable and testable without a Mac. Once a Mac exists, the asset-feed rewrite (Phase 3) and the input/HiDPI polish (Phase 6) can run in parallel lanes because they touch disjoint files. Packaging (Phase 8) is a late, independent lane once boot-to-mission is green.

**Honest effort read.** ~5 XL/L items carry the schedule (asset-feed XL, LP64-struct-audit L–XL, launcher L, conformance-fix pass L, boot-order inversion + surface + signing M/L-clusters). Everything else is a long tail of S/M shims — high in count, low in individual risk, but with a **configure-time failure cascade** that hides depth: fixing SDL2 provisioning only reveals the FFmpeg `FATAL_ERROR`, then the tool-subdir `zlib.lib` `FATAL_ERROR`s, then the GLEW link. Budget Phase 1 as an iterative unblocking loop, not one pass. And note two *circular* gates the naive numbering hides — the GL-free link (Phase 1) cannot close until GL readback call-sites are stubbed, and the Phase-3 parity oracle cannot be RenderDoc (no Metal support) — both are resolved by interim slices below.

---

## 2. Current-State Assessment — What the VK Ownership Ladder Already Bought Us

The VK arc (rungs 1–8.2 done, strict no-interop stack proven correctness-clean, P0 terrain-staging perf fix in flight, 8.7 interop-deletion pending) is, in effect, **most of the macOS renderer port already paid for** — it just wasn't framed that way. Concretely, the ladder delivered:

- **A VK backend that renders all three geometry types** (terrain textured+lit+detail, props near-byte-identical, mechs VK-skinned) and **owns present** (`MC2_VK_SHARED_GBUFFER` umbrella). The macOS renderer is not greenfield.
- **The strict "direct-chain" / `vkOnlyNoGlView` paths** (rungs proven correctness-clean) that **already parameterize the GL-interop layer out**. `vulkan_shared_scene_color.cpp` / `vulkan_shared_scene_depth.cpp` take `vkOnlyNoGlView` and allocate **non-exportable images with no GL view**; `vulkan_color_prop_island.cpp` skips all interop sync objects when `MC2_VK_*_DIRECT_CHAIN=1`. This means the Win32 external-memory/semaphore `HANDLE` layer — the thing MoltenVK *cannot* do — is already on a clean deletion seam.
- **A MoltenVK-clean device profile** (per `METAL-GATE-AUDIT-2.md`): VK 1.1 core, **zero optional device features requested**, SPIR-V 1.0, **no geometry/tessellation/transform-feedback**, only `vkCmdDrawIndexed`, binary semaphores + fences (no timeline), classic descriptor sets (no descriptor-indexing/UAB), 16-byte push constants (vs Metal's 4 KB limit). **⚠ Caveat surfaced by adversarial review:** that profile appears to describe the *islands* as shipped, and it directly contradicts CLAUDE.md's stated engine ("SSBOs, compute, tessellation, MDI") and the terrain recipe/compute path. The "only `vkCmdDrawIndexed`" claim must be re-verified against the *full intended* renderer, not just the island subset, before it is trusted (see Phase 2 `MSL-LIMIT-AUDIT-1` and Unknowns #5, #16).
- **Portable WSI patterns already in-tree, proven.** `SDL_Vulkan_CreateSurface` on an `SDL_WINDOW_VULKAN` window exists verbatim in **four** files (`vulkan_present_object.cpp`, `vulkan_swapchain_present.cpp`, `vulkan_swapchain_probe.cpp`, `tools/vk_present_smoke`). The macOS surface swap is a *transplant*, not new code. Present-mode negotiation already availability-gates MAILBOX/IMMEDIATE and always falls back to FIFO (MoltenVK-native). Swapchain format B8G8R8A8 is MoltenVK-native.
- **volk + VMA**, both vendored and macOS-capable (volk dlopens `libvulkan`/`libMoltenVK`), with VMA imported *from* volk — no hard link to `vulkan-1.dll`.

**What the ladder did *not* buy** and what therefore defines the remaining renderer work:
- **8.7 interop-deletion is still pending** — the exportable/interop `HANDLE` code paths are still present (cleanly gated) in the worktree HEAD. This must land to collapse `vk_image_resource` to a plain VMA image and delete `gl_interop_view.*`. (Correction from review: this is a Windows-branch-canonicalization prerequisite, **not** a macOS-observable one — the Mac simply `#ifdef _WIN32`-guards those TUs — so it need not serialize ahead of Mac work; see Phase 0 note.)
- **The GL asset-feed** (islands reading textures via GL readback) is untouched — this is long-pole #1 and the ladder never had to solve it because GL was always there to read from.
- **The LP64 on-disk struct layer** was never exercised — every prior build was LLP64 Windows, so field-width divergence has never been visible. This is long-pole #2.
- **Fail-soft-to-GL is load-bearing in boot order** — the selector boots GL first; on macOS that inverts to fail-hard, which is architectural.

Net: the ladder converted "port a GL renderer to Metal" (infeasible — macOS GL caps at 4.1) into "finish the VK-native path you were already building, minus one texture-feed producer — and fix the on-disk ABI the codebase has never had to fix." Those are the two facts that most govern pricing this port.

---

## 3. Phased Plan

Sizes: **S** ≤ ½ day · **M** 1–3 days · **L** ~1 week · **XL** multi-week. Every slice carries a **verification oracle** (the project demands proof gates) and a **gate/killswitch** name.

---

### Phase 0 — Windows-Side Prerequisites, ABI Audit & Branch Reconciliation
*Goal: land the arch-defining Windows work, fix the on-disk ABI, and unify branches so the port is done once, not twice. Runs entirely on the Windows box, before any Mac exists.*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **ABI-STRUCT-AUDIT-1** (BLOCKER — long pole #2) | L–XL | Every struct `fread`/`memcpy`'d from a file/pak buffer, across the ~193 `long` fields in 30 mclib headers (`mech3d.h` 40, `ablenv.h` 13, `gvactor.h` 12, `bdactor.h` 12, `heap.h` 11, `cmponent.h`/`bitflag.h` 10…), plus any `wchar_t` wide-file I/O and enum-width/bitfield-width members. Convert on-disk fields to fixed-width (`int32_t`/`uint32_t`/`char16_t`); leave pure-runtime `long`s alone. Add `static_assert(sizeof(T)==N)` guards on every on-disk record. | **Byte-diff a Windows-parsed vs a mac-parsed struct dump** for at least one `.fit`, one save-game, and one `.pak`/`.fst` header — identical bytes at identical offsets. Windows build stays byte-identical (fixed-width == existing size on LLP64). | `MC2_ABI_FIXEDWIDTH` (compile-audit gate; default on once green) |
| **VK-INTEROP-DELETE-8.7** (finish pending rung) | L | `vk_image_resource.h/.cpp` (drop Win32 `HANDLE` fields → plain VMA image), delete `gl_interop_view.cpp`, `vulkan_context.cpp:296-301` interop ext block | tier1 5/5 on Windows with `MC2_VK_SHARED_GBUFFER` **both states**; RenderDoc parity vs pre-deletion baseline on `mc2_01`/`mc2_24` | existing `MC2_VK_*_DIRECT_CHAIN` become the only path |
| **VK-TERRAIN-STAGING-PERFFIX** (in flight) | M | terrain-staging path (P0) | tier1 FPS ≥ baseline; Tracy `stage_us` back in single-digit ms | — (fix, not gated) |
| **MACOS-BRANCH-MERGE-1** | M | Union of `mc2-main` windowing features (`MC2_WINDOW_RESIZABLE`/FREE-RESIZE, `launcher_env.json` injection) ∪ `claude/vk-interop-bridge-1` backend; base-rebase | Windows build green on merged branch; tier1 5/5; windowing features still function | — |
| **METAL-GATE-COMMIT-1** | S | Commit `.claude/METAL-GATE-AUDIT-2.md` (currently untracked, "pending adversarial+meta review") to a stable SHA; annotate the "only `vkCmdDrawIndexed` / zero optional features" claims as *island-scoped, full-renderer-unverified* pending `MSL-LIMIT-AUDIT-1` | file tracked; SHA citable by all downstream slices | — |
| **QPC-SHIM-CENTRAL-1** | S | Add `QueryPerformanceCounter/Frequency` + `GetTickCount64` to `GameOS/src/platform_mmsystem.cpp` (mach_absolute_time/clock_gettime-backed `LARGE_INTEGER`). One shared shim — QPC appears in `mclib/file.cpp:92-99` (load-bearing file clock), `terrain.cpp`, `move.cpp`, `abldbug.cpp`, `gvehicl.cpp`, `mech.cpp` | Windows build unaffected (shim is `#else`-only); symbol resolves in a scratch clang compile | — |

**Sequencing correction (from review):** `VK-INTEROP-DELETE-8.7` is a serial gate only for declaring the **Windows** branch canonical — it delivers **zero macOS-observable benefit** (the Mac `#ifdef _WIN32`-guards those TUs regardless). Run it as a parallel Windows lane; it must land before the merged branch is frozen, but it does **not** block Mac compile/boot work. `ABI-STRUCT-AUDIT-1` and the branch merge are the real "must-land-before-porting" items; the audit is the schedule's co-pole and should start day one.

---

### Phase 1 — Build Bootstrap: Configure & Link on macOS
*Goal: `cmake` configures and `mc2` links on arm64 clang/Ninja. It may not run yet. This is the configure-cascade unblocking loop. Two ABI/toolchain BLOCKERs land here, plus the interim stub that closes the circular GL-free-link gate.*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **PCH-PACK-FIX-1** (BLOCKER) | S | `GameOS/include/pch.hpp:9` bare `#pragma pack(4)` → `#pragma pack(push,4)` … `#pragma pack(pop)` scoped to the engine structs that require it; ensure **all** system/framework includes (`<SDL.h>`, MoltenVK, Foundation, `<dispatch/*>`) sit **outside** the packed region | A scratch TU that includes a MoltenVK/SDL header *after* pch and `static_assert`s `sizeof`/`alignof` of a known system struct (e.g. `SDL_version`, a `CAMetalLayer`-adjacent type) against its expected value | — |
| **GL-READBACK-STUB-1** (unblocks circular gate M1) | M | Every GL-readback call site in the islands (`vulkan_terrain_island.cpp` `glGetTexImage`/`glGetCompressedTexImage`/`glGetTexLevelParameteriv`; `vulkan_color_prop_island.cpp`; mech island) → replace with a `MC2_FATAL("gl readback not available")` placeholder / magenta-fill feed so the TU compiles and **links with no GL symbols**. Phase 3 swaps stubs for real disk/CPU feeds. | `nm` audit: zero GL symbols in `mc2`; each stub is reachable-fatal, not silently no-op | `MC2_VK_NATIVE_TEXFEED` (stub state = "not yet fed") |
| **MACOS-BUILD-BOOTSTRAP-1** (compiler/generator) | S | `CMakeLists.txt:53-62` — split NOT-MSVC into GNU vs AppleClang branch, drop `-fpermissive` for clang; gate `-DPLATFORM_WINDOWS` on `WIN32` (`:62`); set `CMAKE_BUILD_TYPE` default for single-config Ninja; `CMAKE_OSX_ARCHITECTURES=arm64`; set `CMAKE_OSX_DEPLOYMENT_TARGET=13.0` (decided — see Unknown #9) | `cmake -G Ninja` reaches first `find_package` failure (progress metric) | `MC2_GL_BACKEND` option introduced here |
| **MACOS-DEPS-SDL-ZLIB-1** | M | `CMakeLists.txt:234-241` SDL2/mixer/ttf find_package → Homebrew/vcpkg arm64 on APPLE; system `libz`; guard DLL-copy loops `:617-652` with `if(WIN32)` | `find_package(SDL2)` resolves; SDL2 arm64 links | — |
| **MACOS-DEPS-FFMPEG-1** | M | `cmake/FindFFmpegLGPL.cmake` — APPLE branch via pkg-config (libavcodec≥61) or vendored `.dylib` tree producing same 5 `FFmpegLGPL::*` targets; skip `mc2video_dlls.h` gen on non-Windows; guard install/copy `:615-622` | configure passes FFmpeg; `code/mc2video.cpp` `#else` probe compiles | `MC2_ENABLE_VIDEO` (option to build video OFF for first bring-up) |
| **MACOS-GL-BACKEND-GATE-1** | M | `CMakeLists.txt:578,758` — `MC2_GL_BACKEND=OFF` removes GLEW::GLEW + OpenGL::GL from link; compile-gate GL TUs (`gameos_graphics.cpp`, `utils/gl_utils.cpp`, `graphics.h`, `shader_builder.cpp`). **Circular-gate resolution:** the "no GL symbols" exit is achievable in Phase 1 *only because* `GL-READBACK-STUB-1` replaces the island readback calls; real feeds come in Phase 3. | `mc2` link succeeds with **no** GL symbols (`nm` audit) | `MC2_GL_BACKEND` (ON only on WIN32) |
| **MACOS-TOOLGUARD-1** | M | `if(WIN32)` around `tools/mc2_launcher` (`root:390-392`), and every subdir hardcoding `3rdparty/lib/x64/*.lib`: `tools/asset_viewer:31`, `tgl_loader_standalone_spike:135`, `tests/unit:118,188` (glew32.lib/zlib.lib), model_override/static_natural_skip. Guard vk_* interop probe tools (`vk_gl_interop_spike`, `vk_hwnd_present_probe`, etc.) — MoltenVK has no win32 external-memory | configure passes with `MC2_BUILD_TESTS=ON` (default) on macOS | — |
| **MACOS-SHIM-COMPILE-1** (+ implicit `-fpermissive` removal) | **L** *(re-sized from M)* | Compile `GameOS/src/platform_*.cpp` on clang; fill gaps. Known: `platform_winbase.cpp:120-127` CopyFile uses Linux `st_atim`/`st_mtim` → macOS `st_atimespec`/`st_mtimespec`; `#define _snprintf snprintf`; `heap.cpp:238` GCC x86-64 `mov %%rsp` asm → `__builtin_frame_address` (arm64); `gameos_fileio.cpp:38` `"rwb"` → `"r+b"`. **Plus** the repo-wide AppleClang conformance pass this implies: dropping `-fpermissive` exposes 25-yr-old const-char*/narrowing/implicit-decl errors across `mclib`+`code`+`GameOS` (Risk #4/#5). This is on the critical path. | each `platform_*.o` compiles; link-stage symbol audit clean; full `mclib`+`code` `-fsyntax-only` pass green | — |
| **MACOS-WINH-CONVERT-1** | M | Convert ~26 raw `#include <windows.h>` to `platform_windows.h` or delete (GL-backend files delete with `MC2_GL_BACKEND=OFF`; `mc2video.cpp:23` only wanted `<delayimp.h>` — drop on mac) | full-TU compile pass; no raw `windows.h` outside `#ifdef _WIN32` | — |
| **MACOS-VIDEO-BC-VERIFY-1** | S | Vendored assimp/imgui/tracy/volk/VMA/tinyexr/stb recompile pass; `renderdoc_app.h` compile-gated | all vendored libs build arm64 (warning noise OK) | — |

**Exit criterion for Phase 1:** `mc2` (arm64) **links, GL-free** (islands stubbed), pch pack is scoped, and the conformance pass is green. First runtime behavior unknown — that's Phase 2.

---

### Phase 2 — Boot to a VK Surface (GL-Free First Present) + Cross-Platform Oracle & Limit Audit
*Goal: the process launches, creates one `SDL_WINDOW_VULKAN` window, brings up MoltenVK, and presents a cleared swapchain. This is the boot-order inversion. Two review-mandated prerequisites land here: the in-engine readback oracle that replaces RenderDoc for all downstream parity work, and the MSL per-stage limit audit that must pass before Phase 3 is trusted.*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **VK-READBACK-ORACLE-1** (unblocks M2 — prerequisite for all Phase-3 parity) | M | New cross-platform in-engine capture: `vkCmdCopyImageToBuffer` on any scene/island attachment → CPU buffer → deterministic hash + PNG + SSIM vs a reference. Works identically on Windows (for reference capture) and macOS (for parity). Wire to a `MC2_VK_CAPTURE` gate; store references in the capture-set tree the MCP bridge already tails. | On Windows: capture matches a RenderDoc-verified frame within noise floor (bootstraps trust in the oracle itself). On macOS: same API path runs. | `MC2_VK_CAPTURE` |
| **MSL-LIMIT-AUDIT-1** (BLOCKER prerequisite for Phase 3) | M | Run the **full** `.spv` corpus through `spirv-cross --msl` and grep per-stage **sampler (≤16) and buffer (≤31) counts** without argument-buffer tiers. `shaders/gos_terrain.frag` already declares **9 named samplers** (`tex1-3`, `matNormal0-4`, `matNormalArray`) *before* material color arrays + the CSM shadow-array sampler — plausibly ≥12–16 combined image samplers in one stage; SSBOs (`std430 binding=2`) + push-constant blocks compete for 31 buffer slots. Also audit: **CSM layered rendering** (does it use `VK_KHR_multiview`/`gl_Layer`/`render_target_array_index` — restricted on MoltenVK — or a geometry shader?), and the **compute/recipe/MDI terrain path** vs METAL-GATE's "only `vkCmdDrawIndexed`" claim. | For every pipeline: sampler count ≤16 and buffer count ≤31 in MSL; no multiview/geometry-shader/unsupported-feature dependency, or a documented remediation (argument buffers / split passes / bindless-tier). Pipeline creation succeeds under MoltenVK for each. | `MC2_VK_MSL_AUDITED` (informational) |
| **MACOS-VK-WINDOW-1** | M | `gos_render.cpp:306-322` — when backend=vulkan (only macOS mode) create window `SDL_WINDOW_VULKAN` (not `_OPENGL`; SDL forbids both on macOS); skip `SDL_GL_CreateContext`/`glewInit` and the GL-version hard-fails `gameosmain.cpp:1355-1382`. `get_drawable_size`→`SDL_Vulkan_GetDrawableSize`; vsync from VK present mode | window appears; no GL context created (log audit) | reuses `MC2_RENDER_BACKEND=vulkan` |
| **MACOS-VK-SURFACE-1** | M | `engine_present_vk.cpp:1627-1666` — replace `SDL_GetWindowWMInfo`→HWND→`vkCreateWin32SurfaceKHR` (P13b trick) with `SDL_Vulkan_CreateSurface`; delete `SDL_syswm.h`. **Transplant from `vulkan_present_object.cpp:298`**. Confirm single gfx+present queue family on the M-series GPU (Unknown #12). | surface created; swapchain acquires images | guard `VK_USE_PLATFORM_WIN32_KHR` behind `#ifdef _WIN32` |
| **MACOS-VK-PORTABILITY-1** | S | `vulkan_context.cpp:159-175,221-231` — `SDL_Vulkan_GetInstanceExtensions` instead of hardcoded win32; add `VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`; enable `VK_KHR_portability_subset` device ext when advertised | `vkCreateInstance`/`vkCreateDevice` succeed under MoltenVK; validation-layer clean **in the dev-loader config only** (see dual-config note) | — |
| **MACOS-VK-FAILHARD-1** | S | `render_backend_select.cpp:24-67`, `vulkan_context.cpp:120-126`, `engine_present_vk.cpp:1619-1625` — on `__APPLE__`, default backend=vulkan, `forceGLFallback()`→fatal message-box/abort (no GL to rescue). **Inverts the load-bearing fail-soft contract** | induce a VK-init failure → clean fatal, not silent GL attempt (negative control) | `#ifdef __APPLE__` policy |
| **MACOS-VK-DEPTH-1** | S *(+ hidden M — see re-size note)* | `vulkan_shared_scene_depth.cpp:114-127` — depth probe with `D32_SFLOAT_S8_UINT` default (Metal lacks `D24_UNORM_S8`; the D24 preference existed only for GL bit-reinterpret parity, moot in native-only) | depth attachment creates; no format-unsupported error | — |
| **CASE-FOLD-DEBUG-1** (split forward from Phase 7 — Risk #7 bites here) | S | `file.cpp:981-990` `S_strlwr` path — add a **debug-build case-fold fallback**: on a failed open, retry with a case-folded directory scan so dev clones + GitHub runners on **case-sensitive APFS** boot at all. Packaging-time normalize + CI gate stay in Phase 7. | boots on a case-sensitive APFS volume; each fallback logs the offending mixed-case name | `MC2_CASEFOLD_FALLBACK` (debug default on) |
| **MACOS-VK-MAIN-SHIM-1** | S | `gameosmain.cpp:832-937` — guard launcher-bootstrap `CreateProcessA`/`GetModuleFileNameA` in `#ifdef _WIN32` **but port `launcher_env.json` reader** via `setenv`/`SDL_GetBasePath` so curated `MC2_*` gates survive; `freopen("NUL")`→`/dev/null` (`:962/965`) | game reads `launcher_env.json` on macOS; `MC2_*` gates apply | — |
| **MACOS-MVK-ICD-1** | **M** *(re-sized from S)* | Bundle `libMoltenVK.dylib` + (dev) Khronos loader + `MoltenVK_icd.json`; volk points at it. `LC_RPATH @executable_path/../Frameworks`. This overlaps Phase-8 relocatable-bundling work (rpath, ICD discovery without loader, install-name) — not a half-day task. | `volkInitialize` resolves MoltenVK; `vkEnumeratePhysicalDevices` returns Apple GPU | — |

**★ Dual-config note (resolves M4).** Validation layers load **only via the Vulkan loader**, but the shipping runtime (`MACOS-MVK-ICD-1`) has volk call the **bundled `libMoltenVK.dylib` directly**, bypassing the loader. These are two distinct runtime configurations:
- **dev-loader config** — LunarG loader + explicit `VK_LAYER_KHRONOS_validation`; every validation-based oracle (`MACOS-VK-PORTABILITY-1`, `MSL-LIMIT-AUDIT-1` runtime pass, Risk #10) is pinned here.
- **ship-ICD-direct config** — volk → bundled MoltenVK, no loader, no layers; this is what users get and what the smoke gate runs.
Never assert "validation clean" against the ship config.

**Exit criterion (First-Light):** window opens, swapchain clears to a color, one frame presents, clean exit. The readback oracle and MSL limit audit are green. No scene yet.

---

### Phase 3 — Long Pole #1: GL-Free Native Asset Feed
*Goal: terrain/prop/mech islands source textures without GL — swapping the Phase-1 stubs for real disk/CPU feeds. This is the XL that co-dominates the schedule. Once green, a mission scene renders VK-native. **All oracles use `VK-READBACK-ORACLE-1`, not RenderDoc** (which has no Metal support).*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **MACOS-VK-ASSETFEED-TERRAIN-1** | XL | `vulkan_terrain_island.cpp:43,1383-1536,1602-1724` — replace the stubbed `glGetTexImage`/`glGetCompressedTexImage`/`glGetTexLevelParameteriv` readback (color/detail/overlay arrays) with a **CPU/disk producer**: feed from `mclib/txmmgr` CPU texture data or precooked KTX2/BC sidecars. **Follow the water-island template** (`vulkan_water_island.cpp:395-461`: reads BC6H `.ktx2` sidecar from disk, checks `textureCompressionBC` device feature, RGBA16F fallback). | **In-engine readback-oracle parity** (`vkCmdCopyImageToBuffer`→hash/SSIM) vs a Windows GL reference frame on `mc2_01`, deterministic camera (COVERAGE not magnitude, per VK-arc discipline). Xcode GPU Frame Capture is a manual spot-check only, not the gate. | `MC2_VK_NATIVE_TEXFEED` (fed state) |
| **MACOS-VK-ASSETFEED-PROP-1** | L | `vulkan_color_prop_island.cpp:51,647-648` — BPTC/BC7 GL-enum mapping currently reads live GL textures; extend the water-island BC-feature-check pattern to the BC7 prop path | readback-oracle prop parity vs baseline (props were near-byte-identical on Windows — hold that bar) | same gate |
| **MACOS-VK-ASSETFEED-MECH-1** | L | mech island texture feed (mech was VK-skinned correct silhouette; material/lighting was flat-probe deferred even on Windows) | readback-oracle mech silhouette + material parity on deterministic near-camera oracle | same gate |
| **MACOS-VK-BAKE-PIPELINE-1** | M | Asset-pipeline: precook the KTX2/BC sidecars the islands now consume (may extend `tools/` bake steps — flagged as an unknown that could add tools). Ship cooked sidecars in the data tree | sidecars present on disk; islands load them; no runtime GL cook | — |

**Dependency note:** This phase is the concrete expression of `MACOS-GL-BACKEND-GATE-1` + `GL-READBACK-STUB-1` (Phase 1) on the source side, and of the METAL-GATE-AUDIT-2 "long pole = GL-dependent asset-feed layer." It is one of the schedule's two centers of gravity (the other being `ABI-STRUCT-AUDIT-1`), and it is gated behind `MSL-LIMIT-AUDIT-1` passing.

---

### Phase 4 — First Pixel: Main Menu & Video Seam
*Goal: see the MC2 main menu on a Mac. Requires the 2D/immediate path (video + some UI) VK-native.*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **MACOS-VK-GOS2D-1** (or wait for renderer arc) | M–L | `gos_DrawQuads`/`gos_LockTexture`/`gos_NewEmptyTexture` — implemented **only** in the GL renderer (`gameos_graphics.cpp:8547`); VK branch has **zero** VK 2D. Either (a) VK-native gos-2D, or (b) route movie call sites through the existing `video_get_texture()` → ImGui/defs composite seam that `missionselectionscreen.cpp:97-98` already uses | main menu renders; ImGui overlay composites | `MC2_VK_GOS2D` |
| **MACOS-VIDEO-DISPLAY-1** | M | `code/mc2video.cpp:392-751` texture-lock upload is CPU `sws_scale` into a locked staging buffer (backend-agnostic in shape) — needs a **VK-backed `gos_LockTexture`**. Convert intro/briefing call sites (`mainmenu.cpp:156`, `logistics.cpp:364,988`, `controlgui.cpp:3905`, `forcegroupbar.cpp:865`) to the composite pattern if choosing (b) | intro movie plays or clean-skips (`mc2movie.cpp:80-85` SmokeMode gate as fallback lever) | `MC2_ENABLE_VIDEO` / SmokeMode skip |
| **MACOS-AUDIO-SOFTEN-1** | S | `gameos_sound.cpp:118-120` — `STOP()` on `SDL_INIT_AUDIO` fail → `SPEW`+return false (caller `gameosmain.cpp:1457` already tolerates). Audio is otherwise **portable-as-is** (SDL2_mixer 2.8.0, `Mix_OpenAudio` 22050 S16LSB) — **but note** CoreAudio's native 48 kHz rate means `Mix_OpenAudio(22050)` triggers SDL's resampler on the real-time callback thread; non-blocking but not free, and the "one live listen" oracle is not headless-CI-automatable (keep it interactive). | boots on audio-less/headless mac; one live listen confirms `.snd` pak decode (interactive box only) | — |

**★ FIRST-PIXEL MILESTONE** = Phases 0–2 (incl. ABI audit, pack fix, stub, readback-oracle, MSL audit) + `MACOS-VK-GOS2D-1` + `MACOS-AUDIO-SOFTEN-1` (video may be skipped via SmokeMode). See §7.

---

### Phase 5 — Mission Boot & macOS Smoke Gate (tier1-equivalent)
*Goal: a mission renders and the project's proof-gate machinery runs on macOS.*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **MACOS-DEBUGSTATE-POSIX-1** | S | `debug_state_dump.cpp:110,202-204` + `diagnostic_trace.cpp:5` — `MoveFileExW(REPLACE_EXISTING)`→`std::rename` (atomic same-dir on APFS); `GetCurrentThreadId`→`pthread_threadid_np`. **Load-bearing for the smoke gate** (`MC2_DEBUG_STATE_DUMP`/`MC2_DIAGNOSTIC_TRACE_FILE`) | debug_state JSON + trace JSONL emit on macOS | — |
| **MACOS-SMOKE-PROC-1** | M | `run_smoke.py:146,274,864,128` — `Get-CimInstance`/`taskkill`→psutil (`process_iter`/`exe()`); case-**sensitive** path identity; parameterize exe name (`mc2` vs `mc2.exe`) + root (drop `A:/` presets). **Preserve anti-stale-exe semantics** (never kill foreign, block on same-path) — do not naively weaken | `run_smoke.py --tier tier1` exit 0 on macOS (5 missions × 30 s); verdict from engine-side `MC2_SMOKE_MODE` autoplay + heartbeat, no input injection | — |
| **MACOS-DEPLOY-1** | S–M | `deploy_payload.py:201-215,348,793-805` — lock-probe already win32-guarded; add psutil running-process check; parameterize exe names + `A:/` presets. Or a simpler mac deploy script | payload deploys to mac install dir; check-deploy-coherence passes | — |
| **MACOS-FILEAPI-AUDIT-1** | M | `mclib/file.cpp` mod-index Win32 additions (**postdate the Linux shims — guaranteed link failures**): `GetFileAttributesExA`/`FILETIME`/`CompareFileTime` (`:299-340`, no shim exists) → `stat()`-based mtime rewrite; `CreateFileA`/`ReadFile` (`:528-535`)→`fopen`; `GetFullPathNameA`→`realpath` | mod-cache invalidation works; `MC2_ACTIVE_MOD` overlay loads on mac | — |

**★ MISSION-TIER1 MILESTONE** = Phases 0–5. `run_smoke.py --tier tier1` exit 0 on Apple Silicon. See §7.

---

### Phase 6 — Input & HiDPI Correctness (partially parallelizable with Phase 3–5)
*Goal: mouse works correctly on Retina. This is the one genuine input shim. **Authoring** parallelizes with Phase 3–5 (disjoint files); **verification** blocks on Phase 4, because the mouse oracle needs a clickable, rendered UI to test against.*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **MACOS-HIDPI-MOUSE-1** | M | `gameos_input.cpp:35-43,130-161` — mouse normalize/warp is tuned to **Windows** raw==physical-drawable; on Retina SDL delivers **logical points at 2× drawable** → cursor dead-zones at 0..0.5. Introduce one points→pixels scale (drawable/window ratio) in `gos_GetMouseInfo`, `gos_SetMousePosition`, `gos_ComputeUiCanvasBox`/`gos_Compute43Box` denominators. **Confirm SDL 2.30 macOS point-vs-pixel behavior for `FULLSCREEN_DESKTOP`+`ALLOW_HIGHDPI` on hardware first** (Unknown #4) — sets the sign of the fix. | **`MC2_MOUSE_RECON` on hardware** (the recon harness built for exactly this bug class) — full-screen cursor reaches all edges. **Note:** this oracle requires a rendered, clickable UI, so it runs **after Phase 4**, even though the code edit can be authored earlier. | verified via `MC2_MOUSE_RECON` |
| **MACOS-VIEWPORT-DRAWABLE-1** | S | `gameosmain.cpp:454` `glViewport` uses logical event size (half drawable on Retina) — disappears with GL path anyway; ensure VK viewport uses `SDL_Vulkan_GetDrawableSize` | viewport matches drawable; no half-res render | — |
| **MACOS-INPUT-SANITY-1** | S | `gameosmain.cpp:239` `KMOD_ALT`=Option, `gameos_input.cpp:349` `KEY_LWIN`=`SDL_SCANCODE_LGUI`=Cmd; grab confinement (`gos_render.cpp:48-105`, no ClipCursor on mac) | Alt/Option debug chords fire; edge-scroll RTS pan works in fullscreen-desktop | — |

---

### Phase 7 — Persistence, Paths & Case-Sensitivity
*Goal: user-writable data lives in the right place; case-sensitive volumes don't break opens. (The debug case-fold fallback already shipped in Phase 2; this phase adds the packaging-time normalize + CI gate + userdata redirect.)*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **MACOS-USERDATA-PATHS-1** | M | `gameosmain.cpp:1235` bare `fopen("options.cfg")` (representative, not exhaustive); `code/prefs.cpp:306`, saves under `userDataDir/savegame/`. A signed `.app` is read-only → redirect `options.cfg`/`orgprefs.cfg`/logs/`imgui.ini`/`debug_state/` to `~/Library/Application Support/MC2Remastered/` via `SDL_GetPrefPath`. Simplest interim: **chdir to resource root** at startup (preserves pervasive relative-path assumption) + redirect only writable files | **full grep-audit of every cwd-relative *write* site** — `fopen(...,"w"/"a")` + `std::ofstream` reaching non-absolute paths (only representative sites cited today; the audit is a named deliverable). `codesign --verify` after a run that writes options.cfg — a broken seal means a missed write site. | — |
| **MACOS-CASE-NORMALIZE-1** | M | `file.cpp:981-990` `S_strlwr` lowercases every open unless `doNotLower`; `fst_hash.cpp:47-52` loose-key normalizer. Default APFS is case-insensitive (users fine) but **dev clones on case-sensitive volumes break every open**. Packaging step lowercases the data tree + CI `check-*` script rejecting mixed-case data files (the Phase-2 debug fallback covers the interim). **Verify FST internal directory lookup is independently case-insensitive** (Unknown #3) — if not, lowercase-normalize alone is incomplete. | audit shipped data for uppercase names; case-sensitive-volume boot test with the debug fallback *disabled* (proves the normalize is complete) | — |
| **MACOS-FIRSTRUN-CONFIG-1** | S | `mechcmd2.cpp:3008-3011` registry FIRSTRUN read (already dead under `LINUX_BUILD`) → marker file beside `options.cfg` | first-run flag persists via file | — |
| **MACOS-CRASH-TRACE-1** (optional) | M | `gameosmain.cpp:55-131` SEH+DbgHelp compiles away → `sigaction`+`backtrace_symbols` for user bug reports; wrap 3 non-crash `__try` sites (`ablexec.cpp:551`, `GuiRuntime.cpp:469`, `mc2video.cpp:92`) as plain calls | crash produces a symbolized trace (not required to boot) | — |

---

### Phase 8 — Packaging, Distribution & Signing
*Goal: a double-clickable, Gatekeeper-passing Mac build. Late, independent lane once mission-tier1 is green. **Re-sized upward** — the Homebrew-dylib relocation and inside-out notarization work the original roadmap omitted are real.*

| Slice | Size | Files / Areas | Oracle | Gate |
|---|---|---|---|---|
| **DYLIB-RELOCATE-1** (new — was missing entirely) | M | Homebrew SDL2/SDL2_mixer/SDL2_ttf/FFmpeg dylibs carry absolute `/opt/homebrew/...` install names and must be relocated into `@rpath` (`dylibbundler`/`install_name_tool`), **in dependency order**, before signing. FFmpeg alone drags a 10–20 dylib transitive tree (libav*, libsw*, codec libs). | `otool -L` on every bundled dylib shows only `@rpath`/system paths — zero `/opt/homebrew` references; app launches from a machine with no Homebrew | — |
| **MACOS-BUNDLE-1** | **M–L** *(re-sized from M)* | Replace `scripts/build_release.sh` (7z splits) — assemble `MC2.app` (`Contents/MacOS/mc2`, `Contents/Frameworks/libMoltenVK.dylib` + relocated deps, data in Resources or side-by-side), `hdiutil` DMG. GitHub 2 GB-per-asset cap → gamedata likely separate downloadable zips with first-run data-locator. Includes the `DYLIB-RELOCATE-1` step. | `.app` launches from `/Applications`; DMG mounts | — |
| **MACOS-SIGN-NOTARIZE-1** | **M–L** *(re-sized from S–M)* | Replace `sign_release.ps1` (Azure Trusted Signing) — **$99/yr Developer ID** + `codesign --options runtime` (hardened runtime). **Sign every bundled dylib inside-out** (deepest dependency first, then the executable, then the bundle) — the FFmpeg/SDL tree is 10–20 dylibs. Then `notarytool submit`/`staple` on a multi-GB payload. Ad-hoc sign is possible but macOS 15+ removed the right-click-Open bypass (per-machine Settings dance). | notarized build passes Gatekeeper on a **clean machine** via `stapler validate` + a GUI launch test (**`spctl -a` is deprecated for notarization assessment on macOS 14+/15 — do not rely on it**) | — |
| **MACOS-LAUNCHER-2** (deferred) | L | `tools/mc2_launcher/mc2_launcher.cpp` (2869-line raw Win32 GUI) — **v1 ships without it** (engine falls through, `MC2_NO_LAUNCHER`+`launcher_env.json` gives GUI-free path). Later: port the portable mod-scanning/bucketing logic into an SDL2+Dear ImGui shell (both already in tree). Contract = write `launcher_env.json` + `MC2_LAUNCHED=1` + exec game | mod picker writes correct `launcher_env.json`; game honors it | — |
| **MACOS-METAL-DEBUG-DOC-1** | S–M | Document Xcode GPU Frame Capture (`MTL_CAPTURE_ENABLED`), Metal API validation (`MTL_DEBUG_LAYER`), MoltenVK introspection (`MVK_CONFIG_DEBUG`, `MVK_CONFIG_TRACE_VULKAN_CALLS`) as the **manual-spot-check** GPU-debug workflow. Note the primary parity gate is now `VK-READBACK-ORACLE-1` (cross-platform, automatable), not RenderDoc (no Metal support) and not Xcode capture (manual). Tracy survives as the cross-platform perf tool. | Xcode capture of one MC2 frame succeeds | — |

---

## 4. Dependency Graph

```
                    ┌─────────────────────────────────────────────────────┐
   WINDOWS BOX      │ Phase 0  (must land FIRST, before any port)          │
   (no Mac needed)  │  ABI-STRUCT-AUDIT ═══(long pole #2)══╗               │
                    │  BRANCH-MERGE ── QPC-SHIM ── METAL-GATE-COMMIT       │
                    │  8.7-DELETE ∥ PERFFIX  (parallel, Win-canonical only)│
                    └────────────────────────────────────────┬────────────┘
                                                              │
                    ┌──────────────────────────────────────────▼────────────┐
   MAC (or clang    │ Phase 1  BUILD BOOTSTRAP  (configure cascade)          │
   cross-compile    │  PCH-PACK-FIX (BLOCKER) · GL-READBACK-STUB (unblocks   │
   for compile-only)│    circular GL-free link) · BOOTSTRAP → DEPS-SDL →     │
                    │    DEPS-FFMPEG → GL-BACKEND-GATE → TOOLGUARD →         │
                    │    SHIM-COMPILE (L, -fpermissive pass) → WINH-CONVERT  │
                    └───────────────────────────┬───────────────────────────┘
                                                │  (needs a Mac to RUN)
                    ┌────────────────────────────▼──────────────────────────┐
                    │ Phase 2  BOOT TO VK SURFACE (order inversion)          │
                    │  VK-READBACK-ORACLE (prereq for all P3 parity) ·       │
                    │  MSL-LIMIT-AUDIT (BLOCKER prereq for P3) ·             │
                    │  VK-WINDOW → VK-SURFACE → PORTABILITY → FAILHARD →     │
                    │  VK-DEPTH · CASE-FOLD-DEBUG · MVK-ICD(M)               │
                    └───────┬───────────────────────────────┬───────────────┘
                            │                                │
          ┌─────────────────▼──────────┐   ┌─────────────────▼───────────────┐
          │ Phase 3  ASSET-FEED (XL)   │   │ Phase 6  INPUT/HiDPI             │
          │  gated behind MSL-AUDIT    │   │  AUTHOR here (disjoint files);   │
          │  TERRAIN → PROP → MECH     │   │  VERIFY blocks on Phase 4        │
          │  → BAKE-PIPELINE           │   │  (mouse oracle needs a UI)       │
          │  oracle = READBACK-ORACLE  │   └─────────────────────────────────┘
          └─────────────────┬──────────┘
                            │
          ┌─────────────────▼──────────┐
          │ Phase 4  FIRST PIXEL       │  ◄── ★ main menu on a Mac
          │  GOS2D → VIDEO → AUDIO     │
          └─────────────────┬──────────┘
                            │
          ┌─────────────────▼──────────┐
          │ Phase 5  MISSION + SMOKE   │  ◄── ★ tier1 exit 0
          │  DEBUGSTATE → SMOKE-PROC   │
          │  → DEPLOY → FILEAPI-AUDIT  │
          └─────────────────┬──────────┘
                            │
          ┌─────────────────▼──────────┐   Phase 7 PATHS/CASE-NORMALIZE
          │ Phase 8  PACKAGING         │   (case-fold debug fallback already
          │  DYLIB-RELOCATE → BUNDLE   │    in P2; normalize+CI+userdata here,
          │  → SIGN(M–L) → LAUNCHER    │    needed before P8 signing;
          └────────────────────────────┘    can start after Phase 5)
```

**Serial gates (critical path):** Phase 0 → 1 → 2 → 3 → 4 → 5. **Two co-equal long poles** sit on it: `MACOS-VK-ASSETFEED-*` (Phase 3, XL) and `ABI-STRUCT-AUDIT-1` (Phase 0, L–XL). `MSL-LIMIT-AUDIT-1` gates Phase 3; `VK-READBACK-ORACLE-1` is a prerequisite for every Phase-3 oracle.

**Two circular gates resolved:**
- Phase-1 "GL-free link" cannot close while islands still call `glGetTexImage` → **`GL-READBACK-STUB-1`** (Phase 1) stubs those sites so the link closes; Phase 3 swaps stubs for real feeds.
- Phase-3 parity cannot use RenderDoc (no Metal) → **`VK-READBACK-ORACLE-1`** (Phase 2) is the cross-platform gate; Xcode capture is manual spot-check only.

**What must land Windows-side first:** `ABI-STRUCT-AUDIT-1` + branch merge (Phase 0). Without the audit, a booting game silently corrupts loaded data. Without the merge, the port is done twice. `VK-INTEROP-DELETE-8.7` + the perf fix run in **parallel** on Windows (only needed before the Windows branch is frozen), not as serial pre-Mac gates.

**What proceeds in parallel:**
- Phase 0 ABI audit / QPC-shim / Phase 1 CMake / pch-pack-fix / shims / `run_smoke.py`+`deploy_payload.py` de-Windowsing are all authorable **before a Mac exists** (compile-only verification via clang cross-compile or a CI Mac).
- Phase 6 (input/HiDPI) code **authoring** touches `gameos_input.cpp` only — disjoint from the renderer islands of Phase 3 — but its **verification** blocks on Phase 4 (the mouse oracle needs a rendered UI).
- Phase 7 (paths/case-normalize + userdata) can start after Phase 5 and *gates* Phase 8 signing (read-only bundle needs the write-redirect first). The **debug case-fold fallback is pulled forward to Phase 2** so dev/CI on case-sensitive volumes can boot at all.
- Phase 8 packaging is a late independent lane.

---

## 5. The Seam List — Windows API Cluster → Chosen Replacement

| # | Cluster | Windows API today | Replacement | Justification (from recon + review) |
|---|---|---|---|---|
| 0 | **On-disk ABI (LP64 vs LLP64)** | `long`/`unsigned long` (4B on Win64) and `wchar_t` (2B) inside `fread`-ed structs | **Fixed-width** (`int32_t`/`uint32_t`/`char16_t`) for every on-disk field + `static_assert(sizeof)` | LP64 makes `long`=8, `wchar_t`=4 on arm64 → wrong offsets → silent save/asset corruption. ~193 `long` fields in 30 mclib headers; the subset backing on-disk records is long pole #2. **Correctness, not hardening.** |
| 1 | **Window / event pump / input** | *(already SDL2)* `SDL_CreateWindow`, `SDL_PollEvent`, SDL scancodes | **Keep SDL2** — no change | The Win32→SDL replacement "already happened years ago (sebi port lineage)." A Cocoa shim replaces ~900 working lines for zero gain; SDL route touches <300 lines. SDL 2.30.11 vendored, full Metal/MoltenVK support. |
| 2 | **VK present surface** | `vkCreateWin32SurfaceKHR` on GL window's HWND (P13b) | **`SDL_Vulkan_CreateSurface`** on `SDL_WINDOW_VULKAN` | Portable pattern proven in 4 in-tree files. MoltenVK adds `VK_EXT_metal_surface`+`CAMetalLayer` automatically. Native, not SDL-vs-Cocoa. |
| 3 | **Audio** | *(already SDL2_mixer 2.8.0)* `Mix_OpenAudio`, `SDL_ConvertAudio` | **Keep SDL2_mixer** (Homebrew arm64) | Zero DirectSound/XAudio in compiled path. **Stay on SDL2** — SDL3 removes `SDL_AudioCVT`/`SDL_ConvertAudio`/`Mix_HookMusic` → forces an `SDL_AudioStream` rewrite (M). SDL2 = zero-work audio. Note CoreAudio's 48 kHz native rate resamples the 22050 open on the callback thread (non-blocking, not free). |
| 4 | **Video decode** | FFmpeg 7.x via `/DELAYLOAD` + SEH probe | **Ordinary dylib link** (Homebrew or vendored `.dylib`) | Delay-load is fully `_WIN32`-guarded; `#else` branch ("link-loaded, always avail") already exists. On macOS the delay-load question evaporates. Bink decoders confirmed in FFmpeg 7.x. Dylib tree needs `@rpath` relocation at package time (seam #16). |
| 5 | **Video display (2D)** | `gos_DrawQuads`/`gos_LockTexture` (GL-only) | **VK-native gos-2D** *or* `video_get_texture()`→ImGui composite | GL 2D pipeline can't exist on VK-only mac. Composite seam already used by `missionselectionscreen`. Renderer-subsystem dependency. |
| 6 | **Timing / clock** | `QueryPerformanceCounter/Frequency`, `GetTickCount64`, `timeGetTime` | **One shared shim** (`mach_absolute_time`/`clock_gettime`) in `platform_mmsystem.cpp` | `timeGetTime` already shimmed; QPC has **no** shim and is load-bearing in `file.cpp:92-99`. Single shim covers ~10 files without touching them. |
| 7 | **File enumeration / attributes** | `FindFirstFileA` (shimmed), `GetFileAttributesExA`/`FILETIME`/`CompareFileTime`/`GetFullPathNameA` (**unshimmed**) | **`stat()`/`realpath`** rewrite of mod-cache mtime logic | Mod-index Win32 additions postdate the Linux shims → guaranteed link failures. `stat()`-based mtime is cleaner than faking `FILETIME`. |
| 8 | **File open/read** | `_open`/`_stat` (already POSIX via `platform_io.h`) | **Keep** — verify only | Core File class ported for Linux build; `platform_io.h` maps `_open`/`_O_*` to POSIX. |
| 9 | **Config persistence** | Registry (`RegOpenKey`, already dead under `LINUX_BUILD`) | **File-backed** `$HOME`/XDG (already implemented) or `~/Library/Application Support` | `gos_LoadDataFromRegistry` already file-backed on POSIX. Registry FIRSTRUN → marker file. |
| 10 | **Crash / debug** | SEH `__try`, DbgHelp, MiniDump, `OutputDebugString` | **`sigaction`+`backtrace_symbols`** (optional); `OutputDebugString`→`fprintf(stderr)` | All debug-only, compile away. Not required to boot. |
| 11 | **GL↔VK external-memory interop** | `vkGetMemoryWin32HandleKHR`, Win32 `HANDLE` semaphores | **Delete** (rung 8.7) | MoltenVK has **no** win32 external-memory extensions. Direct-chain path never touches the handles. Windows-only-deletable; run in parallel, guard out on mac. |
| 12 | **Threading / sync** | Mostly `std::thread`/`std::mutex` already; 2 `Sleep()`, `abldbug` `CRITICAL_SECTION` | **`std::this_thread::sleep_for`, `std::mutex`** | 17 files already std-threaded; DirectSound-era `CreateThread` is in deletable `dw.cpp`. |
| 13 | **Dev-shell socket** | winsock2 (`WSAStartup`/`SOCKET`/`closesocket`) | **BSD sockets** (optional) or keep inert stub | Already compiles as inert `#ifdef _WIN32` stub; game ships without it. High-value for agent tooling — mechanical port. |
| 14 | **WIC image codec** | `windowscodecs`+`ole32`+`uuid` (consumer unlocated) | **stb_image** (already vendored) | `NOT WIN32` link branch already exists. Locate consumer, gate — likely S. |
| 15 | **Launcher GUI** | Raw Win32 (`CreateWindowA`, comctl32) | **Ship without (v1)** → later SDL2+ImGui | Engine falls through when launcher absent; `launcher_env.json` reader ports via `setenv`. Bucketing logic is portable C++. |
| 16 | **Installer / signing + dylib relocation** | Inno Setup + Azure Trusted Signing + 7z | **`.app`/DMG + `dylibbundler`/`install_name_tool` @rpath relocation + inside-out `codesign` + `notarytool`** | Entirely Windows-only. Homebrew dylibs carry `/opt/homebrew` install names → must relocate to `@rpath` in dependency order, then sign inside-out. Developer ID recommended (project already pays for Azure). |
| 17 | **Process automation (smoke)** | PowerShell `Get-CimInstance`, `taskkill`, ctypes lock-probe | **psutil / `ps`/`kill`** | tier1 verdict is engine-side autoplay + heartbeat, no input injection. Preserve anti-stale-exe semantics. |
| 18 | **Atomic file replace** | `MoveFileExW(REPLACE_EXISTING)` | **`std::rename`** (atomic same-dir APFS) | 2 sites; load-bearing for debug_state dump smoke gate. |
| 19 | **GPU parity / capture** | RenderDoc (`renderdoc.dll`) | **`VK-READBACK-ORACLE-1`** (cross-platform, automatable) + Xcode GPU Frame Capture (manual) | RenderDoc has no Metal support; `dlfcn` branch is a permanent no-op. The **automatable** parity gate is in-engine `vkCmdCopyImageToBuffer`→hash/SSIM; Xcode capture is a manual spot-check. Workflow gap, not code gap. |
| 20 | **Toolchain struct packing** | `#pragma pack(4)` file-scope in `pch.hpp` | **`#pragma pack(push,4)`/`pop`**, system includes outside the packed region | Bare pack(4) mis-lays-out MoltenVK/SDL/Foundation system structs → memory corruption. BLOCKER, not warning. |
| 21 | **MSL per-stage limits** | GL/AMD's generous binding limits (SPIR-V assumed to "just map") | **Audit sampler ≤16 / buffer ≤31 per stage; argument-buffer / split-pass remediation where over** | Metal caps 16 samplers / 31 buffers per stage; `gos_terrain.frag` already has 9 samplers before material+CSM arrays. Deterministic pipeline-creation failure, not a "surprise." |

**Windowing decision, justified:** SDL2 is already the audio, windowing, input, *and* proven-VK-surface layer. The decision "follows the audio" and everything else — every subsystem independently landed on "keep SDL2." **Do not build a Cocoa shim.** The only open SDL question is **SDL2 vs SDL3**, and the audio TU forces the answer: **stay on SDL2** (SDL3 = a mandatory `SDL_AudioStream` rewrite for zero functional gain on this workload).

---

## 6. Risk Register — Top 13

| # | Risk | Severity | Mitigation | Early-detection probe |
|---|---|---|---|---|
| 1 | **LP64 `long`/`wchar_t` on-disk struct divergence.** `fread`-into-struct records with `long`/`wchar_t`/enum/bitfield fields read at the wrong offset/size on arm64 → **silent** save-game and asset corruption. ~193 `long` fields across 30 mclib headers; unknown subset is on-disk. | **BLOCKER** | `ABI-STRUCT-AUDIT-1` (Phase 0, long pole #2): convert on-disk fields to fixed-width + `static_assert(sizeof)`. | Byte-diff a Windows-parsed vs mac-parsed struct dump for one `.fit`, one save, one `.pak` header — must be identical. |
| 2 | **Bare `#pragma pack(4)` in `pch.hpp`** parses MoltenVK/SDL/Foundation system headers under pack(4) → mis-laid-out `NSObject`/`CAMetalLayer`/`dispatch` → memory corruption. | **BLOCKER** | `PCH-PACK-FIX-1` (Phase 1): `push`/`pop` scoping; all system includes outside the packed region. | Scratch TU: include a MoltenVK/SDL header after pch, `static_assert` a system struct's `sizeof`/`alignof`. |
| 3 | **Metal per-stage limits (16 samplers / 31 buffers)** + CSM layered-render / multiview / compute / MDI. `gos_terrain.frag` already crowds samplers; METAL-GATE's "only `vkCmdDrawIndexed`" contradicts CLAUDE.md's SSBO/compute/MDI engine — the audit may have covered only the islands. | **BLOCKER** | `MSL-LIMIT-AUDIT-1` (Phase 2, gates Phase 3): `spirv-cross --msl` per-stage count over the **full** corpus; remediate with argument buffers / split passes; verify no multiview/geometry-shader dependency. | Per-pipeline sampler/buffer count from MSL; pipeline-create each under MoltenVK before trusting Phase 3. |
| 4 | **Asset-feed XL is mispriced as "platform coupling."** GL-readback texture feed sits *outside* WSI seams and is long pole #1. | High | Schedule it as a center of gravity; use the water-island disk-sidecar template. Sequence terrain→prop→mech by parity risk. | Spike `MACOS-VK-ASSETFEED-TERRAIN-1` on **one** texture array end-to-end; measure days-to-parity, extrapolate. |
| 5 | **Boot-order inversion (fail-soft-to-GL → fail-hard) is architectural.** VK boots GL first everywhere; macOS has no GL rescue. | High | Land 8.7 + direct-chain as the *only* path on Windows first, so the inversion is against already-proven code. | Negative control: induce a VK-init failure on the merged Windows branch with direct-chain forced — confirm no GL path reachable. |
| 6 | **Circular gates.** GL-free link (Phase 1) blocked by asset-feed (Phase 3); Phase-3 parity oracle can't be RenderDoc (no Metal). | High | `GL-READBACK-STUB-1` (Phase 1) closes the link; `VK-READBACK-ORACLE-1` (Phase 2) is the cross-platform gate. | Verify `nm` GL-free after stub lands; verify readback-oracle matches a RenderDoc reference on Windows. |
| 7 | **Configure-cascade hides depth.** Fixing SDL2 reveals FFmpeg FATAL, then tool `zlib.lib` FATAL, then GLEW link. | Med | Budget Phase 1 as an iterative unblocking loop; guard *all* Windows-only subdirs in one sweep (`MACOS-TOOLGUARD-1`). | First `cmake -G Ninja` on macOS — log every `FATAL_ERROR` in one pass. |
| 8 | **`-fpermissive` bit-rot / `#else` paths never compiled since Linux-port era.** AppleClang may surface hundreds of const-char*/narrowing errors; `PLATFORM_WINDOWS` always-defined means every `#else` is untested. | Med (schedule) | `MACOS-SHIM-COMPILE-1` re-sized to **L**; dedicated compile-fix pass; expect churn not redesign. | Scratch clang `-fsyntax-only` over `mclib`/`code`/`GameOS/src/platform_*` *before* the full port — the failure set is the true shim gap. |
| 9 | **HiDPI mouse dead-zone.** Normalize/warp tuned to Windows raw==physical; Retina delivers logical points at 2× → cursor pins to 0..0.5. Verification needs a rendered UI (blocks on Phase 4). | Med | Single points→pixels scale in all normalize/warp/canvas-box denominators; confirm SDL 2.30 point/pixel behavior on hardware first. | `MC2_MOUSE_RECON` on hardware after Phase 4 — measure edge reachability. |
| 10 | **Case-sensitive volume = whole-class failure at first boot.** `S_strlwr` lowercases every open unconditionally; dev/CI clones on case-sensitive APFS break every mixed-case open in Phase 2. | Med | `CASE-FOLD-DEBUG-1` pulled forward to Phase 2 (debug fallback); packaging normalize + CI gate in Phase 7; verify FST lookup independently case-insensitive. | Boot on a case-sensitive APFS volume — every failed open logs the offender. |
| 11 | **cwd-relative writes into a read-only signed bundle break the code-sign seal.** `fopen("options.cfg")` is representative, not exhaustive. | Med | chdir-to-resource-root interim + redirect writable files to Application Support; full grep-audit of write sites before Phase 8. | `codesign --verify` after a run that writes options.cfg — a broken seal means a missed write site. |
| 12 | **D32S8 vs D24 changes depth precision** that Windows parity fixes (FAR-BAND-BLACK-1) were tuned against; a **macOS re-baseline** of depth-sensitive parity is required and is scheduled nowhere in the original. | Med | Native-only mode removes the D24-for-GL-parity rationale; **budget the re-tune** (the hidden-M on `MACOS-VK-DEPTH-1`); re-baseline on macOS/D32S8, don't chase Windows byte-match. | Capture fresh D32S8 reference frames via `VK-READBACK-ORACLE-1` on the first green mac scene; compare COVERAGE not magnitude. |
| 13 | **MoltenVK SPIR-V / validation config trap + SSBO robustness.** `.spv` corpus validated only on AMD Windows drivers; validation layers **don't load in the ship-ICD-direct config**; `robustBufferAccess` (OOB SSBO read → 0) that AMD/GL tolerated is not guaranteed under Metal. | Med | Dual-config explicit (dev-loader for validation vs ship-ICD-direct); one-time MoltenVK validation pass in the dev-loader config; enumerate shaders relying on robust-access. | Boot each island's pipeline under `MVK_CONFIG_DEBUG=1` + validation via the **loader route**; grep SSBOs for unbounded indexing. |

**Upgraded from "polish" to correctness/UX (from review):** the **CAMetalLayer `nextDrawable` beachball** — multi-second mission loads that don't pump the runloop will have WindowServer stall the app (`nextDrawable` blocks up to ~1s; over-holding drawables compounds it). Add `SDL_PumpEvents` in load loops and bound drawable acquisition; this is a load-path correctness issue, not cosmetic.

*(Honorable mentions retained: Gatekeeper UX without Developer ID; multi-GB notarization uploads; `mclib/fastfile.cpp` unaligned pointer-casts into `#pragma pack(1)` structs — benign-but-UB on Apple Silicon, run under UBSan.)*

---

## 7. "First Pixel" Milestone Definitions

### ★ FIRST-PIXEL (main menu on a Mac) — minimum slice set
The smallest set that puts the MC2 main menu on screen:

**Phase 0:** `ABI-STRUCT-AUDIT-1` (a corrupted-struct game may boot but must not — the audit is not deferrable past a booting build), `MACOS-BRANCH-MERGE-1`, `QPC-SHIM-CENTRAL-1`
**Phase 1:** `PCH-PACK-FIX-1`, `GL-READBACK-STUB-1`, `MACOS-BUILD-BOOTSTRAP-1`, `MACOS-DEPS-SDL-ZLIB-1`, `MACOS-GL-BACKEND-GATE-1`, `MACOS-TOOLGUARD-1`, `MACOS-SHIM-COMPILE-1`, `MACOS-WINH-CONVERT-1` *(FFmpeg may be `MC2_ENABLE_VIDEO=OFF` to defer)*
**Phase 2:** `VK-READBACK-ORACLE-1`, `MSL-LIMIT-AUDIT-1`, all six boot slices (`VK-WINDOW`, `VK-SURFACE`, `VK-PORTABILITY`, `VK-FAILHARD`, `VK-DEPTH`, `MVK-ICD`), `CASE-FOLD-DEBUG-1`, `MACOS-VK-MAIN-SHIM-1`
**Phase 4:** `MACOS-VK-GOS2D-1` (the 2D/UI path — the menu is drawn through it), `MACOS-AUDIO-SOFTEN-1`

*Deferred for this milestone:* Phase 3 full asset-feed (the menu is UI, not a terrain scene — if the menu background is a static image it rides gos-2D, not the islands), video (SmokeMode-skip the intro), input verification (blocks on this milestone), packaging.

**Oracle:** MC2.app launches, MoltenVK device selected, main menu renders and is navigable, clean exit. Confirmed via screenshot + `MC2_DEBUG_STATE_DUMP` showing `backend=vulkan`, no GL context.

> **Caveat (top unknown to resolve first, §9 #1):** if the main menu background renders through a terrain/scene island rather than gos-2D, a slice of Phase 3 (`ASSETFEED-TERRAIN`) — and therefore `MSL-LIMIT-AUDIT-1` passing on the terrain pipeline — is pulled forward into the First-Pixel set.

### ★ MISSION-TIER1 (playable mission + proof gate) — minimum slice set
Everything in First-Pixel **plus**:

**Phase 3:** `ASSETFEED-TERRAIN`, `ASSETFEED-PROP`, `ASSETFEED-MECH`, `BAKE-PIPELINE` (the full XL — a mission scene needs all island texture feeds), gated behind `MSL-LIMIT-AUDIT-1`, oracles via `VK-READBACK-ORACLE-1`
**Phase 4:** `MACOS-VIDEO-DISPLAY-1` (briefing movies) *or* SmokeMode-skip
**Phase 5:** `DEBUGSTATE-POSIX`, `SMOKE-PROC`, `DEPLOY`, `FILEAPI-AUDIT` (mod overlay for tier1 maps)
**Phase 6:** `HIDPI-MOUSE` (mission needs a working cursor for RTS control — now verifiable, since Phase 4 provides the UI the oracle needs)

**Oracle:** `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs` **exit 0** on Apple Silicon (mc2_01, mc2_03, mc2_10, mc2_17, mc2_24, 30 s each) — verdict from engine-side `MC2_SMOKE_MODE` autoplay + heartbeat + debug_state crash-summary, **no input injection required**. This is the project's canonical proof gate, now green on macOS. Additionally: `ABI-STRUCT-AUDIT-1` byte-diff clean on the tier1 maps' loaded records (proves no silent corruption in a running mission).

---

## 8. Hardware / CI Story

**Developer needs (one-time):**
- **An Apple Silicon Mac** (M1 or later). arm64-only target — no Intel Mac, no universal2 (universal doubles the binary and the MoltenVK test matrix for 2019-and-earlier hardware that lacks Metal headroom for this GPU-heavy workload).
- **Xcode Command Line Tools** (clang, `codesign`, `notarytool`, `hdiutil`) — plus `dylibbundler`/`install_name_tool` (CLT-provided) for the Phase-8 relocation. Full Xcode only if using GPU Frame Capture (a manual spot-check; the automatable parity gate is `VK-READBACK-ORACLE-1`).
- **LunarG Vulkan SDK for macOS** — ships `glslangValidator`/`glslc` (SPIR-V toolchain is platform-neutral; MoltenVK consumes SPIR-V directly), `spirv-cross` (for `MSL-LIMIT-AUDIT-1`), and the Khronos loader + validation layers for the **dev-loader** config. Release builds bundle `libMoltenVK.dylib` directly (volk points at it) — **do not depend on a user-installed SDK**. Validation runs **only** in the dev-loader config (see Phase 2 dual-config note).
- **Homebrew** for SDL2 / SDL2_mixer / SDL2_ttf / FFmpeg 7.x arm64 (or vendored `.dylib` trees) — remember these carry `/opt/homebrew` install names that Phase 8 must relocate to `@rpath`.
- **$99/yr Apple Developer ID** (strongly recommended) — macOS 15+ removed the right-click-Open Gatekeeper bypass; ad-hoc signing forces a per-machine Settings approval that generates support noise. The project already pays for Azure signing on Windows, so this is a parity cost.
- **Deployment target decided: macOS 13.0** (resolves the header's "12+" vs Unknown-#9 contradiction) — comfortably above the MoltenVK/Metal floor for BC6H/BC7 on Apple Silicon and current-enough for the SDK APIs used; revisit only if a required API needs 14+.

**Realistic remote-iteration loop (primary dev box is Windows):**

The recon supports a **Windows-authored, Mac-verified** loop that keeps most work on the primary box:

1. **Author on Windows** — Phase 0 ABI audit, Phase 0/1 CMake, pch-pack-fix, shims, `run_smoke.py`/`deploy_payload.py` de-Windowsing, and even the surface/portability edits are all authorable and *reviewable* on Windows. The Windows build must stay green throughout (every `#else`/`APPLE` branch is additive; the ABI audit is byte-identical on LLP64).
2. **Compile-verify remotely** — a **GitHub Actions macOS runner** (arm64) or a Mac mini as a CI node runs `cmake -G Ninja` + build on push. This catches the configure cascade (Risk #7) and shim bit-rot (Risk #8) without a local Mac — Phases 0–1 need *no interactive Mac*.
3. **Run-verify on the Mac** — Phases 2+ need a real Apple GPU. Deploy via `scripts/deploy_payload.py` (parameterized in `MACOS-DEPLOY-1`) to the Mac, run `run_smoke.py --tier tier1`. The MCP render-state bridge (`get_render_health`, `get_latest_smoke_report`, `get_runtime_gate`) is **read-only JSON tailing** and works on macOS once `MC2_DEPLOY_DIR` points at the mac install — so the Windows agent can inspect a running mac engine's state remotely.
4. **CI caveat:** GitHub macOS runners are **headless** — engine-side `MC2_SMOKE_MODE` autoplay works (no GUI automation needed for tier1), but **menu-canary / `pyautogui` / camera-sweep harnesses need TCC Accessibility + Screen Recording grants** that hosted runners may never provide. Keep those tests on the interactive Mac; they are explicitly *out of scope* for the tier1 gate (which forbids `--with-menu-canary` anyway). Note also that the **audio "one live listen" oracle** is not headless-runnable — interactive box only.
5. **GPU-debug caveat:** the RenderDoc-first evidence protocol has **no automated macOS equivalent via RenderDoc** — but `VK-READBACK-ORACLE-1` (`vkCmdCopyImageToBuffer`→hash/SSIM, cross-platform, automatable) *is* the parity gate and closes most of that gap. Xcode GPU Frame Capture remains a **manual** spot-check for deep pipeline debugging, which slows the VK/Metal debugging loop for hard cases; budget extra time for Phase 3 parity work on genuinely divergent frames. Tracy (cross-platform, always-compiled) survives as the perf tool.

**Recommended minimum viable setup:** one Mac mini (M-series) as both the CI run-node and the interactive debug box + GitHub Actions macOS-arm64 for compile-gating. This lets Phases 0–1 land entirely from Windows and reserves the Mac for Phase 2+ run-verification.

---

## 9. Honest Unknowns — With Resolving Probe

| # | Unknown | Resolving probe | Blocks |
|---|---|---|---|
| 1 | **Does the main menu background render through gos-2D or a terrain/scene island?** Determines whether First-Pixel needs a slice of Phase 3 (+ terrain MSL audit) pulled forward. | Trace `mainmenu.cpp` draw path on the running Windows VK build — check whether the menu backdrop hits `gos_DrawQuads` or an island. | First-Pixel scope |
| 2 | **Exactly which structs are on-disk?** The ~193 `long` fields include pure-runtime ones; only the `fread`/`memcpy`-from-buffer subset needs fixed-width conversion. | Cross-reference every `fread`/`File::read`/`memcpy` target type against the header field list; the intersection is the audit scope. | `ABI-STRUCT-AUDIT-1` sizing (Risk #1) |
| 3 | **Are rung 8.7a-h interop deletions merged into the worktree HEAD (`32e1e976`) or on another branch?** | `git log`/`git branch --contains` for the 8.7 deletion commits; confirm against the merged Phase-0 branch. | Phase 0 sequencing |
| 4 | **Is FST internal directory lookup case-insensitive independent of `file.cpp`'s lowercasing?** If not, the lowercase-normalize plan is incomplete. | Read `mclib/fastfile.cpp` name-matching; run `GAMEOS-PATHSEP-HARNESS-1`-style unit on a mixed-case FST entry on a case-sensitive volume. | `MACOS-CASE-NORMALIZE-1` (Risk #10) |
| 5 | **Exact SDL 2.30 macOS mouse-coordinate behavior** for `FULLSCREEN_DESKTOP`+`ALLOW_HIGHDPI` (points vs pixels) — sets the sign/direction of the HiDPI fix. | Measure on hardware with `MC2_MOUSE_RECON`; do not assume. | `MACOS-HIDPI-MOUSE-1` |
| 6 | **Does any `.spv` in the deployed set exceed Metal's 16-sampler/31-buffer per-stage caps, use multiview/`gl_Layer` CSM, or a compute/MDI path** the island-only METAL-GATE audit missed? | `spirv-cross --msl` per-stage count over the full corpus + pipeline-create under MoltenVK (`MSL-LIMIT-AUDIT-1`). | Phase 3 (Risk #3) |
| 7 | **Full inventory of cwd-relative *write* sites** (saves, logs, sidecars, `debug_state/`, `imgui.ini`) — only representative sites cited. | Grep-audit slice for all `fopen(...,"w"/"a")` + `std::ofstream` reaching non-absolute paths. | `MACOS-USERDATA-PATHS-1` (Risk #11) |
| 8 | **Does the renderer arc plan a VK-native gos-2D pipeline,** or must video/UI go the ImGui-composite route? No VK 2D exists on `vk-interop-bridge` HEAD. | Confirm with the renderer/VK-arc owner; check for any in-flight `gos_DrawQuads` VK slice. | `MACOS-VK-GOS2D-1` / video display |
| 9 | **Who consumes `windowscodecs`/`ole32`/`uuid` (WIC)?** Unlocated in the build-file pass. | Grep for `IWICImagingFactory`/`CoCreateInstance` image-codec calls; the `NOT WIN32` branch already omits these libs — find what breaks. | `MACOS-DEPS` / stb_image swap |
| 10 | **BC6H/BC7 support on the target M-series device** (audit marks NEEDS-VERIFY; believed fine on all AS + macOS 11+; target now pinned to 13.0). | Query `vkGetPhysicalDeviceFormatProperties` for BC feature bits on an M-series device. | (deployment target already decided 13.0) |
| 11 | **Has `PLATFORM_WINDOWS`-off ever compiled with the *current* source?** No non-Windows CI/toolchain file found. The Linux shims are untested against remaster-era call sites. | Stand up the GitHub macOS-arm64 compile job (§8) — its first failure set is the empirical answer. | Phase 1 pricing (Risk #8) |
| 12 | **Which shaders rely on `robustBufferAccess`** (OOB SSBO read → 0) that AMD/GL tolerated but Metal/MoltenVK may not guarantee? | Grep SSBO indexing for unbounded/computed indices; test each under MoltenVK with robust-access queried. | Phase 3 correctness (Risk #13) |
| 13 | **Does `MVK_CONFIG` present-family assumption hold** (single gfx+present family on Apple Silicon)? Present path hard-requires gfx==present. | Log `vkGetPhysicalDeviceQueueFamilyProperties` on the target M-series GPU. | `MACOS-VK-SURFACE-1` (expected: holds) |
| 14 | **Does the asset-feed rewrite add new bake/feed tools** to the pipeline? METAL-GATE-AUDIT-2 flags this as possible. | Falls out of the `MACOS-VK-ASSETFEED-TERRAIN-1` spike (Unknown #1's probe doubles here). | Phase 3 tool scope |
| 15 | **Does `mclib/fastfile.cpp` do unaligned pointer-cast reads into `#pragma pack(1)` structs?** Benign on Apple Silicon (unaligned loads legal) but UB — and interacts with the pch pack fix. | Spot-check `fastfile.cpp` reads; run under UBSan on the first mac build. | Low — hardening only |
| 16 | **Packaging model: fully-embedded `.app` vs split-zip + first-run data-locator?** Multi-GB data + 2 GB GitHub asset cap + notarization upload size all interact. | Decide with project owner; measure notarization upload time for an embedded build vs a first-run-locator engine-only build. | Phase 8 shape |

---

**Bottom line:** The VK ownership ladder already converted the impossible ("port a GL 4.3 renderer to a platform that caps at GL 4.1") into the merely large. But "merely large" now has **two co-equal long poles**, not one: finishing the VK-native asset feed (the texture-feed producer GL always supplied) **and** fixing the LP64 on-disk ABI the codebase has never had to fix — plus two boot-blocking toolchain BLOCKERs (`pch.hpp` pack scoping, MSL per-stage limits) and two circular gates (GL-free link needs a readback stub; Phase-3 parity needs an in-engine oracle, not RenderDoc). The critical path is **Phase 0 (land the ABI audit + 8.7 + perf + merge on Windows) → GL-free boot → asset-feed XL → first pixel → tier1 smoke**. Everything else is a parallelizable long tail of S/M POSIX shims. The single decision that most de-risks the *renderer* schedule is spiking `MACOS-VK-ASSETFEED-TERRAIN-1` on one texture array early to price the XL honestly; the single decision that most prevents a **silently corrupt** shipping build is starting `ABI-STRUCT-AUDIT-1` on day one; and the single fact that most enables remote work is that Phases 0–1 need no interactive Mac, only a headless macOS-arm64 compile runner.