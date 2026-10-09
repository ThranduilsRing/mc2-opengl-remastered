# C++17 strangler-fig plan (measured 2026-10-09)

Backbone: `tools/cxx17_ratchet/cxx17_ratchet.py` (+ `baseline.json`, `test_cxx17_ratchet.py`). Stdlib Python, ~2.5 s for 1186 files.
Owner direction: `project-cpp17-to-cpp23-goal` + `feedback-full-modernization-approved` (mechanical batches OK repo-wide, fix forward).
Counts are HEURISTIC site counts (comments/strings blanked, regex per line), stable not exact. Baseline taken on the dirty working tree at HEAD `a1a25de29`.
Scope = GameOS mclib code RenderCore GuiRuntime RenderWorld GameAdapters HostServices EditorBridge. `editor/ gui/ Viewer/` opt-in via `--dirs`.

```
py -3 tools/cxx17_ratchet/cxx17_ratchet.py --report [--by-file 10] [--depth 2]
py -3 tools/cxx17_ratchet/cxx17_ratchet.py --write-baseline     # only when intentionally re-basing
py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check [--tighten]  # exit 1 if any (dir, idiom) count rose; --tighten lowers baseline on success
```

## 1. Baseline counts

| dir | LG01 | LG02 | LG03 | LG04 | LG05 | LG06 | LG07 | LG08 | LG09 | LG10 | LG11 | LG12 | LG13 | LG14 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| GameOS | 187 | 265 | 301 | 6 | 123 | 609 | 0 | 274 | 2731 | 79 | 118 | 707 | 383 | 67 |
| mclib | 2926 | 316 | 1389 | 17 | 143 | 735 | 0 | 76 | 2598 | 36 | 69 | 954 | 1312 | 128 |
| code | 1857 | 232 | 978 | 21 | 202 | 466 | 0 | 15 | 2526 | 22 | 101 | 1001 | 1203 | 137 |
| GuiRuntime | 1 | 0 | 0 | 0 | 4 | 12 | 2 | 1 | 45 | 4 | 1 | 34 | 0 | 0 |
| RenderCore | 0 | 2 | 0 | 0 | 28 | 15 | 0 | 0 | 38 | 0 | 0 | 2 | 9 | 2 |
| small seams (RenderWorld, GameAdapters, HostServices, EditorBridge) | 0 | 0 | 0 | 1 | 19 | 23 | 0 | 0 | 25 | 0 | 0 | 6 | 0 | 0 |
| **TOTAL** | 4971 | 815 | 2668 | 45 | 519 | 1860 | 2 | 366 | 7963 | 141 | 289 | 2704 | 2907 | 334 |

LG15 (auto_ptr/bind1st/register/throw()) = 0. Biggest files: LG01 code/ablmc2.cpp 202; LG03 code/pilotreviewarea.cpp 93; LG09 GameOS/gameos/vulkan_backend_skeleton.cpp 398;
LG13 mclib/sounds.h 207, code/dobjnum.h 189; LG02/LG14 GameOS/include/gameos.hpp + platform_win*.h (shim headers: consider excluding or accepting).
Caveats: LG04 counts every `switch(x->...type|kind)` including plain enums (45 sites, ~12 true tagged unions); LG09 includes `(unsigned long)x` printf casts; LG06 includes decls and defs.

## 2. Idiom table

