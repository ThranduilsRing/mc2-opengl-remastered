# T3 review fixes

Assignment: A:/Games/mc2-cxxt3, claude/w122-cxx-t3, base ef86f849e0b269ee239537487a97453385c339b8.

- Reverted both job pool owners to raw globals with explicit join-before-delete shutdown.
- Review list: legacy frame pool, fork/join render and sim pools, globalCollisionAlert.
  Reverted the collision alert global too; process exit must not run new static owner destructors.
  Other 03d owners are instance members (sensor pool) or local construction (object type).
- Restored exactly six LG03 baseline sites belonging to the reverted global owners; no regeneration.
- Restored own_inventory rows for Pool, NewPool, GlobalCollisionAlert and one untyped
  job-system free to their exact nifty values; the check caught the prior fix's missing rows.
- Windows startup keeps native path bytes; original CreateDirectory calls did not check failure.
  Preview/movie paths keep Windows bytes, with separator adaptation restricted to Linux.
- Rebased onto 560feb6a8; conflicts only in ratchet baseline head metadata (kept nifty).
  mclib/ablscan.cpp retains replace_filename(fileName).generic_string().
- Added headless subprocess doctest for direct std::exit with live legacy/domain workers.
  Collision alert is not linked into the headless unit target: it depends on mission objects,
  sensor/game globals and engine heaps; its explicit runtime teardown is retained unchanged.
  Windows native-path handling needs Windows filesystem semantics, not Linux doctests.
- Next: ratchet, own_inventory and file_size checks; commit; exact-tip box build/unit/unit-san/
  smoke-lite/D03/saveload, then one lint. Preserve runtime rows before lint overwrites rows.tsv.
- Archive receipts and crash evidence before box cleanup; remove only scratch made by this run.
- Prior base save/load mismatch is documented in 2026-11-05-cxx23-t3.md; do not alter references.

## W122d exact-tip PC proof (2026-11-06)

- Rebased cleanly onto 446d52859; current code tip 6a9dc0492046769e89492b723f5a6543cea75ae9.
- No untracked proofs/ directory existed. Receipts: E:/mc2-archive-2026-10/proofs/cxx23-t3/.
- PC configure and full default Ninja build PASS (1321 steps); no compile fixes needed.
  BelowNormal, CPUs 8-15, six workers maximum; pc-configure.log and pc-build.log.
- C++ ratchet PASS (no increased idiom totals); file-size PASS (38 oversized, max 3363).
  No baseline changes; pc-cxx-ratchet.log and pc-file-size.log.
- Unit contracts running; next: commit state, then one final-tip lane_merge --lint HEAD.
  No deploy or game run.
- Completion requires green build/unit/ratchet/lint receipts; retain raw global owners.
- Exact final tip, all four verdicts, cleanup and TRACK COMPLETE are recorded after
  lint in the proof root's track-state.md, avoiding another tip change after proof.
