# Typed gate registry design (S2) — drafted 2026-10-09, status: awaiting adversarial review
# Typed gate registry: design (plan S2, `.claude/cleanup/opus-modernization-review.md` §7 S2 and §3 "Gate/doc truth drift")

Read-only survey of `claude/nifty-mendeleev` at HEAD `48ea7b3d8`. Counts were measured on the working tree. File:line references are pre-edit.

## 0. Measured baseline

| Fact | Value | How measured |
|---|---|---|
| `getenv("MC2_` lines, 9 owned dirs (ratchet scope, comments blanked) | **1388** | ratchet `strip()` plus regex |
| Same, plus `editor/` 31, `gui/` 13 (outside the ratchet default dirs) | 1432 (202 files) | same |
| Any `getenv(` line in owned dirs (includes helper bodies and non-literal names) | 1446 | same |
| Distinct `MC2_*` names read via `getenv` | 1010 | `grep -oh \| sort -u` |
| Files with at least 20 sites | 16 files hold 751 sites (52%) | per-file count |
| Files with 5 to 19 sites | 42 files, 404 sites | per-file count |
| Files with fewer than 5 sites | 144 files, 271 sites | per-file count |
| Largest files | `gos_postprocess.cpp` 103, `gos_static_prop_batcher.cpp` 94, `gameos_graphics.cpp` 77, `gos_terrain_lod_chunk.cpp` 62, `gameosmain.cpp` 55, `bdactor.cpp` 49 | per-file count |
| Presence-only idiom `(getenv("X") != nullptr)` | about 330 sites | idiom histogram |
| Registered names (`RendererFeatureRegistry.h`, 4160 lines, `kFeatureTable` plus `kAuxEnvVars`) | 363 | file |
| Allowlisted but unregistered (`scripts/check-env-registry.sh:175` `ALLOWLIST=(`) | about 850 lines | file |
| Hand-maintained name lists | 3: registry header, `ALLOWLIST`, `docs/tier1_env_vars.md` (1442 lines) | files |
| Files that reference or parse the text registry | 20 (see section 2) | grep |
| `_putenv_s` / `SetEnvironmentVariable` writers inside the engine | 4: `gameosmain.cpp:1141`, `gos_dev_shell.cpp:296`, `bdactor.cpp:413-435`, `editor/EditorMFC.cpp:177,188` | grep |
| Pre-main gate reads (file-scope `static ... = ...getenv`) | 59 at column 0, plus 16 lambda-initialised statics | grep |
| Function-local `static` gate reads | about 170 | grep |

At least some reads are uncached. For example, `code/simplecamera.cpp:149,169,208,264` calls `getenv("MC2_LOG_PREVIEW")` inside `SimpleCamera::render`. `MC2_LOG_PREVIEW` has 26 raw reads in total. The uncached share is not syntactically measurable, so treat it as an estimate.

## 1. Local helper inventory and truthiness disagreements

Input classes: U = unset, E = `""`, Z = `"0"`, F = `false`/`off`/`no`, O = `"1"`, X = other (`"2"`, `"yes"`, `"abc"`).

| Helper (file:line) | U | E | Z | F | O | X |
|---|---|---|---|---|---|---|
| **ParseEnvBool** (`code/terrobj.cpp:83`, `mclib/bdactor.cpp:2852`, `gos_static_prop_registry.cpp:122` `parseEnvBoolWithDefault`, `code/sim_identity.cpp:71` `envOnOff`, `HostServices/DefaultHostServices.cpp:98` `EnvConfig::flag`, `gui/UiDefs.cpp:97` `envValueIsFalse`; also the `suParseBool` lambda at `gameosmain.cpp:~1246`) | def | def | off | off | on | on |
| `envFlag` (`GameAdapters/MechRenderAdapter.cpp:41`, `StaticPropRenderAdapter.cpp:30`, `RenderWorld.cpp:65`), `VegetationAdapter.cpp envSet`, `debug_state_dump.cpp:188 envFlagOn`, `tglc_envOn` (`gos_terrain_lod_chunk.cpp:60`), `hudimguipanel.cpp:18 envGateOn`, `vis_semantics.cpp env_default_off`, `gameos_graphics.cpp:130 debugEnvEnabled` | off | off | off | **on** | on | on |
| `envFlagDefaultOff` (`debug_renderer.cpp:32`, `gos_cluster_depth_pyramid.cpp:30`, `gos_lightgrid_build.cpp:66`, `gos_postprocess_blur.cpp:40`) | off | **on** | off | **on** | on | on |
| `envFlagDefaultOn` (`gos_mech_batcher.cpp:128`, `mclib/gvactor.cpp:111`, `txmmgr.cpp:158 lightClampEnvFlagDefaultOn`) | on | on | off | **on** | on | on |
| `RenderWorld.cpp:70 envFlagDefaultOn` | on | **off** | off | **on** | on | on |
| `vis_semantics.cpp:13 env_default_on` | on | on | off | on | on | on |
| `debug_state_dump.cpp:193 featureActive` | def | def | off | **on** | on | on |
| Exact-"1" (`vulkan_mech_island.cpp:126 mechPerfGateRequested`, `gameos_graphics.cpp:135 vkScopedGlCanaryEnabled`, `code/gameplay_pick.cpp:172`, `txmmgr.cpp:151 lightClampEnvFlagDefaultOff`) | off | off | off | off | on | **off** |
| Tri-state (`vk_shared_gbuffer_gate.h:73 envTriState`, `subGate:95`, `directChainGate:123`) | inherit | inherit | off | inherit | on | inherit |
| Presence `!= nullptr` (about 330 sites) | off | **on** | **on** | **on** | on | on |
| `atoi(getenv(..)) != 0` (about 19 sites) | off | off | off | off | on | `"true"` is **off**, `"2"` is on |
| Numeric: `envInt` (`gos_rdoc_capture.cpp:44`, `gos_visual_diff.cpp:47`, `VegetationAdapter.cpp:39`) use `atoi`; `envFloat` (`mclib/cliff_decal_tuning.cpp:26`) uses `atof`; `envVfxScale` (`gos_particle_bridge.cpp:143`) clamps. | | | | | | `"abc"` gives 0, `"12abc"` gives 12 |

