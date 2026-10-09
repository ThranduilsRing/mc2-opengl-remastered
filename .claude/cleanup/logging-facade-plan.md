# Logging facade plan (C++23, MSVC 14.44). Design only, nothing built.

## 1. Survey (code GameOS mclib RenderWorld RenderCore GuiRuntime GameAdapters HostServices editor tools)
| Mechanism | Lines | Files |
|---|---:|---:|
| fprintf(stderr/stdout) | 1600 | 172 |
| bare printf( | 789 | 119 |
| LG12 ratchet total (printf-family) | 2704 (code 1001, mclib 954, GameOS 707, GuiRuntime 34, GameAdapters 6, RenderCore 2) | ~190 |
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
3. Stream preserved per site (Stream::Err/Out) so the NUL-redirect-when-MC2_LOG-unset still works. One fwrite per line from a thread_local buffer (no interleave tearing; printf was line-atomic too).
4. printf->format hazards (codemod must flag, golden must cover): %g/%e vs {:g}/{:e}; %5.2f -> {:5.2f}; %x/%08X; %p -> void* cast; %s with nullptr (UB in format; wrap in `safe(s)`); %u/%d signedness; %zu/%lld; char printed as int; %% -> %; locale (std::format is locale-independent, matches "C").
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

extern std::atomic<Level> g_threshold[size_t(Cat::Count)];     // parsed from MC2_LOG
inline bool enabled(Cat c, Level l) noexcept {                  // whole disabled-path cost
  return l >= g_threshold[size_t(c)].load(std::memory_order_relaxed); }
bool topicEnabled(Cat, std::string_view topic) noexcept;       // "terrain:indirect"
bool eventEnabled(Cat) noexcept;                                // legacy-gate equivalent

void write(Cat, Level, Stream, std::string_view line, const std::source_location&);
void writeRaw(Stream, std::string_view line_incl_newline) noexcept;   // events, crash path (no alloc)
bool allowRate(uint64_t siteKey, double perSecond) noexcept;   // per-site token bucket
bool allowOnce(uint64_t siteKey) noexcept;
void init();                      // call in gameosmain after the MC2_LOG NUL-redirect decision
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
| SPEW((group,fmt,...)) | shim forwarding to MC2_LOG(Debug) (group->Cat table), then retire |
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
P2 Adopt per directory, leaf first: GameOS terrain slice -> other gos_* -> utils -> GuiRuntime/GameAdapters -> mclib -> code. Codemod handles `fprintf(stderr,"lit",...)`; gated traces by hand. Each PR lowers LG12 in tools/cxx17_ratchet/baseline.json for that dir (ratchet only goes down).
P3 Retire: SPEW shim, logging.h, getenv statics, OutputDebugString sites, DELETE gates; LG12 -> 0 with allow-list for crash/signal handlers and facade internals.
P4 Optional: parsers consume JSON (MC2_LOG_JSON dual-emit), alias table removal.

## 7. First slice
Pilot: GameOS/gameos/gos_terrain_patch_stream.cpp (14 LG12; gates MC2_PATCH_STREAM_TRACE, MC2_BUCKET_CENSUS; no parsed tag) to prove the pipeline. Then the terrain family: gos_terrain_indirect (46, owns parsed [TERRAIN_INDIRECT v1], exercises byte-exact golden), gos_terrain_water_stream (32), gos_terrain_lod_chunk (43), mask_dispatch (4), height_tex (2), lighting (3). ~144 sites, ~16 gates, self-contained, terrain-gates-plan.md already exists. Exit: GameOS LG12 delta recorded, golden green, TERRAIN_INDIRECT oracle unchanged.

## 8. Verification
- tools/log_tag_census.py before/after: set of `[TAG vN] key=` prefixes and key order identical (empty diff).
- C++ golden harness: legacy printf vs MC2_EVENT bytes per parsed tag with edge values (floats, negatives, large ints, empty/null strings).
- Python: feed harness output through scripts/smoke_lib/logparse.py and oracleparse.py (tests/smoke, tests/unit) -> same summaries as legacy.
- `py -3 tools/cxx17_ratchet/cxx17_ratchet.py` (LG12 down, never up) and tools/cxx17_ratchet/test_cxx17_ratchet.py.
- tier1 smoke (scripts/run_smoke.py --tier tier1): normalize timings/addresses, compare per-tag line counts old vs new.
- Disabled-path: with MC2_LOG unset, no output and no formatting (test-build counter); PERF v1 p99 unchanged.
- grep guards: `git grep -nE 'fprintf\(stderr, *"\[[A-Z_]+ v[0-9]+\]'` count only decreases after golden passes; `git grep MC2_LOG` confirms "1" semantics intact in gameosmain.
