# Track C / Track F — Pre-Flight Checklist + Risk Map + What's NOT in v1

**Date:** 2026-04-30
**Mode:** Cross-cutting consolidation. No new design; synthesizes the implementation-readiness audit, the boundaries deep-dive, the modders-paradise roadmap §9, and the Track F design into a single page a future implementer reads **before** writing C-1's first line of code.
**Primary inputs:**
- [`specs/2026-04-30-track-c-implementation-readiness-audit.md`](2026-04-30-track-c-implementation-readiness-audit.md) (master audit)
- [`specs/2026-04-29-modders-paradise-roadmap-design.md`](2026-04-29-modders-paradise-roadmap-design.md) §9
- [`specs/2026-04-30-track-f-scalable-hierarchical-ai-design.md`](2026-04-30-track-f-scalable-hierarchical-ai-design.md)
- [`explorations/2026-04-30-track-c-mod-boundaries-deep-dive.md`](../explorations/2026-04-30-track-c-mod-boundaries-deep-dive.md) §1 (NO list)

If any item below cannot be confirmed, STOP. Do not begin coding C-1 until §1 passes and §4 is resolved.

---

## §1 — Pre-flight checklist

The literal sequence a future implementer runs before C-1's first commit. Format: checkbox + command/check + expected outcome + what-to-do-if-fails. Items marked `[x]` are already verified.

### Source-of-truth reads (do FIRST, before tooling work)

- [ ] **Read [`blocking-questions-resolution.md`](../explorations/2026-04-30-track-c-blocking-questions-resolution.md) §Q1 in full.** This is authoritative for the `*_impl` extraction pattern. The competing trampolines in [`lua-trampolines-and-tests.md`](../explorations/2026-04-30-track-c-lua-trampolines-and-tests.md) §1–§2 and [`lua-implementation-shape.md`](../explorations/2026-04-30-track-c-lua-implementation-shape.md) §5 are SUPERSEDED. **Failure mode if skipped:** ABL stack reentrancy corruption; the crash class that wasted two design rounds. (Audit "Single source of truth registry" line 1.)
- [ ] **Read [`mod-boundaries-deep-dive.md`](../explorations/2026-04-30-track-c-mod-boundaries-deep-dive.md) §1 (12 NO categories).** Internalize what the sandbox does NOT expose so you don't accidentally add a binding that opens one of these.
- [ ] **Read [`mclib/ablxstd.cpp:84-92`](../../../../../mclib/ablxstd.cpp).** This is the original push/call/pop pattern that the `*_impl` extraction is replacing. Internalize why the original couples to ABL stack state.
- [ ] **Read the audit's Day-1 recipe (steps 1–10) end to end.** It is the literal first ten file changes.

### Engine-state verifications

- [x] **`MAX_STANDARD_FUNCTIONS == 512` in `mclib/ablsymt.h`.** Required by `mc2luadispatch_*` (component j adds 6 ABL primitives). Verified 2026-04-30 per `memory/carver5_mission_playable.md`. **If fails:** bump 256→512, run tier-1 smoke before proceeding.
- [ ] **Pre-baseline tier-1 smoke. Record FPS per mission.**
  ```bash
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --with-menu-canary
  ```
  Expected: all 5 missions pass + menu canary pass; record per-mission FPS into `tests/smoke/baselines/pre-c1-<sha>.json`. **If fails:** the engine is broken before we touched it; fix that first.