Disagreements that matter in practice:

1. Presence-only makes `MC2_X=0` turn the gate ON. About 330 sites do this. `terrain-gates-plan.md` already documents `MC2_TERRAIN_LIGHTING_V1=0` enabling V1.
2. The four DefaultOff helpers treat `MC2_X=` (empty) as ON. `RenderWorld::envFlagDefaultOn` treats it as OFF. `gvEnvFlagDefaultOn` treats it as ON.
3. Exact-"1" readers treat `"true"` and `"yes"` as OFF. Truthy readers treat `"false"` as ON.
4. `HostServices/DefaultHostServices.cpp:98` already implements the clean rule (tokens `0`/`false`/`off`/`no`, case-insensitive, otherwise true) and a unit test exists (`tests/unit/test_host_services.cpp`). That is the precedent for the canonical rule.
5. `RendererFeatureRegistry.h:4123 debugDumpFeatureRegistry` is dead (no callers). It carries yet another rule (`v[0]=='0'` means off) and an unused `[RENDERER_FEATURES v1]` banner. Nothing in the repo parses it.
## 2. Current registry and doc machinery

All of it exists because gates are untyped strings.

| Piece | What it parses, from where | Lines |
|---|---|---|
| `scripts/registry_parse.py` | the one shared parser of `RendererFeatureRegistry.h`, with self-referential invariants. It exists because three private regexes dropped rows (REGISTRY-PARSER-TRUTH-1). | 598 |
| `scripts/flag-registry-auto-gen.py` v2.1.0 | registry rows plus a getenv sweep, `run_smoke.py` passthrough, `tier1_env_vars.md`; writes `docs/flags.auto.yaml` (10054 lines, 1099 flags, 363 registered). | 398 |
| `check-flag-registry-drift.sh` | regenerates the yaml and diffs it. `check-flag-registry-truth.py` checks that `registered:`/`default:`/`kind:` agree with a strict parse. | 34 / 290 |
| `check-env-registry.sh` | greps every quoted `"MC2_*"` in C++ and requires it to be registered or in `ALLOWLIST`. Has its own string-aware comment stripper. | 1174 |
| `check-gate-default-agreement.py` | classifies each reader's unset-case idiom and compares it to the registry `defaultOn`, to other readers, and to comments and `tier1_env_vars.md`. | 1559 |
| `check-new-gates.py`, `check-registered-gate-passthrough.py`, `check-smoke-env-inheritance.py`, `check-smoke-matrices.py`, `check-split-path-contracts.py`, `build_review_pack.py`, `pe_gate_scan.py`, `tools/repo_intel/{env_index,gate_status,dead_gate_scan}.py`, `scripts/mcp/repo_intel_server.py`, `tools/comment_tidy` | each re-reads the registry, the getenv sweep, or both | about 20 files total |
| `dead_gate_scan.py` | works from `env_index`'s getenv grep, plus bounded line windows around reader sites ("result unused" lexical check, `_WINDOW` 6 lines, `_USE_WINDOW` 15) | 481 |

`tools/cxx17_ratchet/cxx17_ratchet.py` defines LG01 to LG18 in the `IDIOMS` dict (lines 49-68). Detectors live in `scan_text()` (lines ~170-293). `strip()` returns `(nc, bl)`: `nc` has comments blanked and strings kept, `bl` has both blanked. LG11 uses `nc` (string-aware); the others use `bl`. Hits are a per-file set of line numbers. `baseline.json` holds per-dir, per-idiom counts. The check does `base["counts"].get(d, zero).get(i, 0)`, so an idiom missing from the baseline reads as 0. The commit that adds LG19 therefore fails unless it also writes the baseline.

