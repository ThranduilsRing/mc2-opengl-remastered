# Typed gate registry — Opus adversarial review (2026-10-09, against design 9b8e02f81)

I worked read-only against `A:/Games/mc2-main`. The design surveyed `48ea7b3d8`. HEAD is now `2f55cdddc`, because a concurrent commit (`f54687963`) added rows to `check-contracts.sh`.

## Verdict up front

The direction is right, but the design is not safe to start as written. Three flaws would quietly change runtime behavior:

1. **One `rule` per gate cannot keep behavior identical.** At least 8 gates already have readers in different files that disagree with each other.
2. **The numeric `lo`/`hi` clamp changes behavior.** Today's readers reject out-of-range values and fall back to the default; a clamp keeps them. The design's own example row and two of the G1 pilot files hit this.
3. **The standalone harnesses are still C++17.** Every one of them that compiles a migrated engine file will stop compiling once `get<"...">()` appears.

---

## (b) Claims checked

| Claim | Result |
|---|---|
| 1388 `getenv("MC2_` lines in the 9 ratchet dirs; 1446 including any `getenv(` | **Correct.** Reproduced exactly with the ratchet's own `strip()` |
| editor 31, gui 13, Viewer 0 | **Correct** |
| 1010 distinct names | **Too low.** The getenv-literal sweep gives 997 in the owned dirs. Counting `fn("MC2_X")` calls gives 1176 names, of which 157 are read only through helpers (`subGate`, `envFlag`, `read_env_flag`, `envIsSet`, `featureActive`, …). A seed built from the getenv sweep misses them. |
| About 330 presence-only sites | **Too low.** 373 lines use `!= nullptr` alone; 492 if you include `if (getenv(..))`, ternaries and `== nullptr`. |
| Helper truthiness table (§1) | **Mostly right, two wrong rows.** `vulkan_mech_island.cpp:126` and `gameos_graphics.cpp:135` test only the first character (`e[0]=='1'`), so `"10"` reads ON; they are not exact-"1". The table also leaves out the dominant inline idioms: `!(v && v[0]=='0')` (about 333 sites), `v[0]=='1'` (about 152), non-empty presence `v && *v` (about 46; differs from Std on `"0"`, so it is SEMANTICS), and `atof` (about 144). |
| 59 + 16 reads before `main` | **Undercounted by at least 2x.** A brace-depth scan finds at least 146 initializers at namespace scope (115 names), plus lambda-initialised statics. The 59 counts only `static` at column 0 and misses `const bool kX = []{..}()` inside namespaces. |
| 4 env writers in the engine | **Incomplete.** Missing: `editor/ModPicker.cpp:71` (`_putenv_s("MC2_ACTIVE_MOD")` on every mod switch), `tools/brain_dispatch_harness/harness_main.cpp:608-640` (13 `_putenv` calls), `render_state_contract_harness.cpp:120`, `job_system_harness.cpp:234`. |
| launcher_env `_putenv_s` at `gameosmain.cpp:1141`, `log::init` at `:1180` | **Correct** |
| `debugDumpFeatureRegistry` is dead, uses a `v[0]=='0'` rule | **Correct** |
| `check-contracts.sh` rows at :745–:860 and :947 | **Correct** |
| Rows at :1038/:1040/:1056/:1058/:1386/:1494 | **Now stale by +8**: they are at 1046, 1048, 1064, 1066, 1394, 1502 |
| Checker line counts (1174 / 1559 / 166 / 272 / 146; registry 4160; ALLOWLIST about 854; yaml 10054) | **Correct** |
| Pilot paths | `terrain_admission_mode.h` and `terrain_surface_trace.h` are in `mclib/`. `gos_terrain_patch_stream.cpp` has 12 sites, not 13. |
| Logging P2 for `gos_terrain_patch_stream.cpp` belongs in G1 | **Already partly done** in `f980105df` (15 `MC2_EVENT` calls exist) |
| Nothing reads the dropped yaml fields | **Correct.** Only `flag-registry-auto-gen.py` and `check-flag-registry-truth.py` mention them, and the latter ignores them. |
| `mc2::log::Lit` gives compile-time precedent | **Thin.** It landed today (`f62bd4bf4`, `e271a6c68`) and has about 14 `MC2_EVENT` uses in 4 files. That is no evidence for about 1400 instantiations. |

## (c) Concrete behavior-change sites

1. **`MC2_SHADOW_ENABLE`** has three readers.
   - `gos_mech_batcher.cpp:1167` and `gos_static_prop_batcher.cpp:9290` use `!(v && v[0]=='0')`: unset means ON.
   - `vulkan_post_screenfx_native.cpp:759` uses `envTriState(..)==1`: unset means OFF.
   - A `Bool, default=true` row flips the VK direct-CSM path when the variable is unset.