- [ ] **Confirm `nifty-mendeleev` HEAD has the carver5 stack-bump.** `grep -n MAX_STANDARD_FUNCTIONS mclib/ablsymt.h` should show `512`. (See audit Important Q3.)
- [ ] **Confirm savegame chunk insertion site is identifiable.** Grep `code/saveload.cpp` for the per-subsystem `save…State()` block; the LUA1 chunk lands adjacent. Required before C-5 component (g) lands. **If unclear:** open a separate exploration before C-5; do NOT block C-1.
- [ ] **Confirm `Mission::update()` end-of-frame slot.** Audit component (d) says `Tick()` runs end-of-update + before render-submit. Grep `code/mission.cpp` for `Mission::update`; identify the line AFTER `objMgr->update()` returns and BEFORE the render-submit call. **If wrong site picked:** the ABL-reentrancy deferral guarantee silently breaks (see §6 failure mode F2).
- [ ] **Confirm Track F's `brainsEnabled[]` access point.** Track F's "engine framework" gate (F-1) needs to short-circuit the per-warrior `brain->execute()` site at `code/warrior.cpp:2155` when a Lua brain is registered for the team. Grep that file for the dispatch + the team-level lookup. (Track F design §1, §13.)

### Vendoring / build verifications

- [ ] **Confirm `nlohmann/json` upstream URL works** and the SHA256 matches the audit's intended vendoring drop. `mod-profile-launcher` worktree already has it; reuse rather than redrop.
- [ ] **Confirm Sol2 single-file amalgamation downloads.** `single/single.py` upstream produces `sol.hpp` + `forward.hpp`. Cache the commit hash for `THIRD_PARTY_LICENSES.md` provenance.
- [ ] **Confirm Sol2 compiles cleanly with our MSVC RelWithDebInfo flags.** Build a 10-line `hello-sol2.cpp` TU outside the engine first:
  ```cpp
  #define SOL_ALL_SAFETIES_ON 1
  #include <sol/sol.hpp>
  int main() { sol::state lua; lua.open_libraries(sol::lib::base); return 0; }
  ```
  Compile with `/std:c++17 /wd4334 /wd4146 /wd4244 /wd4267 /wd4310 /wd4324`. **If fails:** record the specific warning(s) and either (a) extend the suppression set, or (b) pin to an older Sol2 commit. Do this BEFORE step 4 of the audit's Day-1 recipe.
- [ ] **Confirm Lua 5.4.7 `.c` file count == 32 after dropping `lua.c` / `luac.c` / `onelua.c`.** `ls 3rdparty/lua/*.c | wc -l` → 32. **If fails:** redo the drop (provenance issue).

### Per-binding extraction targets (component b)

- [ ] **Each of the M0 ten `*_impl` extractions has a clear target line range in `code/ablmc2.cpp`.** Draft the line ranges into a tracking note before extraction:
  - `mc2_get_time_impl`, `mc2_object_status_impl`, `mc2_object_exists_impl`, `mc2_set_timer_impl`, `mc2_check_timer_impl`, `mc2_set_objective_status_impl`, `mc2_check_objective_status_impl`, `mc2_play_sound_impl`, `mc2_set_global_impl`, `mc2_get_global_impl`.
  - **Verify each body is side-effect-free against `CurWarrior` / `CurFSM` globals.** If any closes over them, pass as `*_impl` parameter rather than reading the global. (Audit component b pitfall.)

### Tooling sanity

- [ ] **`tools/lua_api_doc_gen/` Python entrypoint stubbed.** Even before C-3, drop a `main.py` that emits an empty JSON. Lets CI plumbing land in C-1.
- [ ] **Pre-commit hook for magic-FSM contamination.** Install:
  ```bash
  git grep -E '^void execMagicAttack|^void execCoreGuard|^void execCorePatrol|^void execCoreWait' code/ablmc2.cpp
  ```
  in `.git/hooks/pre-commit`; nonzero exit on match. (Audit free-win 22; `blocking-questions.md` §Open 6.)

### Verification of "I can begin"

- [ ] **All boxes above are checked OR explicitly waived with cited rationale.**
- [ ] **§4 pending decisions are resolved or explicitly deferred to post-C-3.**

When the above is green, C-1 may begin.

---

## §2 — Risk map

Severity-ordered. Each entry: severity / likelihood / what / mitigation / who owns mitigation / source-doc cite.

### Critical

