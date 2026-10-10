# G4 VK umbrella — Opus adversarial review (2026-10-10, bf46620a7..eaa6add53; ACCEPT-WITH-FIX, guards only)

Condensed by the orchestrator. No runtime behavior change beyond the documented deltas (lazy mech gate + launcher_env umbrella honored after apply; 9 Lead0Kill EDGE sites; trivial `""` edges).

## Confirmed
- Algebra: `effective<n>(F)` → true on `fExact1`, false on `fExact0`, else `!F && (load(parent) & kRowBit)`; `classify` sets exact flags only for "1"/"0" (`gate.cpp:90-91`) like the old `envTriState`; umbrella row `L(Exact1, Bool)`; F is a parameter, never in the slot; `umbrellaOn()` = `get<SHARED_GBUFFER>() && !F`; `parentsAreValid` + `rowIsSane` static_asserted; differential doctest 25 rows × 7 × 7 × F.
- Per-call readers: all 17 sites still per-call; no caller caches the wrappers.
- Cached region readers: all 11 tier1&&depth copies map to the right `regionTier1Armed<>` arg (TERRAIN_TIER1, PROP_COLOR_TIER1, MECH_TIER1 ×3, gos_postprocess helpers); `compositeDirectChainOn()` = the same 5-term AND as both old predicates; caching shapes kept.
- `vkMechGateOn()` lazy: all readers runtime in `gos_mech_batcher.cpp`; the delta is process-wide (the old static initializer latched the umbrella pre-main whenever MECH_TIER1 was unset, so launcher_env's umbrella was ignored everywhere; now honored) — matches decision 2.
- G4.4 kill switches: 8 Lead0Kill rows, only "00"/"0x"/"0.5" differ; no harness sets them.
- `--minimal-promotion`: still sets the umbrella (Bool row), drops the 13 members from STRICT_ENV and the inherited shell env; checker unchanged.
- Level gates → Enum: tokens `|0..9|loop` (FX_FORCE_SPAWN) and `|0|1|2`; nifty readers still raw (no runtime change); lane 2's `is<>` reads consistent.
- Codemod g4: all 18 hand sites + 12 tool sites match rows (VFX_TRACE Lead0; OBJECTID_DC/SKIP_GL_HANDOFF/NO_GL_IMPORT Lead1; GL_CANARY Std→Lead1 matching the reader; DUMP_* String via live `str<>`; DUMP_FRAME `isNonEmpty` + Prefix Int). `vk_umbrella_light.h` declares three functions defined once in `mclib/vk_umbrella_gate.cpp`; no ODR issue.
- Deleted helpers: no callers anywhere; `tier1_env_vars.md` drift fixed.

## Findings (routed to lane 1)
- **A (med, guard):** `check-vk-umbrella-readers.py:67` `is_static_cached` catches only `static T x = armed<>()` in one statement; misses `static int state=-1; if(state==-1) state = armed<>();` (every G4.2 region gate), namespace-scope caches, member caches; no selftest.
- **B (low):** `PER_CALL` (:33) omits `gos_terrain_lod_chunk.cpp:1576/1613/1655/1696/1738`, `mclib/appear.cpp:67`, the bodies of `vk_umbrella_gate.cpp`; R1 does not ban direct `gate::get<"MC2_VK_SHARED_GBUFFER">()` outside the adapter.
- **C (med):** `config_premain_vk` near-vacuous: `dumpConfigWith` (`gate.cpp:474-484`) emits `warn=premain` only when the pre-main value differs from the current env → default smoke never trips; emit a `premain_read` line for any snapshotted `kPreMainBit` VK row.
- **D (low, dev-only):** umbrella is now a re-resolvable slot: dev-shell `set_gate MC2_VK_SHARED_GBUFFER` mid-session flips per-call readers live while cached region gates stay (torn state the old latched static could not reach) — document.
- Nits: stale `subGate`/`envTriState` comments (`gameosmain.cpp:12`, `vk_shared_light_feed.cpp:10`, `gos_postprocess.h:521/539/559`, four `vulkan_post_*_native.cpp` headers, `vulkan_terrain_island.cpp:207/226/239`, `weaponbolt_render.cpp:15`, `check-env-registry.sh:1013-1031`, `MC2_VK_POST_FOG_NATIVE` row doc); `#include "Gates/…"` above the banner in four island files; `umbrella_members()` runs at import in the strict runner.