2. **`MC2_DISABLE_GOSFX`** has three readers with three rules.
   - `mclib/mlr/mlr_gate.cpp:37` disables only on a leading `'1'`; an empty value uses the built-in default.
   - `gameosmain.cpp:1340` checks for exactly `"0"`.
   - `GraphicsOptionsWindow.cpp:1477` checks presence only.
   - With a Presence row, the documented dev override `=0` would *disable* FX in `mlr_gate`. With Std, `"true"`/`"yes"` flips there too.
3. **`MC2_TERRAIN_LIGHTING_V1` and `_V2`.** `gameos_graphics.cpp:7175` treats `=0` as strength 0. `gos_terrain_lod_chunk.cpp:2872` checks presence, so `=0` applies the strength. Whichever single rule the row gets, one of those sites changes.
4. **`MC2_VK_TERRAIN_CSM`.** `gos_terrain_lod_chunk.cpp:1858` uses `v[0]=='1'` (no inheritance from the umbrella gate). `:1900` uses `subGate`, which inherits it. Converting both to one Tri helper would arm the CSM subset under the umbrella.
5. **`MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR`** (G1 pilot, and the design's own example row). The reader at `gos_terrain_height_tex.cpp:51` accepts only 2 or 4 and maps everything else to 1. The `lo=1, hi=8` clamp passes 3 and 8 through. `"2.0"` also changes: `atoi` gives 2 today, while a whole-string `from_chars` falls back to the default.
6. **`MC2_LIGHT_GRID_DIM` and `MC2_LIGHT_POOL_MAX`** (`gos_terrain_lighting.cpp:276` and `:859`, G1 pilot). Out-of-range values fall back to the default today (64 / built-in); a clamp would turn 300 into 256.
7. **Editor timing.**
   - `EditorMFC.cpp:188` sets `MC2_SNAPSHOT_STATIC_PROP_BUILD=0` inside `InitInstance`. The reader `s_snapshotBuildEnabled` (`gos_static_prop_batcher.cpp:246`) is a namespace-scope lambda static that has already run by then. So the override is probably ineffective today (please verify in the editor).
   - If the codemod replaces such statics with call-time `get<>()`, the override starts working, which is a behavior change.
   - `editor/CMakeLists.txt:71` builds with `DISABLE_GAMEOS_MAIN`, so none of the three `gameosmain` hooks run in EditRel.
8. **`MC2_ACTIVE_MOD`.** `ModPicker::Activate` calls `_putenv_s` and then `InitModSearchPaths`, which re-reads `file.cpp:727` on every mod switch. A cached slot without `reread` or `invalidate` would mount the wrong mod.
9. **Existing bug, launcher_env missed before `main`.** The dev install's `mc2-win64-v0.4/launcher_env.json` sets `STATIC_PROP_PBR_V1`, `STATIC_PROP_AMBIENT_V1`, `CHEAT_INFINITE_MONEY` and `CHEAT_SALVAGE_ALL`. Their readers (`gos_static_prop_batcher.cpp:1171,838`, plus `code/{logisticsdata,mechlabscreen,mechlistbox,mechpurchasescreen,salvagemecharea}.cpp`) run during static init. On an `MC2_NO_LAUNCHER` run they are OFF today even though `[LAUNCHER_ENV v1]` reports them as applied.

Harness value sweep: I found 429 `MC2_X=value` pairs in `scripts/`, `tests/`, `tools/`, `data/` and `run/`. All 39 `=0` assignments target readers that already treat `"0"` as off. No active harness drives a presence-only gate to `0`; `MC2_TREE_DIAG_TRACE=0` appears only in a `run_smoke.py` comment. No literal `true/false/on/off/yes/no` values were found (`MC2_IMGUI=ON/OFF` is a CMake option). No `str(bool)` values were found. So the EDGE risk is in the code, not the harness.

## (a) Answers to §8

1. **NTTP vs enum.** Build cost is roughly equal. Both need the same about-1010-entry index header, and the binary search is about 10 `string_view` compares, which is trivial. The real cost is symbol and PDB bloat from mangled `char[N]` NTTPs, plus a rebuild of about 202 TUs whenever a gate is added. Probe with `/d1reportTime` on `gos_postprocess.cpp` (102 sites), not only with `/Bt+`. Also test the risky shapes: lambda-static initializers (the most common site shape), namespace-scope initializers, uses inside templates, and uses inside macros.
   - **Type hole:** `if (gate::get<"TRI">())` compiles but tests `has_value()`. That reintroduces presence semantics without anyone noticing.
2. **Index/table split.** Acceptable as a derived artifact, but only if the `static_assert` checks name, kind and count. That requires a `constexpr` table; `constinit` alone cannot be read by `static_assert`.
   - `gate_gen` is itself a regex parser of C++, the same drift class being deleted. The compile check covers only name and kind; default, doc and owner in the yaml can still drift.
3. **Presence gates driven to `=0`.** None found (see c). The design's grep scope misses the JSON matrices (`tests/smoke/matrices/*.json`, `tests/visual/**/capture_run.json`, bookmarks), `.ps1` scripts, installed `launcher_env.json`, the launcher's `offVal` values, and in-process `_putenv` calls. Python values built by formatting are mostly `str(int|float|path)`. Floats are fine for Float rows; `"1.0"` would fail an Int row.
4. **Truthiness rule.** "Banana means true" is right for Bool. Multi-token gates (`MC2_TONEMAP_V2` = 1/2/3/agx/hable, `MC2_WATER_SHORE_MODE`, `MC2_UNIT_DEFINITION_SOURCE`, `MC2_CONFIG_DUMP`) need an Enum kind with allowed tokens. Yes to `MC2_CONFIG_STRICT=1`, set by `run_smoke.py`, which makes `badvalue` or `unregistered` fatal.
5. **`invalidateAll()`.** It never changes a pre-main `static const` copy, and it is *required* to keep call-time readers identical. `premain_stale=0` will **not** hold on an `MC2_NO_LAUNCHER` run from v0.4 once the item 9 gates migrate (at least 4 stale).
   - The warning only sees migrated slots, so it cannot surface misses for gates still read raw.
   - `invalidateAll` wipes the slots, so the pre-main values must be snapshotted first.
6. **Gaming LG19.**
   - The ratchet allows per-dir increases when the total does not rise (`cxx17_ratchet.py:464`), so a new raw read offset by one migration passes.
   - The regex misses `GetEnvironmentVariableA(name, ..)` with a non-literal name (`editor/EditorPlaytest.cpp:163,915`), `_environ` scans (`gos_visual_capture.cpp:254`), `GetEnvironmentStringsA` and `SDL_getenv`.
   - `#pragma deprecated(getenv)` is too blunt: it also fires on `PATH`.
   - Keep the diff-based `new_gates` row until G8. Note it only diffs against `--base HEAD~1`.
7. **Signal lost by deletion.** See (d)5.
8. **Moving essay docs.** Yes, those warnings get lost. Emit a `// history: docs/gate-history.md#MC2_X` line above each row that has history. Comments are now harmless because the compiler is the parser.
9. **`dumpConfig` cost and the editor.** Cost is negligible: about 100 `_environ` entries. `all` mode means about 1010 unbuffered `fwrite` calls (stderr is set `_IONBF` near `gameosmain.cpp:1155`), roughly 1–5 ms.
   - In the editor it never runs, and `markMainEntered` never runs, so every slot would carry the pre-main bit.
10. **G1 scope.** Split it (see e).

**Additional questions the design should answer:**
- Does the codemod keep each site's caching shape (namespace static, local static, or per call)? It must, or timing changes (c7, c9).
- Who calls `invalidateAll` in harnesses that `_putenv` after static init (brain, render_state, job_system)?

## (d) Required amendments

1. **Per-site rule override.** Add `gate::get<"X", Rule::Presence|Exact1|Lead1|Lead0Kill>()` (or a raw `gate::view<>`) so a site keeps its old rule when its row's rule differs. Before migrating any gate, unify its readers in a named, A/B'd commit for each cross-reader conflict.
   - *Why:* items c1–c4. A conflict that "stops the codemod" has no stated resolution path.
2. **Numeric `range=reject` and `Enum` kind.** Default numeric bounds to *reject → default*, keeping clamp only where the old code clamped. Add an `Enum` kind with a token list. Add a numeric verdict column: E, parse failure, out of range, and trailing junk all count as EDGE.
   - *Why:* c5 and c6; `atoi`/`atof` accept prefixes, `from_chars` on the whole string does not.
3. **Language-standard precondition.** Move the standalone CMake projects that compile engine sources up to the standard set by `MC2_CXX_STANDARD`, or at least C++20:
   - `tools/asset_viewer` (`mc2_tglloader`), `tg_import_dump`, `tgl_loader_standalone_spike`, `job_system_harness`, `mech_import_harness`, `render_state_contract_harness`, `rendercore_standalone_spike`, `brain_dispatch_harness` (it `#include`s `code/brain_special_dispatch.cpp`)
   - `tests/model_override` (`cxx_std_14`, compiles `model_override_registry.cpp`)
   - Also drop `17` from the `MC2_CXX_STANDARD` options.
   - *Why:* class NTTP and `consteval` need C++20. `tests/unit` is already at 20 (`CMakeLists.txt:135`).
4. **Library shape.** Keep a STATIC lib; an OBJECT library would give duplicate symbols when linked into several static libs that end up in one exe. Standalone projects add `Gates/gate.cpp` and `gate_table.cpp` as sources. A missed link fails loudly with LNK2019, which is acceptable.
   - *Why:* `cpp_probes` has no engine sources (no audit needed); `contract_harness_common` is Python. The real list is the one in item 3.
5. **Keep the signals the type system does not replace.**
   - From `gate_default_agreement`: rule 2 (cross-reader and cross-backend conflict) for unmigrated gates and for Tri rows. Tri call sites choose `value_or` locally, which reintroduces default disagreement.
   - Rule 3 (default claims in C++ comments, GLSL comments and `docs/tier1_env_vars.md`, checked against the table).
   - The reasoned exceptions in `gate-defaults-allowlist.txt`.
   - Either generate `tier1_env_vars.md` from the table or keep a small doc-vs-table check. No slice currently retires or regenerates it.
6. **Compiler-sourced yaml.** Generate the yaml and registry shim from the *compiled* table (a doctest or `gate_dump` that emits JSON), and have `gate_gen --check` diff against that.
   - *Why:* this removes the last text parser of the truth (§8 Q2).
7. **Make Tri unconvertible to bool.** Return `enum class Tri {Unset, Off, On}` with no implicit bool conversion.
   - *Why:* `if (optional<bool>)` silently means presence.
8. **Hooks in every entry point.** Add `markMainEntered`, `invalidateAll` and `dumpConfig` to `EditorMFCApp::InitInstance` after `editor_set_default_env_vars()`. Add `gate::invalidate("MC2_ACTIVE_MOD")` in `ModPicker::Activate`. Harness mains that `_putenv` call `invalidateAll()`.
   - Make the premain bit meaningful only once `markMainEntered` has been called somewhere.
   - Add the `reread` flag to the gates noted fresh-per-call for `_putenv` (`brain_special_dispatch.cpp:226,234`).
9. **Codemod keeps the caching shape.** A `static const bool s = <expr>` stays a static holding `gate::get<>()`. Removing a cache is a separate commit.
   - *Why:* c7 and c9; changing when a value is read changes behavior.
10. **Seed from all literals.** Seed from every `"MC2_*"` literal passed to any call (1176 names), not from the getenv sweep (997). Include names that only harnesses set, or `unregistered=0` will be flaky.
11. **Keep MCP gate tools working.** `gate_status` and `env_index` keep reading `run_smoke.py` passthrough until G6, or ENV-DROP detection breaks between G2 and G6. `dead_gate_scan` and `env_index` must also grep `get<"MC2_X">` once raw `getenv` is gone.
12. **Fix the doc inconsistencies.** `gate.h` declares `std::string str()` and `std::FILE*`, but says it includes neither `<string>` nor `<cstdio>`. Move `str()` and `dumpConfig` to a cold header.

## (e) G1: no-go as written, go if split

- **G1a (infrastructure only, zero migrated sites):**
  - The `Gates/` module, `gate_gen` (seeded as in amendment 10), and `test_gate` covering the full §1 matrix plus every idiom class from (b).
  - CMake changes and the harness standard bump (amendment 3).
  - Hooks in `gameosmain` and the editor (amendment 8).
  - LG19 written together with its baseline.
  - An NTTP probe (`/d1reportTime`) covering lambda-static, namespace-scope, template and macro contexts.
  - Exit criteria: smoke byte-identical, `[CONFIG v1]` lines present, editor launches.
- **G1b (pilot):**
  - Preconditions: amendments 1, 2 and 9 are in place.
  - Drop `gos_terrain_height_tex.cpp` from the pilot or encode it as `Enum{1,2,4}`.
  - `gos_terrain_lighting.cpp` numeric rows use `range=reject`.
  - Logging P2 for the patch-stream file is already partly done; limit the remaining P2 work to `gos_terrain_lighting.cpp`.
  - Exit criteria: LG19 at most 1349 (39 sites), the TSV shows zero SEMANTICS rows migrated to Std, and a one-mission A/B shows identical golden bytes.

Item 9 in (c) is an existing bug worth its own ticket, separate from this work: `MC2_NO_LAUNCHER` runs from the dev install silently drop PBR/ambient and the cheat gates from `launcher_env.json`.