1. **ABL stack reentrancy if implementer follows superseded trampolines doc.** Critical / Likely. The original trampolines (`lua-trampolines-and-tests.md` §1–§2) push to / pop from the ABL stack inside the Sol2 lambda. This corrupts ABL state when Lua re-enters mid-frame. *Mitigation:* the `*_impl` pattern in `blocking-questions-resolution.md` §Q1 + warning banners at the top of the superseded docs. Pre-flight item: read Q1 first. *Owner:* C-3 implementer.
2. **Magic-ABL contamination via shadow-name re-introduction.** Critical / Possible. Shipping a corebrain.abx with `execMagicAttack` / `execCoreGuard` / `execCorePatrol` / `execCoreWait` redefined causes the v0.2 hotfix hang (`memory/magic_abl_contamination_rule.md`). *Mitigation:* pre-commit hook (pre-flight item) + `blocking-questions.md` §Open 6 policy. *Owner:* every committer to `code/ablmc2.cpp`.
3. **VM teardown order: closing ABL before Lua corrupts Lua state.** Critical / Likely if site copy-paste-error. Final pcalls during teardown reference ABL state. *Mitigation:* lifecycle §4 explicit ordering; pre-flight item identifying the 5 + 1 line edits. *Owner:* C-4 implementer.

### High

