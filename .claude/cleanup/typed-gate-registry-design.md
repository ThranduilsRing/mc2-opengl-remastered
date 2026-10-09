# Typed gate registry design v2 (S2) — 2026-10-09, amendments from Opus review folded; status: ready for G0

Supersedes v1 (`9b8e02f81`). Input: `.claude/cleanup/typed-gate-registry-review.md` (no-go as written; go if G1 is split). All 12 review amendments are accepted; one is narrowed with code evidence (see "Rejected amendments"). Re-measured at HEAD `3eeafcdf0`. File:line references are pre-edit; the review's line numbers were re-checked against source.

## 0. Why v2 differs from v1

v1 had three flaws that would have changed runtime behavior quietly: one `rule` per gate cannot keep behavior when readers of the same gate disagree (at least 10 gates); a numeric `lo/hi` clamp keeps out-of-range values that today's readers reject; and the standalone C++17 harnesses stop compiling once `get<"...">()` appears. v2 adds a reader-unification slice (G0), splits infrastructure from the pilot (G1a/G1b), and makes the compiler, not a Python regex, the parser of the table.

## 1. Measured baseline (corrected)

| Fact | Value | Note |
|---|---|---|
| `getenv("MC2_` lines in the 9 ratchet dirs (comments blanked) | **1388** | reproduced with the ratchet's `strip()`; editor 31 and gui 13 are outside the default dirs; Viewer 0; 1446 counting any `getenv(` line |
| Distinct names | **1176** | every `"MC2_*"` literal passed to any call, outside `RendererFeatureRegistry.h` (re-derived). The getenv-only sweep gives 997; 157 names are read only through helpers (`subGate`, `envFlag`, `read_env_flag`, `envIsSet`, `featureActive`). v1's 1010 was too low. |
| Presence-only sites | **373** lines with `!= nullptr` alone; 492 counting `if (getenv(..))`, ternaries and `== nullptr` | v1 said 330 |
| Pre-main initializers | **at least 146 at namespace scope (115 names)**, plus lambda-initialised statics | v1's 59 counted only column-0 `static`; it missed `const bool kX = []{..}()` inside namespaces |
| In-process env writers | `gameosmain.cpp:1141`, `gos_dev_shell.cpp:296`, `bdactor.cpp:413-435`, `editor/EditorMFC.cpp:177,188`, **`editor/ModPicker.cpp:71`** (`MC2_ACTIVE_MOD`, every mod switch), **`tools/brain_dispatch_harness/harness_main.cpp:608-640`** (13 `_putenv`), **`render_state_contract_harness.cpp:120`**, **`job_system_harness.cpp:234`** | v1 listed 4 |
| Registered / allowlisted | 363 registered; `ALLOWLIST` about 854 lines | unchanged |
| Largest files | `gos_postprocess.cpp` 103 (102 literal), `gos_static_prop_batcher.cpp` 94, `gameos_graphics.cpp` 77, `gos_terrain_lod_chunk.cpp` 62, `gameosmain.cpp` 55, `bdactor.cpp` 49 | 16 files hold 751 sites |
| Pilot paths | `mclib/terrain_admission_mode.h`, `mclib/terrain_surface_trace.h` (not GameOS); `gos_terrain_patch_stream.cpp` has **12** sites, not 13 | |

## 2. Helper truthiness (corrected)

Input classes: U unset, E `""`, Z `"0"`, F `false`/`off`/`no`, O `"1"`, X other. Rules named for the override enum in section 5.3.

| Rule name | Idiom and where | U | E | Z | F | O | X |
|---|---|---|---|---|---|---|---|
| **Std** | ParseEnvBool family: `terrobj.cpp:83`, `bdactor.cpp:2852`, `gos_static_prop_registry.cpp:122`, `sim_identity.cpp:71`, `DefaultHostServices.cpp:98`, `gui/UiDefs.cpp:97`, `suParseBool` (`gameosmain.cpp`) | def | def | off | off | on | on |
| Lead0 | `v && v[0] && v[0]!='0'`: `envFlag` x3 adapters, `envSet`, `envFlagOn`, `tglc_envOn`, `envGateOn`, `debugEnvEnabled`; **about 333 inline sites use the inverse `!(v && v[0]=='0')`** (`Lead0Kill`: unset on, empty on) | off | off | off | **on** | on | on |
| DefaultOff-helper | `envFlagDefaultOff` x4 (`debug_renderer.cpp:32`, `gos_cluster_depth_pyramid.cpp:30`, `gos_lightgrid_build.cpp:66`, `gos_postprocess_blur.cpp:40`) | off | **on** | off | **on** | on | on |
| DefaultOn-helper | `gos_mech_batcher.cpp:128`, `gvactor.cpp:111`, `txmmgr.cpp:158`; `RenderWorld.cpp:70` differs (E gives **off**) | on | on/off | off | **on** | on | on |
| **Lead1** | first char only: `v[0]=='1'` at **`vulkan_mech_island.cpp:126` and `gameos_graphics.cpp:135`** (v1 wrongly called these exact-"1"; `"10"` reads ON); about 152 inline sites | off | off | off | off | on | on if starts with `1` |
| Exact1 | `strcmp(v,"1")==0`: `gameplay_pick.cpp:172`, `txmmgr.cpp:151` | off | off | off | off | on | off |
| NonEmpty | `v && *v` (about 46 sites). Differs from Std on Z, so SEMANTICS | off | off | **on** | **on** | on | on |
| Presence | `!= nullptr` (373 sites) | off | **on** | **on** | **on** | on | on |
| Atoi / Atof | `atoi(v) != 0` (about 19 gate sites); `atof` about 144 sites; both accept prefixes: `"12abc"` gives 12, `"2.0"` gives 2 through `atoi`, `"abc"` gives 0 | | | | | | |
| Tri | `envTriState` `vk_shared_gbuffer_gate.h:73`: exact `"1"`/`"0"` else inherit | inherit | inherit | off | inherit | on | inherit |

