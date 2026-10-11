<!-- doc-status: current | reviewed:2026-10-11 -->
# C++23 step 3 plan: printf-family to std::format, through the logging facade (plan only, nothing edited)

Baseline `tools/cxx17_ratchet/baseline.json` at ed90ebc5e: **LG12a 1295** (printf-style logging), **LG12b 1497** (sprintf-style buffers).
Design lives in `logging-facade-plan.md` (A1-A11); this plan is the mechanics, the proof and the order. Open decisions are in section 9.

## 1. Inventory
| dir | LG12a | LG12b | heaviest files |
|---|---:|---:|---|
| GameOS | 613 | 115 | gos_render 100, gameosmain 42, gos_terrain_water_stream 32, gameos_graphics 25, gos_validate 23 |
| mclib | 449 | 563 | file 31, terrain_init 30, render_contract 23 / cmponent 72, inifile 33+30, terrtxm 29, abldbug_print 29 |
| code | 220 | 790 | gamecam 23 / mover_diag 105, gameobj 74, mpparameterscreen 25, mechlopedia 25, missiongui_uicmd* 48 |
| GuiRuntime / GameAdapters / RenderCore | 7 / 4 / 2 | 27 / 2 / 0 | GraphicsOptionsWindow_passes 25 |

Sinks of the LG12a sites (line scan of tracked sources, superset of the ratchet scope because it includes editor/ and tools/; a call split over lines is
counted once at its first line, so every figure is a floor): stderr 665, stdout 575, SPEW macro 184, other FILE* 140, OutputDebugString 19.
`[TAG vN]` literals on the same line: 186 (stderr 94, stdout 89, file 3), 78 distinct versioned tags. MC2_EVENT-shaped lines are about 12 % of LG12a.
Stdout and stderr are both redirected to NUL when MC2_LOG is unset (gameosmain.cpp ~1145-1175): the sink choice is behaviour and must not change.
LG12b: destinations are stack `char[N]` buffers handed to gos text draw, file-name building, `strcat` chains and ABL; 26 `vsnprintf/vsprintf` sites;
`mclib/estring.cpp:571` (`EString::Format`) already sizes with `vsnprintf` and stays as is. Unbounded `sprintf` into an array is the memory-safety target.
Specifier mix over LG12 lines: `%s` with width 1081, `%l[du]` 272, `%ll[dux]` 38, `%z[ud]` 50, `%p` 30, `%c` 29, `%e/%g` 15, `%I64` 0, `%n` 0, `%*` 0.

## 2. The facade as it stands
`mc2::log` (`GameOS/gameos/utils/mc2log.{h,cpp}`) **is built** (facade P0/P1: `logFmt`, `event<Tag,Ver>`, `MC2_EVENT*`, `MC2_LOG*`, `writeRaw`, `init()`; used by gameosmain, gos_terrain_lighting, gos_terrain_patch_stream; tests/unit/test_mc2log.cpp) -- the first version of this plan said otherwise and was wrong. Today only `GameOS/gameos/utils/logging.h` exists: `log_debug/info/warning/error` over
`logging::logmsg(level, __FILE__, __LINE__, fmt, ...)`, 56 calls in 6 files. Coupling: `logmsg` prefixes `file(line): ERROR`, and
`scripts/smoke_lib/logparse.py:36` matches `shader_builder\.cpp\(\d+\)\s*:\s*ERROR` as a shader-failure bucket. That prefix is a CONTRACT; `logging.h`
stays (held item in `legacy-subsystem-audit.md`) until the pattern and the producer change in one commit.
Batch 0 adds the primitives the conversions need that the facade lacks, in `GameOS/include/mc2fmt.h` (HEADER-ONLY: mclib is linked by standalone tools that do not link GameOS; done, `57f51d61d`):
`mc2::fmt::to_buf` / `to_buf_n` (truncating, always NUL-terminated, the would-be length returned like `snprintf`; the array form takes the bound from the type),
`safe()` and the `as_*` argument helpers. The untagged-printf sink for LG12a (B4) is a raw `print(Stream, fmt, args)` over the existing `mc2::log::writeRaw`
(no level filter, no added newline, ONE `fwrite`): `MC2_LOG` cannot be used for it because `write()` appends a newline and applies the level gate. It lives in `mc2log`
(GameOS only), so mclib's LG12a sites wait for a decision on how mclib reaches the facade (section 9, item 4).