## 3. Runtime behavior of gate reads

- **Pre-main reads.** There are 59 column-0 `static const bool s_x = getenv(...)` plus 16 lambda statics. They run during dynamic initialisation, before `main`.
- **Hazard found.** `gameosmain.cpp:~1090-1150` ([LAUNCHER_ENV v1]) injects `launcher_env.json` into the process env with `_putenv_s` when `MC2_NO_LAUNCHER` is set. That happens after static init. Every pre-main-cached gate silently misses launcher-curated values on the `MC2_NO_LAUNCHER` path. Launcher-spawned runs are unaffected because the env block is built before exec. The registry must make this visible without changing behavior (section 4.5).
- **Runtime mutation.** `gos_dev_shell.cpp:289 cmdSetGate` calls `_putenv_s`, and its reply admits "init-read gates require restart". `bdactor.cpp:413-435` temporarily sets and restores `MC2_GLTF_AXIS` and `MC2_GLTF_YAW_DEG` around a load. A gate with that shape must be flagged `reread` in the table.
- **Caching idioms.** Function-local `static const bool` (about 170), file-scope statics, `static int state = -1` tri-state caches (216 in GameOS), and per-call `getenv` (`SimpleCamera::render`, the `vk_shared_gbuffer_gate.h` `subGate` family when called uncached).
- **Non-literal names.** `gamecam.cpp:1241`, `gameos_graphics.cpp:7372` and `gos_terrain_lod_chunk.cpp:951` loop over name lists. `gos_dev_shell.cpp:cmdGetGate` takes arbitrary names. These need a runtime-name lookup.
- **MC2_LOG special case.** The facade (`GameOS/gameos/utils/mc2log.cpp:171 envView`, `init()` at 176) reads `MC2_LOG` as presence, and presence of any value means streams stay live. It is not a boolean. `MC2_LOG_DECORATE` and `MC2_LOG_EVERY_N_FRAMES` are also facade-owned. `mc2log.h` is a leaf header (std only). The gate module must therefore sit below `mc2log`, never above it.

## 4. Design

### 4.1 Layout (mirrors `tools/gpu_abi_gen.py` and `mclib/terrain_material.h`)

The hand-edited C++ table is the single source. Python derives everything else, and `--check` fails on drift. This is the `check-contracts.sh:947 gpu_abi` shape.

```
Gates/gate.h             public API; includes gate_index.gen.h; <atomic> <cstdint> <optional> <string_view> only
Gates/gate.cpp           cold paths: parse, resolve, invalidate, dumpConfig, lookup(runtime name)
Gates/gate_table.cpp     constinit table, one row per line, alphabetical (static_assert sorted + unique)
Gates/gate_index.gen.h   GENERATED sorted {name, kind} only (about 1010 lines, committed)
tools/gate_gen.py        --import-legacy (once), --write, --check, --selftest, --due
tests/unit/test_gate.cpp doctest parse matrix
```

Reasons for the split:
- The call-site header needs only name and kind, so changing a default or a doc rebuilds one TU instead of every includer. An include-everything table would turn each default flip into a rebuild of about 200 TUs.
- `gate_table.cpp` carries a `static_assert` that each row's kind equals the index's kind for the same position. Drift between the two files is a compile error, not a lint.
- Fallback if `/Bt+` shows the split is unnecessary: put the table in the header.
- Place it as a small leaf STATIC lib `mc2_gate`, added to `include-firewall` `SCOPE_DIRS` and to the ratchet `DEFAULT_DIRS`.
- Harnesses that compile selected sources (`tools/*_harness`, `build_contract_harnesses.py`, `contract_harness_common`) must gain `Gates/gate.cpp` and `gate_table.cpp`. This is a known one-off cost.

### 4.2 Row

```cpp
//  name                           kind  default owner     retireBy     doc (one literal, one line, <= 160 chars)
G("MC2_TERRAIN_LOD_CHUNK",         Bool, true,   "terrain","",          "chunked-LOD terrain path; =0 restores legacy")
G("MC2_PATCH_STREAM_TRACE",        Bool, false,  "terrain","2027-01-31","patch-stream lifecycle trace")
L("MC2_THIN_DEBUG",                Bool, false,  "terrain","2026-12-31","legacy presence rule; convert to Std")  // L = Rule::Presence
G("MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR", Int, 1, "terrain","",           "height-tex resample factor", lo=1, hi=8)
```

Fields: `name`, `kind` (Bool, Tri, Int, Float, String), `default`, `owner` (closed list checked by `gate_gen`), `retireBy` (ISO date or empty), `doc`, `rule` (Std, Presence, Exact1), optional `lo`/`hi`, `reread`, `smokePassthrough`, `logTopic`.