4. **`Tick()` site selection breaks ABL reentrancy deferral.** High / Possible. Picking a `Tick()` site that fires *while* an ABL frame is in flight silently re-enables reentrancy. *Mitigation:* pre-flight item; specific file + position called out (end of `Mission::update()`, after `objMgr->update()`, before render submit). *Owner:* C-4 implementer; reviewer must verify.
5. **`*_impl` extraction body secretly mutates `CurWarrior` / `CurFSM`.** High / Possible. The 10 M0 picks were chosen for triviality, but a body that LOOKS pure may close over these globals. *Mitigation:* per-binding regression smoke (call from ABL test mission, compare result). Audit component (b) pitfall. *Owner:* C-3 implementer + reviewer.
6. **Lua VM memory leak across mission load/unload.** High / Possible. Pimpl `Impl* pimpl_` deletion in `Shutdown()` must happen at all 5 + 1 teardown sites. *Mitigation:* lifecycle §4 file:line citations; valgrind / leak-tracker run after C-4. *Owner:* C-4.
7. **Bytecode-via-`load` sandbox escape.** High / Possible. A mod ships a `.lua` file whose first byte is `0x1B` (Lua bytecode marker). *Mitigation:* belt-and-braces — strip `load`/`loadstring`/`loadfile` from sandbox AND reject any chunk whose first byte is `0x1B` before `luaL_loadbuffer`. (`sandbox-and-errors.md`; `mod-boundaries.md` §1.3.) *Owner:* C-2.
8. **Symlink / junction escape on Windows for `mc2.io.read`.** High / Possible. A mod ships a junction pointing at `C:\Windows\` and reads it. *Mitigation:* realpath + prefix check; reject NUL, drive letters, UNC, `..`. (`mod-boundaries.md` §1.5.) *Owner:* C-2 (sandbox author).
9. **`MAX_STANDARD_FUNCTIONS` overflow when `mc2luadispatch_*` lands.** High / Unlikely (verified). 6 new primitives in a 512-cap. *Mitigation:* pre-flight verification. *Owner:* C-3 implementer.

### Medium

10. **First mod ships with hot-reload off; modder confused.** Medium / Likely. Hot-reload is dev-only (`MC2_LUA_DEV=1`). New modder doesn't know the env var. *Mitigation:* `tools/new-mod` scaffolder drops `meta/.luarc.json` AND `.vscode/settings.json` enabling hot-reload by default. (`modder-tooling-deep-dive.md` §6.) *Owner:* C-5 / tooling.
11. **Sol2 4.x panic-handler API drift breaks our error catching.** Medium / Unlikely. *Mitigation:* pin Sol2 to a specific commit; SHA256 in `THIRD_PARTY_LICENSES.md`. (`build-integration-deep-dive.md` §10.) *Owner:* C-1.
12. **Mod-set drift on save load: silent persist drop.** Medium / Likely. Save written with `[A,B,C]`, loaded with `[A,B,D]`; C's persist drops. *Mitigation:* warn-only in M0; refuse-by-default with `--force` is M1. (`lua-loading-lifecycle.md` §9.2.) *Owner:* C-5 / M1.
13. **Save bloat from misbehaving `mc2.persist`.** Medium / Possible. *Mitigation:* per-mod persist size cap (M1). Document the M0 unboundedness. *Owner:* M1.
14. **Track F engine framework breaks stock missions.** Medium / Possible. F-1 adds the `brainsEnabled[]` gate; if the gate logic is wrong, stock teams stop ticking. *Mitigation:* gate defaults to `false`; absence == legacy ABL path. (Track F §1 hard constraints.) *Owner:* F-1.
15. **Track F tick-rate misread (30 Hz vs 0.44 Hz).** Medium / Likely. The §2 / §11 numbers in the Track F design were originally 30 Hz; resolved to 0.44 Hz native + 1–2 Hz target. *Mitigation:* tick-rate resolution banner at top of doc; explicit mention in F-1 implementation. *Owner:* F-1.
16. **Stale shader cache mimics shader regression after C-3.** Medium / Possible. Adding new TUs to `modding/` + relinking sometimes leaves stale `.bin` shader cache. *Mitigation:* `memory/stale_shader_cache_symptom.md` recipe (clear cache, retest). *Owner:* anyone seeing visual regression.
17. **REPL exposed in shipped builds.** Medium / Unlikely. `MC2_LUA_REPL=1` env var leaking. *Mitigation:* three-pronged auth gate (env + CLI + dev flag); UNDOCUMENTED for shipping users. (`modder-dx-deep-dive.md` §1.) *Owner:* C-2 / M / DX.
18. **Module reload graph too coarse / too fine.** Medium / Possible. M0: any change under `mods/X/` re-runs X's control.lua. Could thrash. *Mitigation:* M0 ships coarse; refine post-feedback. (`lua-loading-lifecycle.md` §9.6.) *Owner:* M1.

### Low

19. **`pairs()` table iteration nondeterminism breaks future MP.** Low / Likely (impact deferred). MP isn't on M0 roadmap. *Mitigation:* document in modder-facing docs that `pairs()` is non-deterministic; recommend `ipairs` over arrays for AI logic. (Track F §11.) *Owner:* documentation.
20. **Sol2 `sol::lib::debug` is all-or-nothing.** Low / Likely. Opening it loads everything we don't want. *Mitigation:* never call `state.open_libraries(sol::lib::debug)`; expose `debug.traceback` as plain function. (`sandbox-and-errors.md` §1.) *Owner:* C-2.
21. **Variadic ABL signatures (`*` / `?`) blocked from STABLE M0.** Low / Likely. None of the M0 ten use them. *Mitigation:* documented; promotion requires shape decision. (`api-surface-catalog.md`; `blocking-questions.md` §Open 1.) *Owner:* M1.
22. **String interning for dispatch keys (~100ns lookup).** Low / Unlikely-impact. Profile first. (`abl-to-lua-reverse-direction.md` §11.) *Owner:* M1.
23. **Cross-mod handler priority race.** Low / Possible. Two mods register for same event; stable registration order works until they fight. *Mitigation:* add `priority` arg in M1 if observed. (`abl-to-lua-reverse-direction.md` §11 Q8.) *Owner:* M1.

---

## §3 — What's NOT in v1 (deferred list, consolidated)

Cross-doc consolidation. Anything below is NOT shipping in M0. Each entry: what / why deferred / what unlocks it.

### From roadmap §9 explicit rejections (lines 337–345)

| Item | Status | Unlocks |
|------|--------|---------|
| **EnTT / full ECS migration** | Rejected | Cull-gate chain fused to GameObject; multi-month engine rewrite breaking saves. ECS-shape *for new subsystems only* is the live policy. |
| **SDL3 migration** | Rejected | Zero modder benefit. Revisit only if window/input layer breaks. |
| **Vulkan / MoltenVK** | Rejected | Pure aspiration; nothing about current bottleneck is API-level. Revisit only if AMD driver wall forces it. |
| **PhysFS** | Rejected | Existing loose-file-overrides-FST works. |
| **spdlog** | Rejected | `[SUBSYS v1]` pattern works. |
| **Taskflow / job system** | Deferred | Single-thread CPU pressure is being deleted by GPU thinning. Revisit when AI brain measurably becomes the wall (post Phase-C, 200+ units). |
| **Wasm component model** | Deferred | Genuine 2026+ north-star, not yet ergonomic. Revisit ~2 years. |
| **KTX2 / Basis Universal** | Deferred | Real win when VRAM matters; today's bottleneck is CPU. Revisit when modders ship 8K mech textures. |
| **Total ABL replacement** | Rejected permanently | Stock-install-playable rule; `.abx` parser stays forever. |

### From boundaries deep-dive §1 (12 NO categories)

All 12 are absent in v1 — most enforce themselves by absence (binding simply not in namespace tree):

- **§1.1 Networking** (sockets, HTTP, IPC). Escape hatch for v2+: `mc2.net.fetch(url, callback)` allowlist.
- **§1.2 Threading** (lanes, effil, OS threads). Coroutines ARE allowed. Escape v2+: `mc2.spawn_task(fn)` cooperative wrapper.
- **§1.3 Raw memory / FFI.** Never; category error. PUC-Rio Lua 5.4 (no FFI link).
- **§1.4 OS shell** (`os.execute`, `os.remove`, `os.exit`, `popen`, `os.getenv`). Allowed: `os.time/clock/date/difftime`.
- **§1.5 Filesystem outside sandbox.** Mod must ship file inside its tree.
- **§1.6 Other mods' internal state.** Cross-mod state via `mc2.persist[mod_id]` only.
- **§1.7 Engine internals** (cull gates, GameObject refs, raw pointers). BindingRegistry only.
- **§1.8 Save format manipulation.** Engine-owned chunk shape; mod data via `mc2.persist`.
- **§1.9 Reentry into the ABL VM from non-ABL context.** `g_ablInTick` flag enforces.
- **§1.10 Modifying other mods' Lua code or environment.**
- **§1.11 Bypass of profile-launcher / sandbox itself.**
- **§1.12 Stock content modification in place.**

### From Track F design

- **MP determinism.** Defer to v2 (Track F §11 q9). Architecturally compatible per §12. Unlocks: dedicated MP track.
- **Fixed-point integer math** for distance / threat. v1 uses floats; fixed-point is MP-determinism prep, not M0. (Track F §11 line 434.)
- **Deterministic-mode Lua VM** (pinned table iteration, no `pairs()` over hash tables). Architectural-compat in v1; enforced only when MP track lands. (Track F §11 line 436.)
- **Behavior-tree replacement substrate** (Track F.4). Multi-month effort. Today: dispatch hooks (Option B engine events). Wholesale BT replacement deferred.
- **Corebrain `.abl` source recovery** (Track F.1). Out of Track C / M0; separate exploration.
- **Coroutine yield-point determinism audit** (Track F §11 q6). Defer until determinism work begins.
- **Groot-style introspection** (BT debug overlay). Defer to Track B sub-slice once F-5 lands.

### From sandbox / boundaries

- `dofile`, `load`, `loadfile`, `loadstring`, `setmetatable`, `getmetatable`, `rawget`, `rawset`, `collectgarbage` — stripped from sandbox.
- `io`, `os`, `package`, `debug` — not opened. Replaced with `mc2.io.read` shim, `os.time/clock/date/difftime` re-export, `debug.traceback` only.
- C extension loading (`package.loadlib`, mod-shipped DLLs) — never; would require Lua-as-DLL build, breaks sandbox guarantees.

### From modder tooling / DX

- **In-game mod browser UI.** Track B/E concern; data hooks only in C.
- **In-game settings UI** for per-mod options. Same — data hooks only.
- **State inspector UI.** Track B-gated.
- **ImGui REPL.** Track B-gated. Stdio fallback ships day-one.
- **Mod-manager UI.** Track B-gated.

### From mod test harness

- **Cross-OS testing.** Windows-first; Linux harness deferred (no Linux build target shipping in M0).
- **Virtual display in CI.** Smoke matrix is desktop-bound (menu canary screen-coordinate-bound per worktree CLAUDE.md). Headless CI deferred.
- **Per-mod × per-mission matrix explosion.** `mod-smoke = {(mod, default_mission)}` only in M0.

### From cross-track perf / boundaries

- **Per-mod resource budgets.** M0 ships VM-wide cap only; per-mod attribution via `lua_setallocf` shim is M1.
- **Async / queued `mc2.events.emit`.** Synchronous in M0; async candidate for M1.
- **Auto-disable threshold for `mc2.on_event`** (3-in-60 default). M1 polish.

---

## §4 — Decisions still pending

| ID | Decision | Severity | Status |
|----|----------|----------|--------|
| D1 | Track F tick rate (native 0.44 Hz vs 1–2 Hz) | High | RESOLVED — native first → 1–2 Hz later (per chat). |
| D2 | `MAX_STANDARD_FUNCTIONS` value at HEAD | Critical | RESOLVED — verified 512 (`memory/carver5_mission_playable.md`). |
| D3 | Track F priority vs Track C M0 | Medium | RESOLVED — Track F deferred until C-3 ships. |
| D4 | Modifier registry design (Track F pilot/chassis stacking) | Medium | PENDING — deep-dive in parallel; NOT a C-1 blocker. |
| D5 | Prototype override policy (first-write-wins vs layered vs explicit `replace()`) | Medium | DEFERRED to first override-needing mod. (`lifecycle.md` §9.1.) |
| D6 | Hot-reload module-graph granularity | Low | DEFERRED — M0: coarse "any change under mods/X/". |
| D7 | Mod-set drift on save: warn vs refuse | Low | DEFERRED — M0: warn-only; refuse-default is M1. |
| D8 | Variadic ABL signature representation in Lua | Low | DEFERRED — no M0 STABLE binding uses `*` / `?`. |
| D9 | `mc2luadispatch_*` Option A (corebrain patching) vs Option B (engine events) | Medium | RESOLVED for M0 — Option B only. Option A deferred to M1 per magic-ABL contamination rule. |

**No D-tier item blocks C-1.** D4 may block Track F-2 (Lua bindings for L4); resolve before F-2 commit.

---

## §5 — Order-of-implementation recommendation

Critical path with dependencies and rough dev-day estimates.

```
C-1 (vendoring + build)              [1d]    can start TODAY
    │
    ├─► C-2 (LuaVM + sandbox)        [2d]    depends on C-1
    │       │
    │       └─► C-3 (bindings + *_impl) [2d]    depends on C-2 + per-binding extraction
    │               │
    │               ├─► C-4 (lifecycle wiring)  [0.5d]   5 + 1 one-line edits
    │               │       │
    │               │       └─► C-5 (demo mod + smoke) [1d]
    │               │
    │               ├─► E-1 (JSON manifest path) [1–2d]   parallel after C-3
    │               │
    │               └─► F-1 (engine framework + brainsEnabled gate) [2d]
    │                       │
    │                       └─► F-2 (Lua bindings for L4)  [1d]   needs D4 resolved
    │
    └─► D-1 (Assimp MVP) [2–4d]    parallel after Phase B (independent of C)

