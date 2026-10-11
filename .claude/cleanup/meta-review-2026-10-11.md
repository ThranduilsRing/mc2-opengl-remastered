<!-- doc-status: current | reviewed:2026-10-11 -->
# Meta-review of the modernization program (read-only, 2026-10-11, nifty HEAD 4a6659fea; lane 2 tip c7168ff98)

Scope: the PROGRAM (plan, ordering, proofs, process, docs), not the per-step Opus reviews. Nothing built or run. Claims checked
against git/tool code are listed in section H; `[unverified]` marks the rest. Line cites are HEAD unless stated.

## A. Verdict
The program is unusually well-proven for what it proves: no-op text rewrites (objhash), moved code (split_verify/ph_verify),
format conversions (corpus + log shapes) and gate semantics (8-probe planner + Opus re-reads) are each backed by a mechanical
check that fails closed, and the deletions-before-tidy ordering was right. The proof system is, however, almost entirely
"same as before" proofs over a narrow runtime sample (two captured missions, five tier1 missions, GL/VK default captures), and the
two steps that remain (4 ownership, 5 math) are exactly the ones where "same bytes" cannot be the proof. The ingredients of the
proof those steps need (sim digest, deterministic RNG, fixed timestep, tier2 = all 24 missions, tier3 = mods) already exist in the
repo but are not wired into any cadence, and no save/load test exists at all. Process-wise the two-lane model works but the
handoff has become a 121 KB append-only log, the memory and the handoff disagree on at least two rulings, and about half of the
recent incidents were fixed locally rather than structurally. Approve the next quarter only with the step-4/5 prerequisites in E
landed first; reorder step 2 away from headers that step 4/5 will delete.

## B. Ranked recommendations
1. **Determinism harness before step 4 B1** — what: a smoke row that runs one mission twice with `MC2_SMOKE_FIXED_TIMESTEP=1
   MC2_DETERMINISTIC_RNG=1 MC2_SIM_IDENTITY_TRACE=1` and diffs the `[SIMID v1]` lines; why: the step-4 plan's exit criterion
   "SIMID unchanged" (cxx23-step4-ownership-plan.md §5.6/§8) is vacuous until run-to-run identity at HEAD is demonstrated; evidence:
   `code/sim_identity.cpp:226-249` emits the line, `Gates/gate_table.json` rows 156/630/687-689 and `MC2_SMOKE_FIXED_TIMESTEP`
   exist, but no smoke artifact carries a digest (grep of `tests/smoke/artifacts/2026-10-11T08-*` finds none) and `scripts/sim_identity.py`
   is a capture refuse-to-grade gate, not a smoke row; effort: 1 session (lane 1); who: lane 1 + owner ruling if HEAD is NOT deterministic.
2. **Save/load round-trip proof before step 4 B3 and step 2 LG14** — what: save at sim frame N, load, compare SIMID digest + packet
   bytes; why: no unit or smoke test covers save/load (grep of tests/unit for saveload/SaveGame = 0); Mission::load's phase-helper
   proof was one re-save byte identity (handoff line 141); enum class (LG14) and ownership (B7 object ids) both touch serialized state;
   effort: 1–2 sessions; who: lane 2 (owns saveload.cpp splits).
3. **Step-2/3 skip list = step-4/5 targets** — what: `rewrite_defines.py`/tidy exclude `estring*`, `elist.h`, `heap.*`, Stuff
   containers/streams, `*_internal.h`, MLR, Stuff math; why: step 4 deletes or redesigns them (plan §2), step 5 replaces Stuff math,
   MLR is retirement-dated (queue 13); the step-2 plan excludes only dirty/lane files; evidence: lane 2 has 31 LG13 commits, 93 header
   edits, 301 dirty files in flight (`git -C mc2-lane2 status`); tidy batch 1 already spent fix-its on MLR light files and
   `mlrprimitivebase.cpp` was reverted (handoff line 138–139); effort: hours; who: lane 2 now.
4. **Fix the 50 overload-sensitive TUs before step 4 touches them** — what: write the float/double math explicitly in the 50 TUs of
   legacy-subsystem-audit.md §D (or add `<cmath>` to all 50 under the determinism harness and accept one measured delta); why: step 4
   adds `<memory>`/`<vector>` includes to `mover*`, `objmgr*`, `mech_update`, `gvehicl*` etc., which makes `sqrt(float)` visible and
   silently changes sim arithmetic — the same hazard the PCH audit found (`tools/pch_audit.py`, 122 skip TUs in
   `cmake/pch/pch_skip.cmake`); effort: 1 session + harness from #1; who: lane 1.
