# Stability Tier 1 — Silent-Failure Instrumentation

**Date:** 2026-04-23
**Branch:** `claude/stability-tier1` (based on `claude/nifty-mendeleev`)
**Worktree:** `A:/Games/mc2-opengl-src/.claude/worktrees/stability-tier1`
**Status:** Approved for implementation planning.

## 1. Goal and Scope

Land the **instrumentation** layer for three known silent-failure modes in MC2. No behavior changes, no speculative fixes — just observability so the next occurrence is diagnosable from a log instead of "mech disappeared."

### In scope
1. **TGL pool exhaustion** — one-line-per-broken-frame log + monotonic total, so "shapes just vanished" stops being a mystery.
2. **Object destruction visibility** — `GameObject::destroy(reason, file, line)` wrapper with cull/lifecycle snapshot; convert every literal `setExists(false)` call site.
3. **OpenGL error drain** — `drainGLErrors("<pass>")` at seven named pass boundaries; print-by-default so a Release-crash triager sees errors without setting an env var.

### Explicitly deferred
- Magenta debug placeholder for NULL-returned TGL shapes (needs non-pool allocation path; can cascade).
- Fixing any Release `GL_INVALID_ENUM` errors the drain reveals (own follow-up spec).
- Making `GameObject::setExists` protected to force-route through wrapper (bigger refactor; invariant enforced by grep for now).
- Crash-on-drain-error and minidump hooks (Tier 3).
- CI grep gate for the destruction-wrapper invariant (needs CI infra).

### Success criteria
- **All 34 literal `setExists(false)` sites** plus the **4 non-literal `setExists(<expr>)` sites** converted or reviewed. After implementation, `grep -rn 'setExists\s*(' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h'` returns only `(true)` calls and the single internal call inside `GameObject::destroy`.
- **Baseline perf:** Mission `mis0101` loaded from main menu, camera at spawn position, 30-second Tracy capture. With all three gates OFF:
  - Median frame time within 2% of pre-change baseline.
  - P99 frame time within 5% of baseline.
  - P99.9 frame time within 10% of baseline.
  Tail latency matters for a stability spec — a regression that adds a 5ms hitch every 100 frames passes a median check and breaks the game. Same scene, same camera position, same hardware session. Implementer captures the pre-change baseline *before* starting code changes, commits the Tracy trace (or its key numbers) to the branch, then re-captures after the three instrumentation commits land. Do not swap the scene mid-measurement — if `mis0101` is somehow unavailable, halt and escalate before picking another.
- **Gates ON produces clean logs:** Same 30-second capture with all three gates ON produces a log under 10MB total. Every log line starts with `[SUBSYS vN]` — no wrapped multi-line traces. Exceeding 10MB is a signal, not a threshold to relax: file it as a discovered Tier 1 bug.
- **Startup banner (Section 5) appears in every log file** produced during verification. If it's missing, instrumentation wiring is incomplete.
- **Zero leaks from trace infrastructure.** All trace state is static/global `int`/`char[]`; no heap allocations introduced.

---

## 2. TGL Pool Exhaustion Trace

### 2.1 Caller-capture mechanism

**Single canonical signature** for each pool's get function. One defaulted parameter for the shape pointer — no overload zoo:

```cpp
// mclib/tgl.h (return types match existing methods — this spec does not change those)
gos_VERTEX *  getVerticesFromPool (DWORD numRequested, const char* caller, const void* shape = nullptr);
gos_LIGHT  *  getColorsFromPool   (DWORD numRequested, const char* caller, const void* shape = nullptr);
gos_FACE   *  getFacesFromPool    (DWORD numRequested, const char* caller, const void* shape = nullptr);
gos_SHADOW *  getShadowsFromPool  (DWORD numRequested, const char* caller, const void* shape = nullptr);
gos_TRI    *  getTrianglesFromPool(DWORD numRequested, const char* caller, const void* shape = nullptr);
```

Every call site wraps via macros:

