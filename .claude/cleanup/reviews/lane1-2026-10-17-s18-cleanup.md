# Lane 1 s17/s18 cleanup review (c12fcb9c5..f1d3f4260), focus B7o.3

Read-only pass (no build, no launch). Read: 911f6c5fd, 2b57b916e, 732cc0f19, code/appearance_owner.h, gameobj.h/.cpp, every actor destroy() and init replacement path.

## Verdict
GO. B7o.3 is correct as designed. No must-fix. Two proof gaps and a debt-entry hardening are should-fix.

## Must-fix
None.

## Should-fix
1. Proof gap, reload gen=2: the only evidence that nothing holds a freed appearance across a reload is one reload (gen=1). The behaviour change (replaced and leftover appearances are now freed) turns a stale holder from a harmless leak into a use-after-free. Run the ASan or build-allocstats reload with S10_RELOADS>=3 until gen=2 prints, or state in lane1-state that gen=2 is unproven.
2. Proof gap, survivor sites: 12 of 57 printed. "None is an actor appearance" is shown only for the subset. Print all 57 (or filter by the Appearance allocation tag) and record "0 of 57".
3. RESOURCE_STRING id-0 debt entry (732cc0f19): reasonable stopgap, not a B7o regression. The report came from 560fc7b28 (2026-10-14, F4-a) and 0f9b069c7 (2026-10-16, presence preserved), so the noise most likely started 10-14 to 10-16 as visibility of an old silent miss. Weaknesses: (a) it silences every missing_id/null_handle event globally, so a new real miss would be hidden; (b) no expiry, against the one-week rule in CLAUDE.md. Finding the caller is cheap: have report() log a one-shot backtrace or break on gos_ResourceStringLength with id 0. Add an expiry (date or BUGS-row check) at minimum.
4. adoptOwnedView(owner, view, p) with p == owner.get() makes reset(p) delete the live object and then store the dangling pointer. No caller does this today (all pass new or nullptr), but the helper is now the single choke point: add an early return (view = p) for that case plus one doctest.

## Nits
- static_assert on the unique_ptr size sits between member function bodies in gameobj.cpp; move it next to the include.
- ~GameObject and releaseAppearance can each log a divergence for the same object; harmless.
- verifyAppearanceView(const char*) justification for LG06 +2 is fine; no action.
- `appearance` is still public; the census is the only seal (already recorded OPEN).
- Keep one number for the sealed list (129 vs the rev 4 text 123) in the notes.

## Checked-OK
- View/owner divergence: only adoptOwnedView/releaseOwnedView write `appearance` (census: 0 raw writers). reset(p) frees the previous before the view takes p.
- Double delete: no remaining raw `delete appearance` in actor code. Every destroy() releases (view null), so the member destructor sees null. Appearance has a virtual destructor (mclib/appear.h:154), so the base-typed default deleter is correct; size static_assert enforces the default deleter.
- Pool zero-fill (G4): operator new memsets raw storage before the constructor, so the unique_ptr is constructed over zeroed bytes afterwards. That is not UB for the member (no live object yet). Nothing memsets or memcpys a live GameObject (grep of memset(this, sizeof(GameObject) found none). Copying GameObject was already removed in B7o.2 and is now compiler-enforced. operator delete frees pool memory after member destruction.
- Release order (G2): every destroy() calls releaseAppearance() at the old delete position. BattleMech retires the RenderWorld handle then releases; Mover releases before contactInfo; TerrainObject frees sub-areas and buffers first. Every derived destructor calls destroy(), so the backstop never skips destroyMech.
- Replacement paths: Mech init releases then adopts; GameObject, Mover and Turret init adopt(nullptr) now free a prior appearance (previously leaked); Artillery::init calls destroy() first.
- Stale holders: no non-actor member stores an actor's Appearance (gamecam compass and logisticsvariant own theirs). Readers (mission.cpp scan, gui probes, weapon hot-spot lookups) call getAppearance() per use on a live object. Residual risk is a local pointer across a destroy in one frame, covered by sim x4 identical chains, saveload digests, ASan doctest and tier1.
- Doctests exercise the production header helpers; adequate since GameObject cannot link into mc2_tests.
- Baseline and census edits match the stated numbers (own_inventory, cxx17 LG03 -9, LG06 +2).
