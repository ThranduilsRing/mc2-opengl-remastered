<!-- doc-status: stale | reviewed:2026-10-11 -->
# Opus adversarial consistency/duplication review (2026-10-09, HEAD f980105df, read-only)

Method: read the prior cleanup reports, then re-derived from code. Ran a throwaway token-window clone scanner (normalized lines, comments stripped; shaders W=15, C++ render dirs W=20). Ran the read-only Python checkers, including the ones nothing wires. I did no builds and no launches. All line numbers are from HEAD f980105df.

Clone totals: shaders have 86 clone runs (2768 duplicated lines). GameOS/RenderCore/GameAdapters/RenderWorld C++ has 404 runs (8653 duplicated lines).

---------------------------------------------------------------------------------------------------
## A. Ranked findings (bug risk first)

### A1. [HIGH, live data bug] terrainTypeToMaterial has 5 copies, and one is already stale
- Canonical: `mclib/quad.cpp:301-321`. Cases 10, 13-20 map to Concrete; 20 is END_CEMENT_TYPE.
- Copies that match: `mclib/terrain_init.cpp:245-251` (lambda), `mclib/terrain.cpp:4928-4934` (lambda), `GameOS/gameos/gos_terrain_indirect_atlas.cpp:15-23`.
- **Divergent copy:** `GameOS/gameos/gos_terrain_water_stream.cpp:360-368` has no `case 20`, so type 20 maps to Rock there and to Concrete everywhere else. Its comment (`:354-359`) says "any drift surfaces at the parity check's frgb_lo the next time PARITY_CHECK runs". No offline gate runs that.
- Fix: one `constexpr` table in a leaf header (e.g. `mclib/terrain_material_map.h`) used by all 5 sites, plus a doctest. If a shader needs the table, emit it through gpu_abi.

### A2. [HIGH, false safety] Two existing offline provers are unwired, and both report problems today
- `tools/glsl_port_diff.py --all` exits 2 and prints 6 DIVERGES plus 1 missing marker:
  - `calc_light`: the GL kernel (`shaders/include/lighting.hglsl:272`) has a `sunAxisMc2Fix` 5th parameter. The VK re-port (`shaders/vulkan/mech_tier1.vert:104`) does not. When `MC2_SUN_AXIS_FIX` is flipped (gate amnesty), VK mech lighting will silently stay wrong. The registry doc (`RendererFeatureRegistry.h:787ff`) admits this; nothing enforces it.
  - `waterBaseColor`: the GL shore-meander block (`u_shoreMeanderWu` + fbm, gos_terrain_water_mdi.frag) is missing from `shaders/vulkan/water_geom.frag`.
  - `waterReflSetup`, `lodNear`, `skirtVertMath`, `calcDynamicShadowCsm`: rename-only differences. They still fail the prover, which makes it unusable as a gate until a rename map exists.
  - `visualDisplaceFar`: markers are missing in `shaders/terrain_lod_chunk.vert`, so the registry row is dead.
- `tools/check_ubo_layout.py` (13 VK UBO pairs) prints `OFFSET MISMATCH` for terrain and terrain_overlay and still **exits 0**. The mismatch is a parser false positive: `terrain_tier1.frag:157-161` declares 3 floats per line (`float tintRockR; float tintRockG; ...`) and the regex reads only the first. So it is unwired, cannot fail, and is misparsing. The seam2 plan names it as the legacy fallback for every pair gpu_abi does not cover. In practice that coverage is zero.
- `tools/check_vk_shaders.py` is also unwired.
- `scripts/check-contract-coverage.py` and `manual-checks.allowlist` scan only `scripts/`, so orphaned checkers in `tools/` are invisible.

