<!-- doc-status: current | reviewed:2026-10-11 -->
# Logging facade plan (C++23, MSVC 14.44). Design only, nothing built.

## Amendments (adversarial review 2026-10-09)
Source: `.claude/cleanup/seam-plans-adversarial.md`, "Logging". These override the sections below where they conflict.

- **A1 LG12 split.** LG12a = printf-style logging (fprintf/printf to stdout/stderr, log_info family): the facade target; reaches 0 via MC2_LOG/MC2_EVENT, allow-list only for crash/signal handlers and facade internals. LG12b = sprintf/snprintf/vsnprintf buffer formatting (R_PRINTF, tools/cxx17_ratchet/cxx17_ratchet.py:86): migrates to `std::format_to`/`std::format_to_n`, NOT the logger; tracked as its own ratchet line. The 2704 figure is LG12a+LG12b combined; report them separately.
- **A2 constinit defaults.** Thresholds and event-enable flags are `constinit` with explicit legacy defaults (MC2_LOG-unset semantics). No dynamic initialisation, so logging before `main` (static init, DLLs) is safe. `init()` only overrides.
- **A3 MC2_LOG parse ordering.** Spec parse runs BEFORE the NUL redirect at gameosmain.cpp:1145-1175; stream liveness is decided from the parsed result. `MC2_LOG=1` keeps legacy meaning exactly (all streams live, all legacy events on). Unset = unchanged NUL redirect. A value containing `:` or `=` is a spec and implies live streams.
- **A4 runtime format strings.** SPEW-style and data-supplied templates use `std::vformat` inside `try/catch (std::format_error&)`. On error emit the raw template plus a one-line `[LOG FORMAT ERROR]` marker. The facade never throws. Only compile-time literals use `std::format_string`.
- **A5 brace escaping.** Literals converted from printf that contain `{` or `}` (JSON-ish text) are escaped to `{{` `}}` by the codemod. The codemod flags any unescaped brace.
- **A6 printf hazards.** `%s` with nullptr goes through `safe(s)` (prints `(null)`). `%ld`/`%lu` are 32-bit on Windows LLP64: map to int32 explicitly, never int64. `%lld` maps to int64. Width/precision variants are covered by the golden harness.
- **A7 level check before args.** The level/category check is inside the macro (`if (enabled) emit(format(...))`), so disabled sites evaluate neither the format nor their argument expressions. The disabled-path test counts formatting calls and must show zero.
- **A8 MC2_EVENT byte-identical.** Output is `[TAG vN] ` + body + `\n`, same FILE* per call site (stdout or stderr), same flush points, same ordering against remaining raw printf calls, one fwrite per line. No decoration. Parsers are not changed in phase 1. Regexes at risk, from `scripts/smoke_lib/logparse.py` and `scripts/run_smoke.py`:
  - `PERF_RE` (`[PERF v1]` strict key order and spacing), `TIMING_RE`, `SUMMARY_RE` (`[SMOKE v1] event=summary result= reason= stage=`), `HEARTBEAT_RE` (`[HEARTBEAT] frames= elapsed_ms=`)
  - Line-anchored: `^CRASH:`, `^\[CRASHBUNDLE\]`
  - Bucket regexes: `[SPLIT_PATH_PARITY] ... match=0`, `[MOVER_COHERENCE] VIOLATION|ADVISORY`, `[ASSET_LOAD v1] event=...`, `[APPEAR_AUDIT v1] event=missing`, `[SMOKE v1] event=banner`
  - Un-bracketed `VEG_SUMMARY instance_count= draw_calls= flush_reason=` (run_smoke.py `_VEG_RE`) and `ORDER_LIVENESS_SUMMARY`
  - Free-text signatures: `Shader filename:...failed to load shader`, `shader...compile...fail`, `GLSL link...failed`, `vkCreate*...failed`, `pipeline_failed`, `Unable to open Mission File`, `Missing file:`, `cannot open...required`, `STOP(...Mission File`
  - Any tag whose golden byte test fails stays on legacy printf.
