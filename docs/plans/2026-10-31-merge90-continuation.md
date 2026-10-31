# Merge window 90 continuation

Assignment: A:/Games/mc2-main, claude/nifty-mendeleev, initial HEAD 47631b0b4.
All four requested tips are ancestors; no additional merges required.
Proof root: E:/mc2-archive-2026-10/proofs/merge90.

Prior receipts: full gate completed (2936 s, exclusive holds 2.2 min), harnesses,
tier1, kit visibility (477 overrides), determinism and all five save/load checks PASS.
Overlap audit ON: all five missions GREEN, mc2_10/24 three local repeats GREEN,
mc2_24 2601 ticks. Farm queue exceeded 59 min. W89 A/B day/night MAD 0.00.

Failure triage:
- REAL: sim_include_firewall new cmponent.cpp -> GameOS/include/mc2fmt.h dependency.
  Extract the pure header-only formatter to mclib/mc2format.h; retain the
  GameOS debugger sink in mc2fmt.h. Firewall remains +0/-0 (265 findings).
  C++ ratchet file key follows the extracted code, with identical limits/counts.
- STALE REFERENCE: quiet signature STATIC_LIGHT_ROW v1 exists in W89; register
  per explicit owner ruling, preserving source output and all limits.
- KNOWN: CVE-G, REAP3, Carver5 content/mission failures; accepted by owner.
- REAL: mod battery cleanup refused read-only staged EXR; retry read-only unlink.
- KNOWN: strict save/load, golden fixture gap, headless mc2_24 LOD exclusions
  remain accepted; no baseline increases.
- NOISY: performance under other active lanes; not a rejection.

Completed: 123 targeted Python tests PASS; sim firewall PASS; C++ ratchet PASS;
W89 day/night montages inspected, no visible differences (MAD rounds to 0.00).
Next: ratchet/failed contract rows, final commit/build, rerun only failed rows;
deploy v0.4 and owner-test (never launch owner-test), reseed farm when idle;
delete merged installs and inactive merged build dirs; export merge90-input;
record receipts/space and report. Owner-only captures skipped while owner away.