`HostServices/DefaultHostServices.cpp:98` already implements Std and has a unit test (`tests/unit/test_host_services.cpp`); it becomes `gate::parseBool`. `RendererFeatureRegistry.h:4123 debugDumpFeatureRegistry` is dead and carries another rule.

## 3. Current machinery and runtime facts

Registry parsers and checkers are as in v1 (`registry_parse.py` 598 lines, `flag-registry-auto-gen.py` 398, `check-env-registry.sh` 1174 with an `ALLOWLIST` of about 854 names, `check-gate-default-agreement.py` 1559, `check-new-gates.py` 166, `check-registered-gate-passthrough.py` 272, plus about 14 more consumers). Ratchet: `IDIOMS` at `cxx17_ratchet.py:49-68`, `strip()` returns `(nc, bl)`, a missing idiom in `baseline.json` reads as 0, and `check()` lines 463-468 only **warn** when a directory rises while the total does not.

Runtime facts that shape the design:
- **Caching shapes.** Namespace-scope initializers and lambda statics (read once, often pre-main), function-local statics, `static int state = -1` caches, and per-call reads (`SimpleCamera::render`, uncached `subGate`).
- **Env mutation after caching.** `ModPicker::Activate` calls `_putenv_s("MC2_ACTIVE_MOD")` and then `InitModSearchPaths`, which re-reads it at `mclib/file.cpp:727`; other readers are `anim_override_registry.cpp:161`, `fx_def_registry.cpp:355`, `model_override_registry.cpp:203`. A cached slot without `invalidate` would mount the wrong mod. `brain_special_dispatch.cpp:226,234` is read fresh per call under harness `_putenv`.
- **Editor.** `editor/CMakeLists.txt:71` builds with `DISABLE_GAMEOS_MAIN`, so no `gameosmain` hook runs in EditRel. `EditorMFC.cpp:188` sets `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` in `InitInstance`; its reader (`gos_static_prop_batcher.cpp:246/247`) is a namespace-scope lambda static that has probably already run, so the override is likely ineffective today (verify in G0). Replacing that static with a call-time `get<>()` would start the override working, which is a behavior change.
- **MC2_LOG** is a presence-and-spec gate read by the facade (`mc2log.cpp:171`); the gate module must stay below `mc2log`.
- **Env writers** are listed in section 1; all of them must be hooked (section 5.5).
## 4. Known cross-reader conflicts (input to G0)

A single table row cannot keep these gates byte-identical, because their readers disagree with each other. Each needs one named, A/B'd commit that unifies the readers (or records an explicit divergence) before the gate migrates. Lines verified at HEAD.