Parallel non-blocking:
    L (modder tooling / Python doc-gen)  [1d]   after C-3 emits BindingRegistry
    M (modder DX REPL stdio fallback)    [0.5d] day-one
    O (test harness --test-mod CLI)      [1d]   after C-3 ships at least one binding
```

**Critical path:** C-1 → C-2 → C-3 → C-4 → C-5. Total: ~6.5 dev-days. Add ~3 dev-days for E-1 + F-1 if landing in same wave. ~12–14 dev-days total scope for the M0 wave.

**What can parallelize:** C-3 binding extraction can split per-`*_impl` (10 small commits). L (Python tooling) and M (REPL) parallel after C-3. F-1 parallel after C-3. D-1 (Assimp) wholly independent.

**Killswitch points:** C-1 lands harmless (modding lib has 1 stub fn, never called). C-2 lands with VM created but never given to anyone. C-3 lands with bindings registered but no mod loaded. The first behavior change visible to a player is C-5 — a `mods/test/` directory exists with a Lua script.

---

## §6 — Failure modes if ignored

Concrete scenarios for skipping each pre-flight item or design boundary. Severity-ordered.

**F1 (Critical) — Skip "read blocking-questions §Q1":** implementer follows `lua-trampolines-and-tests.md` §1, writes Sol2 lambdas that push to ABL stack inline. First Lua mod that fires during ABL execution corrupts `g_ABLStack`. Symptom: random `corebrain.abx` crashes mid-mission, ALWAYS in stock content (proves it's the engine, not the mod). Already happened twice in design rounds — that's why §Q1 exists.

**F2 (Critical) — Wrong `Tick()` site:** picked a site mid-`Mission::update()` instead of end-of-update. Lua handler fires while ABL frame in flight; `mc2_object_status_impl` resolved against half-updated state. Symptom: intermittent wrong-result returns from read bindings, only when player has many active mechs.

**F3 (Critical) — Magic-FSM contamination re-entry:** committer adds `execMagicAttack` body without running the pre-commit hook. Stock corebrain gets an active body for a name that should stay `#if 0`. Symptom: the v0.2 hotfix hang on enemy activation. Already shipped to users once (`memory/magic_abl_contamination_rule.md`).