The grammar is deliberately one macro invocation per line with a single string literal for `doc`. That removes the three row-drop causes recorded in `scripts/registry_parse.py` (adjacent literals, inline comments between fields, trailing commas). `gate_gen` also asserts that its row count equals the compile-time `kCount`.

The 363 existing registry rows carry essay-length docs (some about 2 KB, for example the `MC2_SHADOW_ENABLE` entry). The importer splits each row. The first sentence becomes `doc`. The full text moves verbatim to `docs/gate-history.md`, keyed by name, so no knowledge is lost.
### 4.3 Reader API: `gate::get<"MC2_X">()` (NTTP name) rather than `enum class Id`

```cpp
namespace mc2::gate {
template <size_t N> struct Name { char v[N]; consteval Name(const char (&s)[N]); };   // same trick as mc2::log::Lit
template <Name n> [[nodiscard]] inline auto get() noexcept;   // bool | int32_t | float | std::optional<bool> (Tri), by table kind
template <Name n> [[nodiscard]] bool isSet() noexcept;        // presence, any kind (MC2_LOG, MC2_NO_LAUNCHER)
template <Name n> [[nodiscard]] std::string str();            // Kind::String only; cold paths, owning copy
[[nodiscard]] std::optional<std::string_view> lookupName(std::string_view) noexcept;   // runtime names (dev shell, name loops)
bool parseBool(const char* v, bool def) noexcept;             // THE rule; HostServices::EnvConfig::flag delegates here
void invalidate(std::string_view name) noexcept;              // dev-shell set_gate; also invalidateAll()
void dumpConfig(std::FILE*) noexcept;                         // section 4.5
}
```

The hot path is one relaxed atomic load, a test and a branch:

```cpp
extern std::atomic<uint64_t> g_slot[kCount];   // defined in gate.cpp, zero-initialised
// get<n>(): idx = consteval indexOf(n); v = g_slot[idx].load(relaxed); if (v >> 63) return decode<T>(v); return detail::resolve(idx) [cold, out of line]
```

- No function-local static is used. That avoids MSVC's thread-safe-init epoch check and any init-order dependence.
- Racing first reads compute identical values, so the race is benign.
- Slot encoding: bit 63 = resolved, bit 62 = resolved before `markMainEntered()` (pre-main flag), low 32 bits = value (bool, int32, or float bits).

Why NTTP names rather than `Id` enums:
- The canonical identity is the `MC2_*` string. CLAUDE.md, memory, MCP tools (`gate_status`, `get_runtime_gate`), harness scripts and `tier1_env_vars.md` all key on it. An enum adds a second namespace to keep in sync and breaks `rg MC2_X`.
- The codemod becomes mechanical: `getenv("MC2_X")` becomes `gate::get<"MC2_X">()` with no per-gate identifier or include decisions.
- A typo (`MC2_TERAIN_X`) becomes a compile error. Today it is a silently dead gate.
- The consteval lookup is a binary search over the sorted `kNames`, about 10 compares per instantiation. This is the part to probe first (see section 6, G1).
- `gate.h` must not include `<format>` or `<string>` on the inline path, to avoid the `mc2log` bloat recorded in the review's section 6.1.

### 4.4 The single truthiness rule (Std)

- `Bool`: U or E gives the default. ASCII case-insensitive `0`/`false`/`off`/`no` gives false. `1`/`true`/`on`/`yes` gives true. Any other non-empty value gives true and is flagged `badvalue` in the dump. This is the `ParseEnvBool` family, so about 7 helpers already agree with it.
- `Int` and `Float`: `std::from_chars` on the whole string, optional `lo`/`hi` clamp. U, E, or a parse failure gives the default plus `badvalue`. This is locale-independent and replaces `atoi`/`atof`.
- `Tri`: exactly `"1"` or `"0"` is explicit. Anything else is "inherit". `mc2vkgate::subGate`, `directChainGate`, `umbrellaOn` and `terrainNativeFirstOwnerOn` stay in `vk_shared_gbuffer_gate.h` and are re-implemented over `gate::get<>()` for `Tri`. The umbrella logic (including the per-call `g_mc2VkSessionForcedGL` read) is not parsing and is unchanged.
- `Rule::Presence` and `Rule::Exact1` exist only so migration can stay byte-identical. They require a non-empty `retireBy`. `gate_gen --check` ratchets the count of non-Std rows downward.

### 4.5 [CONFIG] dump, ordering, and pre-main safety

- **Call sites.** `gameosmain.cpp` gets three calls. `gate::invalidateAll()` goes immediately after the launcher_env apply loop (`:1141-1150`). `gate::dumpConfig(stderr)` goes right after `mc2::log::init()` at `:1180`, so the MC2_LOG NUL redirect has been decided. `gate::markMainEntered()` goes at the top of `main`.
- **Format.** One `fwrite` per line, not through `mc2log`, because `mc2log` depends on `gate`:

```
[CONFIG v1] name=MC2_TERRAIN_LOD_CHUNK kind=bool value=0 src=env raw="0" default=1 rule=std owner=terrain
[CONFIG v1] warn=unregistered name=MC2_TERAIN_X          # scanned from _environ: set but not in the table
[CONFIG v1] warn=premain name=MC2_FOO premain_value=0 env_now=1   # launcher_env.json applied after this gate was cached
[CONFIG v1] summary total=1010 set=7 nondefault=3 unregistered=0 badvalue=0 premain_stale=0
```

- By default it prints only env-set gates plus the summary. `MC2_CONFIG_DUMP=all` (itself a String row) prints the full table.
- `src=env|default|badvalue`. `dumpConfig` re-parses from the env using the same `parse` code and the table, so it reports truth rather than a re-implementation.
- The `premain` warning compares the cached value of a pre-main-resolved slot against a fresh parse after `invalidateAll()`. It turns the launcher_env ordering bug into a visible line without changing any behavior.
- Optional follow-up: `scripts/smoke_lib/logparse.py` can ingest `[CONFIG v1]`, giving engine-side provenance that `check-capture-env-declared.py` currently asserts from the harness side.
- `debug_state_dump.cpp` (`featureActive`, `envFlagOn`, 188-195) moves to `gate::`, so the MCP `get_runtime_gate` answer comes from the same parser as the game.

### 4.6 Generation (`tools/gate_gen.py`)

| Output | Content | Notes |
|---|---|---|
| `Gates/gate_index.gen.h` | sorted names plus kinds | committed; `--check` regenerates in memory and diffs |
| `docs/flags.auto.yaml` v3 | pure function of the table: name, kind, default, rule, owner, retire_by, doc | **drops** `referenced_at`, `in_smoke_passthrough`, `documented_in_tier1_env_vars`. Those depend on the working tree and caused the drift row to be advisory-then-strict. |
| `RendererFeatureRegistry.h` rows | `kFeatureTable` and `kAuxEnvVars` emitted from the table; the `RendererFeature` enum stays hand-ordered | keeps `tests/unit/test_rendercore.cpp` and `GuiRuntime/EditorInspector.cpp:357` compiling until they move to `gate::table()`. Removes the dual truth. |
| passthrough allowlist | `smokePassthrough` column becomes generated data for `run_smoke.py` | G6 |
| `--due` | rows past `retireBy` | advisory report; feeds `dead_gate_scan` |

The offline check is the existing `flag_registry_drift` row retargeted to `gate_gen.py --check`: table to index to yaml to registry shim, hermetic. A cheap second check asserts that every `"MC2_*"` literal in C++, `.py`, and the launcher is in the table. This is the only survivor of `env_registry` once LG19 reaches 0.

Seeding (`--import-legacy`, run once). Names come from the 1010-name sweep. Defaults come from the 363 registry `defaultOn` values. Reader-idiom classification comes from the existing logic inside `check-gate-default-agreement.py`, reused so its brain is not rewritten. Kind comes from reader idioms (`atoi` gives Int, `atof` gives Float, a path or `char*` gives String). Ambiguous rows become `Bool` with `owner="unclassified"`. A site's codemod classification must agree with its row, and a conflict stops the codemod. Wrong seed values stay harmless until a site migrates.
### 4.7 Migration mechanics (strangler, file by file)

`tools/gate_codemod.py` is one-shot and discarded in G8. It emits a TSV per file: `file:line, name, old idiom class, new rule, verdict`. The reviewer reads the TSV, not the diff.

| Verdict | When | Action |
|---|---|---|
| NO-CHANGE | old idiom equals Std on U/E/Z/F/O/X (ParseEnvBool family, `EnvConfig::flag`) | rewrite site; row `rule=Std` |
| EDGE | differs only on E, F, or X (`envFlag`, DefaultOff/DefaultOn helpers, exact-1, `atoi`) | rewrite to Std; list the file in the PR. `scripts/`, `run*.bat`, `data/launcher*`, `docs/` are grepped for `=true|false|on|off|yes|no|` values to prove no harness depends on the old edge behavior. |
| SEMANTICS | differs on Z or U (presence: `MC2_X=0` was ON) | row gets `rule=Presence` plus `retireBy`; site is byte-identical. Converting to Std is a separate named commit with an A/B note. |

Helper replacements, applied as each file's last site goes:

| Delete | Replace with |
|---|---|
| `terrobj.cpp:83`, `bdactor.cpp:2852` ParseEnvBool, `gos_static_prop_registry.cpp:122`, `sim_identity.cpp:71`, `suParseBool` (`gameosmain.cpp`) | `gate::get<>` (NO-CHANGE) |
| `envFlag` x3 adapters, `RenderWorld::envFlagDefaultOn`, `hudimguipanel.cpp:18`, `debug_state_dump.cpp:188-195`, `gameos_graphics.cpp:130`, `tglc_envOn`/`tglc_envStrength` | `gate::get<>` (EDGE; RenderWorld E gives default is flagged) |
| `envFlagDefaultOff` x4, `envFlagDefaultOn` (`gos_mech_batcher.cpp:128`, `gvactor.cpp:111`, `txmmgr.cpp:151,158`), `vis_semantics.cpp:13,19` | `gate::get<>` (EDGE; `MC2_X=` empty and `false` change) |
| `envInt` x3, `envFloat`, `envVfxScale`, `cmdGetGate` | `gate::get<>` with `lo`/`hi`; dev shell uses `gate::lookupName` |
| `envTriState` (`vk_shared_gbuffer_gate.h:73`) | `gate::get<>` returning `std::optional<bool>` |
| `HostServices::EnvConfig::flag` | calls `gate::parseBool` (already equal) |

**Logging P2 interaction.** Per the review's S2, `getenv` migration and logging P2 land in the same per-file diff. `if (gate::get<"MC2_PATCH_STREAM_TRACE">()) MC2_EVENT(...)` keeps both unchanged: the gate decides whether, the facade decides how. `MC2_LOG`, `MC2_LOG_DECORATE` and `MC2_LOG_EVERY_N_FRAMES` become String and presence rows read through `gate::str` and `gate::isSet<"MC2_LOG">()`. `mc2log.cpp:envView` is deleted. The `logTopic` column is the single source for the future legacy-alias table (logging plan P3). It is not used in G1 to G4. Migrate only surviving gates: the 28 DELETE and 78 MERGE verdicts in `tierB-trace-gates.md` and the `tierA-gates-plan.md` deletions should land first or in the same diff, since a deleted gate is cheaper than a migrated one.

## 5. LG19 ratchet

**Definition (`cxx17_ratchet.py`).** Add `"LG19": ("raw getenv/GetEnvironmentVariable of an MC2_ gate", "gate::get<\"MC2_X\">() from Gates/gate.h")` to `IDIOMS` (line 49-68). Add the detector in `scan_text()` using the **`nc` line** (strings kept), as LG11 does:

```python
R_ENV = re.compile(r'(?<![\w.>])(?:std::|::)?(?:getenv|_wgetenv)\s*\(\s*(?:"MC2_|[A-Za-z_])'   # MC2_ literal or non-literal arg
                   r'|GetEnvironmentVariable[AW]?\s*\(\s*"MC2_|\b_dupenv_s\b|\bgetenv_s\b')
```

- `getenv("PATH")` and `getenv("USERPROFILE")` stay uncounted.
- The files under `Gates/` are exempted by a path rule like the existing `ALLOC_FILE` precedent. Add `"Gates"` to `DEFAULT_DIRS`.
- `editor/` (31), `gui/` (13) and `Viewer/` (0) stay opt-in. Track them through `--report --dirs editor gui Viewer` until G8.
- `test_cxx17_ratchet.py` gets cases for `std::getenv("MC2_X")`, `::getenv(name)`, `getenv("PATH")` not counted, commented-out and string-embedded text not counted, and one line with two calls counted once (hits are line sets).
- The first commit must run `--write-baseline` (or `--check --tighten`), otherwise the missing key reads as 0 and the check fails.

**Policy.** Classify LG19 as DRIVE-TO-ZERO (review section 5 policy: tool-verified, mechanical), with a "new file has 0" rule. It is not FREEZE.

**Baseline command** (works before the detector exists, comment-stripped like the ratchet):
`py -3 tools/cxx17_ratchet/cxx17_ratchet.py --report` after the change. Pre-change equivalent: `rg -n --pcre2 '(?<![\w.>])(?:std::|::)?getenv\s*\(\s*"MC2_' -g '*.{cpp,cc,h,hpp,inl}' code mclib GameOS RenderCore GuiRuntime RenderWorld GameAdapters HostServices EditorBridge | wc -l` (a little higher, since it keeps comments). The measured authoritative figure is **1388** lines (working tree; 1446 if non-literal `getenv(name)` helper lines are included).

## 6. Ordered slices (each is at most one build + smoke cycle)

Roadmap S2 = G1 to G4. G5 to G8 continue under S3/S4. Counts are against the 1388 baseline.