| ID | Replacement | Value | Risk | Class | Tool |
|---|---|---|---|---|---|
| LG01 NULL | nullptr | low-med (varargs/overload bugs) | very low; check `NULL` in ABL macros / varargs / `#define`d tables | MECHANICAL | `modernize-use-nullptr` (already AUTO-FIX SAFE) |
| LG02 typedef | using | low | very low (anon-struct typedef skipped by tool, leave) | MECHANICAL | `modernize-use-using` |
| LG03 raw new/delete | unique_ptr/make_unique/containers | high (leaks, double free) | med-high: ownership is custom (heap.cpp, Auto_Ptr, `gos_Malloc`) | SEMANTIC (per class); only trivial locals mechanical | `modernize-make-unique` only after owner is already smart; `cppcoreguidelines-owning-memory` for audit |
| LG04 tagged union/switch | variant + visit | highest | high (layout, save files, ABL ABI) | SEMANTIC/design | none |
| LG05 bool+out / sentinel | optional | high (unchecked sentinels) | med | SEMANTIC | `bugprone-unused-return-value` / `[[nodiscard]]` as guard |
| LG06 const char*/const string& | string_view | med (alloc, strlen) | med: NUL-termination callers (fopen, GL, getenv); never on stored members | SEMANTIC-lite (per signature) | none |
| LG07 enable_if/tag dispatch | if constexpr | low (2 sites) | very low | SEMANTIC but trivial | none |
| LG08 .first/.second | structured bindings | low readability | very low | MECHANICAL-ish | `modernize-loop-convert` then hand-bind |
| LG09 C-style cast | static_cast etc | med (hides reinterpret / const-drop) | low per site, huge volume | MECHANICAL for numeric; SEMANTIC for pointer | `google-readability-casting` (review, never `-fix` blind on pointers) |
| LG10 min/max chain | std::clamp | low | low (NaN / lo>hi UB: check) | MECHANICAL | `readability-use-std-min-max` (clang-tidy 19+) |
| LG11 Win32/path concat | std::filesystem | med (portability, `\\`) | med: data paths are case/separator-sensitive | SEMANTIC (path utility seam) | none |
| LG12 printf logging | std::format facade | med (type safety) | low if facade keeps printf backend first | SEMANTIC (facade), then mechanical call-site rewrite | none (script) |
| LG13 #define consts | inline constexpr | med (typed, scoped, debuggable) | low; skip macros used in `#if` or ABL tables | MECHANICAL | `cppcoreguidelines-macro-usage` (reports), `modernize-macro-to-enum` (clang-tidy 18+) |
| LG14 unscoped enum | enum class | med | HIGH volume of implicit int use / bit flags / serialized values | SEMANTIC (flags need operator helpers) | `modernize-use-scoped-enum` doesn't exist; hand/script |
| LG15 removed facilities | n/a | - | - | MECHANICAL | compiler |

Suggested order of mechanical fan-out (matches pareto doc): LG01, LG02, LG13 (`modernize-macro-to-enum` only for related groups; otherwise script to `inline constexpr`), LG10, LG08, then LG09 numeric-only.

## 3. Top 10 semantic seams (file:line)

