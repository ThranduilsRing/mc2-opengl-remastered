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

---

## 3. `getCodeToken` deref of corrupted `codeSegmentPtr` on Exodus

**Status:** bug confirmed, fix deferred.
**First caught:** 2026-04-24, `mc2-asan.exe --mission mc2_01` on a fresh
`A:/Games/MC2-Exodus/` install (base `mc2-win64-v0.1.1` clone +
`mc2_patch` + `exodus_campaign_11`).
ASan log: `A:/Games/MC2-Exodus/asan-exodus-mc2_01.4884`.

### Crash signature

ASan fires at `codeToken = (TokenCodeType)*codeSegmentPtr;` where
`codeSegmentPtr == 0x127f00000001` — an address that looks like a 32-bit
integer extended into a 64-bit pointer field.

Backtrace:

```
getCodeToken                    mclib/ablexec.cpp:361
execDeclaredRoutineCall         mclib/ablxstmt.cpp:400
execRoutineCall                 mclib/ablxstmt.cpp:297
execute                         mclib/ablexec.cpp:646
ABLModule::execute              mclib/ablenv.cpp:920
MechWarrior::runBrain           code/warrior.cpp:2160
MechWarrior::mainDecisionTree   code/warrior.cpp:4975
GroundVehicle::updateAIControl  code/gvehicl.cpp:2798
```

### Root cause

The Exodus campaign scripts use ABL keywords the stock parser does not
recognize. Compile produces SYNTAX ERROR + `"Way too many syntax errors.
ABL aborted"`, but mission init does **not** abort on compile failure.
Execution proceeds with partial/garbage bytecode; the VM's internal
function-call tables trust the bytecode without sanity checks;
`codeSegmentPtr` eventually points at garbage; `getCodeToken` dereferences.

Representative compile errors from stderr:

```
SYNTAX ERROR data/missions/warriors/mc2_01_infantry_ambush.abl [line 246] - (type 55) Missing comma "startposition"
SYNTAX ERROR data/missions/warriors/mc2_01_infantry_ambush2.abl [line 188] - (type 55) Missing comma "startbase1patrolpath"
SYNTAX ERROR data/missions/warriors/mc2_01_urbies.abl [line 234] - (type 55) Missing comma "startbase1patrolpath"
Way too many syntax errors. ABL aborted.
```

This is the same bug family the Omnitech memory documents as "uninit
FunctionCallbackTable" — the stock ABL VM trusts its own tables with
no null / sanity / bounds check. On the normal build the garbage address
often happens to land in valid VA space and doesn't AV immediately;
under ASan's shadow-mapped VA the garbage lands in unmapped space and
crashes on first deref.

### Proposed fix (deferred)

Preferred: `ABLModule::compile` return value must be checked by mission
init; any compile error aborts mission load (hard fail). Matches the
tier-1 instrumentation ethos of "fail loud rather than continue
degraded."

Alternative (defensive, keeps partial playability): sanity-check
`codeSegmentPtr` against the compiled module's code-segment range in
the VM hot loop. Cheaper, but hides future corruption bugs.

Orthogonal work: extend the ABL parser to recognize `startposition`,
`startbase1patrolpath`, and other Magic's patch vocabulary so the
Exodus scripts compile cleanly. That removes the trigger but does not
fix the underlying "VM trusts corrupt state" bug — it only means
*these specific* scripts stop triggering it.

### Why this is valuable

Same class as finding #2 (GlobalMap partial-init): a content-format
mismatch leaves corrupt state that the engine dereferences later in a
completely different subsystem. This is exactly the "paper-over fixes
hide real bugs" pattern the ASan MVP was built to expose.

### Confirmed duplicates of this finding

Magic's Unofficial Expansion triggers the same bug in two
configurations, with the same `getCodeToken` backtrace and the same
`0xNN00000001` pointer pattern:

- `MC2-MagicExpansion/` (fresh base-game clone + Expansion `data/`
  overlay), `--mission mc2_01` — ASan log
  `asan-magicexp-mc2_01.15908`. Pointer `0x12d900000001`.
- `mc2-win64-v0.1.1/` + Expansion `data/` overlay, `--mission mc2_01` —
  ASan log `asan-exp-overlay-mc2_01.10484`. Pointer `0x11c600000001`.

This is a content-triggered bug, not install-specific.

---

## 4. `Turret::update` global-buffer-overflow on `turretsEnabled[-1]`

**Status:** bug confirmed, fix straightforward.
**First caught:** 2026-04-24, tier1 matrix run, mission `mc2_03` on
`mc2-win64-v0.1.1` (base game). ASan log:
`tests/smoke/artifacts/2026-04-24T14-14-43/mc2_03.log`.

### Crash signature

ASan fires at `if (!turretsEnabled[getTeamId()])` (turret.cpp:588) —
READ of size 1 at address 1 byte before the global array.

```
==19868==ERROR: AddressSanitizer: global-buffer-overflow
READ of size 1 at 0x7ff6cb96f17f
  Turret::update                 code/turret.cpp:588
  GameObjectManager::update      code/objmgr.cpp:1847
  Mission::update                code/mission.cpp:504
  DoGameLogic                    code/mechcmd2.cpp:2233

0x7ff6cb96f17f is located 1 byte before global variable
  'Turret::turretsEnabled' defined in 'turret.cpp:96:13' (size 8)
```

### Code

```cpp
// code/turret.cpp:588
if (!turretsEnabled[getTeamId()]) {
    targetWID = 0;
}
```

### Root cause

`getTeamId()` returns a signed `long`. Neutral / environmental turrets
have `teamId == -1`. The index `turretsEnabled[-1]` reads 1 byte
before the array.