5. **lane_merge must verify the merged tree** — what: after the follow-up commit run build + doctest + `check-contracts --rows` and
   record `{sha, build, doctest, board, lite}` in a committed `verified.json`; why: `tools/lane_merge.py:62-68` runs five text
   ratchets only; the merged tip is sometimes built (handoff 113) and sometimes only "ratchets OK" (141, 143); effort: 0.5 session; who: lane 1.
6. **tier2 (24 missions) at every program boundary; tier3 (9 mod missions) before step 4 B5** — evidence: `tests/smoke/smoke_missions.txt`
   defines them; no tier2/tier3 run appears anywhere in the handoff; the whole program's runtime evidence is mc2_01/03/10/17/24;
   effort: runtime only; who: whichever lane holds the lease.
7. **Split the handoff** (section G) and fix the two recorded rulings that disagree (section 5 below); effort: 1 hour; who: orchestrator.
8. **Delete `scripts/smoke_lib/cursor_guard.py` (803 lines)** — its only justification (old exes that grab the cursor,
   docstring lines 4-11) is now impossible: `exe_guard.check_exe` refuses any exe older than `MIN_COMMIT=ffebf5e87`
   (`exe_guard.py:48`) inside `nofocus.spawn`. The second cursor incident was caused by this guard, not by the game. Also delete
   `nofocus.py` `park_window_offscreen` (launch-site-inventory.md §4 "dead") and `smoke_lib/isolation.py` (780 lines, wired to nothing).
9. **Ratchets freeze-only outside the current step** — `--tighten` only for the step's own idiom(s); why: ratchets are shaping code:
   concepts added to `mc2fmt.h:35` to satisfy LG07, `string_view` parameters added "to hold LG06" at
   `gos_static_prop_batcher_internal.h:649`, `vulkan_terrain_island.cpp:48`, the `loadSpv` wrappers (handoff 116, 119, 128),
   `dc1957de5` restructured code to clear reds (legacy-deletions-review F6). Commit messages should carry `ratchet:` when an edit
   exists only for a ratchet.
10. **Gate amnesty next quarter, not the 50 LEFT files** — 132 default-off Feature rows remain (`Gates/gate_table.json`: 1139 rows,
    195 Feature, 132 default-off, 156 Trace); the typed registry makes each deletion a row + a reader; the 50 files >1500 are cohesive
    classes/headers by the survey's own verdict (size-tier-1000-survey.md "LEFT").
11. **Step-4 batch size** 30–60 files per batch (plan §6) is too large for the Opus review model that caught the G5 lambda landmine;
    cap at one subsystem AND ≤ 25 files, with the per-type free-site inventory generated, not prose.
12. **Record the proof relaxation** of lane 2's `canonical_obj.py` (order-blind, `.data/.bss` layout-blind digest is the default;
    plain objhash only under `--strict-proof`) in the step-2 plan and the handoff, which call it "objhash-proved".
13. **One verification ledger instead of prose** — a `tools/verify_at.py` that tidy_batch, lane_merge and the lanes call, writing
    `verified.json` rows; half of the handoff's 120 state bullets are "verified: doctest N, board N/N, tier1 5/5 at sha" sentences.
14. **Archive the 36 stale `.claude/cleanup` docs** (`doc-status: stale` headers; 67 docs, 7,631 lines) to the E: tarball pattern the
    owner already uses; keep plans, reviews, ledgers, state files.
15. **Decide multiplayer now**: 2,994 lines of stubbed `code/multplyr*` + `dmultplyr.h` are untouched by every proof (no harness
    exercises them) and the owner wants a new transport later; either delete with provenance (recoverable) or exclude from steps 2–4.

