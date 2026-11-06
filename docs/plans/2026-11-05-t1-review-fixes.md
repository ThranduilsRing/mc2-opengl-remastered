# T1 review fixes

Assignment: A:/Games/mc2-cxxt1, claude/w122-cxx-t1, base ef86f849e0b2.

- P1: same-mech edit re-entry preserves the editable copy; switching mechs
  cancels the previous edit before taking the next variant snapshot.
- P1 committed a4820453a; P2 committed separately b9744a60e.
- Removed static unique_ptr owners: the vehicle pilot now has explicit teardown
  with LogisticsData; the mech-icon template uses its existing MissionBegin teardown.
- The attempted doctests were removed after the existing engine-linked seam
  failed to compile on Linux: existing tests require process.h, io.h and
  Stuff/stuffheaders.hpp. The standalone mc2_tests target does not link
  LogisticsData/Mech/Variant/Pilot; setMechToModify also triggers campaign/GUI
  data loading unless the cached smart-load gate is disabled before main.
  Rank/skills are private and need FIT pilot fixtures. Testing these production
  functions needs the Windows engine seam or porting that seam and adding
  chassis/pilot fixtures; no new harness or test-only production API was added.
- Ratchet reports LG03 +3 for the requested explicit new/delete teardown;
  no detector, exception, or baseline has been changed to conceal this increase.
- At 2dff3d47e: box build PASS (79s), unit PASS (3797 cases, 9 configured skips).
  Smoke-lite was blocked by other lanes' games at the host lease for ~7 minutes;
  only our waiting process tree was terminated. Saveload did not run.
  Lint FAIL: cxx17 LG03 +3 and generated ownership inventory stale.
- Next: final source build/unit/lint after removing the unportable test addition;
  retain the above lease block rather than retrying runtime rows.
- Populated campaign cross-load cannot run unchanged on the box: capture hardcodes
  <deploy>/mc2.exe rather than the Linux mc2 executable and has no --exe option.
  It needs a portable executable selection plus a populated baseline campaign save,
  the add-variant fixture, and both base/tip binaries for reciprocal load/re-save.
- Finish by archiving receipts and cleaning the box lane and own scratch.

## 2026-11-06 continuation

- Archived untracked proofs to E:/mc2-archive-2026-10/proofs/w122-cxx-t1/pre-rebase-b429a7cca.
- Rebased all 18 commits onto 560feb6a8. Only baseline head metadata conflicted;
  nifty's ABL replace_filename/generic_string and shadow fixes are unchanged.
- Vehicle pilot ownership is now an instance unique_ptr, explicitly reset after
  inventory teardown. This avoids both static destruction and a raw delete above
  nifty's ownership row. The icon retains its existing explicit runtime teardown.
- Restored only the two icon allocation/deletion ratchet sites: code LG03 284
  (nifty 314), icon LG03 1 (nifty 1), missionbegin LG03 10 (nifty 10).
  Ownership icon new=1 and missionbegin untyped frees=3 equal nifty.
- No lifetime doctest added: the edit, vehicle-pilot and icon destruction paths
  require engine-linked LogisticsData/GUI/asset initialization; the Linux seam
  portability and fixture blockers above still apply. Existing tests are retained.
- Next: commit this checkpoint; at that exact SHA run box build, unit, unit-san,
  smoke-lite, determinism:mc2_03 and saveload, then lint. Retry lease-blocked rows
  once, archive final receipts with SHA, and run box cleanup before hand-back.
