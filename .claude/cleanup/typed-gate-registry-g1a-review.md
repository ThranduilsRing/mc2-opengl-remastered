<!-- doc-status: stale | reviewed:2026-10-11 -->
# G1a typed gate registry — Opus adversarial review (2026-10-10, at 354c0071e; ACCEPT-WITH-FIX, G1b not blocked)

Condensed by the orchestrator from the reviewer's report. Line refs are the G1a versions of the files.

## Verdict
Hot reader API and parse rules are sound for the rows G1b touches (Bool legacy rules, Prefix numerics). One fix is urgent and outside the API: table kinds/tokens are unverified and `MC2_CONFIG_STRICT` turns them into exit 78 on real harness values. Two API changes before G3/G4: a typed `Token` return for Enum rows, and a working `reread` flag.

## Confirmed
- Hot path (`gate.h:129-133`): one relaxed load + bit test + `[[unlikely]]` out-of-line resolve; no function-local static; includes only `<atomic> <cstddef> <cstdint> <string_view>`.
- `Tri` is an enum class with no conversion; `if (get<TRI>())` does not compile; per-site `Rule` override honored (`gate.h:158-166`).
- First resolve is a benign race (same 64-bit value, relaxed store, no tearing on x64).
- Truthiness per input class matches design §2, `gate_idioms._RULE_BITS` and `test_gate` vectors for Std/Presence/Exact1/Lead1/Lead0Kill/Lead0/NonEmpty/Is01/Tri. Whole-string Int parse rejects `" 1"`, `"1 "`, `"+1"`, `"1.0"` (default + badvalue); Float `"1e999"` → default + badvalue; `range=Reject` really falls back to default.
- Table: constexpr, `static_assert` sorted/unique/`rowIsSane`/index cross-check; 1155 one-line rows; all 1124 call-passed names present.
- Compiler-sourced truth for the index: `gate_dump` emits the compiled table; contracts row runs `gate_gen --check --rebuild`. yaml/registry shim still regex-derived (G2).
- Hooks: `markMainEntered` `GameOS/gameos/gameosmain.cpp:1015`, `invalidateAll` `:1158` (after launcher_env loop), `dumpConfig` `:1194`; editor, ModPicker, 3 harness mains, dev shell done. `invalidateAll` snapshots pre-main slots before wiping.
- LG19 regex covers std::/::getenv, `_wgetenv`, `GetEnvironmentVariable[AW]`, `_dupenv_s`, `getenv_s`, `_environ`, `SDL_getenv`, non-literal `getenv(name)`; baseline 1391 honest (six `MC2_ABL_TRACE` stub reads deleted in 1dd468eed).
- STATIC lib; C++20 floor enforced by `check-cxx-standard-floor.py`.

