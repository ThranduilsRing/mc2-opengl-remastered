# T1 review fixes

Assignment: A:/Games/mc2-cxxt1, claude/w122-cxx-t1, base ef86f849e0b2.

- P1: same-mech edit re-entry preserves the editable copy; switching mechs
  cancels the previous edit before taking the next variant snapshot.
- P1 committed a4820453a; P2 committed separately b9744a60e.
- Removed static unique_ptr owners: the vehicle pilot now has explicit teardown
  with LogisticsData; the mech-icon template uses its existing MissionBegin teardown.
- Added engine-linked doctests in mc2_authored_order_seam for both edit sequences
  and unequal pilot ranks in both orders (opposing gunnery priorities).
  Run with MC2_SMART_LOAD=0 and --test-case=LogisticsEdit/*, without GUI/assets.
- Ratchet reports LG03 +3 for the requested explicit new/delete teardown;
  no detector, exception, or baseline has been changed to conceal this increase.
- Proof pending at final tip: box build/unit/smoke-lite/saveload, seam, lint.
- Populated campaign cross-load cannot run unchanged on the box: capture hardcodes
  <deploy>/mc2.exe rather than the Linux mc2 executable and has no --exe option.
  It needs a portable executable selection plus a populated baseline campaign save,
  the add-variant fixture, and both base/tip binaries for reciprocal load/re-save.
- Finish by archiving receipts and cleaning the box lane and own scratch.