```cpp
// mclib/tgl.h
#define MC2_TGL_GET_VERTS(pool, n)             (pool)->getVerticesFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_COLORS(pool, n)            (pool)->getColorsFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_FACES(pool, n)             (pool)->getFacesFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_SHADOW(pool, n)            (pool)->getShadowsFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_TRIANGLES(pool, n)         (pool)->getTrianglesFromPool((n), __FUNCTION__)

// Variant that also passes the calling shape pointer for first-null snapshot.
// Use at the TG_Shape::TransformShape call site (mclib/tgl.cpp:1667) and anywhere
// else the caller naturally holds the shape pointer.
#define MC2_TGL_GET_VERTS_FOR_SHAPE(pool, n, shape)  (pool)->getVerticesFromPool((n), __FUNCTION__, (shape))
```

The basic and shape-aware macros both call the same entry point — the defaulted `shape=nullptr` parameter handles the difference. Every existing direct call is converted to the appropriate macro. Mechanical grep-and-replace across the handful of call sites (TransformShape, MultiShape variants, etc.).

### 2.2 Record on NULL

Inside each pool's get function, on the NULL branch (before returning):

```cpp
this->recordNull(caller, numRequested, shape);
```

Signature on the pool base class:

```cpp
void TG_Pool::recordNull(const char* caller, DWORD numRequested, const void* shape);
```

Behavior:
- `nullCountThisFrame` — per-frame counter (resets at frame begin).
- `nullCountMonotonic` — since process start.
- First-null-per-frame snapshot captured when `nullCountThisFrame` transitions 0→1. Snapshot stores: `{ caller, shape, numRequested, numVertices_at_failure, totalVertices }`. `shape` is `nullptr` when the call site used the basic macro, non-null when it used `MC2_TGL_GET_VERTS_FOR_SHAPE`.

### 2.3 Frame-end drain

Add `drainTglPoolStats()` called from `GameOS/gameos/gameos.cpp` frame-end path (just before buffer swap, after the GL error drain — see §4). Iterates five pools, emits one log line per pool with `nullCountThisFrame > 0`, then resets the per-frame counters.

### 2.4 Log format

