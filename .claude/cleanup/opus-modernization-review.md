<!-- doc-status: stale | reviewed:2026-10-11 -->
# Adversarial strategic review: modernization / hardening program (2026-10-09)

Read-only. Nothing was built or launched; the writer agent owns build64. Inputs: memory (cpp23 goal, full-modernization, small-files,
build/stale-obj notes), all 12 cleanup plans, CLAUDE.md, ARCHITECTURE-MAP, `cxx17_ratchet --report` (4.1 s, 1190 files), the last
~600 commits, `build_logs/*` from the C++23 flip (real timings), CMakeCache, and spot reads of hot code. Numbers marked (est) were
not measured.

## 0. Verdict in one paragraph

The plan polishes idioms while the real cost sits in **verification latency** and **build trust**. Compiling is already fast: a
full C++23 rebuild of 532 TUs took **88 s** (`build_logs/cxx23_main.out`), and a small incremental took **19 s**. One fully verified
change costs about **20 min**: contracts 210 s (run serially), tier1 smoke about 4 min, and ab.py day+night about **10.5 min**,
because every capture idles out its 150 s budget. Compile is under 8% of that. In the ordering, "compile speed" sits last and
"verification speed" is missing. Both are wrong. Do the 1-session loop fixes first: capture early-exit, a parallel contracts
runner, smoke-lite, and a Ninja dev build. Next comes the typed gate registry. from-scratch-review ranked it #1, yet it is absent
from the ordered plan. Then logging and the ABI seams, then the warnings-as-errors and STL-hardening lever, which is cheaper and
stronger than a nullptr campaign. clang-tidy cosmetics (using/typedef) go near the end. The logging facade as landed has a
measurable code-bloat problem that must be fixed before P2 fan-out (section 6.1).

## 1. Ordering critique

Plan order: seam#2 slices -> logging P0-P3 -> clang-tidy batches -> nulls/overflow -> compile speed -> perf.

