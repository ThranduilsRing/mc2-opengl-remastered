# ASan MVP — Follow-up Findings

Running log of issues surfaced by the ASan build that are **not** what we
were originally hunting. Not blocking for heap-bug investigation work —
ASan still reports memory bugs correctly even with these present.

## 1. Rendering pipeline: black terrain + persistent FX trails

**Status:** open, not investigated.
**First observed:** 2026-04-24, first interactive `mc2-asan.exe` run on
`mc2_01` (base game). Build = `build64-asan/RelWithDebInfo` off commit
`a3f84a8 feat(build): add ASan MVP configuration`.

### Symptoms (ASan build only — normal build is fine)

- **Terrain renders black.** Mechs, UI, HUD, and object rendering all work
  normally. Combat functions — user verified by running around and shooting.
- **FX trails persist across frames.** When an effect fires (e.g. weapon
  impact, explosion), its pixels are not cleared on the next frame —
  successive frames composite on top, producing a "solitaire end-screen"
  trail pattern.
- No shader compile/link errors in the log.
- No `[GL_ERROR v1]` events.
- Game runs clean to `[SMOKE v1] result=pass` with `frames=1975` over 45 s.
- **ASan itself produced zero reports** during the run.

### Why this is ASan-only (analysis)

- Tracy is ruled out as a cause. `TRACY_ENABLE` is referenced only inside
  `3rdparty/tracy/` — zero MC2 source files check for it. Its macros are
  no-ops when undefined; disabling Tracy cannot change runtime behavior
  in MC2 code.
- That leaves `/fsanitize=address` codegen as the only variable. ASan
  inserts red zones, reorders stack locals, and changes memory layout.
  The classic symptom this produces: latent UB (uninitialized stack
  variable, OOB read of a stale pattern) that happened to work under the
  normal layout now reads different garbage.

### Hypothesis

Post-process / FBO pipeline UB. Symptom pattern ("persistent pixels" +
"nothing where terrain should be") is framebuffer-lifecycle, not
shader/vertex. Most likely site: uninitialized FBO attachment handle,
stale `glClear` mask/state, or a bad bind in `gos_postprocess.cpp`.
CLAUDE.md / `docs/amd-driver-rules.md` already document the FBO pipeline
as AMD-quirky, which is consistent with the class of bug ASan's layout
change would expose.

### Diagnostic path (for the future investigation)

In-game toggles to narrow the failure site (all from `mc2-asan.exe`):

- `F5` — terrain draw killswitch. If toggling terrain off leaves the
  world visibly black *everywhere* (not just where terrain was), the
  problem is upstream of terrain in the composite chain.
- `RAlt+4` — screen shadows toggle. If turning screen shadows off makes
  terrain visible again, the screen-shadow composite pass is the failure.
- `RAlt+6` / `RAlt+7` / `RAlt+9` — god rays / shorelines / static-prop
  debug-mode cycle. Whichever toggle makes the symptom change is on the
  failure path.
- `F3` — shadows on/off. Narrows whether it's shadow-related.

Code-level instrumentation to add before the next ASan rendering run:

1. `glGetError()` drain after every `glBindFramebuffer` in
   `gos_postprocess.cpp`. Log the first `GL_INVALID_*` that wasn't
   present in the non-ASan build.
2. Log FBO attachment handles at init (they should be stable, non-zero
   GL names). If one reads zero or garbage under ASan, that's the UB
   site.
3. Log the `GLbitfield` passed to each `glClear` call. An uninitialized
   mask that used to happen to be `GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT`
   could now be zero, which explains the "no clear" trail symptom.

### Why we are not fixing it now

The rendering regression does not affect ASan's ability to report heap
bugs — those fire regardless of what pixels end up on screen. The
original MVP goal (catch latent heap bugs on mod content: Carver5O,
Omnitech, Exodus) is unaffected. Come back to this as a dedicated
investigation once we have a heap-bug inventory from the mod-content
runs.

### If ASan runs start producing ambiguous results