## Findings
1. **HIGH — strict exits 78 on legitimate harness values.** ~20 rows mis-kinded: `MC2_POSTPROCESS_BACKEND` (reader `v[0]=='v'`, values gl/vk; `run_golden_parity.py:212`), `MC2_DIAG_TAGS` (comma list; `check-brain-opord.py:104`, `check-combat-soak.py:78`), `MC2_UNIT_DEFINITION_SOURCE`/`MC2_EQUIPMENT_DEFINITION_SOURCE` (legacy/generated selectors), path rows as Bool/Int: `MC2_VISUAL_DIFF_OUT`, `MC2_VULKAN_SPV_DIR`, `_VISUAL_TUNING_FILE`, `_TERRAIN_MATERIAL_LIB_FILE`, `_HZB_VIEW_FILE`, `_VISUAL_DIFF_MISSION`, `MC2_IMPORTER_DIR`. Fix: badvalue warn-only for unverified rows; correct kinds/tokens; `rowIsSane` asserts Enum default ∈ tokens.
2. **HIGH — link surface.** `mc2_gate` linked only into mc2/EditRel/standalone projects; `makefst`, `pak`, `aseconv`, `text_tool`, `mc2_asset_viewer`, `mc2fx`, `mc2fx_preview` link gameos/mclib without it and now fail once gameos uses `gate::`. Fix: `target_link_libraries(gameos|gameos_editor|mclib|mlr|gosfx|gui PUBLIC mc2_gate)`.
3. **MED — wrong defaults/ranges.** `MC2_UI_DEFS_IGNORE_PACKAGE_VERSION` default true (reader `gui/UiDefs.cpp:377` OFF when unset); six clamp readers marked Reject: `MC2_CEMENT_LIT`, `_PAD_AREA_GLOW`, `_PAD_AREA_GLOW_SPILL`, `_POST_CAS_SHARPNESS`, `_STATIC_PROP_IBL_SH_STRENGTH`, `_VIEWMODE_LOWLIGHT_GAIN`. 34 rows owner `unclassified`, 71 "default not derivable", 74 seed-unknown.
4. **MED (API) — Enum rows return raw `int32_t`** (`gate.h:151-152`): `if (get<"MC2_WATER_SHORE_MODE">())` compiles and means "not the first token". Return `enum class Token : int32_t` before G3.
5. **MED (API) — `reread` is inert**: set on 3 rows, nothing acts on it. Missed writer: `mclib/bdactor.cpp:403-425` `_putenv_s("MC2_GLTF_AXIS"/"MC2_GLTF_YAW_DEG")` with per-call readers in `assimp_importer.cpp:72,74` and no `invalidate`.
6. **MED — LG19 gameable** without a new wrapper: ~250 helper calls (`subGate`, `envFlag`, `envTriState`, `read_env_flag`, …) take new `"MC2_*"` literals uncounted; `#define` lines skipped; `tools/`, `tests/` unscanned. Fix: companion ratchet on `"MC2_` literals outside `get<>/state<>/isSet<>/str<>/is<>`.
7. **LOW — invalidate race**: `invalidate` during another thread's `resolve` can re-cache the stale value (dev shell vs render threads). Resolve via CAS from 0, or document.
8. **LOW — pre-main bit**: harness mains never call `markMainEntered`, so every read there is flagged pre-main; reads between `markMainEntered` and `invalidateAll` that a reader copies into its own static get no warning.
9. **Strict design**: "unregistered" is computed over `_environ`, so ambient harness names count; registering 16 Python-only names as gates (owner `harness`) is the wrong shape — the runner should drop ambient `MC2_*` names absent from the table, or harness-only rows get a non-readable kind. `dumpConfig` runs after the NUL redirect, so without `MC2_LOG` a strict exit 78 is silent.
10. Minor: 8 probes cannot distinguish `atof(v)!=0` from `AtoiNZ` or `strcmp=="1"||=="true"` from Exact1 — add probes `"0.5" "-1" "true" "on" "01"`; `count_overrides` misses `using enum Rule; get<"X", Presence>`; `EnvConfig::flag` delegates but `HostServices/` is compiled only into tests/unit.

## test_gate gaps (26 cases at G1a)
Int `"+1"`/overflow; Float Std `"1e999"`, Reject/Clamp, Prefix + range (G1b added the last two); Prefix leading whitespace/sign; Enum empty-token default and default-not-in-list; `reread`; invalidate/resolve race; harness pre-main bit; strict exit path; `atof!=0`; `subGate` umbrella inheritance (G4).

## Review amendments status
Fully implemented: 3, 7, 10, 11, 12. Partial: 1 (override API done; count ratcheted; no per-site retireBy), 2 (Reject + Enum exist; token lists wrong), 4 (STATIC, consumers not linked), 5 (`gate_default_agreement` rows kept; tier1 doc check unverified), 6 (index only; yaml/shim in G2), 8 (bdactor writer missed; `reread` inert; pre-main condition not implemented), 9 (kept in G1b diff).

## Fix order
Before the next smoke/golden run: findings 1 (warn-only + row fixes). Before more G1b files: finding 2. Before G3/G4: 4, 5, 6, 3, plus the test gaps.
