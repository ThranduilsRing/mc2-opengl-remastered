<!-- doc-status: current | reviewed:2026-10-11 -->
# G4 — VK umbrella as typed effective<> over Tri rows (design 2026-10-10 at ec57ac1a2; status: ready, runs after G3 on the terrain files)

Condensed by the orchestrator from the planner's report. Counts at HEAD ec57ac1a2.

## 0. Facts (corrections to the brief)
- 25 `Tri` rows (not 30); all read through `subGate`/`directChainGate`, so every Tri row is an umbrella member. Six `MC2_VK_*` rows are Bool opt-ins read as Exact1/Lead1 and do NOT inherit: `TERRAIN_LODFADE`, `TERRAIN_VISUALDISPLACE_FAR`, `POST_SSAO`, `POST_HZB`, `POST_SCREENFX`, `OBJECTID_DIRECT_CHAIN`.
- G3-deferred terrain sites: 9 `subGate` (`gos_terrain_lod_chunk.cpp:1644,1681,1723,1765,1807`; `vulkan_terrain_island.cpp:188,189,218,237`) + 4 `envTriState` (`lod_chunk:436,1904`; `vulkan_terrain_island:165,256`). The 9 consumer calls of `mc2_vk_terrain_*_enabled`/`mc2_vk_overlay_enabled` contain no env read → no edit.
- Helper census: 38 `subGate` (1 in a comment), 8 `directChainGate`, 17 `envTriState` = 62 code sites + `umbrellaOn()` in `mclib/appear.cpp:67`. These are LG20 material (literals via helpers), not LG19.

## 1. The algebra as it exists (`vk_shared_gbuffer_gate.h`)
- `envTriState(n)`: exact "1"→1, "0"→0, else −1 (live `getenv` per call).
- `umbrellaOn() = static s_on(envTriState("MC2_VK_SHARED_GBUFFER")==1) && !g_mc2VkSessionForcedGL` (env cached in a function-local static; fail-soft read per call).
- `subGate(n)` ≡ `directChainGate(n)`: explicit 1→true, explicit 0→false, else `umbrellaOn()`.

| sub \ umbrella | U | 0 | 1 |
|---|---|---|---|
| U | 0 | 0 | 1, but 0 if F |
| 0 | 0 | 0 | 0 |
| 1 | 1 | 1 | 1 even if F |

F = `g_mc2VkSessionForcedGL`, set by `mc2::forceGLFallback` (`render_backend_select.cpp:65`) from `gameosmain.cpp:1647,1689,2575,2637` — the last two can fire mid-session. Fail-soft vetoes only the inherit leg (explicit "1" survives; documented debug knob) — the typed form must keep this.
- Derived: `terrainNativeFirstOwnerOn = subGate(TERRAIN_TIER1) && subGate(SHARED_SCENE_DEPTH)`; `postDirectChainUpstreamOn = terrainNativeFirstOwnerOn && dc(PROP_DC) && subGate(WATER_TIER1) && dc(WATER_DC)`; "tier1 && shared depth" is hand-copied 11× (`terrainTier1Requested`, `propColorTier1Requested`, `mechGateRequested`, legacy-shape `gateRequested`, `s_vkMechGateOn`, three `gos_postprocess.cpp` helpers, the `engine_present_vk.cpp:292` lambda, `terrainNativeFirstOwnerOn`); `compositeDirectChainGateEnabled` (`engine_present_vk.cpp:292`) and `mc2VkSharedSceneDirectResourceEnabled` (`GameOS/gameos/gos_postprocess_gates.cpp:91`) compute the same predicate.

## 2. Typed form
**Table-driven parent (recommended):** row option `inherits("MC2_VK_SHARED_GBUFFER")` on the 25 Tri rows; `rowIsSane`: `kind==Tri ⇔ !inherits.empty()`; constexpr check in `gate_table.cpp`: parent exists, is Bool `Rule::Exact1`, has no parent itself (one level). `IndexEntry` gains `std::string_view parent{}` (defaulted; 25 generated lines change); `gate_gen`/`gate_table_gen`/`gate_table.json` carry it; `dumpConfig` may print `inherits=`. Why not call-site parent: a new VK sub-gate could silently ignore the umbrella; census/runner/docs would have to grep.

`Gates/gate.h`:
```cpp
template <Name n> [[nodiscard]] inline bool effective(bool parentSuppressed) noexcept;
//  state<n>()==On -> true; ==Off -> false; Unset -> !parentSuppressed && (load(parentIdx) & kRowBit)
template <Name n> consteval bool hasParent() noexcept;
```
`effective` takes the fail-soft as a bool with NO default: `Gates/` is a static lib that must not see `g_mc2VkSessionForcedGL` (lives in `mclib/tgl.cpp`); the slot caches env state only; cost = two relaxed loads + branch (today: a CRT `getenv` per call, 380–450 env scans per frame per HUD-GATE-HOIST-1). `state<n>()`, `isSet<n>()`, `get<>`→`Tri` already exist.