Optional fallback experiment: rebuild the ASan build with Tracy
re-enabled (`-DTRACY_ENABLE` added manually alongside `MC2_ASAN=ON`).
Low-prior based on the grep analysis above, but it's a 10-minute
empirical check if other evidence later points at Tracy.

---

## 2. `GlobalMap::clearPathExistsTable` uninit-pointer AV on Carver5O

**Status:** bug confirmed, fix deferred.
**First caught:** 2026-04-24, `mc2-asan.exe --mission mc2_01` on the
Carver5-feasibility install (mc2x-import content merged into nifty).
ASan log: `A:/Games/Carver5-feasibility/asan-carver5-mc2_01.18080`.

### Crash signature

ASan fires at `memset(pathExistsTable, ..., tableSize)` where
`pathExistsTable == 0xBEBEBEBEBEBEBEBE` — MSVC `/fsanitize=address`
uninitialized-stack/heap poison pattern.

Backtrace:

```
_asan_memset
GlobalMap::clearPathExistsTable   mclib/move.cpp:3069
Building::update                   code/bldng.cpp:855
GameObjectManager::update          code/objmgr.cpp:1769
Mission::update                    code/mission.cpp:504
DoGameLogic                        code/mechcmd2.cpp:2233
```

### Root cause (hypothesis)

The content load path for Carver5O hits partial-init of `GlobalMap`:

```
[PAUSE] GlobalMap.init: doorInfo sum 9498 exceeds header numDoorInfos 200 — growing buffer to 9498
[PAUSE] Mission.init: bad/old move data — skipping gate callback wiring; pathfinding degraded
```

Under partial init the `pathExistsTable = NULL` assignment at
`mclib/move.cpp:1500` appears to be skipped, leaving the field holding
whatever the allocator returned (under ASan: `0xBE`-filled poison). The
`clearPathExistsTable` null-guard on line 3066 passes (non-zero pointer)
and the `memset` dereferences garbage.

### Code

```cpp
// mclib/move.cpp:3064-3070
void GlobalMap::clearPathExistsTable (void) {
    if (!pathExistsTable)                          // non-zero garbage passes
        return;
    long tableSize = numAreas * (numAreas / 4 + 1);
    memset(pathExistsTable, GLOBALPATH_EXISTS_UNKNOWN, tableSize);   // AV here
}
```

### Proposed fix (deferred)

Force `pathExistsTable = NULL` (and any peer fields) in the `GlobalMap`
constructor, not only inside `init()` — so partial-init objects are
always safe to call member methods on. Independently, audit all
`if (!ptr) return;` pattern sites in `move.cpp` — the null-guard
convention only works if the pointer is explicitly zeroed on construction.

### Why this is valuable

This is exactly the class of bug the ASan MVP was built for: mod-content
format mismatch leaves partial-init state that the engine then
dereferences during the mission update loop. The normal build produces
some other garbage value that happens not to AV on this specific
Carver5O data — ASan's memory-layout change guarantees a poison pattern
that does AV, surfacing the latent bug.

### FFmpeg DLL deploy gotcha (sibling finding)

The Carver5-feasibility install originally lacked the FFmpeg runtime
DLLs (`avcodec-61.dll`, `avformat-61.dll`, `avutil-59.dll`,
`swresample-5.dll`, `swscale-8.dll`). Without them, `mc2-asan.exe` fails
to load with `STATUS_DLL_NOT_FOUND (0xC0000135)` — the loader reports
`api-ms-win-crt-locale-l1-1-0.dll` missing. This is misleading: the
actual blocker is the UCRT/VCRUNTIME SxS activation context, which the
FFmpeg DLLs transitively provide. The normal `mc2.exe` build tolerates
missing UCRT somehow (possibly via static CRT in Release), but
`/fsanitize=address` forces dynamic UCRT binding regardless of `/MT`.

Deploy rule for any ASan target install: copy the FFmpeg DLLs alongside
`mc2-asan.exe`, or install UCRT system-wide via the VC++ Redistributable.
