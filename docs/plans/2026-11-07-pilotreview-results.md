# pilotreviewarea / mechlopedia LG03 + LG12b results (2026-11-07)

Done: code/mechlopedia.cpp, 12 LG03 sites. Every `new MechlopediaListItem` / `new aTextListItem` that was handed to a list box
is now `std::make_unique` and hands over with `.release()` into AddItem/InsertItem (the idiom already used at the top of the same file).
Side effect: three list-item allocations that were abandoned (the `continue` after the allocation in the vehicle loop; the
overwritten `pItem` in `MechScreen::setMech` for vehicles) are now freed instead of leaked. No ownership or lifetime of anything else changes.

Skipped, with reason:
- LG12b, all 28 (pilotreviewarea 9, mechlopedia 19): every format string is a runtime resource string from `cLoadString`
  (`IDS_PILOT_*`, `IDS_EN_*`). `std::format_string` needs a literal; the step-3 ledger (.claude/cleanup/step3-lg12b-ledger.md)
  already lists this class as "stays on sprintf"; a runtime-printf helper would be a new helper. Nothing converted, so no doctest.
- LG03 pilotreviewarea.cpp (about 70): all are class-static raw pointers (`s_area`, `s_outline[]`, `s_icons[]`, ...) created in
  `init()` and deleted when `s_itemCount` reaches 0. Static unique_ptr would destroy aText/aRect/aAnimation during static
  destruction after the engine shut down; commit fc4786a2f (docs/plans/2026-11-05-T6-exit-ownership.md) reverted exactly this
  pattern on this branch's lineage. Needs an explicit teardown owner, not unique_ptr; not done.
- LG03 mechlopedia.cpp: `subScreens[6]` (6 `new` of screens, Mechlopedia dtor is empty and never freed them; adding delete
  changes destruction order against `listBox` they borrow) and static `s_templateItem` (static owner, same exit hazard).