1. **ABL value cells**: `mclib/ablexec.h:62` (`StackItem` union) + `:73` (`ABLStackItem {long type; union data}`) + `mclib/ablsymt.h:30` (`Value`) and `:109` (`DefinitionInfo` union keyed by `Definition.key`). `switch(value.type)` consumers at `code/ablmc2.cpp:6427,7335`, `mclib/ablstd.cpp:229`, `mclib/ablxstd.cpp:1038,1054`. `std::variant<int,float,bool,char,int*,float*,bool*,char*>` + `std::visit` removes type/data desync in the interpreter; keep a layout shim for ABL save state. Highest bug density in the repo.
2. **HUD draw calls**: `GameOS/gameos/gameos_graphics.cpp:1321` (`HudDrawKind` + `HudDrawCall` carries `fontTexId/foregroundColor` "kHudTextQuadBatch only") replayed by `switch(call.kind)` at `:8697`. Becomes `variant<QuadBatch,LineBatch,TriBatch,TextQuadBatch>`; fields can no longer be read for the wrong kind. Pure GameOS-internal, no ABI, fastest proof of the pattern.
3. **GameOS input/events**: `GameOS/gameos/gameosmain.cpp:502` `switch(event.type)` over SDL union -> typed `GosEvent = variant<KeyEvent,MouseEvent,WindowEvent,QuitEvent>` produced at one translation point, consumed via visit (`input::handleKeyEvent`).
4. **Trigger areas**: `code/trigger.h:34-42` (`char type; long param; dim[4]`) with `switch(type)` at `code/trigger.cpp:119,148`. `variant<MoverArea,TeamArea,GroupArea,CommanderArea>`; small, self-contained, good second proof.
5. **Dynamics/control unions**: `code/mover.h:231` (`DynamicsInfo`) and `:310` (`ControlData`), selected by `DynamicsType` (`:228`). Variant per vehicle class; also `code/goal.h:33` (`GoalInfo` object|region), `code/artlry.h:165`, `code/carnage.h:32`, `mclib/cmponent.h:106` (component stats union keyed by type). Touches save/load: validate with save round-trip.
6. **Sentinel returns in logistics**: `code/logisticsdata.cpp:727-795` (`purchaseMech/sellMech/removeVariant` return `-1`), `code/logisticsdata.cpp` 40 sites, `code/logisticsmissioninfo.cpp`, `code/objective.h` (23). `optional<int>` / result enums; many callers ignore the int, so add `[[nodiscard]]` first.
7. **Out-param getters on render seams**: `GameOS/gameos/gos_static_prop_registry.h:298-299` (`staticPropGetTexArrayLayer/GetMaterialIdx(..., T* out)`, "returns false + sets *out to sentinel"), `GameAdapters/StaticPropRenderAdapter.h:168-177` (`recipeTypeId/recipeLightDataIndex/recipeInstanceFlags/recipeHadCullRecord/getMaterialGpuData`), `RenderCore/KtxLoader.h:55` (`ktxLoadRgba8(path, KtxImage& out)`), `GameOS/gameos/gos_static_prop_batcher.cpp:11219` (`mc2PeekStaticPropTypeInfo`). All -> `std::optional<T>`; new code, few callers, low risk.
8. **string_view on name lookups**: `mclib/txmmgr.h:530-540` (`textureInstanceExists/loadTexture/saveTexture`), `mclib/inifile.h:85-102,153-` (`textToX(const char*)`, `readIdX(const char* varName, T&)` - also an optional seam: `readIdLong(name, long&)` returns status), `GameOS/gameos/gameos_graphics.cpp` (114 LG06 sites), `gos_postprocess.cpp` (93). Watch NUL-termination at fopen/GL boundaries.
9. **if constexpr**: `GuiRuntime/GuiRuntime.cpp:288,294` (`makeImTextureID` enable_if pair on pointer/non-pointer) -> single function with `if constexpr (std::is_pointer_v<T>)`. Only 2 SFINAE sites exist, so this is a showcase/cheap win; the larger "type switching" lives in macros (`mclib/stuff/*`, `ABL_*` tables) not templates.
10. **Paths/FS**: `mclib/file.cpp:234,319,330,815,829` (FindFirstFileA/GetFileAttributes loops, manual `\\`), `code/mechcmd2.cpp` (24), `code/logisticsdata.cpp` (18), `mclib/terrain.cpp:1406,1575` (`strrchr(...,'\\')`), `GameOS/gameos/gos_dev_shell.cpp` (17). One `mc2::fs` helper over `std::filesystem` (MSVC fully supports; gcc 9+ links without -lstdc++fs), then migrate callers. Case sensitivity of game data on Linux is the risk.

Also: `code/brain_order_intent.h:57` `BrainOrderIntent` (kind + "unused fields are zeroed") and `code/brain_special_dispatch.h:105` `FsmTodoEntry` are already post-C++11 tagged structs - good variant targets that need no ABI care (new code, owned by the brain lane).

## 4. Strangler process per seam

States, recorded in this file's checklist (section 6) and enforced by the ratchet:
1. **Introduced**: add the modern type next to the legacy one (new header, e.g. `HudDrawCall` stays, add `HudDrawVariant`). Adapter both directions (`toLegacy()/fromLegacy()`). Ratchet: count unchanged, legacy site count must not rise.
2. **Validated**: new path runs behind the existing consumer in shadow/assert mode (compare legacy vs variant result) - unit/harness test under `tools/*_harness`, `check-contracts.sh`, smoke + A/B (`ab.py`) as required by the C++20/23 goal doc. For save-format unions, a round-trip test.
3. **Adopted**: callers switched file-by-file; each batch = one commit + build both configs + contracts + smoke. After each batch run `--check --tighten` and commit the lowered `baseline.json` with it.
4. **Retired**: delete the legacy type/adapter; ratchet count for that file reaches 0 and stays (per-file counts in baseline.json show regressions; `--report --by-file` finds the next target).
The baseline only moves down: `--write-baseline` is for deliberate re-basing (new scan rule, new dir), never to "fix" a red check. A red check means a new legacy site was added; use the modern form.
Escape hatch for unavoidable legacy (e.g. a Win32 header needing `typedef`): put it under 3rdparty/shims or accept by lowering elsewhere in the same dir - no suppression comments, so counts stay honest.