**F4 (Critical) — Teardown order ABL-before-Lua:** copy-paste swap at one of the 5 + 1 sites. Final pcalls reference torn-down ABL state. Symptom: heap corruption on mission unload; manifests as a crash on the *next* mission load, not the unload. Diagnostic: enable `MC2_LUA_DEV=1` to widen the death window.

**F5 (High) — Skip pre-baseline tier-1 smoke:** C-1 lands and you discover an FPS regression weeks later. Cause was a pre-existing engine state, not C-1, but the bisect window is now huge. Always record per-mission FPS before starting.

**F6 (High) — `*_impl` body closes over `CurWarrior`:** extraction looks pure but the inner ABL builtin reaches for `CurWarrior` mid-execution. Lua-side caller has no `CurWarrior` set. Symptom: SEGV in `code/ablmc2.cpp` when binding called from Lua; works fine when called from ABL.

**F7 (High) — Sol2 panic handler unhooked:** Lua error escapes Sol2's `protected_function`; landed at top-level `mc2.exe` C++ runtime as a `lua_State*` panic. Symptom: process exit with no log line. Mitigation: confirm `SOL_ALL_SAFETIES_ON=1` is set on the `modding` target.

**F8 (High) — Bytecode-via-`load` smuggling:** mod ships a `.lua` with bytecode payload. `load` was stripped but `0x1B` rejection wasn't added. Symptom: arbitrary code execution inside the engine process — full sandbox escape.