**Adapter** (`vk_shared_gbuffer_gate.h` → ~40 lines; keep `mc2vkgate`, the `extern bool g_mc2VkSessionForcedGL`, the disarm prose):
```cpp
template <gate::Name n> inline bool armed()           { return gate::effective<n>(g_mc2VkSessionForcedGL); }
inline bool umbrellaOn()                              { return gate::get<"MC2_VK_SHARED_GBUFFER">() && !g_mc2VkSessionForcedGL; }
template <gate::Name n> inline bool regionTier1Armed(){ return armed<n>() && armed<"MC2_VK_SHARED_SCENE_DEPTH">(); }
inline bool terrainNativeFirstOwnerOn()               { return regionTier1Armed<"MC2_VK_TERRAIN_TIER1">(); }
inline bool postDirectChainUpstreamOn()               { /* terrainNativeFirstOwnerOn && armed<PROP_DC> && armed<WATER_TIER1> && armed<WATER_DC> */ }
inline bool compositeDirectChainOn()                  { return postDirectChainUpstreamOn() && armed<"MC2_VK_POST_COMPOSITE_DIRECT_CHAIN">(); }
```
A check row bans direct `gate::effective<"MC2_VK_` outside this header (so the veto is never forgotten). Callers keep their caching shape.

## 3. Migration mechanics (`tools/gate_codemod.py --family g4`)
| Old | New | Verdict |
|---|---|---|
| `subGate("N")`, `directChainGate("N")` (46) | `mc2vkgate::armed<"N">()` | NO-CHANGE |
| `envTriState("N")==1` (7; rows Exact1) | `gate::get<"N">()` | NO-CHANGE |
| `envTriState("N")==0 ? 0 : 1` (8: fence-defer ×4, LIGHTFEED_DEDUP, POST_SCREENFX_{CLOUD,SHORELINE,SHADOW}) | `gate::get<"N">()` | EDGE ("00"/"0x" only) — decided: EDGE |
| `envTriState("N")!=-1` (2; Is01) | `gate::get<"N">()` | NO-CHANGE |

Hand edits: the 11 tier1&&depth copies → `regionTier1Armed<…>()`; `terrainTier1Requested()` becomes the cached wrapper over `terrainNativeFirstOwnerOn()`; `engine_present_vk.cpp:292` + `GameOS/gameos/gos_postprocess_gates.cpp:101` → `compositeDirectChainOn()`; `gos_mech_batcher.cpp:151` `s_vkMechGateOn` (namespace-scope initializer = pre-main read) → lazy local-static function, likewise the `:178` deprecation lambda (the one deliberate shape change; see risk 2); `lod_chunk:1723/1765` `csmSubsetOn` also reads `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL` raw (G3 Lead1 site). Composite predicates stay hand-written in the adapter; per-site preconditions (ALL_LODS requires Tier1, SKIRTS requires ALL_LODS, DETAIL/DYNSHADOW require lightingOn) stay at the site. Delete `envTriState`, `subGate`, `directChainGate`, the umbrella static, ~130 lines of prose (→ `docs/tier1_env_vars.md`).

## 4. Proof plan
1. Differential `test_gate`: keep the verbatim old helper bodies as a test-local oracle; for every `inherits` row: sub env {unset,"","0","1","yes","10","true"} × umbrella env (same) × F {false,true}; assert `effective<n>(F) == oracle`; pin Tri⇔inherits, parent Bool Exact1, Tri-without-parent = compile error; probe vectors for the 17 `envTriState` replacements.
2. `gate_gen --check --rebuild`, `cxx17_ratchet --check`, `gate_conflicts --check`, contracts, doctest.
3. Strict no-interop runner (`run_vk_nointerop_strict_visible.py --minimal-promotion --resource-strict --headless`) pre vs post; generalize `--minimal-promotion` to drop every `inherits` row from `STRICT_ENV` (proves the umbrella alone arms the family); `check_vk_nointerop_log.py` PASS incl. B5 required HEALTH tags; diff `[VK_*] [HEALTH]` lines with timing masked.
4. Fail-soft at process level: `MC2_VK_SHARED_GBUFFER=1 MC2_RENDER_BACKEND=vulkan MC2_VK_FORCE_FAIL=1` → `[RENDER_BACKEND] fail-soft`, no VK island HEALTH, capture identical to plain GL; smoke assertion: no `[CONFIG v1] warn=premain name=MC2_VK_`.
5. GL+VK day/night captures per `vk-default-flip-checklist.md`.

## 5. Census, ledger, docs
- `tools/gate_conflicts.py`: add reader class `Typed(effective)`; hard rule: any reader of an `inherits` row other than `effective`/`state`/`armed` is an unresolved conflict. Ledger `intentional-optin` for the six opt-ins plus `SHADOW_INTEROP`, `CSM_ARRAY_INTEROP`, `CSM_FULLMAP_INTEROP` (documented parity hazard).
- Doc drift: `docs/tier1_env_vars.md:123` lists LODFADE as umbrella member (opt-in); `:129` "the 10 direct-chain gates" (there are 3).
- 61 non-Tri `MC2_VK_*` rows remain in the census (Exact1 11, Lead1 17, NonEmpty 1, Is01 2, Lead0 3, Lead0Kill 9, Presence 10, Std 5, Int 2, String 1) — none umbrella members. Later gate-deletion candidates (no-ops): `PRESENT_NO_GL_IMPORT`, `PRESENT_SKIP_GL_HANDOFF`, `MECH_FEED_NATIVE`, `STATIC_SHADOW_MAP`; maybe `PROP_DIRECT_CHAIN`/`WATER_DIRECT_CHAIN` (only composite terms).