## 3. Contract lines that must stay byte-identical
Enumerated from the consumers, not the C++:
- `scripts/smoke_lib/logparse.py` (27 regexes), `oracleparse.py` (12), `run_smoke.py` (`_VEG_RE`, 1), `fingerprint.py`, `evidence_classifier.py`,
  `crash_evidence.py`, plus `sim_identity.py`, `sweep_campaign.py`, `check_vk_nointerop_log.py`, `run_editor_playtest_smoke.py`, `tools/residency_slim.py`.
- Strict-shape tags: `[PERF v1]` key order and spacing, `[TIMING v1]`, `[SMOKE v1] event=summary result= reason= stage=`, `[HEARTBEAT] frames= elapsed_ms=`,
  `[CONFIG v1] warn=premain_read`, `[SPLIT_PATH_PARITY] ... match=0`, `[MOVER_COHERENCE] VIOLATION|ADVISORY`, `[ASSET_LOAD v1] event=`, `[APPEAR_AUDIT v1] event=missing`,
  line-anchored `^CRASH:` and `^[CRASHBUNDLE]`, `[GPUPROPS] failed to compile`, `[SHADER ERROR]`, un-bracketed `VEG_SUMMARY`/`ORDER_LIVENESS_SUMMARY`.
- Free-text signatures: `Shader filename:...failed to load shader`, `vkCreate*...failed`, `pipeline_failed`, `Unable to open Mission File`, `Missing file:`.
- `comment_guard_needles.txt` protects COMMENTS that checkers read; it is not a log contract and is not used for this.
- A line scan finds 359 distinct bracket tags in printf-family calls and 32 in Python parsers, but only 11 in both because `[PERF v1]` and friends are
  emitted from a format string on a continuation line (`gos_smoke.cpp:204`). **Batch 0 builds `tools/log_tag_census.py`** (joins adjacent string literals and
  continuation lines) and freezes `tests/unit/log_tag_baseline.json`: tag, version, key order, stream, file. The contract set is every tag with a Python consumer plus
  every free-text signature. A converted site whose census row changes is rejected.

## 4. Mechanics
- Tool: `tools/tidy/printf_to_format.py`, a text transform behind the same `--rewrite` plumbing as `typedef-aggregates` and `null-tokens` (pick, apply, build, verify, revert per file).
  It tokenises the call (callee, FILE* argument, adjacent format literals, top-level argument split); anything it cannot prove is left alone and listed in a ledger.
- Specifier map (typed, so `std::format_string` rejects a mismatch at compile time): `%d %i`->`{:d}`, `%u`->`{:d}`, `%x %X %o`->`{:x} {:X} {:o}` with width/fill (`%08X`->`{:08X}`),
  `%5.2f`->`{:5.2f}`, `%c`->`{:c}`, `%s`->`{}` over `const char*` wrapped in `safe()` (nullptr prints `(null)` as the CRT does), `%%`->`%`, literal `{` `}` doubled (A5).
  `%ld %lu`->int32/uint32 explicitly (LLP64: long is 32-bit), `%lld %llu`->int64/uint64, `%zu`->size_t.
- LG12b: `snprintf(buf, N, ...)` -> `mc2::fmt::to_buf(buf, ...)`; `sprintf(buf, ...)` into an array -> `to_buf` bounded by the array extent (the safety gain);
  `sprintf` into a pointer or unknown size is not rewritten and goes on a hand list.
- LG12a: untagged `fprintf(stderr|stdout, ...)`/`printf` -> `mc2::log::print(Stream::Err|Out, ...)`; `[TAG vN]` lines -> `MC2_EVENT` only after their golden test (facade rule A8);
  SPEW and OutputDebugString stay for facade P3.
- Do NOT use `std::print`: on a Windows console it transcodes to UTF-16 and changes bytes; the facade writes with `fwrite`, as printf did.