| Item | Keep / move | Why |
|---|---|---|
| Verification latency (absent) | **ADD, first** | Largest share of the loop (section 2). About 1 session of Python/CMake work, no engine risk. |
| Build trust: Ninja + precise deps (absent) | **ADD, first** | Memory records three classes of stale-binary sessions lost: `build_target_mc2_skips_mclib_recompile`, `msbuild_stale_object_same_second_edit` (1 s mtime), `feedback_class_layout_change_needs_clean_first` (silent ODR corruption). The Ninja configure already exists (`scripts/configure_analysis.py` -> build-analysis). |
| Typed gate registry (from-scratch #1, absent from the order) | **ADD before logging P2** | 1504 `getenv(` calls in 204 files. One git-log bug class is gate/doc truth drift: "fix stale gate defaults", "registry truth", "documented-vs-actual lies", REGISTRY-PARSER-TRUTH re-landed twice. Several contract rows (env_registry, flag_registry_truth, gate-default checker, dead-gate-scan heuristics) exist only because gates are untyped strings. Logging P2 touches the same lines, so do both in one diff per file. |
| Seam #2 slices | keep, but interleave | It has proven value (stride-12 vs 40 bug c26b1eb2a, mech padding 1fbaa8b31). Slice 1 is done. Slices 2-4 can run in parallel with session 2. |
| Logging P0/P1 | done. **Fix bloat before P2** | See 6.1. |
| clang-tidy safe batches | **split** | `use-override` and `use-nullptr` find bugs (virtual signature drift, varargs NULL), so do them early. `use-using` / `redundant-void-arg` are pure churn with full-rebuild cost, so do them last or never. |
| nulls/overflow | **reshape** | The evidence points to narrowing, not nulls: the full C++23 build had **3332 warnings, 3178 of them C4267** (size_t->smaller), and ~2000 come from about 10 `mclib/mlr/*.hpp` headers that every includer re-emits. Also **24 C4477** (printf/sscanf format mismatch, e.g. `stuff/note.cpp:178` `%f` into `Radian*`) and **12 C4312** (int->pointer, `mclib/heap.cpp:245-272`). Fix the mlr headers, then `/we4267 /we4244 /we4477 /we4312 /we4018` per directory. That is a compiler-enforced ratchet with no regex. Nulls: prefer references/`not_null` at new API edges only. The smoke crash bucket (crash_no_summary) is mission-load, so a headless mission-load doctest catches more than a nullptr sweep. |
| Compile speed (PCH/unity/modules) | **demote** | Full rebuild is 88 s on 16 cores. PCH saves maybe 20-30 s full and a few seconds per incremental (est). Split giant TUs for **agent cost** and incremental critical path, not for the overall build. |
| Perf | keep medium-term, **but take 2 free shots now** | `/Ob1` (CMake RelWithDebInfo default) ships in the game exe, so non-`inline` helpers are never inlined. Also, with MC2_LOG unset every printf still formats and does an unbuffered WriteFile to NUL. Both are flag/config-level and measurable with smoke FPS. |

Bigger levers being ignored:
1. ab.py capture idle time: 8 min per A/B.
2. Serial contract runner: about 170 s per run.
3. Build trust: hours lost per incident.
4. The 154-row contract board is itself a bug source. About 25 of the last 600 commits are `fix(contracts)` / tooling-truth fixes: parsers that dropped rows, selftests that injected nothing.
5. Giant TUs, for agent read cost more than compile cost.

## 2. Iteration loop: measured and estimated

| Step | Now | Source |
|---|---|---|
| Full rebuild (mc2 + libs, C++23, VS generator, /MP, 16 cores) | 88 s | cxx23_main.out "Time Elapsed 00:01:28.75" |
| Small incremental | 19 s | cxx23_probe3.out |
| Link mc2.exe (238 MB PDB, /INCREMENTAL, static libs) | about 15-16 s | obj vs exe mtimes 12:17:17 -> 12:17:33 |
| Deploy (sha256-verified, 885 files) | 20-40 s (est) | not measured |
| check-contracts | **210 s wall, 154 rows, serial** | contracts23.txt. Slow rows: no_raw_gl_from_game 37 s, include_firewall 20 s, delivery_installs 16 s, input_capture_visibility_selftest 10 s, sampler_bindings 8 s |
| tier1 smoke (5 x [30 s + 6.5-8.4 s load + start/stop]) | about 4 min (est) | smoke23.txt |
| ab.py GL+VK, day+night | **about 10.5 min** | ab23.txt: each bookmark 157 s. Cause: `run_visual_capture.py:357-359` breaks the poll loop once the PNGs exist, then still `proc.wait`s until `t0+duration+15`, and the exe was launched with `--duration 150`. Every capture idles about 2 min. |
| **Total per verified render change** | **about 20 min** | compile is under 1.5 min of it |

Plan, ranked by minutes saved per day. Assumes about 8 contract runs, 8 smokes, 4 A/Bs and 10 builds per day (est).

| # | Change | Saves/day (est) | Effort |
|---|---|---|---|
| 1 | **Capture early-exit**: kill or quit the child as soon as all expected PNGs and sidecars exist (or pass `--duration` = trigger+settle+margin). Keep 150 s only as a timeout. | 4 A/B x 8 min = **~32 min** | 1 h |
| 2 | **Parallel contracts**: run rows in a pool (rows are independent `eval`s; serialize only the few that build temp hardlinks or run git). Fix no_raw_gl_from_game and include_firewall (single `rg` pass, not per-file Python). 210 s -> ~40 s. Add `--changed` (rows keyed by path globs) for the inner loop. | 8 x 2.8 min = **~22 min** | 0.5-1 session |
| 3 | **smoke-lite** profile: mc2_10 (night, heaviest) + mc2_01 (day), 10 s each, shader-error gate on. Run full tier1 only at batch end. | 8 x 2.5 min = **~20 min** | 1 h |
| 4 | **Ninja dev build** (`build64-ninja`, deploy reads its exe) with `/showIncludes` deps and 100 ns mtimes. Removes the clean-first rule and the mclib-skip trap, and adds cross-target parallelism. | about 5 min of build time, plus avoided lost sessions (hours/incident) | 0.5 session |
| 5 | **Facade de-bloat** (6.1) before 1400 sites migrate. Otherwise compile time and obj size grow sharply. | prevents a regression | 2 h |
| 6 | Split `gos_static_prop_batcher.cpp` / `gameos_graphics.cpp` / `vulkan_backend_skeleton.cpp` (giant-file-split-plan) | incremental critical path ~10-20 s per touch (est), plus large agent-token savings | 1 session |
| 7 | PCH (`target_precompile_headers` on gameos/mclib/code: windows.h, gameos.hpp, stuff.hpp, `<format>`, `<vector>`, `<string>`) | ~5 min | 2 h, measure with `vcperf` / `/Bt+` first |
| 8 | Merge the 16 `build64-h-*` harness trees into one CMake tree with N targets | 1-3 min per harness run (est) | 2 h |
| 9 | Headless mission-load doctest (offline-checks #4) | replaces smoke for load crashes | days |
| - | Unity builds | low. Hostile to the ODR-fragile TU-internal classes noted in the giant-file plan. | skip |
| - | C++20 modules / `import std` | **negative** | see 6.2 |

Target after session 1: a verified change in about **6-7 min** (incremental 20 s + deploy + contracts 40 s + smoke-lite 1.5 min +
A/B 2.5 min), down from about 20.

## 3. Hardening: recurring bug classes and the foundation fix for each

Source: git log (last ~600 commits) plus memory.

| Class | Evidence | Foundation fix | In plan? |
|---|---|---|---|
| C++/GLSL ABI drift | c26b1eb2a stride 12 vs 40; 1fbaa8b31 mech padding; mech_shadow_instance_struct_stride_bug; 9411c1687 WATER_SHINE uploading to undeclared uniforms "since c50e99a6" | seam #2 generator (yes); vk_pipeline_layout lint (landed); **checked uniform handles**: location -1 at program link = one loud error, typed `Uniform<T>` | uniform part missing |
| Shader edit silently kills one backend | bbea91d82 static_prop.frag splice disabled GPU props on both backends while tier1 passed; d27a64521 comment edit drifted the SPIR-V hash | gl_shader_variants (landed). Make it a pre-commit hook on `shaders/**` (seconds). Hash SPIR-V on stripped/normalized source so comment edits don't rebake. | partly |
| Stale binary / stale deploy | 3 memory files, multiple lost sessions | Ninja deps; deploy refuses an exe older than any changed source (fingerprint exists; add an mtime/hash check of the libs) | **missing** |
| Gate/doc truth drift | 6335f5f26, 74a4dcbf1, 9b9e59dd6, ba6f7a5e4, 115e9a394, REGISTRY-PARSER-TRUTH x3 | **typed gate registry**: `constinit` table `{name, default, kind, owner, retireBy}`; `gate::get<Id>()` is the only reader; one `[CONFIG]` dump; flags.auto.yaml generated from the table; ratchet idiom LG19 = raw `getenv("MC2_` | **missing from order** |
| Hand-maintained cache invariants | LIGHTROW-FRESHNESS-1 (db221dcc2, 480ceaafa); snapshot "ANY new registry mutation MUST bump s_registryGeneration" (memory); 60cb4cf8 fix | Encapsulate: snapshot-captured fields become private, and setters bump the generation. A `MC2_VERIFY_CACHES` debug mode recomputes and compares every N frames. | **missing** |
| Coordinate/matrix convention | abffc9ab7 VK wrong viewpoint (default flip reverted); 2fc06b5a9/8184a1af8 CSM viewport flip/unflip/reflip; 96bc2957a sky matrix transpose | Strong types at the RenderCore boundary: `Mat4<RowMajor,Space>` upload helpers; `ClipSpaceGL` vs `ClipSpaceVK` with an explicit `toVk()` | from-scratch #4 only |
| VK lifetime / sync | 378 vkCreate / 463 vkDestroy, 0 RAII; per-slot fence waits; 7 `vkDeviceWaitIdle` in runtime paths | Unique handles plus a deferred-deletion queue keyed by frame index (also a prerequisite for perf 4.1) | from-scratch #3 only |
| Narrowing / 64-bit truncation | 3178 C4267, 12 C4312, 24 C4477 | mlr header fix, then `/we` per dir; `_MSVC_STL_HARDENING=1` in RelWithDebInfo (bounds checks on span/mdspan/vector/string `[]`; MSVC 14.44 = VS 17.14 should have it, verify with a 3-line probe) | **missing** (the plan's "mdspan debug bounds checks" never run: there is no Debug config) |
| Tooling self-bugs | ~25 `fix(contracts)` / truth commits | **Contract diet**: each typed seam that lands deletes at least 1 text-lint row (seam#2 -> check_ubo_layout pairs + material-gpu-mirror; gate registry -> env_registry / flag_registry_truth / gate-default checker). Board target: 154 -> under 110 rows. | partly |
| Mission-load crashes | smoke crash_no_summary bucket (offline-checks #4) | Headless mission-load doctest over 24 FST missions (`mclib_standalone` / engine_standalone_stub exists) | listed, unscheduled |

## 4. Performance (medium-term): the likely big wins, grounded in code

Measured context: tier1 avg fps mc2_01 146, mc2_03 87, mc2_10 85, mc2_17 127, mc2_24 115 on a 7900 XTX (smoke23.txt). That is
almost certainly CPU/serialization-bound. Smoke runs with MC2_LOG=1 and unbuffered pipes, so its FPS is contaminated by telemetry
I/O. Use Tracy, not smoke, as the perf signal.

1. **CPU/GPU serialization on VK.** One `inFlight` fence: `engine_present_vk.cpp:1972` retires frame N-1 before recording N, so
   effectively 1 frame is in flight. Also per-slot `vkWaitForFences(UINT64_MAX)` in `vk_image_resource.cpp:157`, GL<->VK interop
   semaphores, and `glFinish` in `gameos_graphics_light_ssbo.cpp:238` (resize path). Instrument first: the `VKIsl.Present.CompositeWait`
   zone and the `[QWI-WAIT] wait_us_avg` log already exist. If the wait exceeds 1-2 ms, move to 2 frames in flight. That needs
   per-frame resource rings plus the deferred-delete queue from section 3.
2. **Compiler config.** RelWithDebInfo uses `/O2 /Ob1`, no `/Gy`, and the link has no `/OPT:REF,ICF` (CMakeCache). `/Ob2` lets
   non-`inline` TU-local helpers inline, which is common in mclib/code hot loops. One flag, A/B on Tracy. LTCG (`/GL /LTCG`) only
   for a ship config, never in the dev loop.
3. **Telemetry cost in normal play.** `setvbuf(..._IONBF)` plus the redirect to NUL means every legacy printf still formats and
   makes a syscall (gameosmain.cpp ~1158-1185). The facade fix: when `init()` returns "not live", set every threshold and event to
   Off, so formatting is skipped entirely. Per-site disabled cost is already 1 relaxed load. Also give smoke a buffered/async file
   sink so perf runs stop paying for pipes (memory: the 400 ms hitch was this).
4. **Single-threaded game logic.** The job system lives in `code/` (objmgr, mission, terrobj, msl only). The memory shows ms-scale
   `GameLogic.Units.*` zones (TerrainObjects ~1.4 ms, Mechs 1.1 ms saved by one slice). The next lever is jobifying mover/mech
   update and cull.
5. **Per-frame uploads and rebuilds.** 124 `glBufferData/SubData` sites vs 23 persistent-map sites. The 2.4k-line `flush()` is in
   the static-prop batcher. Snapshot rebuilds are generation-gated bypasses, not retirements. Add Tracy plots for bytes uploaded per
   frame and draws per pass before any rewrite.

Instrument first, in this order: Tracy baseline on mc2_10 and mc2_03 (CPU zones + GPU zones), the composite-wait histogram,
bytes-uploaded per frame, and allocation count per frame (Tracy memory hooks on the frame arena + global new).

Shape now so perf is cheap later:
- Hot APIs take `std::span<const T>` / mdspan views over caller-owned storage, with no globals inside. That makes them job-friendly.
- Gates and log levels are read once (`constinit` / static); there is never a getenv per frame. This is mostly true already.
- RAII handles get frame-indexed deferred deletion from day one.
- Single-source GPU structs, so SoA or instance-layout changes become one-header edits.
- No `std::function` or virtual dispatch introduced in per-element loops.
- New per-frame containers come from the frame arena.

## 5. Strangler/ratchet risks at this scale

1. **Gaming by semantics, not syntax.** The ratchet rewards dropping counts. LG06 to zero pushes `string_view` into fopen, GL and
   getenv boundaries, which means either `.data()` into C APIs (missing-NUL bugs) or `std::string(sv).c_str()` (new allocations).
   LG14 `enum class` breaks serialized ints and bitflags. LG09 casts and LG03 new/delete hide real ownership questions. LG16 mdspan
   over every grid is churn without checks unless hardening is on.
   **Policy:** split idioms into DRIVE-TO-ZERO (tool-verified mechanical: LG01, LG08, LG10, LG12a, LG13-partial, LG15) and FREEZE
   (no new sites; reduce only inside a seam: LG03, LG04, LG05, LG06, LG09, LG11, LG14, LG16, LG17, LG18, LG12b). Report FREEZE
   idioms but never set targets for them.
2. **Syntax gaming:** line splitting, macro wrapping, moving code to unscanned dirs (editor/, gui/, Viewer/ are opt-in). Scan all
   owned dirs, and make the rule "a new file has 0 for drive-to-zero idioms".
3. **Half-migrated states.** There are already 4 logging mechanisms (printf, SPEW, mc2_diag, log_info) plus a 5th (MC2_LOG). Gates
   use getenv, the registry, `subGate`/`directChainGate`, and soon the typed registry. GL and VK twin every pass.
   **Retirement rules:**
   - Every introduced mechanism records `retireBy = session N+3` and a legacy-site budget in the plan checklist.
   - `cxx17_ratchet --report` gets a **MIXED** column: files that contain both the old and the new form. MIXED must only fall
     after the first adoption session.
   - Miss a deadline and adoption of the new form is **frozen** until the legacy form is retired in the already-adopted dirs. No
     third form while a second is unfinished.
   - Alias tables (logging trace-gate aliases, gate renames) carry a removal session, and the scripts setting them are updated in
     the same commit.
   - SPEW shim and `utils/logging.h`: retire by the end of session 3.
4. **Shared baseline.json** conflicts. That is fine with one lane. Tighten only on main and only in the commit that paid it down
   (already the rule).
5. **Ratchet scope creep.** 19 idioms is too many to steer by. Show 6 headline numbers per session: LG12a, LG01, raw getenv
   (new LG19), C4267 warning count, MIXED files, contract rows.

## 6. C++23 / MSVC specifics the plans get wrong or omit

### 6.1 Logging facade: code bloat (measured)
`gos_terrain_patch_stream.obj` was **415 KB before the P1 pilot** (build-cxx23) and **1.33 MB after** (build64). That is 3.2x for
15 `MC2_EVENT` sites in an 887-line file. Cause: `event<Tag,Ver,A...>` and `logFmt<A...>` instantiate `std::format_to(back_inserter)`
and the whole formatter machinery per arg-pack and per NTTP tag, inline in every TU.
Extrapolated to about 1461 LG12a sites, this means noticeably slower compiles and a bigger exe and PDB.
**Fix before P2:** keep `std::format_string<A...>` at the call site (the consteval check stays). Pass `fmt.get()` plus
`std::make_format_args(args...)` to **non-template, out-of-line** `detail::emitEvent(Stream, std::string_view tag, int ver,
std::string_view fmt, std::format_args)` and `detail::emitLog(...)`, which call `std::vformat_to` once in mc2log.cpp. The tag
becomes a runtime `string_view` (no `Lit` NTTP).
Also keep `<format>` out of mc2log.h's public surface where possible: forward-declare, or put `<format>` in the PCH.
Re-measure obj size; the target is under 1.1x the pre-pilot size.

### 6.2 Modules / `import std`
MSVC supports them (CMake 3.28+ named modules; `import std` is experimental in CMake 3.30+). But this repo just standardized on
**clangd and clang-tidy over a clang-parsed compile DB**, and clang cannot consume MSVC IFCs. Modules would blind the analysis
tooling the owner chose. PCH gives most of the parse win, and clangd tolerates `/FI`. Verdict: no modules this program.

### 6.3 `/std:c++latest`
This enables C++26-draft features, and the latest-mode STL is not ABI-stable, so toolset updates can break the build. MSVC 14.44
has `/std:c++23preview` (VS 17.13+). Prefer it, so an MSVC update cannot silently change semantics. Also add:
- `/Zc:__cplusplus` (today `__cplusplus` = 199711L for any vendored header that feature-tests).
- `/utf-8` (a quick scan found no non-ASCII bytes in code/mclib/GameOS, so it should be byte-safe; re-verify).
- Consider `/Zc:preprocessor` only after testing legacy Stuff/ABL macros.

### 6.4 std::print
Don't use it in the facade or in hot code. On MSVC, `std::print` to a console does UTF-8->UTF-16 transcoding through
WriteConsoleW and locks the FILE per call. The facade's single `fwrite` per line is right; keep it. gcc 13 lacks `<print>`, but
gcc 14 has it.

### 6.5 mdspan
- Zero-cost in /O2: accessors are in-class, so they are inline-eligible even under /Ob1.
- Bounds checks only exist with `_MSVC_STL_HARDENING` or `_CONTAINER_DEBUG_LEVEL`, and no Debug config is built. The plan's
  "debug bounds checks" are fictional until hardening is enabled.
- Pick one index type alias (`mc2::Grid2D<T> = std::mdspan<T, std::dextents<int32_t,2>>`, or size_t) to avoid creating new C4267
  sites.
- Linux impact: libstdc++ has mdspan only from GCC 15. Adopting LG16 widely pins the deferred Linux port to GCC 15+ or libc++.

### 6.6 `<format>` compile time
It is one of the heaviest MSVC headers, and every `format_string` constructor is a consteval parse per call site. Measure with
`vcperf` / `/Bt+` before and after P2. PCH plus 6.1 contain it.

### 6.7 std::expected / optional
Fine on 14.44. Watch `[[nodiscard]]`: `std::expected` is not nodiscard by default in MSVC STL, so add `[[nodiscard]]` on returning
functions.

### 6.8 Other build notes
- `gameos_editor` recompiles **all** gameos SOURCES a second time (GameOS/gameos/CMakeLists.txt ~399-402). Making the default ALL
  build green doubles GameOS compile. Make it EXCLUDE_FROM_ALL, or build the shared sources as an OBJECT library used by both.

## 7. Roadmap (next 5 sessions)

### S1 "Loop and trust" (no engine-behavior change)
- Goals:
  - capture early-exit
  - parallel contracts + `--changed` + fix the 2 slow grep rows
  - smoke-lite profile
  - `build64-ninja` dev build wired into deploy
  - drop the clean-first / mclib-first rules from CLAUDE.md and memory once Ninja is proven (touch `camera.h`, then confirm every includer rebuilds)
  - facade de-bloat (6.1)
  - `loop_timings.json` recorder
- Exit criteria:
  - A/B day+night GL+VK under 3 min
  - contracts under 60 s
  - smoke-lite under 90 s
  - full Ninja build at or under 88 s
  - patch_stream.obj under 460 KB
  - tier1 5/5
- Metrics: minutes per verified change (target 7, from 20).

### S2 "Typed truth"
- Goals:
  - typed gate registry (constinit table, `gate::get`, `[CONFIG]` dump, yaml generated)
  - new LG19 raw-getenv idiom (freeze)
  - logging P2 terrain family, migrating getenv to typed gates in the same diffs
  - facade dead-stream fast path (thresholds Off when not live)
  - delete env_registry / flag_registry_truth / gate-default rows that the type system now guarantees
- Exit criteria:
  - LG12a down by at least 150
  - raw getenv down by at least 100
  - contract rows down by at least 5
  - golden event bytes identical
  - smoke-lite + tier1 green
- Metrics: LG12a, LG19, rows.

### S3 "ABI and narrowing"
- Goals:
  - seam #2 slices 2-4 (PerDrawEntry, GpuMechInstance, MaterialGpu), retiring the mirror checks
  - checked uniform handles (-1 = error)
  - fix the mlr headers' C4267, then `/we4267 /we4244 /we4477 /we4312` on RenderCore, GuiRuntime, GameAdapters, RenderWorld, GameOS
  - `_MSVC_STL_HARDENING=1` probe, then enable in RelWithDebInfo with a measured fps delta
  - `/Ob2` + `/Gy` + `/OPT:REF,ICF` A/B on Tracy
  - clang-tidy use-override + use-nullptr on GameOS/RenderCore
  - `/std:c++23preview` + `/Zc:__cplusplus`
- Exit criteria:
  - warnings under 300 total, 0 in the `/we` dirs
  - static_asserts replace at least 3 text checks
  - fps delta recorded for each flag
- Metrics: warnings, rows, fps p50/p99.

### S4 "Ownership and offline coverage"
- Goals:
  - VK RAII handles + frame-indexed deferred deletion in one island (shadow or color-prop), then present
  - headless mission-load doctest over 24 missions
  - giant-file splits #1-#2 (vulkan_backend_skeleton, static_prop_batcher)
  - SPEW shim + utils/logging.h retired
  - MIXED-file column in the ratchet
- Exit criteria:
  - 0 raw vkDestroy in the migrated island
  - 24/24 missions load offline
  - no TU over 6k lines in GameOS
  - MIXED count falling
- Metrics: vkDestroy raw count, offline mission coverage, largest-TU size.

### S5 "Perf baseline" (start of the medium-term track)
- Goals:
  - Tracy baselines on mc2_10/mc2_03 (CPU+GPU zones, composite-wait histogram, upload bytes and allocations per frame)
  - 2-frames-in-flight experiment on VK present (behind a temporary kill-switch with retireBy)
  - first jobified update (mover or cull) using span APIs
  - logging alias-table removal date set
- Exit criteria: perf numbers checked in as a baseline; any change accepted only with Tracy deltas, tier1 5/5, and A/B eyeballed.
- Metrics: frame ms p50/p99 per mission, composite wait us, ms per GameLogic zone.

Per session, always: commit-first deploy, contracts (parallel), smoke-lite for the inner loop, full tier1 + A/B at batch end,
ratchet `--check --tighten` only in paying commits, and handoff with the 6 headline numbers.