| # | Gate | Readers and what they do | Why one rule fails |
|---|---|---|---|
| 1 | `MC2_SHADOW_ENABLE` | `gos_mech_batcher.cpp:1167`, `gos_static_prop_batcher.cpp:9290`: Lead0Kill, unset is ON. `vulkan_post_screenfx_native.cpp:759`: `envTriState==1`, unset is OFF | A `Bool, default=true` row flips the VK direct-CSM path when unset (cross-backend parity defect already named in the registry text) |
| 2 | `MC2_DISABLE_GOSFX` | `mclib/mlr/mlr_gate.cpp:37`: disables only on a leading `'1'`; `gameosmain.cpp:1340`: exactly `"0"`; `GraphicsOptionsWindow.cpp:1477`: presence | Presence row makes the documented dev override `=0` disable FX in `mlr_gate`; Std makes `true`/`yes` flip there |
| 3 | `MC2_TERRAIN_LIGHTING_V1` | `gameos_graphics.cpp:7175`: `=0` gives strength 0; `gos_terrain_lod_chunk.cpp:2872`: presence, so `=0` applies strength; `GraphicsOptionsWindow.cpp:613` third reader | whichever rule is chosen, one site changes |
| 4 | `MC2_TERRAIN_LIGHTING_V2` | same shape: `gameos_graphics.cpp:7188`, `gos_terrain_lod_chunk.cpp:2873`, `GraphicsOptionsWindow.cpp:643` | same |
| 5 | `MC2_VK_TERRAIN_CSM` | `gos_terrain_lod_chunk.cpp:1858`: Lead1, no umbrella inheritance; `:1900`: `subGate`, inherits the umbrella | one Tri helper would arm the CSM subset under the umbrella |
| 6 | `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR` | `gos_terrain_height_tex.cpp:51`: accepts only 2 or 4, everything else gives 1 | a `lo=1,hi=8` clamp passes 3 and 8; `"2.0"` changes under whole-string `from_chars` |
| 7 | `MC2_LIGHT_GRID_DIM` and `MC2_LIGHT_POOL_MAX` | `gos_terrain_lighting.cpp:276`, `:859`: out-of-range falls back to the default | a clamp turns 300 into 256 |
| 8 | `MC2_SNAPSHOT_STATIC_PROP_BUILD` | editor override ordering (section 3) | call-time reads change timing |
| 9 | `MC2_ACTIVE_MOD` | four readers re-read after `_putenv_s` | needs `invalidate`, not a one-shot slot |

Also in G0: a tool pass (`gate_gen --conflicts`, reusing the idiom classifier inside `check-gate-default-agreement.py`) lists every gate with two or more readers whose idiom classes differ, because the table above is the review's sample, not a proven complete list.
## 5. Design

### 5.1 Layout and build

The hand-edited **constexpr** C++ table is the single source of truth. The compiler, not a regex, is its parser (section 5.6).

```
Gates/gate.h            hot API only: get/isSet/state; includes <atomic> <cstdint> <string_view> + gate_index.gen.h
Gates/gate_cold.h       cold API: str(), dumpConfig(FILE*), lookupName, invalidate/invalidateAll, markMainEntered, parseBool (<string> <cstdio> <optional>)
Gates/gate.cpp          parse, resolve, dump, _environ scan
Gates/gate_table.cpp    inline constexpr Row kTable[]; static_asserts on name, kind, count, sort order, uniqueness vs the index
Gates/gate_index.gen.h  GENERATED sorted {name, kind} (about 1176 lines)
Gates/gate_table.json   GENERATED from the compiled table by gate_dump (committed)
tools/gate_dump/        tiny exe: gate.cpp + gate_table.cpp, built with -DGATE_INDEX_BOOTSTRAP, emits the JSON
tools/gate_gen.py       JSON -> index / yaml / registry shim / passthrough list; --check, --selftest, --conflicts, --due
tests/unit/test_gate.cpp  doctest parse matrix
```

- **STATIC lib `mc2_gate`.** An OBJECT library would duplicate symbols when linked into several static libs that end in one exe. Standalone projects add `Gates/gate.cpp` and `gate_table.cpp` as sources; a miss fails at link (LNK2019), which is acceptable. Add `Gates` to include-firewall `SCOPE_DIRS` and to the ratchet `DEFAULT_DIRS`.
- **Standard floor (G1a precondition).** Class-type NTTP and `consteval` need C++20. The in-tree projects inherit `MC2_CXX_STANDARD` (default 23; `target_compile_features(... cxx_std_17)` is only a floor), so they are fine. The projects that own a `project()` and pin 17 must move to at least 20 (23 to match): `tools/tg_import_dump`, `tools/tgl_loader_standalone_spike`, `tools/rendercore_standalone_spike`, `tools/job_system_harness`, `tools/mech_import_harness`, `tools/render_state_contract_harness`, `tools/brain_dispatch_harness` (it `#include`s `code/brain_special_dispatch.cpp`), `tools/brain_missionfit_oporbd_test`, `tools/brain_symbol_resolve_test`, `tools/glstate_guard_harness`; plus `tests/model_override` (`cxx_std_14`) and the non-`mc2_tests` targets in `tests/unit` (directory default 17 at line 25; only `mc2_tests` is 20 at line 135). Drop `17` from the `MC2_CXX_STANDARD` options. A new `scripts/check-cxx-standard-floor.py` row fails when any CMake project listing a source that contains raw `MC2_` getenv or `Gates/` is below 20, so the list cannot rot.
- **Probe before commit (G1a).** `/d1reportTime` on a `gos_postprocess.cpp`-sized TU (102 sites) plus PDB and exe size, covering the risky shapes: lambda-static initializer (the commonest site), namespace-scope initializer, use inside a template, use inside a macro. Budget: at most +5% frontend time on that TU and +0.5% PDB. `mc2::log::Lit` (landed the same day, about 14 uses) is not evidence at 1400 instantiations. Fallback if the budget fails: the codemod emits pre-resolved indices (`gate::get<Idx>`) instead of names.

### 5.2 Row