### A3. [HIGH, red now] gate_default_agreement fails at HEAD
`py -3 scripts/check-gate-default-agreement.py --quiet` exits 1 with `READER_CONFLICT MC2_BUCKET_CENSUS`: `gos_terrain_patch_stream.cpp:314` is classified `[ON] if(unset) return true` and `mclib/txmmgr_render_passes.cpp:555` `[OFF]`. Both readers actually do `getenv()!=nullptr` (OFF). The conflict appeared with f980105df (logging facade P1 converted this file's fprintf calls to `MC2_EVENT`). The classifier's statement window probably now straddles `s_killswitch = (env == nullptr) || ...` on `:312`. Either fix the classifier or name the reader. **Tell the writer agent: check-contracts is not green.**

### A4. [HIGH, structural] The VK terrain shaders are a 3-way copy, and the extracted include is unused
- `shaders/vulkan/terrain_tier1_skirt.frag` is a byte copy of `terrain_tier1.frag`: 544 normalized lines; only the header differs, and the header itself says "keep in LOCKSTEP by copying". The `.vert` pair shares 92 lines.
- `shaders/vulkan/include/terrain_frame_vk.glsl` was extracted (UBO, CSM helpers, `mv_calcDynamicShadowCsm`), but only the overlay and decal shaders include it. terrain_tier1 and skirt keep private copies: 85 + 53 + 21 lines each.
- `TerrainFrameUBO` therefore appears as 3 full GLSL copies (tier1.frag, skirt.frag, terrain_frame_vk.glsl), 2 prefix copies (tier1.vert, skirt.vert, 75 of 92 members) and the C++ struct (`vulkan_terrain_island.cpp:329`). check_ubo_layout (unwired) covers 2 of the 5 GLSL copies. The skirt copy is guarded only by 3 pinned constants in `check-lighting-divergence.py:283-309`.
- Fix: in `GameOS/gameos/CMakeLists.txt:359`, let the bake loop map basename `terrain_tier1_skirt` → source `terrain_tier1.frag`, or make skirt.frag `#include "terrain_tier1.frag"` behind a define. Have tier1 include `terrain_frame_vk.glsl`. Move TerrainFrameUBO to gpu_abi (std140 member-list macro) so the C++ static_asserts and the GLSL come from one source.

### A5. [HIGH] VK ports silently drop default-OFF GL features, so gate amnesty will create VK drift
Each GL twin has a feature that its VK twin lacks:
- `fog_oob.frag` and `edge_fog.frag` sky-exclude (`u_skyExcludeEnabled`, `gos_postprocess.cpp:3632`) are absent in `shaders/vulkan/{fog_oob,edge_fog}.frag`.
- Shore meander is absent in water_geom.frag (A2).
- SUN_AXIS_FIX is absent in mech_tier1.vert (A2).

Flipping any of these gates ON (CLAUDE.md "features ship ON") changes GL only. Proposed check: for each GL↔VK twin pair (table in gl-vk-duplication.md §1), every `u_*Enabled`/`u_*Wu` uniform consumed by GL must be in the VK UBO or listed in a `VK_NOT_PORTED` table with its gate name. check-new-gates / gate-amnesty should refuse a flip while the gate is in that table.

### A6. [MED-HIGH] Mirrored structs that still have no check (gpu_abi covers 1)
- `GpuMechInstance`: C++ `gos_mech_batcher.h` plus 3 GLSL copies (`mech.vert`, `shadow_mech.vert`, `vulkan/mech_tier1.vert`; layouts agree, pad names differ). This is the struct of memory `mech_shadow_instance_struct_stride_bug`. No checker covers it. **Next gpu_abi candidate.**
- `ObjLightsMirror` (`vulkan_color_prop_island.cpp:496`, literal `[32]`) is a 6th light-ABI site that `check-light-abi-lockstep.py` (5 sites) does not know about.
- `TG_LIGHT_*` enum is defined 4 times: `mclib/tgl.h:163`, `shaders/include/lighting.hglsl:17`, `vulkan/mech_tier1.vert:75`, `vulkan/color_prop_tier1.vert:45`.
- `shadowKnobFlags` bit meanings (b0 FADE_CONT, b1 RAMP0, b2 ALT_SCALE, b3 BANDFIX_V2) exist only as comments in 6 places (`gameos.hpp:2438`, `GameOS/gameos/vulkan_terrain_island_run.cpp:272`, `.h:200`, 3 shaders).
- Static-prop vertex attribute offsets 0/12/24/32/36 are hard-coded at `vulkan_color_prop_island.cpp:1174-1178`. The stride is single-sourced; the attribute offsets are not.
- Same block name, different struct: `ShadowFrameUBO` is mat4 + instBase (`shadow_prop.vert:38`, "column-major") versus mat4[4] + cascadeIndex (`shadow_depth.vert:23`, "row-major"). Both are mirrored in C++ (`vulkan_shadow_island_live.cpp:206`, `vulkan_backend_skeleton.cpp:6248`).
- UBO PODs carrying only a `static_assert(sizeof)` (no field check): MechFrameUBO, LegacyFrameUBO, SkyPC, OvlPC, HudTexPush (`engine_present_vk.cpp:628`), ScreenFxShadowStaticParams, HzbLevelParams.
- `check-vk-pipeline-layout.py` covers descriptor and push-constant *presence/size*, not member offsets. Its default `--cpp` glob is `vulkan_*.cpp`, so `engine_present_vk.cpp` (HUD pipelines) is not linted. A pipeline with no `.spv` name on the line before its binding decl gets no shader comparison (silent). `VBO_CONTRACT` lists 3 shaders.

### A7. [MED] (DONE: D7 phase 1, 938ad14b2) The VK probe fixture is out of date and is mistaken for a live path
`vulkan_backend_skeleton.cpp` (7640 lines, runs only when `MC2_VULKAN_PROBE` is set, `gos_render.cpp:677`) carries stale mirrors:
- `EdgeFogParams` is 100 B (`:1988`), while the shader and the native path are 112 B (`vulkan_post_fog_native.cpp:142`, horizon clamp added).
- `FogOobParams` is 84 B (`:2690`) against 96 B.

The probe binds a smaller UBO than the shader block, which triggers validation errors or reads garbage. `vulkan_shadow_island.cpp` is called only from this probe. Its header still says "MC2_VULKAN CMake option, default OFF" (`CMakeLists.txt:122` is ON), and `docs/ARCHITECTURE-MAP.md` lists it as the VK shadow island beside `_live`. It also holds many internal clones (55/50/42-line blocks). Proposal: retire the probe fixtures, or at minimum drop them from the arch map and mark them FIXTURE.

### A8. [MED] VK island boilerplate copied 13 times
- `spv_dir()` + `load_spv()` are copied in 13 TUs (`vulkan_{water,vfx,terrain,sky,shadow_island_live,mech,legacy_shape,color_prop}_island.cpp`, `vulkan_post_{ssao,screenfx,hzb,fog}_native.cpp`, `engine_present_vk.cpp:354-360`). `mc2_vulkan_load_spv` already exists (`vulkan_backend_skeleton.h:33`).
- `vlog()` is copied in 13 TUs; `log()` appears in the skeleton and `asset_scale.cpp`.
- The 2-color + depth LOAD render-pass setup is cloned across `vulkan_mech_island.cpp:761-809`, `vulkan_legacy_shape_island.cpp:389-437` and `GameOS/gameos/vulkan_terrain_island_overlay.cpp:373-408`.
- The post_{fog,ssao,screenfx} descriptor/pipeline setup is a 3-way clone (`vulkan_post_fog_native.cpp:325-365` ~ `screenfx:422-460` ~ `ssao:429-457`).
- Risk: a fix to one (for example the SPV path resolution, or a subpass dependency mask) does not reach the others.
- `docs/tier1_env_vars.md:110` says `MC2_VULKAN_SPV_DIR` is "Only consulted when MC2_VULKAN_PROBE is set". That is false: 13 islands read it. It is also only allowlisted (`check-env-registry.sh:877`), not registered.

### A9. [MED] Duplicated C++ helpers
- `textureOrKtxSidecarExists` exists 3 times, identical: `mclib/bdactor.cpp:88`, `mclib/gvactor.cpp:34`, `mclib/mech3d_init.cpp:8`. A 4th reader of the same env var, `mclib/txmmgr_texture.cpp:353`, parses differently: int mode 0/1/2 via `atoi` versus `v[0]!='0'` bool. So `MC2_TEXMGR_KTX_PRIMARY=false` or `=2` behaves differently in the actor-existence probe and the actual loader. This matters for mod installs.
- `isAllConcreteTerrainBatch` exists twice, semantically identical: `mclib/txmmgr.cpp:529` (DWORD) and `GameOS/gameos/gameos_graphics.cpp:160` (int). It depends on the frgb material byte that A1 produces.
- Others: TinyJson parser (`terrain_material_lib.cpp:37-85` ~ `visual_tuning_profile.cpp:51-98`, "kept independent on purpose"), `inMapBounds` (3), `agsqrt` (5), `rgb2hsi`/`hsi2rgb` (2), `envIsSet` (2), `executableDirectory` (2), `argbToImU32` (2).
- GL-GL shader clones: `gos_terrain_water_fast.vert` ~ `gos_terrain_water_fast_mdi.vert` (~130 lines across 5 runs), `static_prop.frag:85-124` ~ `static_prop_depth.frag:37-74`, and `noise.hglsl` body re-typed into `cloud.frag`, `shadow_screen.frag`, `vulkan/cloud_shadow.frag` and `vulkan/shoreline.frag`. The port prover checks 2 of those 4; none uses the include.
- Dither: props are single-sourced (`include/prop_dither.hglsl`). `gos_vegetation_card.frag:42` keeps its own Bayer table with k/16 thresholds, while prop_dither uses (k+0.5)/16, so a==0 never discards there. This may be deliberate, but it is undocumented.
- Shading: two `calc_light` definitions and `kAmbientFloor = 0.35` x3 (pinned). `docs/lighting-divergence-map.md:28,244` justifies the VK re-port as "forced by the VK toolchain / cannot use the GL #include". That is **no longer true**: `vulkan/mech_tier1.frag:66-67` includes `../include/*.hglsl`. The duplicate is now removable.
- Shadows: `include/shadow.hglsl` cascade/PCF versus `vulkan/include/lighting_shadow_vk.glsl` (15-23-line clones at `:122-146`, `:253-301`, plus internal self-clones), and `shadow_screen.frag:108-148` versus `vulkan/screen_shadow_{static,csm}.frag`.
- Tonemap: single (`postprocess.frag:201-208`). VK post reuses it (no clone found).

### A10. [MED] Ten ways to read a boolean env var, with incompatible truthiness
- Volume: 1016 distinct `MC2_*` vars read by raw `getenv`, at 1438 call sites in 211 files.
- About 45 local helpers exist. A sample of semantic splits:
  - `=false`/`=off` reads as **ON** in `envFlag` (`StaticPropRenderAdapter.cpp:30`, `RenderWorld.cpp:65`) and as **OFF** in `ParseEnvBool`/`parseEnvBoolWithDefault` (`mclib/bdactor_diag.cpp:249`, `terrobj.cpp`, `GameOS/gameos/gos_static_prop_registry.cpp:117`, `UiDefs.cpp:97`).
  - Empty string reads as **ON** in `vis_semantics.cpp:13` `env_default_on` and as **OFF** in `RenderWorld.cpp:70` `envFlagDefaultOn`.
  - `vk_shared_gbuffer_gate.h:73` is tri-state, and `"10"` reads as unset.
- The registry header claims to be "the single place to look up what env vars exist" (`RendererFeatureRegistry.h:5-8`). The facts: 362 registered; `check-env-registry.sh` ALLOWLIST holds 850 names; `docs/flags.auto.yaml` has 1099 flags, about 737 with `default: unknown`.
- `RendererFeatureRegistry.h` is parsed by 10 independent Python parsers (check-delivery, comment_tidy, gate-default-agreement, check-new-gates, registered-gate-passthrough, smoke-env-inheritance, smoke-matrices, split-path-contracts, pe_gate_scan, run_smoke). Only 6 tools use `scripts/registry_parse.py`.

### A11. [MED] Coordinate swizzles are hand-coded, with inconsistent names
The permutation `(-x, z, y)` is hand-written at about 9 GLSL sites and about 8 C++ sites. It is labeled "Stuff->MC2" (`mech.vert`, `mech_tier1.vert:181`, `shadow_mech.vert:45`) and "Stuff->GL" (`static_prop.frag:544,547`), and the same permutation is the MC2->GL `makeAxisSwapMC2toGL` (`mclib/xform_conventions.cpp:24`). The `MC2_SUN_AXIS_FIX` saga is this class of bug. Only xform_conventions has a test.

### A12. [MED] Knowledge contradictions that steer agents wrong
1. **C++ standard.**
   - What is true: `CMakeLists.txt:12` defaults to 23, build64 caches 23, and `mc2log.h:22` uses `<format>`.
   - What sources say: `CLAUDE.md:71` "C++17", `docs/cxx17-coding-rules.md:20` "Project standard: C++17", `.claude/skills/mc2-render-spine-advisor.md:234` "No C++20-only types ... baseline is C++17" (it would reject the logging facade), and memory `INDEX-BUILD-DEPLOY.md:35` "C++14 game, C++17 tests".
   - Split standards in the build: `tests/unit/CMakeLists.txt:25` sets 17, overridden to 20 at `:134`. About 8 tool and harness CMakeLists pin 17 (`tools/mech_texname_harness`, `mech_import_harness`, `vk_*` probes). Harnesses therefore compile engine leaf TUs as C++17 while the game uses 23, and a C++20 construct in a shared header will break the harness lane first.
2. **Build target.** CLAUDE.md and `skills/mc2-build.md:21` use `--target mc2 mc2_launcher`; `AGENTS.md` and `agents/mc2-haiku-scout.md:31` use `build_quiet.py --target mc2`. Memory `build_target_mc2_skips_mclib_recompile` and `ARCHITECTURE-MAP.md` §4 say that exact command ships a stale mclib. `CMakeLists.txt:606` links `mclib` as a normal target (a project reference), so the memory is probably obsolete. Settle it with one measurement, then delete one side.
3. **Deploy.** `agents/mc2-build-system-expert.md:64,76` and `agents/mc2-render-perf-expert.md:51` say "Deploy is `cp -f` per file + `diff -q`", which bypasses the manifest and fingerprint. Canonical is `deploy_payload.py --target game`. `skills/mc2-build-deploy.md:22` says the editor preset "defaults to the golden 0.4c"; the code (`deploy_payload.py:1190,1223`) and `skills/mc2-deploy.md:19` say v0.4.
4. **Verify-deployed.** `skills/mc2-check.md` uses timestamps and `diff -q` over `.vert/.frag/.tesc/.tese` only. It omits `.comp` (memory `deploy_must_include_compute_shaders`) and `shaders/vulkan/*.spv`, so it can report "All up to date" on a stale compute or VK deploy. Retire it in favor of `deploy_payload.py --verify-only`.
5. **Dead CLAUDE.md citations.** About 18 citations in `.claude/agents/*.md` point at CLAUDE.md sections that no longer exist: "Critical Rules", "Profiling", "Review discipline", "Documentation discipline", "Pending durable artifacts", "Debug instrumentation rule", "Do NOT upscale these atlases". CLAUDE.md is now 73 lines.
6. **No Vulkan in render agents.** `mc2-render-expert.md`, `mc2-shader-expert.md` and `mc2-render-contract-synthesizer.md` never mention Vulkan, `shaders/vulkan`, or SPIR-V, even though VK is the destination backend. `mc2-shader-expert.md:105` says "some setups use code/shaders/".
7. **New single-source tools are undocumented.** gpu_abi (`tools/gpu_abi_gen.py`, `RenderCore/gpu_abi/`) and the logging facade (`GameOS/gameos/utils/mc2log.h`, `MC2_EVENT`) appear in none of CLAUDE.md, ARCHITECTURE-MAP, agents, skills, or memory. Memory `cpp_glsl_ubo_struct_lockstep.md` (a Shaders must-read) still prescribes hand-lockstep. `project-cpp17-to-cpp23-goal.md:21` says seam #2 is "in progress", but it landed in 624a96ada.
8. **Stray artifacts.** A literal `` `r`n `` (PowerShell escape) is committed in comments at `vulkan_post_fog_native.cpp:141` and `GameOS/gameos/gos_postprocess_region.cpp:374`. A tool wrote these.

---------------------------------------------------------------------------------------------------
## B. "ONE way" inventory and retirement path

| Concern | Variants today | The ONE way | Retirement path |
|---|---|---|---|
| Env/config | raw getenv (1438 sites), ~45 local helpers, registry (inventory-only), 850-name allowlist, INI/.fit (game data), python mc2_env_pin | `RenderCore/env.h`: `mc2::env::flag(id)`, `::number(id)`, `::str(id)` keyed by a registry enum, with one truthiness rule (`0/false/off/no` → false, empty → default) and the default taken FROM the registry entry. | (1) Add the accessor, plus a doctest of the truthiness table. (2) Ratchet `getenv("MC2_` count per dir in cxx17_ratchet (new LG id). (3) Migrate the ~45 helpers first, since they are all wrappers. (4) gate-default-agreement then reads the registry only, and the reader classifier retires. INI/.fit stays for game data, not engine knobs. |
| Registry parsing (py) | 10 private parsers + registry_parse.py | `scripts/registry_parse.py` | Switch each script; add a check that greps for `RendererFeatureRegistry.h` opens outside registry_parse. |
| Logging | fprintf/printf (~2400), SPEW 286, 13 `vlog` + 2 `log`, utils/logging.h, mc2_diag JSONL, OutputDebugString | `mc2::log` / `MC2_EVENT` (landed P0/P1) | Per `logging-facade-plan.md`. First replace the 13 `vlog` copies with `MC2_LOG(Vk, ...)` (one TU each, no parser impact). Teach gate-default-agreement about `MC2_EVENT(` (A3). |
| Assert/fatal | gosASSERT 1560, assert 330, Assert 407, STOP 470, PAUSE 151 | `gosASSERT` (debug) + `STOP` (fatal), a `MC2_CHECK` alias later | Ratchet `assert(`/`Assert(` count; out of scope for logging P1 (A9 of that plan). |
| File IO | fopen 192 (105 files), fstream 65, CreateFile 14, std::filesystem 135, FastFile/File (mod-aware) | Game data goes through `File`/FastFile (canonical key, mod VFS); engine sidecars use one `mc2::io::readAll(path)` helper | `check-path-normalization.sh` ratchets code/gui/mclib only. Extend it to `GameOS/` (most texture, sidecar and shader IO lives there). |
| SPIR-V load | 13 copies + `mc2_vulkan_load_spv` | one `vk_util.{h,cpp}`: `spvPath(name)`, `loadSpv()`, `vlog` | Mechanical, about 13 x 40 lines removed. |
| GPU structs | gpu_abi (1), static_assert-only (~10), check_ubo_layout (13, unwired), material/light checks | `tools/gpu_abi_gen.py` for every C++↔GLSL struct (std430 and std140 member macros) | See roadmap R2. |
| Coord swizzle | ~17 hand-coded sites | `mc2StuffToMc2(v)` in `shaders/include/xform.hglsl` + `mc2xform::stuffToMc2()` in C++, with one name per frame pair | Replace sites; add a grep check forbidding `vec3(-*.x, *.z, *.y)` outside the include. |
| Strings / math | EString/ECharString/EWCharString (3 near-identical classes: `wideToInt`/`StrStr`/`StrRev` x3), std::string, char[]; Stuff::Vector3D/Point3D, raw float[3], glm-less GLSL mirrors | std::string / std::string_view at new APIs; Stuff types inside legacy | Out of scope here; the cxx17_ratchet already tracks it. |

---------------------------------------------------------------------------------------------------
## C. Checker coverage honesty (what each enforced check cannot see)

- **gate_default_agreement:** RED now (A3). 109 of 362 registry vars have no reader in scanned source (C++ only), so they can never fail. About 737 unregistered flags are never classified. Truthiness differences between helpers (A10) are invisible, because it classifies only the unset default.
- **flag_registry_drift --strict:** freshness of the yaml only. `default: unknown` for 2/3 of flags is accepted.
- **gpu_abi:** 1 struct. It cannot see GpuMechInstance, TerrainFrameUBO, ColorFrameUBO, ObjLightsMirror, push constants, or vertex attribute layouts.
- **vk_pipeline_layout:** no UBO member offsets; does not lint `engine_present_vk.cpp`; pipelines without an adjacent `.spv` name are skipped silently; VBO contract covers 3 shaders.
- **gl_shader_variants (231):** compile and link only, under glslang (no driver quirks). It cannot see GL↔VK semantic divergence (A2, A5).
- **spirv_artifacts:** 16 pilots. The baked SPIR-V of the other ~15 VK basenames is checked only by the build.
- **lighting_divergence:** pins constants. It does not check function bodies (the calc_light divergence passes).
- **light_abi_lockstep:** 5 sites; the 6th (ObjLightsMirror) is unknown to it.
- **env_registry:** passes anything in an 850-name allowlist.
- **path_normalization:** code/gui/mclib only.
- **comment_tidy:** report-only, 2601 findings, not a gate. It would not catch the `` `r`n `` artifacts.
- **check-contract-coverage:** scans `scripts/` only, so orphaned `tools/check_*.py` and `tools/glsl_port_diff.py` are invisible.
- **Missing entirely:** a twin-shader feature-coverage check (A5), a copy-equality check for declared byte copies (skirt), and a duplicate-helper/clone ratchet.

---------------------------------------------------------------------------------------------------
## D. De-duplication roadmap (ordered; size S < 0.5 day, M ~1 day, L multi-day)

1. **S: unblock and close live bugs.**
   - Fix the A3 classifier, or annotate the reader.
   - Add `case 20` to `gos_terrain_water_stream.cpp:365`, then single-source all 5 sites behind one header plus a doctest (A1).
   - Delete the `` `r`n `` artifacts.
2. **S: wire the provers honestly.**
   - check_ubo_layout: fix the multi-decl parse, make it exit 1 on mismatch, and add a check-contracts row.
   - glsl_port_diff: add a per-block rename map (`mv_`, `uT.`, `F.fN` ↔ `u_*`), drop or fix `visualDisplaceFar`, add a check-contracts row. Record the real divergences (calc_light sunAxis, shore meander) as pinned debt so the row is green but cannot rot.
   - Extend check-contract-coverage to `tools/check_*.py`.
3. **M: VK terrain shader de-copy (A4).**
   - CMake bake maps skirt.frag to tier1.frag (or include + define).
   - tier1/skirt include `terrain_frame_vk.glsl`.
   - TerrainFrameUBO into gpu_abi (std140 member macro).
   - Expected removal: about 800 duplicated lines.
4. **M: gpu_abi wave 2.** In order: GpuMechInstance (stride-bug history), ObjectLights/TG_HWLightsData (replaces ObjLightsMirror and folds in light_abi_lockstep), ColorFrameUBO, MechFrameUBO, LegacyFrameUBO, the post_* params, the `TG_LIGHT_*` enum, and the shadowKnobFlags bits. Retire check_ubo_layout rows as each struct migrates. Add vertex-attribute layout emission (static-prop 40 B, mech).
5. **S: VK util TU (A8).**
   - One `loadSpv`/`spvPath`/`vlog` plus a `makeLoadColorDepthPass()` helper.
   - Register `MC2_VULKAN_SPV_DIR` and fix `tier1_env_vars.md:110`.
6. **M: twin feature-coverage check (A5).** Uniform-gate table per GL↔VK pair, plus a `VK_NOT_PORTED` list; check-new-gates refuses a default flip while the gate is listed. Then remove the VK `calc_light` copy by including `lighting.hglsl` (the toolchain objection is obsolete).
7. **M: env accessor (A10)** and the `getenv("MC2_` ratchet. Collapse the 10 Python registry parsers onto registry_parse.py.
8. **S: knowledge sweep (A12).**
   - Set CLAUDE.md "Code rules" to C++23 and point it at the gpu_abi and mc2log rules, each replacing one old rule.
   - Retarget `docs/cxx17-coding-rules.md` to C++23 or archive it. Fix render-spine-advisor:234.
   - Settle the `--target mc2` vs mclib-stale question by measurement, then edit CLAUDE.md/AGENTS.md or delete the memory.
   - Remove `cp -f` deploy text from 2 agents. Fix mc2-build-deploy:22. Retire the mc2-check skill.
   - Strip the about 18 dead CLAUDE.md section citations. Add a VK section (shaders/vulkan, islands, SPIR-V bake, ab.py) to the render and shader agents.
   - Update memory `cpp_glsl_ubo_struct_lockstep` (use gpu_abi) and INDEX-BUILD-DEPLOY:35.
9. **M (DONE, D7 phase 1, 938ad14b2): retire the VK probe fixture.** Remove `vulkan_backend_skeleton.cpp` fixtures and `vulkan_shadow_island.cpp`, or quarantine them out of the default build. Remove them from ARCHITECTURE-MAP. This kills the stale EdgeFog/FogOob mirrors and about 4k lines of internal clones.
10. **S: clone ratchet.** Commit a clone scanner (the scratch one is about 50 lines of Python, normalized line windows) as report plus ratchet: total duplicated lines per dir may only fall. Seeded numbers: shaders 2768, GameOS render C++ 8653.
11. **L, later:** merge `lighting_shadow_vk.glsl` into `shadow.hglsl` behind `#ifdef MC2_VULKAN` resource decls; one include for the noise copies; GL water fast/mdi vert merge; the `xform.hglsl` swizzle helper and its grep guard.