## C. Proof-gap table
| regression class | currently caught by | hole | smallest fix |
|---|---|---|---|
| sim behaviour on the 19 non-tier1 campaign missions | nothing in cadence | tier2 exists, never run in the program | tier2 at program boundaries (B6) |
| mod content (tier3, .fx/.fit/.abl readers) | nothing | tier3 exists, never run; inifile/memorystream repairs changed mod-reachable paths | tier3 before step 4 B3 |
| save/load format and object ids | one re-save byte compare (handoff 141) | no test; LG14 + B7 touch it | round-trip test (B2) |
| sim determinism (RNG order, tie order, float) | goldens' MAD; "mech-noise only" verdicts | SIMID not in smoke; `MC2_DETERMINISTIC_RNG` default off; artifacts carry no digest | determinism row (B1) |
| editor behaviour | 15-case editor smoke, `editor_pass_parity` row | objhash covers only built arms; editor objects not hashed by tidy_batch (compile only via build_all_targets) | hash editor objects in header batches |
| VK-only island paths | default VK captures (umbrella armed once, handoff 128) | `SubmitDrawCommands` VK hand-off helper proof+compile only (132); shadow island mech casters parked | armed-VK capture in lite loop weekly |
| first-frame behaviour | found by accident (frame-2 frustum NaN, 47a9623ed) | captures settle first; log_identity masks numbers | `[SIMID v1]` at frames 1–5 in the determinism row |
| numbers in converted logs | corpus (literal formats) | 37 runtime-supplied formats, 88 value-used printf, `%p`/hex case invisible to log_identity (sets, not counts) | count-aware log_identity for parsed tags |
| math overload visibility | pch_audit at PCH time only | any new include in the 50 TUs (step 4) | B4 |
| Linux path | none (deferred; `LINUX_BUILD` is defined on Windows, CMakeLists.txt:194) | every step edits Linux-arm code blind | state "Windows-only" in the plans, or a cl /P of the Linux arm per batch |
| multiplayer | none | stubbed, 2,994 lines | B15 |
| merged tree (two lanes) | ad hoc | lane_merge runs text ratchets only | B5 |
| comment-pass → checker needles | comment_guard needles (430) | needles are a heuristic; 8 rows went red once | board run is already in the loop; keep |

## D. Ordering changes
1. Insert "determinism harness + save/load round-trip + 50-TU math fix" as step 3.5 (before step 4 B1 and before LG14).
2. Step 2 header batches: skip step-4/5/MLR targets and every `*_internal.h` (extern-state inventories that step 4 will turn into
   owned state structs; rewriting them twice is waste and a merge hazard with lane 1).
3. Step 3 remainder (SPEW ~180, OutputDebugString ~19, value-used 88, lane-2 files): fix the facade's final API (categories, sinks,
   P3) first so those sites convert once; B4's header-only `mc2::fmt::print` is otherwise a second migration later.
4. LG14 (enum class) after the save/load test and after the `enum_census.py` ledger exists (plan says so; keep it that way).
5. Step 5 opens with the bit-identity harness (ruled); add: the harness must replay real call sequences (matrix chains in
   `Stuff::` row-vector order, `axis_convert.h`), not element-wise ops, and run under the same `/fp:precise` flags as the game.
6. Gate amnesty before MLR retirement: MLR retirement needs a VFX parity oracle nobody has built; amnesty needs only the registry.

## E. Requirements for the step 4 and step 5 plans (approval bar)
Step 4 (plan `cxx23-step4-ownership-plan.md` is a good start; it must add): (a) B0 proves run-to-run SIMID identity at HEAD under the
determinism gates, else the exit criterion is dropped and replaced; (b) a generated per-type free-site inventory committed per batch
(`tools/own_inventory.py`), the plan's "lists every free site" (§7) as a file the reviewer diffs; (c) the pointer-ordering scan
(`map<T*`, pointer `<`, pointer hashing) committed as a baseline before B1; (d) `build_all_targets.py` + editor smoke per batch that
touches a shared header (editor shares AppearanceType/shape caches, plan §7 last line); (e) no new includes in the 50 overload TUs
until B4 of this review lands; (f) ABL arena (§2 ABL) only after a save containing ABL state round-trips (saved ABL state is format,
plan §2); (g) decision points §9.1–9.4 answered by the owner before B1 (external modder tools, `.fx` ABI, MLR, ALLOC_STATS shipped);
(h) batch cap one subsystem/≤25 files with an Opus review per batch, because the G5 landmine shows planners' equivalence models miss
multi-statement bodies; (i) game-object handles (B7) need the `objmgr_contract_harness` extended with generation checks before the
batch, not after; (j) tier3 once before B3 (file/stream readers are mod-reachable).
Step 5: (a) glm pinned by sha in `3rdparty/`, `GLM_FORCE_*` defines fixed in one header; (b) the doctest corpus covers each `Stuff::`
operation AND the composite sequences the sim uses (transform chains, `MultiTransformShape`, pathfinding distance math), bits compared,
`/fp:precise`, no contraction; (c) a smoke determinism row is the second oracle (same as step 4 B0); (d) render-side migration proven by
captures (day+night, GL+VK, armed islands) and `gpu_abi` generator unchanged; (e) the 50-TU explicit-precision fix precedes any swap;
(f) exit rule stated now: sim migrates only if the corpus is bit-identical; otherwise `Stuff::` stays for sim and the plan says which
files are sim.
Feasibility of the sim checksum: high. `sim_identity.cpp` already digests movers (graded/visual/strict), `det_rng.h` + the
`gameos.cpp:296` latch give a seeded LCG, `MC2_SMOKE_FIXED_TIMESTEP` pins the clock. A determinism harness here = run_smoke
`--determinism` (two launches, same seed, fixed step, trace on) + a comparator that prints the first divergent sim frame and its mover
line (`[SIMID v1 mover]`, `MC2_SIM_IDENTITY_TRACE_DETAIL`). Unknown until run: whether HEAD is deterministic at all [unverified].