```cpp
//  name                      kind  default owner     retireBy     doc (one literal, one line)
G("MC2_TERRAIN_LOD_CHUNK",    Bool, true,   "terrain","",          "chunked-LOD terrain path; =0 restores legacy")
G("MC2_LIGHT_GRID_DIM",       Int,  64,     "terrain","",          "light-grid cells per side", range(16,256,Reject))
G("MC2_TONEMAP_V2",           Enum, "1",    "post",   "",          "tonemap curve", tokens("1","2","3","agx","hable"))
L("MC2_THIN_DEBUG",           Bool, false,  "terrain","2026-12-31","legacy Presence rule; convert to Std")  // L = row rule Presence
// history: docs/gate-history.md#MC2_SHADOW_ENABLE       <- comment line above any row with long history
```

Kinds: `Bool`, `Tri`, `Int`, `Float`, `Enum`, `String`. Other fields: `rule` (Std, or a legacy rule from section 2), `reread`, `divergent`, `smokePassthrough`, `logTopic`, `retireBy` (ISO date). The history comment is harmless because the compiler is the parser.

### 5.3 Reader API

```cpp
namespace mc2::gate {                       // gate.h (hot)
template <size_t N> struct Name { char v[N]; consteval Name(const char (&s)[N]); };
enum class Tri : uint8_t { Unset, Off, On };            // no bool conversion: if (tri) does not compile
template <Name n> [[nodiscard]] auto get() noexcept;    // bool | int32_t | float | Tri | Token(Enum), by table kind
template <Name n, Rule r> [[nodiscard]] bool get() noexcept;   // per-site rule override (below)
template <Name n> [[nodiscard]] Tri  state() noexcept;  // raw exact "1"/"0" state for any Bool/Tri row (divergent readers)
template <Name n> [[nodiscard]] bool isSet() noexcept;  // presence, any kind (MC2_LOG, MC2_NO_LAUNCHER)
template <Name n, Name tok> [[nodiscard]] bool is() noexcept;  // Enum token test, consteval token index
}
// gate_cold.h: str<n>() (owning std::string, String kind), lookupName(sv), parseBool(const char*, bool),
//              invalidate(sv), invalidateAll(), markMainEntered(), dumpConfig(std::FILE*)
```

- **Hot path:** one relaxed atomic load of `g_slot[idx]`, a test and a branch; the resolve is out of line. No function-local static, so no MSVC init epoch check and no init-order dependence. Slot bits: 63 resolved, 62 resolved-before-`markMainEntered`, low 32 value.
- **Per-site rule override.** `get<"X", Rule::Presence|NonEmpty|Exact1|Lead1|Lead0Kill>()` keeps a site's old rule when it differs from the row. It is the escape hatch only for readers G0 did not unify; each override is counted, carries a `retireBy`, and the count must not rise (`gate_gen --check`).
- **Tri is an enum class.** `optional<bool>` would make `if (get<...>())` silently mean presence. The VK umbrella helpers (`subGate`, `directChainGate`, `umbrellaOn`, `terrainNativeFirstOwnerOn`) stay in `vk_shared_gbuffer_gate.h` and are rewritten over `Tri`; the per-call `g_mc2VkSessionForcedGL` read is unchanged.
- **NTTP names, not an `Id` enum.** The canonical identity is the `MC2_*` string (CLAUDE.md, memory, MCP tools, harness scripts all key on it); the codemod is mechanical; a typo becomes a compile error. Cost is roughly equal either way (both need the same index header, binary search is about 10 compares). The real costs are mangled `char[N]` symbols and a rebuild of about 202 TUs whenever a gate is added, accepted and probed in G1a.

### 5.4 Truthiness rules

- **Bool (Std):** U/E give the default; `0/false/off/no` (case-insensitive) false; `1/true/on/yes` true; other non-empty true plus `badvalue`.
- **Int/Float:** whole-string `from_chars`; E gives the default. Parse failure, trailing junk and out-of-range all give the default plus `badvalue`, and all count as EDGE in the codemod verdict. Range policy defaults to **Reject (fall back to default)**; `Clamp` is allowed only where the old code clamped (e.g. `clampVfxScale`). A row may declare `Parse::Prefix` (atoi/atof-compatible) when byte identity needs `"2.0"` to read as 2; it needs a `retireBy`.
- **Enum:** closed token list, case-insensitive; unknown gives default plus `badvalue`. Used for `MC2_TONEMAP_V2`, `MC2_WATER_SHORE_MODE`, `MC2_UNIT_DEFINITION_SOURCE`, `MC2_CONFIG_DUMP`, and `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR` as `Enum{1,2,4}` (G3).
- **MC2_CONFIG_STRICT=1** (set by `run_smoke.py`) makes `badvalue` and `unregistered` fatal.

### 5.5 [CONFIG v1] dump, ordering and hooks

