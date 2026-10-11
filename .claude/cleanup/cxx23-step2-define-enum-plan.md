<!-- doc-status: current | reviewed:2026-10-11 -->
# C++23 step 2: `#define` literal constants -> `inline constexpr` (LG13) and unscoped enums -> `enum class` (LG14)

Owner: lane 2 (lane 1 keeps step 1, the clang-tidy safe set, `cxx23-step1-tidy-plan.md`). Same proof idea as step 1: every rewrite is a no-op when the including
objects hash identically (`tools/objhash.py`), so nobody has to read the diff.

## LG13: what the tool rewrites (`tools/tidy/rewrite_defines.py`)
`#define NAME <literal>` becomes `inline constexpr <type> NAME = <literal>;` at the same line (a `.cpp`-local define becomes plain `constexpr`: internal linkage, so two files
may keep different values for one name). Census at 2026-10-11 (literal defines only; the ratchet's 2,892 also counts expression defines such as `(A + 1)`): 3,344 literal
defines, 2,311 eligible (1,968 in headers, 343 `.cpp`-local). Refused: 613 name also appears in a script / cmake / resource / shader / third-party file, 232 defined
more than once, 105 header reachable from a C / `.rc` / `.asm` file, 36 used inside another `#define`, 26 not at namespace scope, 10 `__asm` file, 10 `#if` / `#ifdef` /
`#undef` / reserved name.

Eligibility (all of them): used only as a value (no directive use, no use in another macro body, so no pasting or stringizing, no string adjacent to a string literal,
no `sizeof` of a string macro); defined once in the repo; at namespace or `extern "C"` scope; not an include guard (`_H` and similar); type unambiguous on MSVC (LLP64):
`int`, `unsigned int` (hex beyond INT_MAX or a `u` suffix), `long` / `unsigned long` (`L`), `long long` (`LL`), `float` (`f`), `double`, `const char*`; a decimal beyond INT_MAX,
`-` on an unsigned, an octal with 8 or 9, expressions and wide strings stay macros. For a header define every `.cpp` that mentions the name must be one of its including
objects, so the new external name cannot meet a same-named global in a TU that never saw the macro.

## Proof and cadence
1. Batch = files chosen by `rewrite_defines.py run` (`.cpp`-local first, then headers by including-object count; `--max-files`, `--max-objs`). Excluded: files dirty in this
   worktree or in `A:/Games/mc2-main` (lane 1's tidy headers; `--skip-dirty-in`), `run_tidy_safe` owner / lane exclusions, anything `--done` already lists.
2. `Proof` (shared with `run_tidy_headers.py`): hash every including object, apply the edits, rebuild them in one ninja call, compare code / data / relocations; a file whose
   includers differ or fail to compile is bisected out and reverted (typical causes: a `const T&` binding that now ODR-uses the constant, a `sizeof` of a string).
3. Per batch (one-line report `BATCH n: kept F file(s) / S site(s), reverted R`): commit by pathspec (`--commit`), `cxx17_ratchet --check --tighten`, commit the baseline.
   Doctest (`build_contract_harnesses.py --run --unit`) after each batch; smoke-lite plus the board per two batches; tier1 per six. A header batch also needs
   `build_all_targets.py` (tools and the editor include game headers too) before its smoke.
4. Merge nifty first and keep batches small while lane 1's header tidy is running: both lanes edit headers, and `tools/lane_merge.py` refuses a dirty overlap.

## Risks the proof does not cover (why the eligibility list is strict)
- Anything not compiled in the build tree (the editor and tool targets when only `build64-ninja` is built): run `build_all_targets.py` before each header batch's smoke.
- A third-party or SDK header that checks `#ifndef NAME` and defines its own: names found in `3rdparty/` and non-C++ files are refused; SDK headers are covered by the compile.
- Debugger / tooling that reads a macro by name: none in the repo (scripts naming the identifier refuse the rewrite).

## LG14: unscoped enum -> `enum class` (333 sites) - design note first, no batches until it lands
Converting an enum changes every use that relied on implicit `int` conversion, arithmetic, comparison with ints, use as an array index, bit-flag `|` / `&`, `switch` on an int,
or a `case` label of the enumerator from an int. The rewrite is therefore NOT a no-op in source terms (the proof still says whether the object changed, but a compile
failure is the common outcome). Plan:
1. `tools/tidy/enum_census.py` (to write): per enum, list the enumerators, every use site, and flag arithmetic / index / bitmask / int-compare / serialized use (file or
   network layout) by a conservative token scan plus a compile probe (flip the enum in a scratch copy, count the errors).
2. Convert only enums with zero flagged uses, in the same batch loop and proof as LG13; add `using enum` at use sites only where it removes churn (C++20).
3. Enums used arithmetically, as flags or as indices stay unscoped, listed in a skip ledger with the reason, so the ratchet baseline documents the floor.
4. Enums in headers shared with tools or editor: compile every target (`build_all_targets.py`) before the proof counts.

## Status
- 2026-10-11: tool + tests + census written, first `.cpp`-local batches in progress (see `lane2-state.md` for the last batch line).
