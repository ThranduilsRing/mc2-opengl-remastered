<!-- doc-status: current | reviewed:2026-10-11 -->
# clang-tidy Pareto set (measured 2026-10-09)

Config: `/.clang-tidy` (22 checks). clang-tidy 23.1.3, `-p .` against root `compile_commands.json`
(from `scripts/configure_analysis.py`). Report-only; nothing was fixed.

## Sample (12 TUs, 4 parallel)
GameOS/gameos: gos_static_prop_batcher, vulkan_shadow_island_live, gos_font. RenderCore: KtxLoader,
PipelineRegistry. GuiRuntime/GuiRuntime. code: mission, warrior. mclib: txmmgr, move, terrain, tgl.
Counts are UNIQUE file:line:col sites (headers deduped across TUs; raw total in parens).
All hits were in owned dirs (HeaderFilterRegex works). Only compile error: batcher.cpp:10043
`gos_GetTerrainLightDir` language-linkage mismatch (already known).

| check | unique (raw) | .cpp-only | verdict |
|---|---|---|---|
| modernize-redundant-void-arg | 2221 (7276) | 133 | DROP (pure churn, 2k sites, zero value; removed from config) |
| modernize-use-override | 1252 (4142) | 0 (all headers) | AUTO-FIX SAFE (header batch; mass rebuild) |
| modernize-use-nullptr | 884 (2266) | 473 | AUTO-FIX SAFE (mclib 507, code 298; check `NULL` passed to varargs/ABL macros builds) |
| misc-include-cleaner | 754 (754) | 754 | DROP from default (flags `uint32_t`, `sqrt` as "not directly included"; 151 in one VK file). Ad hoc only |
| modernize-use-using | 588 (2331) | 2 | AUTO-FIX SAFE (headers; typedefs of anonymous structs are skipped by the tool) |
| cppcoreguidelines-pro-type-member-init | 298 (1015) | 9 | DROP from default (POD/vec.h math types; adding init costs perf in hot code). Ad hoc review |
| bugprone-macro-parentheses | 108 (398) | 0 | REVIEW (e.g. ablgen.h:40, packet.h:46 are real; many are type/token macros where parens are wrong) |
| modernize-use-equals-default | 90 (356) | 0 | REVIEW (vec.h:24/338 change triviality/aggregate-ness of hot math types) |
| readability-redundant-declaration | 82 (152) | 27 | REVIEW (gameos.hpp:146 operator new/delete, gos_mech_killswitch.h:138; 9 in batcher.cpp) |
| modernize-loop-convert | 82 (132) | 56 | REVIEW (IblShRegistry.h:79/92 fine; index loops often use `i` elsewhere) |
| modernize-use-equals-delete | 6 (23) | 0 | AUTO-FIX SAFE (noncopyable.hpp:18, elist.h:80, gosvertexpool.hpp:162) |
| modernize-use-emplace | 2 | 2 | REVIEW (GuiRuntime.cpp:1285/1292) |
| readability-redundant-control-flow | 2 | 2 | AUTO-FIX SAFE (terrain.cpp:4561/4617) |
| readability-redundant-string-init | 1 | 0 | AUTO-FIX SAFE (shader_builder.h:99) |
| bugprone-integer-division | 1 | 1 | REVIEW (mclib/terrain.cpp:365, `sqrt(float(tmp/sizeof(..)))` truncation, maybe a real bug) |
| performance-unnecessary-copy-initialization | 1 | 1 | AUTO-FIX SAFE (tgl.cpp:3477 `TG_TypeTriangle` copy -> `const&`) |
| use-after-move, suspicious-string-compare, sizeof-expression/container, container-size-empty, redundant-smartptr-get/string-cstr, for-range-copy, move-const-arg | 0 | 0 | KEEP (zero noise guards) |

Note: zero hits in the 12-TU sample is not proof of zero repo-wide; re-measure in the batch phase.

## Conflicts with repo rules (docs/cxx17-coding-rules.md)
- Sec 3/7: "no standalone modernize slices" and "EXPLICITLY NOT recommended: modernizing mclib/, GameOS/gameos/, gui/". The one-time mechanical batch below directly contradicts that; it needs an explicit owner override (2026-10-08 "best modern practices" feedback supports it) and the doc updated. Per-touched-TU use is consistent with the rules.
- Sec 6.2: Auto_Ptr / custom containers: never accept `modernize-use-auto`/`make-unique`/`pass-by-value` style fixes (not enabled). Hot render path: skip `equals-default` / `loop-convert` there unless already editing.
- Headers (gameos.hpp, mclib/*.h) fan out to hundreds of TUs; each header batch = full rebuild.

## Rollout
1. Now: per-touched-TU. Before committing a change, run the report on the touched files; fix hits in lines you edited only.
2. One-time batch for AUTO-FIX SAFE set (use-nullptr, use-override, use-using, use-equals-delete, redundant-control-flow, redundant-string-init, unnecessary-copy-initialization), by directory, order: RenderCore, GuiRuntime, GameOS/include+gameos, mclib, code. Each batch = one commit + full build + `check-contracts` + ab.py smoke. Apply with `clang-apply-replacements` (export-fixes) rather than `-fix` so shared headers are edited once. Manually diff-review; do not mix other work.
3. Later: REVIEW checks only per-touched-TU. Retire (delete) the batch tooling note once finished.

## Commands (Windows, Git Bash or PowerShell)
Report one file:
`"C:/Program Files/LLVM/bin/clang-tidy.exe" -p . --quiet GameOS/gameos/gos_static_prop_batcher.cpp`
Several, 4 procs (run-clang-tidy is a python script; call via python):
`python "C:/Program Files/LLVM/bin/run-clang-tidy" -p . -j 4 -quiet "GameOS/gameos/.*batcher\.cpp" "mclib/txmmgr\.cpp"`
Batch fixes to a yaml dir for review/apply:
`python "C:/Program Files/LLVM/bin/run-clang-tidy" -p . -j 4 -checks=-*,modernize-use-nullptr -export-fixes fixes/ "mclib/.*\.cpp"` then `clang-apply-replacements fixes/`
Ad hoc dropped check: add `-checks=-*,misc-include-cleaner`.
Files under 3rdparty are excluded by regex; don't pass them.

## C++17 easy wins spotted (suggestions only)
- Constants: `#define MC_MAXTEXTURES 4096`, `MAX_MC2_GOS_TEXTURES 3000` (mclib/txmmgr.h:61-63) -> `inline constexpr`.
- string_view: `const char*` name params in txmmgr.h:530-540 (`textureInstanceExists/loadTexture/saveTexture`).
- Range-for / structured bindings: IblShRegistry.h:79,92; `.first/.second` use in GuiRuntime.cpp / gos_font.cpp.
- const-ref: tgl.cpp:3477 (copy of `firstTriType`).
- if-init: vulkan_shadow_island_live.cpp:162/352/660 patterns where a computed value is used only in the condition.
- [[nodiscard]]: bool-returning make_buffer-style helpers (vulkan_shadow_island_live.cpp:352) and KtxLoader load functions.
- optional: lookup helpers returning sentinels in non-hot code (e.g. txmmgr `textureInstanceExists` returns DWORD sentinel) -- cold path only.
- enum class: new enums only (`MC2_ISTERRAIN`-style #define flags in txmmgr.h:72 are the candidates).
- Real bug candidate: terrain.cpp:568 integer division before float cast.