Output is one `fwrite` per line to the stream the caller passes, not through `mc2log` (which depends on `gate`):
```
[CONFIG v1] name=MC2_TERRAIN_LOD_CHUNK kind=bool value=0 src=env raw="0" default=1 rule=std owner=terrain
[CONFIG v1] warn=unregistered name=MC2_TERAIN_X
[CONFIG v1] warn=premain name=MC2_FOO premain_value=0 env_now=1
[CONFIG v1] summary total=1176 set=7 nondefault=3 unregistered=0 badvalue=0 premain_stale=0
```
Default: env-set gates plus the summary; `MC2_CONFIG_DUMP=all` prints every row (about 1176 unbuffered writes while stderr is `_IONBF`, roughly 1 to 5 ms). The `_environ` scan is about 100 entries.

Hooks, at every entry point (all three calls, in this order: `markMainEntered`, `invalidateAll`, `dumpConfig`):
- `gameosmain.cpp`: `markMainEntered()` at the top of `main`; `invalidateAll()` right after the launcher_env apply loop (`:1141-1150`); `dumpConfig(stderr)` right after `mc2::log::init()` (`:1180`).
- Editor: `EditorMFCApp::InitInstance` (`EditorMFC.cpp:462`) right after `editor_set_default_env_vars()` (`:464`); none of the `gameosmain` hooks run in EditRel.
- `ModPicker::Activate` (`ModPicker.cpp:71`) calls `gate::invalidate("MC2_ACTIVE_MOD")` after its `_putenv_s`; the `MC2_ACTIVE_MOD` row is `reread`.
- Harness mains that `_putenv` after static init (brain_dispatch, render_state_contract, job_system) call `invalidateAll()`. The `reread` flag goes on gates read fresh per call under `_putenv` (`brain_special_dispatch.cpp:226,234`).
- `invalidateAll()` first **snapshots** every pre-main-resolved slot (value plus raw env), then clears; the `premain` warning compares the snapshot against a fresh parse. The pre-main bit is only meaningful once some entry point has called `markMainEntered`. The warning only sees migrated slots; gates still read raw are invisible to it.
- The dev shell's `cmdSetGate` (`gos_dev_shell.cpp:289`) calls `invalidate`, so its reply can say `applied` truthfully for typed gates.

### 5.6 Generation: the compiler is the parser

1. `tools/gate_dump` (built by the same pattern as `build_contract_harnesses.py`, a few seconds) compiles `gate.cpp` + `gate_table.cpp` with `-DGATE_INDEX_BOOTSTRAP` (skips the index static_assert, so adding a row can bootstrap) and emits `Gates/gate_table.json`: name, kind, default, rule, owner, retireBy, doc, range, tokens, reread, divergent.
2. `gate_gen.py --write` derives from that JSON only: `gate_index.gen.h`, `docs/flags.auto.yaml` v3 (pure function of the table; drops `referenced_at`, `in_smoke_passthrough`, `documented_in_tier1_env_vars`), the `RendererFeatureRegistry.h` rows (`kFeatureTable`, `kAuxEnvVars`; the hand-ordered `RendererFeature` enum stays), and from G6 the `run_smoke.py` passthrough list.
3. `gate_gen.py --check`: stage A regenerates from the committed JSON and diffs (hermetic); stage B rebuilds `gate_dump` and diffs the JSON against the committed one. Name, kind and count drift also fails the normal build through the `static_assert` against the index; default, doc and owner drift is caught by stage B. There is no Python regex parser of the table.
4. Seeding (`--import-legacy`, once): from all 1176 call-passed `"MC2_*"` literals, including names only harnesses set (otherwise `unregistered=0` would be flaky), with defaults from the 363 registry `defaultOn` values, idiom classes from `check-gate-default-agreement.py`'s classifier, ambiguous rows `Bool` with `owner="unclassified"`.
5. Essay docs move verbatim to `docs/gate-history.md`; the row keeps a one-line `doc` and a `// history:` pointer, so the warnings stay discoverable.

### 5.7 Migration mechanics

`tools/gate_codemod.py` (one-shot, removed in G8) emits a TSV per file: `file:line, name, old idiom, row rule, verdict`; the reviewer reads the TSV.

| Verdict | When | Action |
|---|---|---|
| NO-CHANGE | old idiom equals the row's rule on all input classes | rewrite site |
| EDGE | differs only on E, F, X, or numeric parse failure, trailing junk, out-of-range | rewrite; file listed in the PR |
| SEMANTICS | differs on U or Z (presence, NonEmpty) | **never migrated to Std in the same commit**: the row keeps the legacy rule or the site uses the per-site override; conversion is a separate named commit |

