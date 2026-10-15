# Lane 1 cleanup review: B3.4b session (2026-10-15)

Read-only review pinned to abbc6afc8 7c3456d1f 2cc50eb68 55f62bc4d ced027df9 2cbd20486 d4bb734a0 0f413f7a6 96f699f7d.
Counts: **0 blockers, 1 should-fix, 5 nits.**

## Checks that passed
- **F4** (`mclib/file.cpp:1280-1290`): the oversized reject now calls `closeFast`, nulls `fastFile`, sets the handle to -1, clears
  `m_fileName` and `length`, and logs the size unsigned. The test (`tests/unit/test_fst_roundtrip.cpp:191-225`) is real. It patches
  `realSize` in an authored FST to 0x50000000, checks that the archive reports that size, then asserts FILE_NOT_OPEN, `!isOpen()`,
  a null name and `sizeFast(h) == -1` (the entry is released).
- **fit_number.h**: the three bodies match `FitIniFile::textToFloat/Long/ULong` (`mclib/inifile.cpp:245/311/467`) apart from
  whitespace on blank lines. At 96f699f7d, inifile.cpp is byte-identical to abbc6afc8, so existing callers behave exactly as before.
  The lockstep doctest (`tests/unit/test_fit_number.cpp:60-73`) calls the real members through a `FitProbe` using-declaration. It
  feeds separate writable copies to each side and covers the three divergent inputs ("7 // 0xff", "0x-1", "0X16") plus five more.
- **B3.4b**: `MissionLoadFrame f` is a local of `Mission::load` (`code/saveload.cpp:917`), the same scope that held the old raw
  pointer. The value is never copied: every phase takes `FitIniFile* const = &f.gameSystemFile`. `FitIniFile::close` checks
  `isOpen`, so the explicit close followed by the destructor's close is safe. `code/mission_init.cpp` is not touched by any of
  the listed commits.
- **pak.cpp**: both `PacketFile`s are locals, and the leak on the `create` error path in `pack()` is gone.
- **Debug-build files**: `OpenWeaponFireLog` had no caller, so deleting it is dead-code removal. The gesture.log `logFile`
  global has no remaining references in `mclib/mech3d*`. All six longpath reports are now `File` values, and `writeDebug(&f)`
  keeps its signature. Every edited line is under `_DEBUG`, so release builds are unchanged.
- **d4bb734a0** (`tools/saveload_roundtrip.py:349-352`): on a frame mismatch it returns one line naming the baseline's frames.
  The digest and runs comparison is unchanged, and the selftest covers the new path. All five baselines carry `save_at` and `load_at`.
- **Ratchet rewrite (0f413f7a6)**: the commit message lists this lane's falls and its intended rises (LG06 +3, LG09 +2,
  dup_lines +56 for the fit_number.h copy) and states that no merged rises were absorbed. The N6 correction of 4e322fdd8 is named
  separately.
- **CRLF**: none of the six code commits adds a `\r`. The include that a CRLF edit lost (2cc50eb68) has no lasting effect,
  because 55f62bc4d restored inifile.cpp wholesale.
- **Comments**: they state WHAT and WHY. The one rule-shaped comment (`fit_number.h:15-16`, the "keeps a verbatim copy" note)
  names the condition for removing the copy.

## Findings
| # | Sev | Where | Finding | Fix |
|---|---|---|---|---|
| 1 | should-fix | `tools/saveload_roundtrip.py:424` | The frame check runs only after a full game run, so `--check --mission mc2_03` without `--save-at 1300 --load-at 1400` burns a host lease and a launch just to be refused. | In check mode, take `save_at` and `load_at` from the baseline before launching, and keep the refusal for explicit flags that conflict with it. |
| 2 | nit | `mclib/fit_number.h:19` | The comment says "the argument must be writable" but the signature is `const char*` (inherited cast-away). | Note that the const is a lie kept for caller compatibility, or take `char*` when the members forward. |
| 3 | nit | `tests/unit/test_fit_number.cpp:70` | `textToFloat` is compared with `==` on floats. This is fine for identical `atof` bodies but is the only non-mutating case. | Leave it, or add a hex-looking float input ("0x1p3") so `atof` hex parsing is pinned on both sides. |
| 4 | nit | `tests/unit/test_fst_roundtrip.cpp:216-219` | After `ff->closeFast(h)`, `sizeFast(h) == -1` assumes `File::open` reacquired the same slot `h`. | Re-run `FastFileFind` after the reject and assert that slot is not in use, rather than reusing the stale `h`. |
| 5 | nit | commit ced027df9 message | It claims gamesys.fit "leaked" on an early return of `Mission::load`, but the only early return (`openSaveFile`, `saveload.cpp:921`) comes before `loadGameSystemRanges` opens it. The real gain is on an exception or assert-continue path. | Correct the claim in notes 10.4 (commits stay as they are). |
| 6 | nit | `.claude/cleanup/cxx23-step4-b3-notes.md:142` | "MechData packets 5828-5974" reads as bytes in hand-offs (this review's brief said "bytes"). | Write "objList packets 5828-5974 (mech slots)" and record which packet differed first. |

## mc2_16 opinion
5828-5974 are **packet indices, not byte offsets**. In an mc2_16 save, packets from 5827 onward are objList slots
(`saveload-fidelity-2026-10-11.md:62-63`; `MechData` is 3992 B, `code/save_layout_guard.h:47`). The failing digest therefore covers
the whole run of mech records, and no single field can be named from the evidence on file. The resave dir holds only save1/save2
of one run, with no quiet-host twin. The candidates inside `MechData` (`code/mech.h:~285-330`) are the time and "this frame" fields:
`lastJumpTime`, `centerTorsoBlowTime`, `damageThisFrame`, `hitFromBehind/FrontThisFrame`, `left/rightArmBlownThisFrame`,
`torsoRotation`/arm rotations and `rotateValues[6]`. The run used MC2_SMOKE_FIXED_TIMESTEP=1 and MC2_SMOKE_FREEZE_ANIM=1, and the
save fires at a fixed sim frame. Under that design none of these fields should depend on frame rate.
- If one of the "ThisFrame" flags or accumulators is cleared per render frame rather than per sim step, a loaded host (several sim
  steps per render frame at 70 fps) would save different bytes. That would be a real determinism defect, not noise.
- So would a wall-clock read (`gos_GetElapsedTime`) inside the mech update.

Recommendation: re-run under load, diff save1.ims packet by packet against the quiet-host save with the mask off, and map the first
differing offset through `save_layout_guard.h`. Until then, treat the result as an OPEN determinism question rather than accepted
flakiness. Do not widen the mask.