## 5. Wiring into scripts/check-contracts.sh (do not apply from here)

Add next to the other `py -3 tools/...` rows (e.g. after `bless_selftest`, ~line 776). Rows are `run_check "label" "cmd"`; commands run from the worktree root; a prose comment line precedes each row group:

```bash
# CXX17-RATCHET-1: C++17 strangler-fig ratchet. Fails if the count of ANY legacy idiom
# (NULL, typedef, raw new/delete, tagged unions, sentinels, C casts, printf logging, ...)
# rose in ANY owned dir vs tools/cxx17_ratchet/baseline.json. Pure regex scan, ~3 s, read-only.
# After paying idioms down run `--check --tighten` and commit baseline.json with the change.
run_check "cxx17_ratchet"          "py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check"
run_check "cxx17_ratchet_selftest" "py -3 tools/cxx17_ratchet/test_cxx17_ratchet.py"
```
The `_register_row` guard rejects duplicate labels/commands; both are unique. Also check `scripts/check-contract-coverage.py` accepts a `tools/` script (it credits runners invoking it; the selftest row covers the unit test file). Caveat: the baseline is taken on a dirty tree at a1a25de29; re-run `--write-baseline` once the in-flight batch (nullptr/using/override) lands, so improvements are locked in immediately.

## 6. Roadmap to C++20/23

Environment facts to verify at each step (do not assume): MSVC (VS 2022 17.x) supports C++20 fully and C++23 partially under `/std:c++latest`; `<print>` and `std::expected` need 17.7+/17.3+ resp. (`<print>` 17.7, `std::expected` 17.3, `std::format` 17.2 with /std:c++20). Linux gcc: C++20 solid from 11; `<format>` needs gcc 13+, `std::expected`/`<print>` gcc 12.1/14. clang+libc++ lags on `<format>`/ranges; keep `{fmt}` as a fallback if the Linux toolchain is older.

| Phase | Gate | Work |
|---|---|---|
| P0 now | consistency finished | ratchet in contracts; mechanical LG01/02/13/10/08 batches per dir (RenderCore, GuiRuntime, GameOS, mclib, code); seams 2,4,7,9 as pilots |
| P1 C++17 depth | ratchet counts falling | `[[nodiscard]]` on sentinel getters, `string_view` signatures (seam 8), `std::filesystem` helper (10), `enum class` in new/edited headers, if-init, `std::optional` (6,7), variants (1,3,5) |
| P2 logging facade | P1 seams 7/8 done | single `mc2::log(level, fmt, args...)` in GameOS with a printf backend first; macro `MC2_LOG` captures file/line; call sites migrated LG12 (2704) by script, 'sprintf into buffer' sites go to `std::format_to_n`/`snprintf` wrapper; count LG12 in ratchet already |
| P3 C++20 bump | MSVC + gcc builds green on `/std:c++20`; both configs, contracts, smoke, A/B | `CMAKE_CXX_STANDARD 20`; fix `u8` literal and `operator==` rewrite diagnostics, `char8_t`; then adopt `std::format` in facade, `std::span` for (ptr,count) params (new ratchet LG16), `<ranges>`/algorithms selectively, `consteval`/`constinit` for tables, `std::bit_cast` for LG09 pointer punning, designated initializers for render descs, `<=>` for POD compare |
| P4 C++23 bump | toolchains verified (above) | `std::print/println` in facade, `std::expected` for fallible loaders (ktx, ini, shader build) replacing LG05 bool+error-string, `std::mdspan` for terrain grids (optional), `if consteval`, `std::unreachable`, `std::to_underlying` for `enum class` flags, deducing this where it removes CRTP |
Each phase: build both configs, `check-contracts.sh`, smoke (must fail on shader compile errors - lesson 2026-10-09), A/B capture eyeballed. Add new ratchet IDs (LG16 raw pointer+count params -> span; LG17 bool+error-string -> expected) as the phases open, via a deliberate `--write-baseline`.