- **The codemod preserves each site's caching shape.** A `static const bool s = <expr>` stays a static holding `gate::get<>()`; a lambda static stays; per-call stays per-call. Removing a cache is a separate commit, because changing when a value is read changes behavior (editor override ordering, launcher_env ordering).
- A site whose gate is on the G0 conflict list is not migrated until G0 resolved it (unified, or `divergent` with `state<>()` at the divergent site).
- Helper replacements follow v1's mapping (ParseEnvBool family NO-CHANGE; `envFlag`/DefaultOff/DefaultOn/`envInt`/`envFloat` EDGE; `envTriState` becomes `state<>()`; `HostServices::EnvConfig::flag` calls `parseBool`). Harness sweep: 429 `MC2_X=value` pairs found by the review; all 39 `=0` target readers that already treat `"0"` as off; no literal true/false/on/off/yes/no values. The sweep scope widens to `tests/smoke/matrices/*.json`, `tests/visual/**/capture_run.json`, bookmarks, `.ps1`, installed `launcher_env.json`, the launcher's `offVal`s, and in-process `_putenv`.
- **Logging P2** rides the same per-file diffs, except `gos_terrain_patch_stream.cpp`, whose 15 `MC2_EVENT` sites already landed in `f980105df`. `MC2_LOG`, `MC2_LOG_DECORATE`, `MC2_LOG_EVERY_N_FRAMES` become String/presence rows read through `gate::str` / `isSet`; `mc2log.cpp:envView` is deleted. Migrate only surviving gates: the DELETE/MERGE verdicts in `tierB-trace-gates.md` and the `tierA-gates-plan.md` deletions land first.
## 6. LG19 ratchet

- Add `LG19` to `IDIOMS` (`cxx17_ratchet.py:49-68`) and a detector in `scan_text()` on the **`nc` line** (strings kept), as LG11 does:
  `(?<![\w.>])(?:std::|::)?(?:getenv|_wgetenv)\s*\(\s*(?:"MC2_|[A-Za-z_])` | `GetEnvironmentVariable[AW]?\s*\(\s*"MC2_` | `\b_dupenv_s\b|\bgetenv_s\b` | `\b_environ\b|GetEnvironmentStrings[AW]?|SDL_getenv`. `getenv("PATH")` stays uncounted. `Gates/` is exempt by a path rule (precedent: `ALLOC_FILE`). `gos_visual_capture.cpp:254` (`_environ` scan) is the real current miss for a naive regex; `EditorPlaytest.cpp:163,915` use literal names and are matched.
- **Strict per directory.** Introduce `PER_DIR_STRICT = {"LG19"}`: a per-directory rise fails even when the total did not rise (today `check()` only warns, lines 463-468, so a new raw read offset by one migration elsewhere would pass).
- Policy: DRIVE-TO-ZERO with "a new file has 0". The `#pragma deprecated(getenv)` idea is dropped (it also fires on `PATH`). Keep the diff-based `new_gates` row until G8; note it diffs only against `--base HEAD~1`.
- The adding commit runs `--write-baseline` (a missing key reads as 0 and would fail). `editor/`, `gui/`, `Viewer/` (44 sites) stay opt-in until G8; track with `--report --dirs editor gui Viewer`.
- `test_cxx17_ratchet.py` cases: `std::getenv("MC2_X")`, `::getenv(name)`, `getenv("PATH")` not counted, comment and string-embedded text not counted, two calls on one line count once, `_environ`, per-dir strict.
- **Baseline: 1388** (authoritative: `py -3 tools/cxx17_ratchet/cxx17_ratchet.py --report` after the change). Pre-change equivalent: `rg -n --pcre2 '(?<![\w.>])(?:std::|::)?getenv\s*\(\s*"MC2_' -g '*.{cpp,cc,h,hpp,inl}' code mclib GameOS RenderCore GuiRuntime RenderWorld GameAdapters HostServices EditorBridge | wc -l` (slightly higher; keeps comments).

## 7. Ordered slices

Each slice is at most one build + smoke cycle. Roadmap S2 = G0 to G4. G2 to G8 keep their v1 numbers and content; v1's G1 is replaced by G0, G1a and G1b.