- **A9 unchanged in phase 1.** `gosASSERT`, `STOP()`, `PAUSE()`, and ErrorHandler are not migrated.
- **A10 measured call-site cost (2026-10-10, `gos_terrain_lighting.cpp`, 5 events, MSVC 14.44 `/O2 /Zi`).** The facade is already out of line: the TU holds no `vformat_to` or formatter instantiation, only the per-site `mc2::log::event<Tag,Ver,A...>` and `std::make_format_args<...>` COMDATs (`dumpbin /symbols`: 11 format-related symbols, all of those two kinds); every `vformat` lives in `mc2log.cpp::vevent`. Gate edits alone: obj 501.7 KB -> 499.7 KB. Adding the five events: `.text` +3.1 KB (30,293 -> 33,377 B; about 0.6 KB per event, the 14-argument LIGHT_POOL_DUMP line is the largest), `.rdata` -0.05 KB, `.debug$S` +36 KB, TU pdb +287 KB (the `<format>` types `basic_format_arg`/`_Fmt_buffer`/`basic_format_context` enter this TU's CodeView), front end +0.2 s from the include. That is the entire +63 KB object delta: code is 5% of it, the rest is CodeView and mangled symbol names. A variant with `Tag`/`Ver` as ordinary arguments (shorter mangled names, `[[msvc::forceinline]]` wrapper) changed `.text` by +0.3 KB and the object by -1.9 KB, so the NTTP tag was kept. Rule of thumb for budgets: about 0.6 KB `.text` and 7 KB CodeView per distinct event signature in a TU, plus a one-time ~0.2 s / 290 KB pdb for the first `mc2log.h` include in a file.
- **A11 per-frame loops.** An event or log call inside a per-element loop (per vertex, per quad, per light) goes through a `[[msvc::noinline]]` static helper, as `Parity_PrintMismatch` in `gos_terrain_lighting.cpp` does. Inlined into the loop, the format call's argument setup cost the parity pass about a third of its frame rate (51/40 fps against 112/66 once out of line). Cold branches that fire rarely but sit in a hot loop follow the same rule.

## 1. Survey (code GameOS mclib RenderWorld RenderCore GuiRuntime GameAdapters HostServices editor tools)
| Mechanism | Lines | Files |
|---|---:|---:|
| fprintf(stderr/stdout) | 1600 | 172 |
| bare printf( | 789 | 119 |
| LG12 ratchet total (printf-family), split per A1 into LG12a (logging) and LG12b (buffer formatting) | 2704 combined (code 1001, mclib 954, GameOS 707, GuiRuntime 34, GameAdapters 6, RenderCore 2); LG12a/LG12b split not yet counted | ~190 |
| SPEW( | 286 | 69 |
| gosASSERT | 1633 | 138 (assert, not logging: leave) |
| STOP( | 459 | 121 (fatal; keep) |
| PAUSE( | 151 | 51 |
| OutputDebugString | 31 | 16 |
| getenv("MC2_*TRACE/DIAG/PROBE/DEBUG") | 300 | 95 (106 distinct gates, tierB-trace-gates.md) |
| mc2_diag::writeEvent (JSONL, own whitelist MC2_DIAG_TAGS) | ~46 | 23 |
| utils/logging.h log_info/... (printf style) | 56 | 6 |
| std::cerr/cout | 11 | 1 |
| TracyMessage | 0 (Tracy only used for ZoneScopedN, ~170 lines) | - |

Heaviest LG12 files: mover.cpp 120, gos_render 100, move.cpp 80, terrain.cpp 78, gameobj 74, bdactor/cmponent 72, inifile 74, gameos_graphics 69, gos_postprocess 63, gameosmain 62, terrtxm2 54, ablmc2 53, file.cpp 52, warrior 49, gos_terrain_indirect 46, gos_terrain_lod_chunk 43.

IMPORTANT existing gate: `MC2_LOG` (any value) keeps stdout/stderr live; unset => gameosmain.cpp (~line 1160) setvbuf's then redirects stdout AND stderr to NUL, and it also suppresses the launcher. Oracles/scripts set MC2_LOG=1. The facade must not redefine it: "1" = legacy (everything as today); a value containing ':' or '=' is a spec and also implies live streams.

## 2. Structured-line contract (byte-identical)
Consumers: scripts/smoke_lib/{logparse,oracleparse,fingerprint,evidence_classifier,crash_evidence}.py, run_smoke.py, run_editor_smoke.py, run_editor_playtest_smoke.py, smoke_with_screenshots.py, check-order-liveness.py, render_pass_report.py, slice_gate.py, pipeline_visual_gate.py, sweep_campaign.py, residency_slim.py, whatran.py.

Tags matched by regex (scan of scripts/tools/tests; re-run in P0 to freeze):
SMOKE v1 (event=summary result= reason= stage=), PERF v1 (strict order and spacing: avg_fps p50_ms p99_ms p1low_fps peak_ms samples), TIMING v1, HEARTBEAT (frames= elapsed_ms=), BUILD_FINGERPRINT v1, ESMOKE v1, INSTR v1, MOVER_COHERENCE (VIOLATION|ADVISORY), ASSET_LOAD v1 (event=parse_fail|required_missing|appr_type_fail|icon_oob), APPEAR_AUDIT v1, SPLIT_PATH_PARITY (match=0), AIMED_FIRE v1, ORDER_FIXTURE, BRAIN_FREEZE_TRACE, GPU_CULL v1, MECH_MATERIAL_GPU v1, OBJBATCHER v1, QUADSETUP_SKIP v1, RENDER_SNAPSHOT v3, TERRAIN_INDIRECT v1, TEX_RESOLVE v1, FASTPATH_DROP, RENDER_PASS v1, DEPLOY_FINGERPRINT, ENV, PIPELINE_BIND, SOAK, TEXLOAD, GPUPROPS, SHADER / "[SHADER ERROR]", CRASHBUNDLE (line-anchored `^\[CRASHBUNDLE\]`), `^CRASH:`; un-bracketed VEG_SUMMARY and ORDER_LIVENESS_SUMMARY (key order/spacing regex); free-text signatures ("Unable to open Mission File", "Missing file:", "Shader filename:...failed to load shader", vkCreate*...failed, pipeline_failed). Also treat every other `[A-Z_]+ vN]` line (VISUAL_DIFF, MOVER v1/v2, SIMID, TGL_POOL, GL_ERROR, ASSET_SCALE, SCREENSHOT, ...) as frozen.

Rules the facade guarantees:
1. Structured lines use a separate entry, never the free-form logger: `MC2_EVENT(Cat, "TAG", N, "k={} k2={:.3f}", ...)`. Tag and version are consteval template args; facade writes exactly `[TAG vN] ` + body + `\n`. Unversioned legacy tags use `MC2_EVENT_NOVER`. No prefix, timestamp, level or category decoration is ever added to event lines. Body format = old printf string transliterated, so key order/spacing is the author's by construction.
2. Events bypass the level filter. They obey only the category/topic toggle that replaces their legacy gate; default state identical to legacy (always-on stays always-on).
3. Stream preserved per site (Stream::Err/Out) so the NUL-redirect-when-MC2_LOG-unset still works. One fwrite per line from a thread_local buffer, to the same FILE* as the call site it replaces (no interleave tearing; printf was line-atomic too). See A3, A7, A8.
4. printf->format hazards (codemod must flag, golden must cover; see A5, A6): %g/%e vs {:g}/{:e}; %5.2f -> {:5.2f}; %x/%08X; %p -> void* cast; %s with nullptr (UB in format; wrap in `safe(s)`); %u/%d signedness; %zu/%lld; char printed as int; %% -> %; locale (std::format is locale-independent, matches "C").
5. Parsers are NOT changed during migration. Any tag whose golden byte test fails stays on legacy printf. Parser changes (e.g. JSON events) are a separate later change, dual-emit under MC2_LOG_JSON.
6. Free-text signature lines go through `MC2_LOG_RAW` with legacy text intact; decoration (time/cat) off unless MC2_LOG_DECORATE=1.

## 3. API sketch (GameOS/gameos/utils/mc2log.h, namespace mc2::log; supersedes utils/logging.h)
```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
namespace mc2::log {
enum class Level : uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Off };
enum class Cat : uint8_t { Core, Io, Render, Vk, Gl, Terrain, Water, Mech, Veg, Ai, Sim,
                           Ui, Audio, Net, Shader, Perf, Smoke, Assets, Editor, Count };
enum class Stream : uint8_t { Err, Out };

inline constinit std::atomic<Level> g_threshold[size_t(Cat::Count)] = {};  // legacy constexpr default; overridden from MC2_LOG in init() (A2)
inline bool enabled(Cat c, Level l) noexcept {                  // whole disabled-path cost
  return l >= g_threshold[size_t(c)].load(std::memory_order_relaxed); }
bool topicEnabled(Cat, std::string_view topic) noexcept;       // "terrain:indirect"
bool eventEnabled(Cat) noexcept;                                // legacy-gate equivalent

void write(Cat, Level, Stream, std::string_view line, const std::source_location&);
void writeRaw(Stream, std::string_view line_incl_newline) noexcept;   // events, crash path (no alloc)
bool allowRate(uint64_t siteKey, double perSecond) noexcept;   // per-site token bucket
bool allowOnce(uint64_t siteKey) noexcept;
void init();                      // call in gameosmain BEFORE the NUL redirect at gameosmain.cpp:1145-1175 (A3); spec parse decides stream liveness
void setSpec(std::string_view s); // "render:debug,vk:trace,*:warn"; dev-shell can call at runtime

template <class... A>
void log(Cat c, Level l, const std::source_location& loc, std::format_string<A...> f, A&&... a) {
  thread_local std::string buf; buf.clear();                    // only reached after enabled()
  std::format_to(std::back_inserter(buf), f, std::forward<A>(a)...);
  write(c, l, Stream::Err, buf, loc);
}
template <size_t N> struct Lit { char v[N]; consteval Lit(const char (&s)[N]) { std::copy_n(s, N, v); } };
template <Lit Tag, int Ver, class... A>
void event(Stream, std::format_string<A...> f, A&&... a);       // emits "[TAG vVER] "+body+"\n"
}
// Macros capture source_location and check BEFORE formatting (args are not evaluated when off).
#define MC2_LOG(cat, lvl, ...) do { if (::mc2::log::enabled(::mc2::log::Cat::cat, ::mc2::log::Level::lvl)) \
  ::mc2::log::log(::mc2::log::Cat::cat, ::mc2::log::Level::lvl, std::source_location::current(), __VA_ARGS__); } while (0)
#define MC2_LOG_ONCE(cat, lvl, ...)         /* static atomic<bool> + enabled() */
#define MC2_LOG_EVERY(cat, lvl, hz, ...)    /* enabled() && allowRate(siteKey, hz) */
#define MC2_LOG_EVERY_N_FRAMES(cat, lvl, n, ...)
#define MC2_LOG_TOPIC(cat, topic, lvl, ...) /* topicEnabled() cached per site (static) */
#define MC2_EVENT(cat, tag, ver, ...) do { if (::mc2::log::eventEnabled(::mc2::log::Cat::cat)) \
  ::mc2::log::event<tag, ver>(::mc2::log::Stream::Err, __VA_ARGS__); } while (0)
```
Compile-time floor `MC2_LOG_MIN_LEVEL` (CMake; Release default Debug) makes Trace sites `if constexpr` dead code. std::format_string gives compile-time format/arg checking (the thing printf lacks).

Sinks (runtime bitmask): stdout/stderr FILE* (respects NUL redirect), file (MC2_LOG_FILE, rotate like diagnostic_trace), OutputDebugStringA (only when IsDebuggerPresent), Tracy (TracyMessage with severity, compiled only under TRACY_ENABLE, Warn+ default), bridge to mc2_diag::writeEvent (keeps MC2_DIAG_TAGS and JSONL). Thread safety: format in thread_local buffer, emit under one short sink mutex; Error/Fatal synchronous + flushed; crash handlers use writeRaw (no allocation).

Config grammar: MC2_LOG unset = legacy quiet; `1` = legacy loud (all cats Info + all legacy events on); `render:debug,vk:trace,*:warn`; `terrain=indirect+surface` topic syntax (matches the MC2_TRACE=<subsystem>=<topic> idea in tierB doc, as alias).

## 4. Mapping old -> new
| Old | New |
|---|---|
| `fprintf(stderr,"[TAG vN] ...")` | `MC2_EVENT(Cat,"TAG",N,...)` |
| `printf("[TAG vN] ...")` | `MC2_EVENT` with Stream::Out variant |
| `fprintf(stderr,"plain ...")` | `MC2_LOG(Cat, Info/Warn/Error, ...)` |
| `if(getenv("MC2_X_TRACE")) fprintf(...)` | `MC2_LOG_TOPIC` / gated `MC2_EVENT`; static getenv removed |
| per-frame prints | `MC2_LOG_EVERY` / `_ONCE` / `_EVERY_N_FRAMES` |
| SPEW((group,fmt,...)) | shim forwarding to MC2_LOG(Debug) (group->Cat table) via vformat in try/catch (A4), then retire |
| `sprintf/snprintf` into buffers (LG12b) | `std::format_to` / `format_to_n`, NOT MC2_LOG (A1) |
| utils/logging.h log_* (6 files) | wrappers over mc2::log, then delete header |
| OutputDebugString (31) | OutputDebugString sink only |
| mc2_diag::writeEvent | unchanged API; facade bridge sink |
| STOP/PAUSE | semantics unchanged; message formatted through facade first (P3) |
| gosASSERT | unchanged (assert, not log) |
| std::cerr | MC2_LOG |
| TracyMessage | new sink (none today) |

## 5. Trace-gate folding (tierB-trace-gates.md: 106 gates, 28 DELETE, 78 MERGE, 0 KEEP)
Only gates that merely print fold in (names ending _TRACE/_DIAG/_PROBE/_DEBUG/_CENSUS). Behavior switches (MC2_PATCHSTREAM_THIN_RECORDS*, *_DRAW, *_FORCE_INIT_FAIL, DIRECT_TEXTURE_BIND*, MC2_MODERN_TERRAIN_SURFACE) stay getenv.
- Terrain/water (16 gates): Cat::Terrain/Water topics indirect, surface, patchstream, bucket, water, shoreline, lod.
- Mech/model (12): Cat::Mech topics.
- Remaining subsystems per doc knobs: Render, Vk, Gl, Ai (BRAIN_FREEZE_TRACE), Sim (MOVER_COHERENCE_AUDIT, SIMID), Assets (MC2_ASSET_TRACE, APPEAR_AUDIT), Veg, Ui, Io.
- 28 DELETE-verdict gates die with their call site when the file migrates.
- A legacy alias table (old env name -> cat/topic/level) keeps every old gate working for one release; run_smoke force-sets MC2_MOVER_COHERENCE_AUDIT, MC2_SPLIT_PATH_PARITY_TRACE, MC2_APPEAR_AUDIT and oracle scripts set many *_TRACE=1, so these MUST keep behaving identically. Alias removal is the last step, after scripts are updated.

## 6. Strangler phases
P0 Freeze: tools/log_tag_census.py (scan C++ for `[TAG vN] key=` literal prefixes + python parser regexes) -> tests/unit/log_tag_baseline.json; add C++ golden harness (follow build64-h-* harness pattern) comparing legacy printf bytes vs MC2_EVENT bytes.
P1 Facade: add utils/mc2log.{h,cpp}, init() in gameosmain; zero call-site changes; unit tests for spec parser, rate limiter, once, thread-tear.
P2 Adopt per directory, leaf first: GameOS terrain slice -> other gos_* -> utils -> GuiRuntime/GameAdapters -> mclib -> code. Codemod handles `fprintf(stderr,"lit",...)`; gated traces by hand. Each PR lowers LG12a in tools/cxx17_ratchet/baseline.json for that dir (ratchet only goes down). LG12b is tracked separately and is not a facade target.
P3 Retire: SPEW shim, logging.h, getenv statics, OutputDebugString sites, DELETE gates; LG12a -> 0 with allow-list for crash/signal handlers and facade internals (A1). LG12b is not driven to 0 by this plan.
P4 Optional: parsers consume JSON (MC2_LOG_JSON dual-emit), alias table removal.

## 7. First slice
Pilot: GameOS/gameos/gos_terrain_patch_stream.cpp (14 LG12; gates MC2_PATCH_STREAM_TRACE, MC2_BUCKET_CENSUS; no parsed tag) to prove the pipeline. Then the terrain family: gos_terrain_indirect (46, owns parsed [TERRAIN_INDIRECT v1], exercises byte-exact golden), gos_terrain_water_stream (32), gos_terrain_lod_chunk (43), mask_dispatch (4), height_tex (2), lighting (3). ~144 sites, ~16 gates, self-contained, terrain-gates-plan.md already exists. Exit: GameOS LG12 delta recorded, golden green, TERRAIN_INDIRECT oracle unchanged.

## 8. Verification
- tools/log_tag_census.py before/after: set of `[TAG vN] key=` prefixes and key order identical (empty diff).
- C++ golden harness: legacy printf vs MC2_EVENT bytes per parsed tag with edge values (floats, negatives, large ints, empty/null strings, nullptr %s, %ld/%lld widths, braces). Must pass before any P2 adoption (A8).
- Disabled path: zero formatting calls and zero argument evaluation when a site is off (A7).
- Python: feed harness output through scripts/smoke_lib/logparse.py and oracleparse.py (tests/smoke, tests/unit) -> same summaries as legacy.
- `py -3 tools/cxx17_ratchet/cxx17_ratchet.py` (LG12 down, never up) and tools/cxx17_ratchet/test_cxx17_ratchet.py.
- tier1 smoke (scripts/run_smoke.py --tier tier1): normalize timings/addresses, compare per-tag line counts old vs new.
- Disabled-path: with MC2_LOG unset, no output and no formatting (test-build counter); PERF v1 p99 unchanged.
- grep guards: `git grep -nE 'fprintf\(stderr, *"\[[A-Z_]+ v[0-9]+\]'` count only decreases after golden passes; `git grep MC2_LOG` confirms "1" semantics intact in gameosmain.