**F9 (Medium) — Hot-reload OFF, mod author confused:** new mod author sees no behavior change after editing Lua. Files have updated, but `MC2_LUA_DEV` was never set. Symptom: bug reports against the mod system that are actually env-config issues.

**F10 (Medium) — Track F tick-rate read as 30 Hz:** F-1 implementer reads design §2 / §11 unaware of the resolution banner; sizes perf budget for 30 Hz when native is 0.44 Hz. Symptom: over-engineered batching, premature SIMD, no shipped F-1 because it "isn't fast enough" — when at native rate it's 140× cheaper than imagined.

---

## §7 — Memory entries to create or update post-C-1

After C-1 lands, the following memory entries should exist (group under "Modding / scripting" — new section in `MEMORY.md`):

- **`track_c_vendoring_done.md`** — Lua 5.4.7 + Sol2 commit hashes, `lua_static` target shape, MSVC warning suppressions list, why we link static. Loaded when anyone updates Sol2 or Lua.
- **`lua_abl_bridge_pattern.md`** — the `*_impl` extraction pattern verbatim from `blocking-questions-resolution.md` §Q1; the canonical "how to add a new STABLE binding" recipe. Cited by C-3 follow-on commits.
- **`lua_sandbox_invariants.md`** — the `0x1B` reject, the symlink prefix-check, the stripped fn list, the three-pronged REPL auth gate. The "do not let this drift" reference.
- **`mc2_lua_lifecycle_sites.md`** — the 5 + 1 file:line table for `initLuaVM` / `closeLuaVM` calls; teardown order rule (Lua before ABL). Loaded when anyone touches `mission.cpp` / `missionbegin.cpp` / `saveload.cpp`.
- **`mc2_namespace_tree_v1.md`** — the locked `mc2.<sub>.<verb>` tree for `mc2_api_version=1`. Cited any time someone wants to add a binding (cross-check the name fits the tree).
- **`magic_fsm_precommit_hook.md`** — the pre-commit hook command + rationale (separate from `magic_abl_contamination_rule.md` which is the policy doc).
- **Update `MEMORY.md`:** add "Modding / scripting" section with the entries above. Add load-bearing star to `lua_abl_bridge_pattern.md` and `mc2_lua_lifecycle_sites.md`.
- **Update `stock_install_must_remain_playable.md`:** add Lua-additions-are-sidecar paragraph + cross-link.