## F. Process and tooling changes
Keep: two lanes with file-disjoint worktrees; `lane_merge.py`/`handoff_append.py` (pathspec-only commits are the structural fix for the
sweep incidents); exe_guard inside `nofocus.spawn` (structural fix for the golden-exe incident); objhash/split_verify/ph_verify;
tidy_batch's all-targets + mc2fx-dump + doctest loop; state files + lane rotation; Opus per step; the flake log.
Drop: `cursor_guard.py` (B8); `isolation.py`; `nofocus.park_window_offscreen`; prose verification claims (B13); the second
"Co-Authored-By" variants (lane files say Sonnet 5.5, this review says Fable 5.1 — pick one in a shared constant);
per-milestone handoff bullets in the live doc (G).
Add: `verify_at.py` ledger (B13); determinism row (B1); tier2/tier3 cadence (B6); a `retirement-dated files` list that every
mechanical pass reads (`tools/tidy/pick_batch.py` already has `--done`; add `--never`); the `canonical_obj` relaxation to the plan text.
Incidents, structural or local: cursor guard = structural (exe_guard) but the guard that bit remains (local until B8); golden exe =
structural; by-name compiler kill = local (`kill_build_tree.py` + a rule in one state file; make `ninja_build.py` write its pid file and
have the kill tool refuse names); swept WIP commits = structural for tools, local for agents (rule only); hung heredoc agent = local
(rule "use Write"); the lane_merge "unable to unlink" failure = structural (cleanup-on-failure, 092d155e4).
Over-engineered: comment_guard (1,790 lines) + cite_guard (1,553) + a three-model loop bought −13.5 % comment bytes on 186 files
(≈ 60k tokens) and one 8-row red board; the codebase-wide run should be gated on files agents actually read (the split pieces were right).
Efficiency: 773 commits on 2026-10-10, handoff edited 77 times in three days; every rotated agent reads 121 KB. The 40-line lane state
files are the right shape; make the handoff that shape (G).

## G. Handoff restructure
Keep `ORCHESTRATOR-HANDOFF.md` ≤ 120 lines: (1) ground rules (link to memory, do not duplicate), (2) lanes now (two lines, from the state
files), (3) live queue (items not DONE; move DONE items to the log), (4) open owner questions (numbered), (5) exit bar, (6) tools.
Move the "State at handoff" bullets (lines 29–151) verbatim to `.claude/cleanup/handoff-log-2026-10.md` (append-only; `handoff_append.py
--bullet` writes there). Add `verified.json` as the machine-readable twin of "green at sha". Fix on the way: handoff line 9 says
"max 4 subagents", memory `project-session-rules-2026-10-09` and MEMORY.md say max 5; MEMORY.md RESUME line is a day stale (LG19 546,
"G5 giants typed", G8 as remaining — G8 is done, step 1 closed, step 3 ≈ done); `lane2-state.md` in mc2-main says lane 2 does 1000–1500
splits while lane 2 is on step 2 (31 LG13 commits); queue item 3 says LG19 = 1 (baseline: 3, G8 review F5); the exe_guard rule and the
`canonical_obj` relaxation are in no memory file (grep: 0 hits each); 36 of 67 cleanup docs are `stale`.