## 6. Risks
1. Fail-soft vs caching: 17 per-call readers (`engine_present_vk.cpp:250,262,277,285`; `GameOS/gameos/gos_postprocess_gates.cpp:42`; `vulkan_vfx_island.cpp:434`; `vulkan_water_island.cpp:141`; `gameos_graphics.cpp:7591,8068,8079,8420`; `code/gamecam.cpp:732,843,967`; `code/objmgr.cpp:2192`; `code/weaponbolt.cpp:71`; `mclib/appear.cpp:67`) must never gain a cache (check row). Cached region readers keep first-call values (existing DISARM-2 design).
2. Pre-main: `gos_mech_batcher.cpp:151,178` read gates at static init; after G4 `invalidateAll()` re-resolves the umbrella after launcher_env apply but a pre-main `s_vkMechGateOn` stays stale → make it lazy (intended delta on the dev-only `MC2_NO_LAUNCHER`+launcher_env path).
3. `postDirectChainUpstreamOn` composition after B5: table test umbrella-on, umbrella-on+PROP_DC=0, umbrella-off+all-forced-1.
4. Live env: `subGate` read env per call; `effective<>` caches first read — safe: no C++ `putenv` of any `MC2_VK_*` (grep at HEAD); no `reread` needed.
5. TU cost: `gamecam.cpp`, `objmgr.cpp`, `weaponbolt.cpp`, `mclib/appear.cpp` would pull `Gates/gate.h` (1155-entry index): measure obj vs 1.05× and `/d1reportTime`.
6. Overlap with G3: G4.3 after G3 lands on `gos_terrain_lod_chunk.cpp`/`vulkan_terrain_island.cpp`.
7. LG19 honesty: umbrella work moves LG20 (−62), not LG19; VK TUs hold 54 raw `getenv("MC2_` + `gos_mech_batcher.cpp` 44 more → G4.5.

## 7. Slices
| Slice | Content | Exit |
|---|---|---|
| G4.0 infra | `inherits`, `effective`, `hasParent`; 25 rows; differential tests | doctest green; `gate_gen --check --rebuild`; default smoke byte-identical |
| G4.1 per-call readers | adapter beside old helpers; the 17 per-call sites | strict runner PASS; `MC2_VK_FORCE_FAIL=1` GL-identical; VK day/night montage unchanged |
| G4.2 cached region readers | island gate fns (prop, mech, legacy shape, sky, post natives), `gos_postprocess` statics, `gameosmain.cpp:84`, `engine_present_vk.cpp:292`; `regionTier1Armed`; `compositeDirectChainOn`; mech-batcher lazy gate | strict runner PASS + HEALTH diff; no `warn=premain name=MC2_VK_`; GL+VK captures unchanged |
| G4.3 terrain 13 + wrappers (after G3) | 9 `subGate` + 4 `envTriState`; `terrainTier1Requested` family | strict runner PASS; night terrain capture unchanged; LG20 −13 |
| G4.4 remaining `envTriState` (13) | fence-defer ×4, LIGHTFEED_DEDUP, screenfx ×4, ssao, hzb, MECH_FEED_AUDIT/NATIVE, STATIC_SHADOW_MAP | probe tests green; screenfx HEALTH unchanged; LG20 −13 |
| G4.5 delete + census + docs + local raw reads | delete helpers; shrink header; `Typed(effective)` census rule + ledger; generalize `--minimal-promotion`; tier1_env_vars drift; planner `--family g4` for the 54 raw VK getenv + local helpers | LG20 ≥ −62; census 0 unresolved; full tier1; GL+VK day/night A/B |

## Orchestrator decisions
1. The six opt-in `MC2_VK_*` rows stay explicit-only with `intentional-optin` ledger entries; the umbrella ruling covers DIRECT_CHAIN/VFX/Tri rows only.
2. The `MC2_NO_LAUNCHER` + launcher_env.json ordering change (umbrella honored after apply; mech batcher gate lazy) is accepted as an intended delta — the parked pre-main bug partially fixed; verify with the no-`warn=premain` assertion.
3. The 8 fence-defer `==0 ? 0:1` sites take the EDGE `get<>()` rewrite, files named in the commit.
4. G4.3 runs only after G3 lands on `gos_terrain_lod_chunk.cpp` and `vulkan_terrain_island.cpp`.
5. The five G0-ledger level-valued gates (`FX_FORCE_SPAWN`, `SHADOW_DYNAMIC_PROP_CASTERS`, `SPOTLIGHT_DIRECTION_FIX`, `STATIC_PROP_BUILDING_SHADOW`, `_REBAKE`) are Bool rows without tokens → Enum rows with level tokens before any migration; the codemod hard-fails on ledger `enum`/`divergent`.