After C-3 (bindings ship): add `mc2_binding_extraction_recipe.md` documenting the per-binding workflow + per-binding regression smoke pattern.

After C-5 (first mod): add `track_c_m0_demo_mod.md` documenting the demo mod's structure + how to reproduce.

After Track F-1: add `track_f_brain_gate_pattern.md` documenting the `brainsEnabled[]` short-circuit at `code/warrior.cpp:2155`.

---

## §8 — Open questions remaining

Only items not covered by D1–D9:

1. **Sol2 commit pin SHA.** Audit references "the commit hash for `THIRD_PARTY_LICENSES.md`" but doesn't pin it. Pick at C-1 drop time; record in `track_c_vendoring_done.md`.
2. **`tools/lua_api_doc_gen` JSON schema version.** Audit calls out `"schema_version": 1` field but no full schema doc exists yet. Resolve at C-3 alongside `BindingRegistry` shape.
3. **CI integration for per-binding regression smoke.** Audit suggests "call from ABL test mission, compare result" — exact harness shape unspecified. Likely a thin wrapper around existing tier-1 smoke; resolve at C-3.
4. **Linux build viability of `modding/` target.** `LINUX_BUILD` global is harmless to Lua per build-integration §9, but Sol2 hasn't been confirmed on Linux. Out of M0 scope (no Linux target shipping); flag if Linux ever lands.
5. **Track F modifier registry shape (D4).** Pilot/chassis affinity stacking math is being deep-dived in parallel; not a C-1 blocker but an F-2 blocker.
6. **Persisting REPL command history across sessions.** DX nice-to-have; resolve at M / DX polish.

End of pre-flight + risk-map document.