## H. Verified against the repo (how)
- 22 load-bearing shas exist (`git cat-file -t`): 395a37af0 47a9623ed 16936e6ab 15f028ab5 39df08046 c023b4533 1291b1a56 e08429c08
  602362d63 b996c6c5d 59320ec77 2958c596e 6676fb7be 81f0a4265 b9ac335fe 14be97b2a 57f51d61d 8b9f81232 ecc094a53 a242a6246 c4185c352 1a9757c2c.
- Board: 164 `run_check` rows in `scripts/check-contracts.sh` (text-only by design, header lines 17–23); no row launches the game.
- Ratchet baseline at 830b29c6e: LG01 693, LG02 691, LG03 2397, LG12a 493, LG12b 624, LG13 2817, LG14 321, LG19 3, LG20 136, LG09 6685.
- Files >1500 = 50 (`tools/file_size_ratchet.baseline.json`, head 1e2fa987d). `mc2fmt.h` included by 312 files. `pch_skip.cmake` 122 TUs.
- objhash masks: `.debug*`, `.chks*`, `MC2FP1|…|FP1END`, `__DATE__`/`__TIME__` bytes and COMDAT names (`tools/objhash.py:32-60`).
- log_identity proves shapes + tag SET (`tools/log_identity.py:9-14`, `:66`). tidy_batch proof modes (`tidy_batch.py:154`, `run_tidy_safe.py:314-317`).
- Lane 2: `canonical_obj.py` default proof (docstring), `rewrite_defines.py` eligibility; step-2 plan tracked (0e797aad9); 31 LG13 commits; 301 dirty files.
- No save/load unit test (grep tests/unit). SIMID: trace default off, no digest in the two smoke artifacts; `MC2_SMOKE_FIXED_TIMESTEP` Presence/off.
- Tier manifest: tier1 5, tier2 24, tier3 9 (`tests/smoke/smoke_missions.txt`). Gate table counts as in B10. doctest `TEST_CASE(` 2,626 incl. corpus.
- Handoff: 77 commits, 121,183 bytes; commits/day 384 (10-09), 773 (10-10), 108 (10-11 so far). Cleanup docs: 67 files, 26 current / 36 stale / 5 no header.
- exe_guard/cursor_guard content and `MIN_COMMIT`; `lane_merge.py` RATCHETS (file_size, cxx17, dup_lines, axis_swizzle, split_map) and no build.
- Not verified (no runs): any green/identical/viewed claim; compile-time numbers; whether HEAD is deterministic; objhash results per batch.

## I. Open questions for the owner
1. Is run-to-run sim determinism (fixed step + seeded RNG) a REQUIREMENT of the engine, or only a proof tool? (Decides whether a
   non-deterministic HEAD is a bug to fix before step 4 or a harness to loosen.)
2. Save-game compatibility across builds: must a save from v0.4 load after step 4 B7 / LG14, or is "same build only" acceptable?
3. Multiplayer: delete the stubbed transport now (recoverable by sha) or freeze it out of steps 2–4?
4. Linux: state "Windows-only until further notice" in the plans, or keep paying for an arm nobody compiles?
5. Mods: may steps 4/5 change `.fx`/`.fit` READER behaviour on malformed content (repairs already did: inifile, memorystream)?
6. Comment pass codebase-wide: gate on agent-read frequency, or run everywhere as planned?
7. Which single engine (Fable/Sonnet) signs commits — the trailer differs between lane files and this review.
8. Gate amnesty target "<30 gates" (reset ruling 2026-10-05) vs 195 Feature rows today: still the number?

## What would have made this review faster
- A machine-readable `verified.json` (sha → build/doctest/board/tier/captures) instead of 120 prose bullets.
- The handoff split into live state + log; a one-screen "what proves what" table per step (C above) kept by the orchestrator.
- One memory file per ruling with the date, linked from the handoff line that applies it (two rulings disagreed; took greps to find).
- Lane state files kept in the lane's worktree AND mirrored in mc2-main at every rotation (the mc2-main copy of lane2-state.md was stale).
- `cxx17_ratchet` baseline with a `total` block (it is summed per dir only; I doubled it once).