| Slice | Content | Exit criteria |
|---|---|---|
| **G1 infra + pilot** | `Gates/` module, `tools/gate_gen.py --import-legacy --write --check`, CMake lib, harness link updates, `tests/unit/test_gate.cpp` (the section 1 matrix), hooks in `gameosmain.cpp`, LG19 added with baseline. Probe first: a throwaway TU with 1400 `get<"...">` instantiations over a 1010-name index, measured with `/Bt+`, so NTTP cost is known before committing. **Pilot (terrain, about 30 sites in 7 files):** `gos_terrain_patch_stream.cpp` (13 presence-only sites), `terrain_admission_mode.h:12` (inverted presence), `terrain_surface_trace.h:42`, `mclib/terrain_runtime.cpp` (3), `gos_terrain_surface.cpp:73`, `gos_terrain_height_tex.cpp:51` (Int), `gos_terrain_lighting.cpp` (about 20, mixed Bool/Int/Float). Logging P2 for `gos_terrain_patch_stream.cpp` and `gos_terrain_lighting.cpp` is in the same diffs. | LG19 1388 to at most 1355. Unit tests green. `gate_gen --check` green. Smoke-lite plus tier1 5/5. `[CONFIG v1]` summary shows `unregistered=0`. `gos_terrain_patch_stream.obj` at most 1.1x pre-pilot. Golden event bytes unchanged. Codegen spot-check: one `get<>` site compiles to load, test, branch. |
| **G2 single truth** | Registry header rows and `flags.auto.yaml` v3 generated from the table; essay docs moved to `docs/gate-history.md`; `env_index.py`, `gate_status.py`, `build_review_pack.py` read the yaml instead of `registry_parse.py`; delete `registry_parse.py`, `flag-registry-auto-gen.py`, `check-flag-registry-truth.py`. | Rows deleted: `check-contracts.sh:787` registry_parser_selftest, `:807` and `:808` flag_registry_truth plus selftest, `:791` replaced by a gate_gen selftest. `:812` flag_registry_drift retargeted to `gate_gen --check`. No row reads the old text registry as a parser. |
| **G3 terrain family** | `gos_terrain_lod_chunk.cpp` (62), `gos_terrain_indirect.cpp` (41), `mclib/terrain.cpp` (43), `gos_terrain_water_stream.cpp` (15), `vulkan_terrain_island.cpp`, `gos_terrain_mask_dispatch.cpp`. Logging P2 in the same diffs. | LG19 down at least 120 more. LG12a down at least 150 in these files. Contract board at most 5 rows below start (this plus G2 satisfy S2's "rows down 5"). |
| **G4 VK gate helpers + GameOS local helpers** | `vk_shared_gbuffer_gate.h` `envTriState` family, the 10 `vulkan_*_island.cpp` gate functions, `engine_present_vk.cpp`, the GameOS local helpers in section 4.7. | LG19 down at least 100 more (cumulative at least 100 against the S2 target). `check-contracts.sh:1494` new_gates stays until G8. Both backends A/B per `vk-default-flip-checklist.md` before merge. |
| **G5 giant singles** | `gos_postprocess.cpp` (103), `gos_static_prop_batcher.cpp` (94), `gameos_graphics.cpp` (77), `gameosmain.cpp` (55): one file per cycle. | Each file reaches LG19=0 independently; file-level obj size within 1.05x. |
| **G6 mclib + passthrough** | `bdactor.cpp` (49), `txmmgr.cpp` (41), `cone_glow.cpp`, `camera.cpp`, `mech3d.cpp`, `assimp_importer.cpp`; `smokePassthrough` column generates the `run_smoke.py` allowlist. | Delete rows `:859` and `:860` (registered_gate_passthrough plus selftest). |
| **G7 code/ + adapters** | `code/mission.cpp` (36), `warrior.cpp` (23), `objmgr.cpp` (22), `missiongui.cpp`, `brain_special_dispatch.cpp` (the `s_*Gate` helpers), `logisticsdata.cpp`, `gamecam.cpp`; `GameAdapters`, `RenderWorld`, `HostServices`. | LG19 in owned dirs at or below about 120. |
| **G8 finish** | `editor/`, `gui/`, `Viewer/`, the tail; `tools/mc2_launcher/mc2_launcher.cpp` consumes generated data. | LG19 = 0 everywhere; flip to "new file must have 0"; delete rows `:745`, `:755` (env_registry plus selftest), `:828`, `:829` (gate_default_agreement plus selftest), `:1494` (new_gates); delete `check-env-registry.sh` (1174), `check-gate-default-agreement.py` (1559), `check-new-gates.py` (166), `check-registered-gate-passthrough.py` (272), `check-env-allowlist.sh` (146), plus the 850-entry `ALLOWLIST`. About 4,600 lines gone. |

Rows that stay: `smoke_env_inheritance` (`:848`), `capture_env_declared` and its ratchet (`:1038`, `:1040`), `env_drive_guard` and its ratchet (`:1056`, `:1058`). They check harness behavior, not the registry. `review_pack_registry_guard` (`:1386`) is re-pointed at the yaml. The comment-claim half of `check-gate-default-agreement.py` (in-source comments and docs asserting a default) remains useful only if comments next to gates still assert defaults. Decision for the owner: either trim it to a small comment-vs-table check or ban default claims in comments.

The dead-gate heuristics get simpler. `retireBy` plus zero literal `"MC2_X"` readers makes a gate deletable by declaration, so most of `dead_gate_scan.py` (481 lines, windowed idiom heuristics) shrinks to a table query.

## 7. Risks

1. **Silent behavior change.** EDGE and SEMANTICS verdicts alter results for `MC2_X=`, `false`, and `=0` on presence gates. The TSV plus the harness-value grep are the controls. Presence rows keep `rule=Presence`, so default behavior never changes without a named commit.
2. **Pre-main reads.** 59 file-scope gates can resolve before `main`. Behavior is unchanged, but the `premain` warning will surface latent launcher_env misses. Decide whether `MC2_NO_LAUNCHER` should apply the JSON before static init (a separate, behavior-changing fix).
3. **Link and harness churn.** Every exe or harness that compiles a migrated file needs `Gates/gate.cpp` and `gate_table.cpp`. A missed harness fails at link time, not compile time. `contract_harness_common` and `tools/cpp_probes` need an audit.
4. **Rebuild blast radius.** Adding or removing a gate regenerates `gate_index.gen.h` and rebuilds all includers. Changing a default or doc does not. This is the cost of the split and is accepted for rare adds.
5. **Two truths during G1.** Registry `defaultOn` and the table's default coexist until G2. A temporary cross-check row compares them (deleted in G2).
6. **Seed quality.** About 650 allowlisted names have guessed kind and default. Conflicts surface when a site migrates; a wrong default does not matter until then.
7. **NTTP compile cost.** 1400 template instantiations plus consteval searches. Mitigated by binary search, no `<format>` in `gate.h`, and the G1 `/Bt+` probe. If it fails the budget, switch to a pre-resolved index from the codemod.
8. **Facade coupling.** `gate` must never include `mc2log`. The dump uses `fwrite` to `stderr`, honoring the MC2_LOG NUL redirect order at `gameosmain.cpp:1180`.
9. **Writers.** `_putenv_s` at `gameosmain.cpp:1141`, `gos_dev_shell.cpp:296` and `bdactor.cpp:413-435` mutate the env after caching. `bdactor`'s gates need `reread`. The dev shell must call `gate::invalidate`.
10. **Parallel lanes.** `baseline.json` conflicts across worktrees (review section 5.4). Tighten LG19 only on main, only in the commit that pays it down.

## 8. Adversarial questions for the Opus reviewer

1. Is `gate::get<"X">()` really cheaper to the build than an `enum Id` once 1010 instantiations are counted? What does the G1 `/Bt+` probe show per TU?
2. Does the `gate_index.gen.h` split reintroduce the drift class we're deleting (two files, one fact)? Is the `static_assert` cross-check enough, or should the index be the only compile-time source and the table be the .cpp only?
3. Which of the 330 presence-only gates are driven to `=0` by a harness script today? If any is, the "no change" verdict is wrong. How does the harness-value grep handle values built inside Python string formatting?
4. The `Std` rule says "other non-empty means true". Is that right for default-ON gates, where `MC2_X=banana` stays ON, while the exact-"1" readers it replaces read it as OFF? Should `badvalue` be a hard error in smoke runs (`MC2_CONFIG_STRICT=1`)?
5. Does `invalidateAll()` after the launcher_env apply loop change any value that a pre-main static already captured, and does `premain_stale=0` hold on a clean `MC2_NO_LAUNCHER` run?
6. The ratchet counts lines. Can LG19 be gamed by wrapping `getenv` in a local macro or inline helper in a file the ratchet doesn't scan (`editor/`, `gui/`, `Viewer/`, `tools/`)? Should `Gates/gate.h` provide a `-D` poison (`#pragma deprecated(getenv)`) for owned dirs?
7. Which deleted rows lose real signal? In particular, does `gate_default_agreement` catch "comment says default OFF" drift that nothing else will?
8. Does moving 2 KB essay docs to `docs/gate-history.md` lose the `GATE-DEFAULT-TRUTH-1` style warnings that readers see next to the row today?
9. What does `dumpConfig` cost on startup (an `_environ` scan plus up to 1010 table parses in `all` mode), and is it correct under the editor, where `_putenv_s` runs at `EditorMFC.cpp:177`?
10. G1 mixes infrastructure and a pilot of about 30 sites. Is that one build+smoke cycle, or should infrastructure land alone with zero migrated sites first?

### Critical Files for Implementation
- A:/Games/mc2-main/tools/cxx17_ratchet/cxx17_ratchet.py
- A:/Games/mc2-main/RenderCore/RendererFeatureRegistry.h
- A:/Games/mc2-main/scripts/check-contracts.sh
- A:/Games/mc2-main/GameOS/gameos/gameosmain.cpp
- A:/Games/mc2-main/GameOS/gameos/vk_shared_gbuffer_gate.h