Env gate: `MC2_TGL_POOL_TRACE=1` enables per-frame print. Default-off (inverted from GL_ERROR's default-on) because the per-frame line is for active debugging — the monotonic summary (§2.5) already surfaces "did this ever happen" without any env var. An operator who doesn't know about the trace still sees the summary every 600 frames and on shutdown, which is enough to flag the failure mode; turning on the per-frame stream is a deliberate next step once they're investigating.

```
[TGL_POOL v1] frame=1234 pool=vertex nulls=37 first_caller=TransformShape shape=0x2a3f1800 req=512 high_water=499842/500000 mono_total=1841
```

Fields:
- `frame` — engine frame counter
- `pool` — one of `vertex|color|face|shadow|triangle`
- `nulls` — per-frame NULL count for this pool
- `first_caller` — `__FUNCTION__` of first NULL this frame
- `shape` — shape pointer if available, else `0x0`
- `req` — requested count at first NULL
- `high_water` — `numVertices/totalVertices` at first NULL
- `mono_total` — monotonic NULL count for this pool since process start

### 2.5 Monotonic summary (unconditional — not env-gated)

Every 600 frames and on shutdown, emit one line even with `MC2_TGL_POOL_TRACE` unset:
```
[TGL_POOL v1] summary mono_total={vertex:1841, color:0, face:221, shadow:0, triangle:0} since=process_start
```

This is the "did this ever happen" signal. At 200fps that's a 3-second cadence — not log-flooding.

---

## 3. Destruction Wrapper

### 3.1 API

Add to `code/gameobj.h`:

```cpp
// public method on GameObject
void destroy(const char* reason, const char* file, int line);
```

And a macro for call sites:

```cpp
#define MC2_DESTROY(obj, reason) (obj)->destroy((reason), __FILE__, __LINE__)
```

Rationale for macro over `std::source_location`: MC2 is C++17-era MSVC with mixed conventions; `__FILE__ __LINE__` macros match the existing codebase style. `std::source_location` would be a singleton style change for this one API.

### 3.2 Implementation

`GameObject::destroy` in `code/gameobj.cpp`:

1. **Snapshot all log fields first**, before any other logic runs. `kind`, `cull`, `block_active`, `update_ret`, `exists_was`, `framesSinceActive` are read in the first three lines of the function into locals. Prevents teardown logic anywhere in the call chain from stomping a field the log later reports. The snapshot locals — not the live fields — are what the log emits.
2. **Double-destroy semantics.** If `exists_was` is already `false`, emit the log line (with `exists_was=0`, which §3.4 already renders) and return *without* calling `setExists(false)` a second time. Legitimate case: mission cleanup sweeping objects that combat already killed. Explicitly **not** an assert — an assert here would turn a latent bug into a crash during an instrumentation-only change, violating the "no behavior changes" premise of this spec.
3. If `exists_was` was `true`, call `setExists(false)` (unchanged existing behavior).
4. If `MC2_DESTROY_TRACE` env var is set, emit one log line (format §3.4).

**Null-pointer contract.** `MC2_DESTROY(obj, reason)` expands to `obj->destroy(...)` — same null-dereference contract as every current direct `setExists(false)` call site. The wrapper does **not** null-check. Callers responsible for ensuring `obj != nullptr`, same as today. Adding a null-check here would be a behavior change (convert a would-be crash into a silent no-op) and is out of scope for this spec.

### 3.3 `framesSinceActive` counter

New member on `GameObject`:
```cpp
uint8_t framesSinceActive;  // saturating at 255
```

Semantics: zero when the object is considered active this frame (any of the three cull gates `inView` / `canBeSeen` / `objBlockInfo.active` says "keep alive"); incremented (saturating at 255) otherwise. Anything over ~4 seconds of inactivity at 60fps is pathological; the exact count beyond 255 doesn't matter.

**Single update site, not distributed.** Instrumenting every gate transition is fragile — any future gate added without updating the counter silently breaks the log field. Instead: one update site in `GameObjectManager::update` (or equivalent per-frame object sweep in `code/objmgr.cpp`) where the composite "considered active this frame" decision is final:

```cpp
bool activeThisFrame = obj->inView || obj->canBeSeen || obj->objBlockInfo.active;
if (activeThisFrame) {
    obj->framesSinceActive = 0;
} else if (obj->framesSinceActive < 255) {
    obj->framesSinceActive++;
}
```

Implementer identifies the exact function in `objmgr.cpp` that's the canonical per-frame sweep and inserts this snippet there. No other sites touch `framesSinceActive`.

Padding audit: `GameObject` is declared in `code/gameobj.h` (class body at line 307). During implementation, inspect the layout — if existing 1-byte padding is available, steal into it; otherwise accept an 8-byte bump after alignment. Either is fine for the purpose.

### 3.4 Log format

Env gate: `MC2_DESTROY_TRACE=1`.

```
[DESTROY v1] frame=1234 obj=0x2a3f1800 kind=BattleMech reason=update_false file=objmgr.cpp line=1421 exists_was=1 in_view=1 can_be_seen=1 block_active=1 frames_since_active=0 last_update_ret=0
```

Fields (all fixed, no post-hoc taxonomy):
- `frame` — engine frame counter
- `obj` — `GameObject*` (hex pointer)
- `kind` — stringified `ObjectClass` via helper (§3.5)
- `reason` — literal string passed by caller
- `file`/`line` — source location of the `MC2_DESTROY` call
- `exists_was` — `exists` flag snapshot immediately before `setExists(false)` (`0` or `1`)
- `in_view` — raw `inView` flag snapshot (`0` or `1`)
- `can_be_seen` — raw `canBeSeen` flag snapshot (`0` or `1`)
- `block_active` — raw `objBlockInfo.active` flag snapshot (`0` or `1`)
- `frames_since_active` — counter from §3.3 (`0`-`255`)
- `last_update_ret` — most recent `update()` return value cached on the object; `-1` sentinel if no update has ever been called or the destruction path did not come through update

**Rationale for raw booleans over a composed `cull=<enum>`:** a composed enum requires a taxonomy that doesn't yet exist and couples the log schema to cull-chain internals. Raw booleans are dumb, stable, and preserve all the information; any consumer can compose them into a state string later without the log format committing to one. Also makes v1→v2 unnecessary when someone inevitably reshapes the cull chain — the field names stay valid.

**Cached `last_update_ret`:** add `int8_t lastUpdateRet = -1` to `GameObject`. Every existing `update()` call site that writes the return value updates the cache. There are few enough update-return-checked sites that this is a one-afternoon grep-and-set; implementer identifies them from the same cull-chain sweep that places the `framesSinceActive` update.

### 3.5 `getObjectClassName` helper

Add to `code/gameobj.cpp`:

```cpp
const char* getObjectClassName(ObjectClass kind);
```

Switch-table over every existing `ObjectClass` enumerator (BATTLEMECH, GROUNDVEHICLE, ELEMENTAL, BUILDING, TREEBUILDING, GATE, TURRET, etc.). Default returns `"UNKNOWN"`. Referenced by the `[DESTROY]` log formatter and reusable elsewhere.

### 3.6 Conversion pass

Literal `setExists(false)` sites by file (from grep at spec time):
- `code/objmgr.cpp` — 26
- `code/ablmc2.cpp` — 2
- `code/collsn.cpp` — 2
- `code/gvehicl.cpp` — 1
- `code/mission.cpp` — 2
- `code/weaponbolt.cpp` — 1

**Total: 34 literal sites.** Each converts to `MC2_DESTROY(obj, "<short_literal>")`. Reason literal is chosen by reading ±10 lines of surrounding context.

**Non-literal sites (4 total):** `grep -rn 'setExists\s*(' | grep -v '(\s*true' | grep -v '(\s*false'` yields 4 `setExists(<expr>)` call sites. Each is manually reviewed during implementation — if the expression can evaluate to `false` at runtime, the site is converted to:
```cpp
if (!<expr>) MC2_DESTROY(obj, "<reason>");
else obj->setExists(true);  // or the original structure
```
If the expression is always true in practice (dead branch), document and leave unchanged.

### 3.7 Reason-string dedup step

After the 34-site conversion pass, run:
```bash
grep -rh 'MC2_DESTROY' code/ mclib/ GameOS/ --include='*.cpp' | grep -oP '"\K[^"]+' | sort -u
```

Collapse near-duplicates (`update_false` / `update_returned_false` / `bad_update` → single canonical `update_false`). Commit the final taxonomy as a comment block above `destroy()` in `gameobj.h`:
```cpp
// Canonical destruction reasons (keep in sync with MC2_DESTROY call sites):
//   update_false        — update() returned false
//   killed_by_weapon    — damage model zeroed hitpoints
//   mission_cleanup     — teardown at mission end
//   ... (emerged from diff)
```

### 3.8 Invariant

> All `setExists(false)` call sites outside `GameObject::destroy` are violations. `GameObject::destroy` is the only legitimate caller of `setExists(false)`.

**Enforcement (this spec):**
- Checked-in script `scripts/check-destroy-invariant.sh` (committed in commit 4). Runs the literal + non-literal greps described below, prints violations (or `OK` if clean), and exits non-zero if violations exist:

```bash
#!/bin/sh
# scripts/check-destroy-invariant.sh
set -e
violations=0

# Literal: setExists(false) must only appear inside GameObject::destroy (code/gameobj.cpp).
# Uses grep -E with POSIX [[:space:]]* — plain grep / BRE does NOT interpret \s.
lits=$(grep -rEn 'setExists[[:space:]]*\([[:space:]]*false' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h' | grep -v 'code/gameobj.cpp' || true)
if [ -n "$lits" ]; then
  echo "[INVARIANT] literal setExists(false) outside GameObject::destroy:"
  echo "$lits"
  violations=1
fi

# Non-literal: setExists(<expr>) where expr is not true/false literal — must be manually
# vetted. This script flags them for review; does not fail the check by itself.
nonlit=$(grep -rEn 'setExists[[:space:]]*\(' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h' \
  | grep -Ev 'setExists[[:space:]]*\([[:space:]]*(true|false)' | grep -v 'code/gameobj.cpp' || true)
if [ -n "$nonlit" ]; then
  echo "[INVARIANT] non-literal setExists(<expr>) — manual review required:"
  echo "$nonlit"
fi

[ $violations -eq 0 ] && echo "OK"
exit $violations
```

- `CLAUDE.md` documents: "run `sh scripts/check-destroy-invariant.sh` before any commit that touches object lifecycle; exit 0 = clean."
- CI grep gate is future work (separate spec). The checked-in script is one step from CI integration — add it to whatever hook or workflow file when CI exists.

---

## 4. GL Error Drain

### 4.1 Seven named pass boundaries

Per-frame, in render order:
1. `shadow_static` — end of static terrain shadow render
2. `shadow_dynamic` — end of dynamic mech/object shadow render
3. `terrain` — end of terrain draw (after overlay split)
4. `objects_3d` — end of 3D-object pass (`renderLists` flush)
5. `post_process` — end of bloom/FXAA/screen-shadow composite
6. `hud` — end of HUD / overlay pass
7. `frame` — after final draw, before swap (catch-all)

### 4.2 API

In `GameOS/gameos/gos_validate.cpp` / `.h` (existing files in worktree):

```cpp
void drainGLErrors(const char* pass);
```

Behavior:
- Loop `glGetError()` until `GL_NO_ERROR`.
- For each error, increment per-(pass,frame) count.
- Increment monotonic per-pass counter.
- If `MC2_GL_ERROR_DRAIN_SILENT` is **not** set AND this is the first error for (pass, frame), emit one log line (format §4.4).
- Subsequent errors in same (pass, frame) bump the count silently — AMD drivers can spray thousands.

**Cross-pass/cross-frame rate limit (default-on print still needs a budget cap).** If a pass has already emitted a print in *any* of the last 30 frames, suppress further first-error prints for that pass for the next 30 frames; the count continues to accumulate silently and a single summary line emits at suppression end: `[GL_ERROR v1] pass=shadow_static suppressed frames=30 count_in_window=842`. Preserves signal (you see the problem), bounds volume (one persistently broken pass can't blow the 10MB budget in 30 seconds). Window is fixed 30 frames — no backoff escalation; this is instrumentation, not a reconnect strategy.

**Semantic ownership of `glGetError()`.** These drains **consume** the GL error queue. Any downstream code that calls `glGetError()` after a drain sees `GL_NO_ERROR` for errors that occurred before the drain. Existing callers audited at spec time:

- `GameOS/gameos/utils/shader_builder.cpp` (7 sites) — runs at shader compile / init, separate phase from per-frame render passes. Not affected.
- `GameOS/gameos/utils/gl_utils.h` (2 sites in a macro) — localized pre-drain pattern; self-contained, not affected.
- `GameOS/gameos/gos_static_prop_batcher.cpp:798` — self-contained pre-drain pattern. Not affected.
- `GameOS/gameos/gos_validate.cpp:123` — this IS the existing drain we extend. Subsumed.
- `GameOS/gameos/gameos_graphics.cpp:2816-2819` — local clear-then-check pattern in a render-adjacent location. **Needs review during implementation** to confirm it isn't on a drain boundary.

Implementer runs a fresh `grep -rn 'glGetError\s*(' code/ mclib/ GameOS/ --include='*.cpp' --include='*.h'` during the drain commit, walks every new site that's been added since this spec, and confirms no cross-pass dependency. Document any audit outliers in the commit message.

### 4.3 Default ON

**Env gate: `MC2_GL_ERROR_DRAIN_SILENT=1` suppresses print.** Default is print-enabled — a fresh operator triaging the Release crash sees errors with no setup.

The drain itself (the `glGetError` loop + counters) runs unconditionally regardless of env var, so Tier 3 hooks and monotonic counts work even silently.

### 4.4 Log format

```
[GL_ERROR v1] frame=1234 pass=shadow_static code=GL_INVALID_ENUM(0x0500) mono_count=1
```

Fields:
- `frame` — engine frame counter
- `pass` — one of the seven names above
- `code` — symbolic name + hex for known errors; `UNKNOWN(0x????)` for anything else. Switch-table covers `GL_INVALID_ENUM(0x0500)`, `GL_INVALID_VALUE(0x0501)`, `GL_INVALID_OPERATION(0x0502)`, `GL_STACK_OVERFLOW(0x0503)`, `GL_STACK_UNDERFLOW(0x0504)`, `GL_OUT_OF_MEMORY(0x0505)`, `GL_INVALID_FRAMEBUFFER_OPERATION(0x0506)`, `GL_CONTEXT_LOST(0x0507)`. Default branch emits `UNKNOWN(0x<hex>)` — do not crash on unknown codes.
- `mono_count` — monotonic error count for this pass since process start

### 4.5 Call-site placement

Single `drainGLErrors("<pass>")` call inserted at the end of each of the six render-pass functions in `GameOS/gameos/gameos_graphics.cpp` / `mclib/txmmgr.cpp` / `GameOS/gameos/gos_postprocess.cpp`. The seventh call (`"frame"`) goes in the same frame-end site as the TGL drain (§2.3), before buffer swap.

---

## 5. Cross-Cutting

### 5.1 Schema version

All three loggers stamp `v1` in their prefix: `[TGL_POOL v1]`, `[DESTROY v1]`, `[GL_ERROR v1]`, `[INSTR v1]`. Any future format change bumps the version.

Grep patterns (documented in CLAUDE.md) match `\[SUBSYS v[0-9]+\]` so v1→v2 transitions don't break tooling.

**Migration policy:** none. Downstream log parsers re-adapt to new versions when they encounter them. This is a three-format logger in a small project — designing a field-compat or rename-shim layer would be overkill. The version prefix exists so parsers can *detect* incompat, not reconcile it.

### 5.2 Startup banner (unconditional, one line per run)

**Emitted at process/logger init, not mission init.** The first call from the engine into the logging subsystem — whichever of MC2's init paths initializes `stdout`/log-file redirection — is where the banner goes. A mission-init banner would fail the "appears in every log file" success criterion whenever logging captured output before mission init (e.g., crash during asset load, config errors on startup, the shader-compile phase that runs `glGetError` all by itself).

```
[INSTR v1] enabled: tgl_pool=<0|1> destroy=<0|1> gl_error_print=<0|1> build=<short-hash>
```

Values derived from env vars at banner-emission time. Build hash comes from the existing build-stamp mechanism if present; otherwise literal string `UNKNOWN` is emitted. Adding a new build-stamp mechanism to CMake is explicitly out of scope for this spec — if the implementer finds an existing mechanism they use it; if not, `UNKNOWN` ships. A follow-up spec can add `git rev-parse --short HEAD` to CMake when there's a reason to prioritize it.

Success criterion: this line appears in every log file produced during verification — including logs where the process crashed before reaching mission init. A missing banner = instrumentation not wired up (or wired too late).

### 5.3 Thread safety

**Assumption:** MC2 is effectively single-threaded for the renderer and for object lifecycle. All trace state (per-pool null counts, first-null snapshots, per-pass GL error counts, `framesSinceActive`) is accessed from one thread only — no locks required.

**Note for future:** If any subsystem is threaded later (Vulkan port, async resource loading), this instrumentation must be re-audited. Each logger should grow a `std::atomic<int>` for its counters at that point, or the trace points move to single-thread fences.

### 5.4 Commit structure

Four commits on `claude/stability-tier1` (destruction wrapper split to keep bisection targeted):

1. `feat(instr): TGL pool exhaustion trace + monotonic summary`
   - Pool method signatures, macros, `recordNull`, frame-end drain, log formats.
2. `feat(instr): GameObject::destroy wrapper + helper + frames-active counter`
   - `GameObject::destroy` method, `MC2_DESTROY` macro, `getObjectClassName` helper, `framesSinceActive` field + single-site update, log formatter. **Zero call-site conversions** — the old `setExists(false)` sites are untouched.
3. `feat(instr): convert 34 setExists(false) sites to MC2_DESTROY + dedup taxonomy`
   - All literal-site conversions, 4 non-literal-site manual reviews, reason-string dedup pass, taxonomy comment block in `gameobj.h`.
4. `feat(instr): GL error drain at 7 pass boundaries + startup banner`
   - `drainGLErrors`, call-site insertions, banner emission, CLAUDE.md updates (invariant, env vars, schema-version grep pattern).

Each commit builds, deploys, and is bisectable standalone. If a regression shows up mid-bisect, the split between commit 2 (the wrapper itself, which runs on zero objects) and commit 3 (the conversion sweep, which activates it across 34 sites) pinpoints whether the break is in the wrapper implementation or in a specific conversion's reason-string / missing guard.

### 5.5 CLAUDE.md updates

Bundled with commit 4 (GL error drain + banner):
- Document the `setExists(false)` invariant and the manual grep check.
- Document the three env vars: `MC2_TGL_POOL_TRACE`, `MC2_DESTROY_TRACE`, `MC2_GL_ERROR_DRAIN_SILENT`.
- Document the schema-version grep pattern `\[SUBSYS v[0-9]+\]`.
