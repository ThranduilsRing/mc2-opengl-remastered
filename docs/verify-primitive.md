# MC2_VERIFY — the live data-contract guard primitive

Slice: **MC2-VERIFY-LIVE-1** (roadmap Tier 1 #1, the modernization keystone).
Sources of truth: `.claude/MODERNIZATION-ROADMAP-1.md` +
`.claude/MODERNIZATION-ROADMAP-1-ADVERSARIAL.md` (BINDING amendments A0–A6).

## Why

Every `gosASSERT` in the tree (~1.1–1.6k sites depending on scope) compiles to
`((void)0)` in every build the project actually ships: the guard is gated on
`_ARMOR`, which is set only in the Debug config (`CMakeLists.txt`,
`CMAKE_CXX_FLAGS_DEBUG`) — never in RelWithDebInfo, the only config that runs.
Result: data-contract violations (malformed/imported/mod `.fit` data, OOB ids,
null derefs) corrupt state silently and crash later, far from the cause.

`MC2_VERIFY` is the live replacement: **compiled in ALL configs**, including
RelWithDebInfo, with zero overhead when the condition holds (a single branch;
the failure handler is the cold path).

## The family

Declared in `GameOS/include/mc2_verify.h`; implementation in
`GameOS/gameos/mc2_verify.cpp` (in the `gameos` and `gameos_editor` libs).

```cpp
MC2_VERIFY(cond, "fmt", ...)          // printf-style message; returns bool
MC2_VERIFY_BOUNDS(idx, count, "what") // 0 <= idx < count
MC2_VERIFY_NOTNULL(ptr, "what")       // ptr != NULL
```

The return value is the adoption seam: `true` means "proceed as if the check
passed", `false` (log mode only) means "the caller may degrade gracefully"
(clamp / skip / zero) — which is how a verify can also *bound* the failure it
reports:

```cpp
if (!MC2_VERIFY((long)teamID < MAX_TEAMS, "TeamId %d exceeds MAX_TEAMS-1", teamID))
    teamID = MAX_TEAMS - 1;   // reached in log mode only; off mode = legacy path
```

## Runtime modes — `MC2_VERIFY_MODE`

Resolved once (first use, thread-safe magic static).

| Mode | Behavior on a failed condition |
|---|---|
| `log` (**default**) | Shadow-log: `[VERIFY] file:line (cond) msg` to stderr + `OutputDebugString` + the crash-bundle instrumentation ring (`crashbundle_append`, so a later crash's `last_trace.txt` shows the fires that preceded it). Handler returns `false` → guarded degradation blocks run. Per-process line cap (64) prevents hot-loop floods; counting continues past the cap. |
| `fatal` | STOP with message: same logging, then `InternalFunctionStop` routing (the `STOP()` contract), then a **non-continuable** `RaiseException(0xE0564631 /* 'VF1' */)` so the crash-bundle SEH filter captures minidump + stack + ring, then hard `TerminateProcess`. Never returns — even if an intermediate SEH wrapper (editor `SafeRunGameOSLogic`) swallows the exception. The eventual default once the soak record justifies it. |
| `off` | Exactly-legacy: the handler returns `true` (as if the check passed), so degradation blocks are skipped and legacy behavior — **including the legacy corruption/crash** — is preserved. No output, no counters. |

**Mission-end counter line:** `Mission::destroy` calls
`mc2verify::MissionSummary(missionFileName)` →
`[VERIFY] mission-end fires=<per-mission> total=<process> mode=<m> label=<mission>`
(stderr + ring; silent in `off`). `fires=0` on stock missions is the soak
oracle — positive confirmation the primitive is armed and silent.

## Rules for writing a verify

1. **Condition expressions MUST be side-effect-free.** They evaluate in every
   mode, including `off`.
2. Conditions must be **no worse than legacy to evaluate**: don't introduce a
   read legacy would not (eventually) perform on the same data.
3. Degradation blocks (`if (!MC2_VERIFY(...)) { clamp/skip; }`) must only be
   reachable on *malformed* data — stock content must never enter them, or the
   soak fires and the site is a wrong reclassification.
4. Each converted site carries the comment
   `// MC2_VERIFY reclassified from gosASSERT (slice <SLICE-NAME>)`.

## How future slices adopt it (the binding protocol, adversarial §2d)

1. **Hand-picked sites only. NO mechanical sweeps** — the census showed ~2–8%
   of gosASSERT sites (order 30–120) would fire spuriously or fail to compile
   if blanket-armed.
2. **Shadow-log first:** land sites, soak full tier1 (and ideally a campaign
   soak) in `log` mode. **Zero `[VERIFY]` fires expected on stock missions.**
   Any fire = investigate: a real latent bug (document, decide) or a wrong
   reclassification (revert that site). Only then run tier1 in `fatal` mode.
3. Batches ≤ 50 sites; tier1 per batch.
4. Convert only read→deref / OOB sites where the crashing next line is
   identified.
5. **Editor caveat:** EditRel's `SafeRunGameOSLogic` SEH wrapper swallows
   in-frame faults — the editor-side verdict is a crash-bundle-count diff,
   never rc==0. (`fatal` mode still terminates: the handler calls
   `TerminateProcess` after the raise precisely so a swallowed exception
   cannot resume into the guarded code.)

## UNTOUCHABLE site classes (adversarial census — binding)

These may **never** be converted by any blanket mechanism; each needs an
explicit per-site decision:

- The **80 always-fire `gosASSERT(0/false)`** sites ("not implemented"
  markers, e.g. `gameos.cpp:363` reached by any 3-axis controller) — arming
  one is an instant STOP for a healthy configuration.
- The **32 `_DEBUG`-entangled** sites (asserted variable declared only under
  `#ifdef _DEBUG`, e.g. `move.cpp` `insertErr`) — they do not even compile if
  armed without per-site edits.
- All **`mp*.cpp` / `multplyr.cpp`** sites — dormant, earmarked for MP
  revival (user ruling REVISE-not-RETIRE).
- The **moderate-spurious-fire class** flagged by the census (optional-ish
  .fit keys: `camera.cpp:209/231/243`, `logisticspilot.cpp:108`,
  `saveload.cpp:836`; tolerated-garbage bounds: `mapdata.cpp:1564`;
  survivable inconsistency probes: `move.cpp:3726`) — these encode invariants
  the shipped game routinely tolerates violating.

## First reclassification pass (this slice)

`code/mission.cpp` Mission::init — 20 verifies / 18 sites:

- **(a) commanders-null**: verify aligned in front of the existing
  CRASH-HARDEN STOP (`b71702ba`); fatal stops at the verify, log/off keep the
  legacy STOP path bit-for-bit.
- **(b) the marquee (adversarial A5)**: file-supplied `TeamId`/`CommanderId`
  (chars, up to 127) raised `maxTeamID`/`maxCommanderID` unchecked and the
  init loops **wrote past** `Team::teams[MAX_TEAMS=8]` /
  `Commander::commanders[MAX_COMMANDERS=8]` — static-memory corruption
  reachable from any hand-made/imported map. Verified + bounded at both the
  read site and the write site.
- **(c)** SP `commandersToLoad` loop: id bounds + created-slot null verify
  before the `setTeam` deref. (The MP-gated `:2700` OOB left untouched — it
  matters only under the MP-revival earmark.)
- **(d)** `[Parts]`/`NumParts`/per-part block contract, `[Warriors]`/
  `NumWarriors` contract + `MAX_WARRIORS` bound, warrior profile open/init,
  `Brain` key, `warriorList[i]` null.

## Env reference

| Var | Values | Default |
|---|---|---|
| `MC2_VERIFY_MODE` | `log` / `fatal` / `off` | `log` |
