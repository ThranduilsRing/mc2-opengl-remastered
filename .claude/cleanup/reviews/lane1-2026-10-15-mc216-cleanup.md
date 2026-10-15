<!-- doc-status: current | reviewed:2026-10-15 -->
# Lane 1 cleanup review, 2026-10-15 evening (mc2_16 race + B3 close)

Reviewed commits: 110251737 82cff74b1 375f7dfa9 76174cd44 5c5efec0e. This was a read-only review: nothing was built, launched or edited.
VERDICT: **0 blockers, 4 should-fix, 5 nits.** The mc2_16 fix is correct and minimal.

## mc2_16 fix (82cff74b1)
- The shared state is the archive's `FILE*` (a seek followed by a read) and its LZ buffer. The parse also uses the slot table that `File::open` claims, and that is shared as well. After this commit nothing else reads through the archive concurrently. `git grep std::async/std::thread` finds only `code/logistics.cpp:175` (below) and `code/job_system.cpp`. The job system's users (terrobj, objmgr, mission, msl) are per-frame jobs that do no file I/O.
- A successful parse behaves exactly as before. The per-job body is unchanged, and so are the merge order (buildings.csv) and the refCount and ownership handling. A file that fails to open logs one Warn per file and is skipped, the same skip as before.
- **should-fix** `82cff74b1` commit msg / BUGS row 37: the startup-time cost of going serial is never stated. Under smart-load it lands on the first mech-bay access; with `MC2_SMART_LOAD=0` it lands in `initializeLogData`. Fix: add the measured `initVariants` ms before and after (Tracy or a timer) to row 37 / notes 10.6.
- **should-fix** `code/logisticsdata_init.cpp:477`: a variant that fails `init` is deleted with no log line. That breaks ruling 5 ("every reader touched rejects malformed content with one log line"), which the after-review section 2 claims for this reader. Fix: log the file, row and varCount before the `delete`.
- **nit** `code/logisticsdata_init.cpp:460`: the Warn does not say why the open failed. Fix: include the `open()` return code.
- The WHAT+WHY comment at :452-455 is good. It describes the race, not the steps.

## Logistics::begin / initializeLogData (`code/logistics.cpp:166-201`)
- **Verdict: no live race today.** The `std::async` is launched at :175 and joined at :200, inside the same `log_SPLASH` case. Between the two, the main thread only runs dead `bTestScript` code and `setLogisticsState`, an inline field write (`logistics.h:90`). The main thread reads no files in that window, and the intro movie does not run inside it. Under smart-load (the default), the thread only runs Phase A (`campaign.fit`) and never reaches `initVariants`.
- **should-fix** `.claude/cleanup/cxx23-step4-b3-after-review.md:44`, open item 5 (and the matching lane1-state OPEN line): it says the main thread "can read files (intro movie)" during the init. That is wrong, because the join at :200 comes first. Fix: restate it as "the thread gives no overlap; a hazard only if someone moves the join".
- The minimal safe fix (owed, not urgent) is to replace the `std::async` at :175 with a direct `initializeLogData()` call and delete `m_initFuture` with its three joins (:167, :200, :395). The async buys nothing today. Its comment at :171-174 is stale: it says the movie overlaps and calls the FastFile globals read-only, and both are false. A per-thread FastFile read path is not needed.

## saveload_roundtrip (110251737)
- `tools/saveload_roundtrip.py:395-405`: the check reads the baseline, fills unset frames from it, and refuses contradicting flags before `run_fixture`. The new selftest pins both behaviours: the frames reach `run_fixture`, and a refusal leaves `fixture.called` false. The selftest is honest.
- **nit** `tools/saveload_roundtrip.py:403`: a baseline with no `save_at`/`load_at` produces the refusal "recorded at --save-at None". Fix: name the missing key and say "re-record with --baseline".
- `baseline` mode (base=None) still defaults to 1000/1100. That matches the help text.

## FST test (110251737)
- **nit** `tests/unit/test_fst_roundtrip.cpp:222`: the every-slot loop passes vacuously if `getNumFiles()` is 0. Fix: add `REQUIRE(ff->getNumFiles() > 0)` before the loop.

## B3 after-review (375f7dfa9)
- It follows the B1/B2 shape: what shipped, rules held, rules bent, open items, and a KPI before/after table (census 132/55 -> 35/21, own_inventory, statics, cxx17 15,993 -> 15,818). The 35-site breakdown sums correctly (18+4+8+4+1).
- **nit** section 6: the cxx17 "before" includes merged work from other lanes, so the delta is not lane 1's alone. The table says this. Fix: add lane 1's own delta if it was measured, or leave as is.

## B4 notes (375f7dfa9)
- **should-fix** `.claude/cleanup/cxx23-step4-b4-notes.md:12`: the census is a hand grep. No command or script is recorded, so the 321/22 count cannot be re-run, and the census is not sealed. Fix: paste the exact `git grep` command and file list, or state plainly that sealing is B4.0's job (`deprecate_b4` probe).
- The rules are consistent with the plan: one subsystem per commit, at most 25 files, and a proof per batch. The claims it needs are named in section 3.1 (radio.cpp, gamesound.cpp, collsn.cpp), along with the render-proof ruling for B4.2/B4.3.
- **nit** section 1: "unequal pairs ... are the first thing B4.0 explains" is fine. B4.5 crater "rides with B4.2" contradicts "one subsystem per commit". Fix: say it is a separate commit in the same proof run.

## Journal baseline (76174cd44)
- The reason is correct. It starts from 6498 at 7fd4c954a, subtracts the one FIT-NUMBER-1 arc-id stripped in 110251737, and lands at 6497. The rev is a real commit.
- **nit** `tools/journal_comment_ratchet.baseline.json`: the `batch-tag` key (10) disappears without a word in the message. Fix: one line naming where the 10 went (lane 5's strip).

## Efficiency retrospective
- Reading the five diffs in one `git show` and then grepping only the two thread sites was enough. No subagent or build was needed.
- The Logistics::begin verdict came from reading 40 lines of `logistics.cpp`, not from the after-review's description, which was wrong.
- The B4 census could not be checked without its command. Docs that record counts should carry the exact command.