| Slice | Content | Exit criteria |
|---|---|---|
| **G0 reader unification** (no `Gates/` code) | One named commit per conflict gate in section 4 (SHADOW_ENABLE, DISABLE_GOSFX, TERRAIN_LIGHTING_V1/V2, VK_TERRAIN_CSM, LIGHT_GRID_DIM, LIGHT_POOL_MAX, HEIGHT_RESAMPLE_FACTOR, SNAPSHOT_STATIC_PROP_BUILD, ACTIVE_MOD) plus whatever `gate_gen --conflicts` adds. Each commit makes all readers share one rule, or records the gate as `divergent` (an owner decision, e.g. VK `MC2_SHADOW_ENABLE` default flip needs a VK capture). Verify the editor override ordering for `SNAPSHOT_STATIC_PROP_BUILD` in the real editor. Output: `docs/gate-reader-conflicts.md` (gate, readers, chosen rule, behavior delta, evidence). | Every gate in section 4 is unified or `divergent` with a note; GL+VK ab.py day+night for render-visible gates; tier1 5/5; no gate on the list has two reader idioms left unless `divergent`. |
| **G1a infrastructure, zero migrated sites** | `Gates/` module (STATIC lib, hot/cold headers), `gate_dump`, `gate_gen` seeded from 1176 names, `test_gate` covering the full section 2 matrix plus every idiom class, standard-floor bump and its check row, hooks in `gameosmain`, editor `InitInstance`, `ModPicker`, harness mains, LG19 plus baseline, `env_index.py`/`dead_gate_scan.py` taught to grep `get<"MC2_X">`, `MC2_CONFIG_STRICT` in `run_smoke.py`, NTTP probe (`/d1reportTime`, four shapes). | Smoke byte-identical; `[CONFIG v1]` lines present in the game log; editor launches and prints its `[CONFIG v1]`; all standalone harnesses still build; `gate_gen --check` and `test_gate` green; LG19 baseline 1388 with no count change; probe within budget or fallback chosen. |
| **G1b pilot (terrain)** | Preconditions: amendments 1, 2 and 9 implemented (per-site override, range=Reject plus Enum, caching-shape-preserving codemod) and G0 done. Files (about 39 sites): `gos_terrain_patch_stream.cpp` (12), `mclib/terrain_admission_mode.h:12`, `mclib/terrain_surface_trace.h:42`, `mclib/terrain_runtime.cpp` (3), `gos_terrain_surface.cpp:73`, `gos_terrain_lighting.cpp` (about 20, numeric rows `range=Reject`). **`gos_terrain_height_tex.cpp` is dropped from the pilot** (moves to G3 as `Enum{1,2,4}`). Logging P2 only for `gos_terrain_lighting.cpp`. | LG19 at most 1349; the TSV shows zero SEMANTICS rows migrated to Std; a one-mission A/B shows identical golden bytes; smoke-lite plus tier1 5/5; `gos_terrain_patch_stream.obj` at most 1.1x pre-pilot; one `get<>` site compiles to load, test, branch. |
| **G2 single truth** | Registry rows and `flags.auto.yaml` v3 generated from `gate_table.json`; `env_index.py`, `gate_status.py`, `build_review_pack.py` read the yaml/JSON; delete `registry_parse.py`, `flag-registry-auto-gen.py`, `check-flag-registry-truth.py`. `gate_status`/`env_index` keep reading the `run_smoke.py` passthrough directly until G6. | Rows removed: `:787`, `:807`, `:808`; `:791` replaced by a `gate_gen` selftest; `:812` retargeted to `gate_gen --check`. |
| **G3 terrain family** | `gos_terrain_lod_chunk.cpp` (62), `gos_terrain_indirect.cpp` (41), `mclib/terrain.cpp` (43), `gos_terrain_water_stream.cpp` (15), `vulkan_terrain_island.cpp`, `gos_terrain_mask_dispatch.cpp`, `gos_terrain_height_tex.cpp` as Enum; logging P2 same diffs. | LG19 down at least 120 more; LG12a down at least 150 in these files. |
| **G4 VK helpers + GameOS local helpers** | `envTriState` family becomes `Tri`/`state<>()`, the 10 `vulkan_*_island.cpp` gate functions, `engine_present_vk.cpp`, section 2 local helpers. | LG19 down at least 100 more; GL and VK A/B per `vk-default-flip-checklist.md`. |
| **G5 giant singles** | `gos_postprocess.cpp` (103), `gos_static_prop_batcher.cpp` (94), `gameos_graphics.cpp` (77), `gameosmain.cpp` (55): one file per cycle. | each file at LG19 = 0; obj size within 1.05x. |
| **G6 mclib + passthrough** | `bdactor.cpp` (49), `txmmgr.cpp` (41), `cone_glow.cpp`, `camera.cpp`, `mech3d.cpp`, `assimp_importer.cpp`; `smokePassthrough` generates the `run_smoke.py` list; MCP gate tools repointed. | rows `:859`, `:860` deleted. |
| **G7 code/ + adapters** | `mission.cpp` (36), `warrior.cpp` (23), `objmgr.cpp` (22), `missiongui.cpp`, `brain_special_dispatch.cpp`, `logisticsdata.cpp`, `gamecam.cpp`, adapters, `HostServices`. | LG19 in owned dirs at most about 120. |
| **G8 finish** | `editor/`, `gui/`, `Viewer/`, the tail, launcher consumes generated data. | LG19 = 0 everywhere, flipped to "new file must have 0"; rows `:745`, `:755`, `:1502` deleted; `check-env-registry.sh` (1174), `check-new-gates.py` (166), `check-registered-gate-passthrough.py` (272), `check-env-allowlist.sh` (146) and the `ALLOWLIST` removed. |

Honest roadmap note: the S2 exit criterion "contract rows down at least 5" is met only after G6 (net: G2 removes 3 and adds the standard-floor and `gate_gen` rows; G6 removes 2 more; G8 removes 3 more). Raw getenv down at least 100 is met at G3/G4.