On the normal RelWithDebInfo build the adjacent memory just happens to
hold another scalar (probably the preceding `Team::relations` 64-byte
array) and the read "works" — neutral turrets end up disabled or
enabled based on unrelated state. Player-facing symptom: neutral
turrets occasionally fail to engage appropriately. Has been in MC2
since 1.0.

### Proposed fix

One-line guard:

```cpp
const long tid = getTeamId();
if (tid < 0 || !turretsEnabled[tid]) {
    targetWID = 0;
}
```

Safer against further team-table changes than the raw index.

---

## 5. ABL `getCodeToken` heap-buffer-overflow (1 byte past code-segment end)

**Status:** bug confirmed, fix straightforward.
**First caught:** 2026-04-24, tier1 run, mission `mc2_10` on
`mc2-win64-v0.1.1` (base game). ASan log:
`tests/smoke/artifacts/2026-04-24T14-14-43/mc2_10.log`.
**Also fires:** mission `mc2_24` same run (finding #6 below — same
root cause, different call-chain and allocation size).

### Crash signature

ASan fires at `codeToken = (TokenCodeType)*codeSegmentPtr;`
(ablexec.cpp:361) — READ of size 1 at exactly 1 byte past the end of
a heap allocation made via the ABL code-segment allocator.

```
==980==ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 1 at 0x12b031b5f5df
  getCodeToken                    mclib/ablexec.cpp:361
  execStatement                   mclib/ablxstmt.cpp:218
  execute                         mclib/ablexec.cpp:657
  execDeclaredRoutineCall         mclib/ablxstmt.cpp:382
  execRoutineCall                 mclib/ablxstmt.cpp:297
  executeChild                    mclib/ablexec.cpp:700
  ABLModule::execute              mclib/ablenv.cpp:1057
  MechWarrior::checkAlarms        code/warrior.cpp:4838
  ...

0x12b031b5f5df is located 0 bytes after 31-byte region
  [0x12b031b5f5c0,0x12b031b5f5df)
allocated by thread T0:
  gos_Malloc                     GameOS/gameos/gameos.cpp:268
  UserHeap::Malloc               mclib/heap.cpp:698
  ablCodeMallocCallback          code/ablmc2.cpp:7432
  createCodeSegment              mclib/ablexec.cpp:267
  routine                        mclib/ablrtn.cpp:1319
```

### Code

```cpp
// mclib/ablexec.cpp:264-276
char* createCodeSegment (long& codeSegmentSize) {
    codeSegmentSize = codeBufferPtr - codeBuffer + 1;           // ← final byte count
    char* codeSegment = (char*)ABLCodeMallocCallback(codeSegmentSize);
    if (!codeSegment)
        ABL_Fatal(0, " ABL: Unable to AblCodeHeap->malloc code segment ");
    for (long i = 0; i < codeSegmentSize; i++)
        codeSegment[i] = codeBuffer[i];
    codeBufferPtr = codeBuffer;
    return(codeSegment);
}

// mclib/ablexec.cpp:359-363
void getCodeToken (void) {
    codeToken = (TokenCodeType)*codeSegmentPtr;  // ← read 1 byte past
    codeSegmentPtr++;                             //    the allocated end
}
```

### Root cause

The allocation holds exactly `codeSegmentSize` bytes. The VM's
`getCodeToken` / `execute` loop reads one byte past the end when it
processes the final statement of a routine — the `++codeSegmentPtr`
advances past the last valid opcode, then something reads the next
byte before the end-of-segment check fires.

On the normal build this is invisible: MSVC's default heap rounds
small allocations to a 16-byte granule, so a 31-byte request gets
32 bytes actual, and the 1-byte overread lands in slack. ASan's red
zones after each allocation eliminate the slack, exposing the bug.

Present in stock MC2 1.0 — has been latent since release.

### Proposed fix

Smallest: allocate `codeSegmentSize + 1` and write a sentinel (e.g.
`HALT_OPCODE` or `0`) at the last byte:

```cpp
char* createCodeSegment (long& codeSegmentSize) {
    codeSegmentSize = codeBufferPtr - codeBuffer + 1;
    char* codeSegment = (char*)ABLCodeMallocCallback(codeSegmentSize + 1);  // +1 for sentinel
    ...
    codeSegment[codeSegmentSize] = 0;  // sentinel; decodes as end-of-routine
    return(codeSegment);
}
```

This keeps `codeSegmentSize` semantically "bytes of actual bytecode" (no
caller needs updating) while the allocation is a byte longer so the
VM's 1-byte overread never AVs.

Alternative: audit `getCodeToken` call sites to find the one reading
past the last valid opcode and add an explicit bounds check. Cleaner
but more invasive.

### Why this is valuable

This is a bug in the base-game ABL VM that has been shipping in MC2
since 2001. It has no visible symptom on normal builds because
allocator slack absorbs the overread. ASan's red zones made it
immediately reproducible on stock tier1 content.

---

## 6. Duplicate of finding #5 on `mc2_24` (same root cause, different trigger path)

**Status:** bug confirmed, same fix as finding #5.
**First caught:** 2026-04-24, tier1 run, mission `mc2_24`. ASan log:
`tests/smoke/artifacts/2026-04-24T14-14-43/mc2_24.log`.

### Differences from finding #5

- Allocation size: **1533 bytes** (vs 31 bytes on mc2_10).
- Call chain top: `MechWarrior::runBrain` (warrior.cpp:2160) rather
  than `MechWarrior::checkAlarms`.
- Same backtrace from `getCodeToken` down; same 1-byte-past-end pattern.

Documented as a separate finding only because it's a separate mission
/ allocation site; the fix is the same.

### Implication

The ABL code-segment off-by-one is not specific to any one module
size or any one call site. The `+1` allocation-size fix at
`createCodeSegment` addresses both in one change.