## 5. Proof (objhash does not apply: the output code changes)
1. Compile-time format/argument checking by `std::format_string` (the property printf lacks); every rejection is a finding, not a silent fix.
2. **Corpus**: the tool emits `tests/unit/printf_corpus.tsv` (old printf format, new format string, argument types) for every converted site; a doctest instantiates old and new with an
   edge-value table per specifier (0, negative, INT_MIN/MAX, UINT_MAX, 1e-7, 1e22, NaN, inf, empty and over-long strings, nullptr `%s`) and compares bytes (`snprintf` vs `format_to_n`).
3. **Runtime log identity**: smoke-lite, mc2_03 and mc2_24 with `MC2_LOG=1`, stdout and stderr captured before and after each batch; normalise addresses, pids and timing values;
   the SET of line shapes (every number masked) and the SET of tags must be equal. This proves spellings, key order and lost lines, NOT numbers (sign, width, precision, hex case: the corpus proves those), and a tag that never fires on these missions is not exercised at all. Known printf/format difference, pinned in test_printf_corpus.cpp: the indefinite NaN prints `-nan(ind)` under the CRT and `-nan` under std::format; no parser accepts either. Fed through `logparse.py`/`oracleparse.py` (tests/smoke) the summaries must be equal.
4. Census diff empty (`log_tag_census.py --check`, new board row), ratchet LG12a/LG12b only down, board 165+1, doctest, tier1 every sixth batch.

## 6. Batching (about 50 files each; LG12b first because it is the memory-safety win)
B0 census tool, corpus harness, `mc2fmt.h` + doctests, no call-site change. B1 LG12b GuiRuntime/GameAdapters/GameOS (144). B2 LG12b mclib (563, by file; `EString::Format` kept).
B3 LG12b code (790; mover_diag, gameobj first). B4 LG12a untagged GameOS/mclib/code (no census row). B5 LG12a tagged, no Python consumer. B6 LG12a parsed tags, one tag at a time, golden first.
A batch is rejected whole if any proof step fails; the worktree is left for a human, as in `tidy_batch.py`.

## 7. Risks
| risk | handling |
|---|---|
| `%p` | MSVC prints 16 upper-case hex digits without `0x`; `{:p}` prints `0x...` lower-case. Convert as `{:016X}` of `uintptr_t` (30 sites) |
| `%s` with nullptr | printf prints `(null)`; `std::format` is UB. `safe()` wrapper, covered in the corpus |
| `%ld/%lu` width | 32-bit on Windows; a 64-bit argument is an existing bug: the tool lists it instead of widening |
| `%e/%g` (15) | exponent digits and precision rules compared in the corpus before any site converts |
| locale | engine never calls `setlocale`; printf is the C locale and `{}` is locale-independent, so equal. Never use `{:L}` |
| varargs UB / wrong-type args | become compile errors under `format_string`; each is reported as a finding and left unconverted until fixed by hand |
| `%n`, `%I64`, `%*` | 0 sites; the tool refuses them |
| compile time and code size | out-of-line `vformat` (A10); measure `gos_terrain_lighting.cpp` and the PCH-heaviest TUs per batch; `<format>` goes in the PCH only if the audit stays clean |
| interleaving and flush | same FILE*, one `fwrite` per line, no new buffering: order against remaining printf calls is unchanged |

## 8. Logparse changes per batch
None are planned: parsers are not touched while sites migrate (facade rule A5/8). The only coupling that needs a paired change is `logging.h`'s `file(line): ERROR` prefix vs
`logparse.py:36`, handled in its own commit with both sides, after B6.

## 9. Decisions for the reviewer
1. Build `mc2fmt.h` (to_buf, print) now, ahead of facade P1, or fold step 3 into P1-P2 and wait for categories/levels?
2. LG12b destinations that are not arrays (pointer buffers, `char*` members): hand-fix list or out of scope for step 3?
3. Accepted: parsed tags (B6) wait on a golden harness per tag; a tag that fails it stays on printf with an allow-list entry.
4. How does mclib (linked by standalone tools without GameOS) emit through `mc2::log` in B4? Header-only `print` over `fwrite` for mclib, or a link dependency?