## 8. Contract rows (current line numbers) and what survives

| Row (`scripts/check-contracts.sh`) | Disposition |
|---|---|
| `:745` env_registry, `:755` selftest | deleted at G8; the "every literal is in the table" check moves into `gate_gen --check` (needed for `.py`, launcher and the unmigrated tail until then) |
| `:787` registry_parser_selftest, `:807`/`:808` flag_registry_truth | deleted at G2 |
| `:791` generator selftest, `:812` drift | replaced and retargeted at G2 |
| `:828`/`:829` gate_default_agreement | **kept, trimmed.** Rule 2 (cross-reader and cross-backend conflict, for unmigrated gates and for `Tri`/`state<>()` sites where `value_or` is chosen locally), rule 3 (default claims in C++ comments, GLSL comments and `docs/tier1_env_vars.md`, checked against the table JSON), and `scripts/gate-defaults-allowlist.txt` stay until a replacement exists. Rule 1 (registry vs reader) is deleted when the registry is generated. `tier1_env_vars.md` stays hand-written; the small doc-vs-table check is its guard (owner decision: regenerate it later or keep the check). |
| `:859`/`:860` registered_gate_passthrough | deleted at G6 |
| `:1502` new_gates | kept until G8 (diff-based, `--base HEAD~1`) |
| `:848` smoke_env_inheritance, `:1046`/`:1048` capture_env, `:1064`/`:1066` env_drive_guard | stay (harness behavior) |
| `:1394` review_pack_registry_guard | repointed at JSON |
| `:947` gpu_abi | precedent for the new `gate_gen_check` row |

v1's roughly 4,600-line deletion shrinks: `check-gate-default-agreement.py` (1559) is trimmed, not removed.

## 9. Known pre-existing bug (separate ticket)

On an `MC2_NO_LAUNCHER` run from the dev install, `launcher_env.json` values are applied by `gameosmain.cpp:1141` after static init, so gates read at namespace scope never see them even though `[LAUNCHER_ENV v1]` reports them applied. The install `mc2-win64-v0.4/launcher_env.json` sets `STATIC_PROP_PBR_V1`, `STATIC_PROP_AMBIENT_V1`, `CHEAT_INFINITE_MONEY` and `CHEAT_SALVAGE_ALL`; their readers (`gos_static_prop_batcher.cpp:1171,838`, and `code/logisticsdata.cpp`, `mechlabscreen.cpp`, `mechlistbox.cpp`, `mechpurchasescreen.cpp`, `salvagemecharea.cpp`) run in static init, so they are OFF today. This work does not fix it (the codemod preserves caching shape). It will show up as `premain_stale` of at least 4 once those gates migrate, so `premain_stale=0` will not hold on such a run. Fix options for the ticket: apply the JSON before static init (a launcher-side change), or make those readers call-time.

## 10. Rejected amendments

None rejected. One is **narrowed**, with evidence: amendment 3's list. In-tree projects (`tools/asset_viewer`, `tools/mc2fx`, `tools/mc2fx_preview`, `ui_editor`) have no `project()` and set only `target_compile_features(... cxx_std_17)`, which is a floor, so they inherit `MC2_CXX_STANDARD` (default 23). The projects that actually need the bump are the ten with their own `project()` listed in section 5.1, plus `tests/model_override` and non-`mc2_tests` targets in `tests/unit`. The enforcement row (`check-cxx-standard-floor.py`) makes the list self-maintaining instead of relying on this enumeration. Also, amendment 6 adds a few seconds of compile to `gate_gen --check` stage B; accepted.

## 11. Risks and remaining questions

1. G0 commits change behavior by design (that is the point); each needs an owner decision and an A/B. The VK `MC2_SHADOW_ENABLE` default is the largest.
2. NTTP cost at 1400 instantiations is unproven; the G1a probe decides name-based versus index-based call sites.
3. `gate_dump` bootstrap: a table row added without regenerating the index fails the normal build by design; the workflow is `gate_gen --write`.
4. Pre-main reads stay pre-main; section 9 is the only fix path.
5. Seed kinds for about 650 unregistered names are guessed; a codemod/row conflict stops that file until a human fixes the row.
6. Reviewer should ask: is `divergent` a permanent state or does it carry a `retireBy`? Does `state<>()` on a Bool row weaken the Tri type guarantee? Is the +5% TU budget right for `gos_postprocess.cpp`?

### Critical Files for Implementation
- A:/Games/mc2-main/tools/cxx17_ratchet/cxx17_ratchet.py
- A:/Games/mc2-main/RenderCore/RendererFeatureRegistry.h
- A:/Games/mc2-main/scripts/check-contracts.sh
- A:/Games/mc2-main/GameOS/gameos/gameosmain.cpp
- A:/Games/mc2-main/GameOS/gameos/vk_shared_gbuffer_gate.h
- A:/Games/mc2-main/editor/EditorMFC.cpp