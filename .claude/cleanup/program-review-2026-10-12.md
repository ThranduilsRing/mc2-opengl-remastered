<!-- doc-status: current | reviewed:2026-10-12 -->
# Program review 2026-10-12: the week plan vs the target architecture, reconciled (adversarial, read-only)

Reviewed at nifty HEAD 73a57e758 (plan dce5a1f1a, architecture df000eb36). Every sha both docs cite was checked with `git cat-file`/`git show --stat`
(53 shas, all exist, subjects match). Code claims were spot-read, tool claims were checked on THIS host. `harness-cull-audit.md` does not exist yet
(neither worktree); lane 1's third session is live (0.4 smoke lock at 19:41, `tests/saveload/baselines/mc2_01.json` untracked, `smoke_missions.txt`
modified) and has not committed its state since 17:08. Owner rulings of 2026-10-11 night are the bar; where a doc still asks a question the owner
answered, the question is deleted below and the ruling applied.

## 0. Verdicts
- `program-plan-2026-10-12.md`: **ACCEPT-WITH-FIX.** Right spine (step 4 with a trustworthy digest first), wrong shape in three places: T4 puts step 4
  on two lanes (ruling: one lane), T1 is a 20 h task labelled 10 h, and the owner's night rulings (harness cull, multiplayer compile-out, editor arc,
  `[DT]` compile-time, tier3 parked, disable table zeroed, step 2 spot-check, thresholds) are absent or still posed as questions. Fixes in section 9.
- `target-architecture-2026-10-12.md`: **ACCEPT-WITH-FIX** as the north star; its attack order is REJECTED for this week. Tooling cull "first, solo"
  and a 10-day sim kernel before any ownership work would park the only arc that changes how the code reads for two weeks, and the kernel's biggest
  item (headless) is blocked by a split (appearances draw from the sim RNG inside the sim step) the doc does not list. Stale: Q8 (page heap IS set,
  two bounded runs passed), AMD RGP/RGD "agent-run" (Radeon Developer Panel is not installed), `LINUX_BUILD` deletion (ruling 4 keeps the arm).

## 1. Factual errors and stale claims (doc -> claim -> what the tree says -> fix)
| doc | claim | tree at 73a57e758 | fix |
|---|---|---|---|
| plan 1 | Feature 195 "about 113 default-off" | 132 default-off (`registry(Feature` rows with `false`: 132; arch says 132) | 132 |
| plan 1, T1 | `saveload_roundtrip --baseline` never recorded, `tests/saveload/baselines` absent | `tests/saveload/baselines/mc2_01.json` exists, untracked (sha 5e7e9a3fd, save 1000 / load 1100, `resave_identical: false`) | T1 commits it; mc2_16 baseline still missing |
| plan 1 | tier3 = "four mod campaign packs" | 9 rows: 7 mod missions (4 mods) + `1kbasicmap`, `metropolitan` (stock perf fixtures that need no mod) | tier3 parked by ruling; the two stock rows can run any time |
| plan 6 Q1, Q4, Q6, Q7, Q3 | open questions | answered 2026-10-11 night: tier3 parked (T7 = B5 prep), `[DT]` = compile-time facility off by default, disable table stays zeroed, step 2 closes by spot-check + checklist, 35/15 accepted | delete the questions, apply |
| plan 6 Q2, T4 | B4 on lane 2 inside step 4 | ruling: step 4 ONE lane, each batch reviewed before and after | T4 removed; lane 2 never touches step 4 files |
| plan 3 T1 | 10 h | 12 pairs + camera-park pair + 4 baselines + re-save hunt (1,279 packets, unbounded) + five new B0 tools: 18-20 h | split T1 / T3 (section 8) |
| plan 3 T1(b) | "re-save differing packets 1279 -> 0" as an exit criterion | ruling: saves may break freely; old-format byte identity has no product value, only diagnostic value (uninitialised memory) | demote to a diagnostic line, not an exit bar |
| plan 2.9, T1 | `isolation.py` / `park_window_offscreen` "not verified" | `scripts/smoke_lib/isolation.py` absent; `cursor_guard.py` gone at 5d45e969c (verified `--stat`) | mark done, only `park_window_offscreen` left to grep |
| plan | no harness cull, no multiplayer compile-out, no validation grading, no editor arc | all four are owner rulings | section 8 adds them on lane 2 |
| arch 6 Q8 | page-heap IFEO key "still pending" | `HKLM\...\Image File Execution Options\mc2-ph.exe` exists; `mc2-win64-lane3/mc2-ph.exe` 19:21; `crashes/` empty; owner: two bounded runs passed | delete Q8; mc2_15 = writer not reproducible under ASan OR page heap (section 4) |
| arch 2.8 table | AMD RGP + RGD "agent-run" | no Radeon Developer Panel on the host (`C:/Program Files/AMD` has no RDP; `where` empty) | owner installs RDP; until then "not available" |
| arch 2.8 table | RenderDoc python API | `renderdoccmd.exe` + `renderdoc.dll` + bundled python36 present; no `pymodules/`; `import renderdoc` [unverified] | first step of any RenderDoc task is a 5-line import probe |
| arch 2.8 table | GFXReconstruct | VulkanSDK 1.4.350: layer, `gfxrecon-capture.py`, `-convert`, `-replay`, `-info` all present | runs autonomously; VK only (fine) |
| arch 2.8 table | Tracy `tracy-csvexport` | present in `C:/Users/Joe/Downloads/TracyProfiler/` (capture, csvexport, profiler), not on PATH | the capture script takes the path |
| arch 2.8 table | jscpd / cppcheck | neither installed; node + npm present (`npx jscpd` needs one network fetch); `clang-tidy` (LLVM) and `appverif` present | jscpd = owner-approved install; cppcheck = Linux arc |
| arch 2.8 | validation layer "not graded" | true: `vulkan_context.cpp:68` counts `validationErrors` into health; no script reads it (grep scripts/ tools/ = 0) | one smoke row (lane 2, 2 h) |
| arch 2.9 / 7.8 | `LINUX_BUILD` deleted, `#else` arms dead | ruling 4 (2026-10-11): keep portability seams, do not delete the Linux arm, rename the misnomer later | reword: rename, keep |
| arch 7.8 | "no file over 1,000 lines" | plan DROPs the size target (survey verdict LEFT, 50 files > 1,500, ratchet holds) | pick one: keep the ratchet, drop the number |
| arch 4 | multiplayer compile-out "1-day item inside T1" | `code/multplyr*` 3,148 lines; mp screens 8,143; `MPlayer` referenced in 93 TUs, header included by 48; CMake lists the 3 .cpp directly (568-570), no option | scope = the 3 .cpp off the target behind `MC2_MULTIPLAYER` (default OFF), `MPlayer` stays a null pointer, screens stay; 4-6 h |
| arch 2.3 | headless "no window, no GPU" blocked only by the firewall | `Mech3DAppearance::update` takes `RollDice(50)` from the sim stream INSIDE the sim step (lane 1 finding 2); appearances are updated by the sim | headless needs the appearance/sim RNG split first (T1 does it) |
| arch 2.6 | `PacketFile` stays "only if a tool reads it [unverified]" | `tools/save_inspect.py`, `saveload_roundtrip.py` read it; both in-repo | verified: both are rewritten with the schema |
| both | `[DT]` as a gate row (`MC2_SIM_TRACE_DETAIL`) | ruling: compile-time facility, off by default, not a runtime gate | CMake option `MC2_SIM_TRACE` (T3) |

## 2. Sequencing: which first, and why
Ownership first, with the digest made trustworthy INSIDE the first task, not as a separate ten-day arc. WHY: (a) one final release means no arc has
product value until all of them land, so the order is set by proof dependencies and owner time, not by payoff per arc; (b) every step-4 batch is
proved by "digest unchanged", which today holds for mc2_03/10/16 and fails on mc2_24 in 5 of 9 pairs under the SAME fixed step and seeded RNG, so the
missing piece is not the kernel (tick default, headless) but one structural fix the hunt already named: appearance/VFX consumers drawing from the sim
stream in a frame-dependent order; (c) the kernel's remaining items (tick accumulator default-on, headless, order queue) change gameplay pacing and need
owner Q1/Q2 answered, while the appearance stream split, the four `projectForScreenXY` sites and the SIMID extension need nothing and take two days;
(d) the harness cull is real but does not gate B0/B1: it runs on lane 2 in parallel, merged at one fixed point so the board every task relies on
changes once, not continuously. The headless runner moves to week 2 after B1: it is the fix for the lease bottleneck (section 4) and it needs the
split from (b) plus the heaps B4 removes from the load path to be worth building once.
Editor arc: it cannot start as code this week (B4 heaps and R1 viewport are its blockers, and B1 converts 16 `editor/` files, so a second editor lane
would collide). It starts as its own proof floor on lane 2 Sunday (E0: editor smoke expanded to every dialog the 15-case smoke can reach, a generated
dialog -> panel inventory, `tests/smoke/editor/` adopted or deleted), so that week 2's E1 shell (SDL+ImGui host replacing the MFC loop, independent of
B4 and R1) starts with a regression floor. The owner's Q4 (which workflows are must-have) decides E1's panel order and is asked now (section 7).

## 3. Feasibility of proofs and hours (a Fable day = 6 h)
- Pairs: a 30 s mc2_24 pair is about 4 min on a quiet host; 12 pairs = 1 h plus build/deploy; a FAIL restarts the hunt (open-ended). Cap daytime pairs at
  6, run the 12 overnight in the background from the session's last command, read `pairs.log` next morning.
- B0 tools: `MC2_ASAN` in Mc2Standalone (1 h), ASan doctest run (build doubles: 1 h once), `alloc_probe.cpp` (2 h), `own_inventory.py` + baseline + row
  (3 h), pointer-ordering scan row (1 h), reload-delta seam (2 h), SIMID extension + re-recorded baselines (2 h): 12 h, not "part of a 10 h task".
- B1: code 46 + editor 16 + mclib 3 files = 3 batches of at most 25; per batch build + ASan doctest + `--check` on 3 missions + roundtrip + tier1 is
  about 1.5 h of proof; 10 h holds ONLY if the before-review of the batch note happens the evening before (section 8 slots) and no pair fails.
- Amnesty wave 1: 65 Feature + 56 Trace deletions in 8 h is 7 min per gate including its capture A/B; not credible. Target wave 1 at Feature -130 -> 150
  and Trace -> 120 with day+night GL+VK captures ONCE per family (4 families), not per gate.
- Captures: day+night x GL+VK = 4 runs x 2 states = 20 min of lease per A/B; three A/Bs a day is the lane-2 ceiling.
- Harness cull: deletion is cheap; the cost is re-proving the board after each deleted row (full `check-contracts.sh` is minutes, fine) and tier1 after
  each deleted smoke helper (10 min each): one merge point, not per commit.

## 4. Risks neither doc names
1. **Teardown that never ran.** Today nothing is freed at exit or heap destroy; the first owned-value batch makes destructors run at process exit and
   mission destroy. ASan will then report use-after-free in EXIT paths that were always broken and are irrelevant to the batch. Rule: the ASan proof is
   the reload delta and `saveload_roundtrip` (destroy + rebuild), never the exit path; each batch note carries the written teardown order of its owners.
2. **Determinism regressions from ownership moves** come from iteration order (EList -> `unordered_*` is forbidden; vector or stable order only), pool
   slot reuse order (B7), and the zero-init already landed (66cdc9ec3 changed what garbage the sim reads: baselines must be re-recorded ONCE at T3 and
   frozen). A changed digest stops the batch; nobody "re-baselines because the move is right".
3. **mc2_15 is reproducible under neither ASan nor page heap** (lane 3 + the two owner runs), only with Tracy ON at high fps. So "page-heap tier2 green"
   proves nothing about it, and it will surface as a 1-in-3 flake in every long proof run. Treat it as a known flake row: proof builds for step 4 run
   with Tracy compiled OFF (passes 8/8 on lane 3), and the writer hunt resumes only when the tick/headless work makes timing reproducible.
4. **Capture oracles no agent can judge.** "Montage viewed, MAD is not a verdict" has no stop rule. A Fable agent can look at a PNG but cannot certify
   night-lighting parity. Rule for the week: region digests (`visual_verify` regions) decide; the agent views only regions over threshold and records
   WHAT differs; a difference it cannot name is a STOP, not an accept. The owner stays the final viewer for VK flips (memory rule) until GFXReconstruct
   stream diffs name draws.
5. **The host lease.** One lease; tier2 is 60-90 min, 12 pairs 60 min, each A/B 20 min, tier1 10 min. The plan's 08-14 / 14-20 split leaves 12 h of
   lease idle. Fix: lanes own the daytime halves, the 20:00-08:00 window belongs to background proofs (overnight pairs, tier2 at merge points) started
   by the last command of a session and read from the artifact next morning. Headless (week 2+) is the structural fix.
6. **Shared headers across lanes.** B1 edits `estring.h` (included by mclib, gui, editor); amnesty regenerates `RendererFeatureRegistry.h` and edits
   `gate_table.cpp`; the cull deletes scripts the board names. Rules: generated files are regenerated at merge, never hand-merged; lane 2 names its
   mclib render files in `register_lane` and stays out of every header B1 lists; the cull merges at ONE point (Tue evening) and both lanes re-run the
   board after it.
7. **A failing mc2_24 pair after the stream split** means the race is not RNG consumption order at all (data race, uninit read). The fill-pattern run
   (plan section 5.6) is then the next step, not more pairs.

## 5. What is missing from both docs
- **Replay as the sim oracle.** SIMID detects divergence; it cannot reproduce a 1-in-9 race. A replay file (mission seed + tick count + the
  `TacticalOrder` log; the smoke issues no orders, so it is the seed and the tick) turns the next bimodal race into a deterministic repro and is the
  save schema's `header` by another name. Design item for S1 week 2; T1 records the seed and tick in every smoke artifact now (two log lines).
- **Content manifest for installs.** The jaegermech incident was a loose override; `STOCK-DATA-TRUTH` pins missions only. A `data/` fingerprint
  (tgl, fx, fit) in the smoke artifact and in every baseline file (T1 writes it; lane 2 extends the gate to `data/tgl` in T2) makes "baseline at
  install X" checkable.
- **The cull's effect on the board.** Every task's exit says "board green"; the cull changes the board. One merge point, a `rows deleted` list in the
  ledger, and each deleted row's replacement proof named (ruling: maintained tool over home-grown).
- **The editor's own smoke** (`scripts/run_editor_smoke.py`, 15 cases) is the only editor proof and no plan expands it; `tests/smoke/editor/` exists
  untracked in mc2-main with no owner. E0 (section 8).
- **Save schema vs the final release.** Arch 2.6 lands the schema once; the plan's B7 "extension packet after END" (3c4e850d8) is the OLD format's
  additive patch. Under the ruling these contradict: B7 writes the new schema, not an extension packet. The schema must be designed (sections, per-type
  `serialize`, header with seed/tick/data fingerprint) BEFORE B7, i.e. S3 is a week-3 design doc, and the B7 extension-packet text is withdrawn now.
- **Budget denominator.** 35 % / 15 % of what? The week's Fable hours are not stated anywhere; the ledger needs the number (owner Q1 below).
- **Tracy-off proof builds** as a named configuration (`MC2_NO_TRACY` is a local edit on lane 3, not in CMake).

## 6. The maintained-tool table, host reality (what can run autonomously today)
Runs now: GFXReconstruct (layer + capture.py + convert + replay, VulkanSDK 1.4.350); validation layers (+ `vkconfig`); RenderDoc `renderdoccmd` (python
module unverified); Tracy capture/csvexport/profiler (Downloads path); clang-tidy (LLVM); MSVC `/analyze`; MSVC ASan; Application Verifier (`appverif`,
IFEO set for `mc2-ph.exe`); glslang. Needs the owner: AMD RGP/RGD (install Radeon Developer Panel; RGP capture is hotkey/GUI driven, the CLI service
is [unverified]); jscpd (`npm i -g jscpd`); cppcheck (Linux arc). The arch table's verdicts stand with those three marked "owner install first".
`gflags` is absent but unnecessary (IFEO key is set). Nothing in the table is impossible on this host; two rows are not possible THIS week.

## 7. Questions the owner is not being asked (numbered; answers change the plan)
1. The week's Fable hour budget (the 35 % / 15 % thresholds need a denominator). The amended plan is about 71 h incl. reviews.
2. Sim tick semantics when the default flips (week 2): under 30 fps the sim SLOWS (one tick per frame, today's smoke behaviour) or an accumulator runs
   several ticks per frame with a spiral-of-death cap? The second is the design; it changes feel at low fps and needs a yes.
3. Proof builds with Tracy compiled OFF for step 4 (mc2_15 passes 8/8 without Tracy): acceptable, with mc2_15 kept as a known-flake row until the
   writer hunt resumes? (Default in the plan: yes.)
4. Capture verdicts: the night ruling says agents run captures autonomously; the standing memory rule says the owner views every VK change before it
   is accepted. Which holds for amnesty deletions and the VK flip? (Default: agent decides by region digest; owner views VK flips only.)
5. `LINUX_BUILD`: the architecture deletes the `#else` arms as dead; ruling 4 keeps the Linux arm. Confirm "rename the misnomer, keep the arm, no new
   Windows calls outside platform".
6. Harness cull: may a contract row be deleted before its maintained replacement is wired (e.g. `dup_lines_ratchet` before jscpd is installed)?
   (Default: no; delete only with the replacement named and running.)
7. Editor must-have workflows for the final release (arch Q4): terrain paint, object placement, objectives, teams/ABL assignment, campaign links,
   mission export; which dialogs may be dropped? E1's panel order is this answer.
8. Save schema: confirm the B7 "extension packet after END" (3c4e850d8) is withdrawn in favour of the one-schema design (arch 2.6), and that S3's design
   doc is wanted in week 3 before B7.
9. GL captures: until the VK flip, must amnesty A/Bs still run under GL AND VK (doubles lease time), or VK only with GL at merge points?
10. mp screens (8.1k lines, UI): stay compiled this week (only the transport compiles out) or go behind the same option later? (Default: stay.)
Carried from the architecture (still unanswered, still needed): tick rate (Q1), order-queue latency (Q2), GL deletion / minimum GPU (Q3), ABL frozen +
debugger fate (Q5), effects render-only (Q6), cull of `terrain_beautify`/`blender_pipeline`/`unit_converter` (Q7).

## 8. Amended week plan, 2026-10-12 to 10-19 (lane 1 = `A:/Games/mc2-main`, lane 2 = `A:/Games/mc2-lane2`; step 4 single-lane on lane 1)
Lanes are file-disjoint by `register_lane`: lane 1 claims `code/`, `mclib/` (non-render), `tests/`, `cmake/`, `tools/sim_determinism.py`,
`tools/saveload_roundtrip.py`, `tools/own_inventory.py`; lane 2 claims `GameOS/`, `RenderCore/`, `shaders/`, `Gates/`, `scripts/`, `tools/` (the rest),
`CMakeLists.txt` (multiplayer option only), `tests/smoke/editor/`, `docs/`. Lease: lane 1 08:00-14:00, lane 2 14:00-20:00, 20:00-08:00 background proofs.
Every runner call passes `--wait-timeout`, never `--no-lease`; launches background-only; commit by pathspec; 6 h cap with the state file as hand-over.

| id | lane | day | task | h |
|---|---|---|---|---|
| T1 | 1 | Mon 12 - Tue 13 | Digest trustworthy: mc2_24 closed by the appearance-stream split, camera out of the sim step, baselines, save round-trip baseline | 10 |
| T2 | 2 | Mon 12 - Tue 13 | Harness cull wave 1 (from `harness-cull-audit.md`), multiplayer compile-out, validation layer graded, Tracy-off proof config | 10 |
| T3 | 1 | Wed 14 - Thu 15 | Step 4 B0 harness + SIMID extension + `[DT]` as `MC2_SIM_TRACE` compile-time facility | 10 |
| T4 | 2 | Wed 14 - Thu 15 | VK proof substrate: GFXReconstruct oracle at the golden frame, RenderDoc import probe, capture region digests as the A/B verdict | 8 |
| T5 | 1 | Fri 16 - Sat 17 | Step 4 B1: EString -> `std::string` for non-widget callers (3 batches), B2 batch note written for review | 10 |
| T6 | 2 | Fri 16 - Sat 17 | Gate amnesty wave 1 (render side): audit then deletions by family, VK sub-gates folded | 8 |
| E0 | 2 | Sun 18 | Editor proof floor: editor smoke expanded, dialog -> panel inventory generated, `tests/smoke/editor/` adopted or deleted | 6 |
| RB1 | ro | Thu 15 eve | Fable reviewer BEFORE B1: the batch notes (file lists, `own_inventory` rows, teardown order, COW/`Description()` sites) | 2 |
| RA1 | ro | Sun 18 | Fable reviewer AFTER B1 against the step 4 checklist; RB2 for the B2 note in the same sitting | 3 |
| M | ro | Mon 19 am | Meta-review of the week; next plan (week 2: B2, headless + tick default per Q2, E1 shell, amnesty wave 2) | 4 |
Orchestrator: Mon 08:00 step 2 spot-check (3 random LG13/LG14 commits re-proved with `objhash compare`) then kick-off; Tue 20:00 merge T2 (the ONE
board change point; both lanes re-run the board Wed morning); Thu 20:00 merge T4; Sat 20:00 merge T6; Sun merge E0; budget line in the log after each
task; cut E0 then T6 if T1 + T2 exceed 35 %; stop any task past 15 % without its first exit criterion.

### T1 (lane 1, 10 h): the digest is trustworthy
Goal: close plan section 11 prerequisites 1-2 so B0 is built on a digest that is identical run-to-run on all four gate missions. Scope: (1) commit
the untracked `tests/saveload/baselines/mc2_01.json` and the `smoke_missions.txt` edit the live session left; (2) `pairs-until-fail --mission mc2_24
--max 6 --duration 30` on the quiet morning host; on a FAIL apply the hunt patches (`determinism-hunt/`) once to confirm the consumer; (3) the
structural fix regardless of (2): appearance and VFX consumers (`mech3d_update.cpp:167` fall direction, crater/bolt visual draws, anything reached only
from `*Appearance::update`) draw from a render-side LCG stream seeded per mission; sim consumers keep `gos_rand`; (4) the four `eye->projectForScreenXY`
sites (`mech_update.cpp:786/788`, `gvehicl_update.cpp:790/792`) move to the HUD pass; (5) `write-baseline` for mc2_03, mc2_10, mc2_16
(`MC2_FIXTURE_ORDER_LIVENESS=1`), mc2_24 with the install data fingerprint and the mission seed in the file; (6) camera-park pair on mc2_24 and
mc2_10; (7) `saveload_roundtrip --baseline` for mc2_16; the re-save diff is printed as a diagnostic (packet classes still differing), not fixed.
Non-scope: B0 tools, tick default, per-system sim streams, mc2_15, tier3, any `editor/` or render file. Prerequisites: lane 1's third session closed
with its state file; 0.4 deployed from HEAD. Proof/exit: 12 consecutive mc2_24 pairs identical (6 by day, 6 overnight, host load recorded in
`pairs.log`); camera-park pairs identical on mc2_24 and mc2_10; four baseline files committed under `tests/smoke/` with `data_fingerprint` and `seed`
fields; `sim_determinism.py --check` green on all four at HEAD; mc2_16 roundtrip baseline committed; `build_contract_harnesses.py --run --unit` green;
`check-contracts.sh` green; tier1 5/5 `--deploy 0.4`; `lane1-state.md` rewritten. Hours: 4 + 6.

### T2 (lane 2, 10 h): cull wave 1, multiplayer out, validation graded
Goal: the board every later task relies on changes ONCE this week, and three owner rulings land. Scope: (1) read `harness-cull-audit.md` (if absent
at 08:00 Monday: write it in the first 2 h with the rule "a script survives only if a tracked caller names it and that caller is live"; verdict per
file DELETE / KEEP(caller) / REPLACE(maintained tool)); (2) delete wave 1 = scripts with no tracked caller (50 named by nothing, 5.3k lines), the
one-time codemods that have landed (`gate_codemod.py`, `comment_guard.py`, `cite_guard.py` and their rows), `nofocus.park_window_offscreen` if still
present, `lane3-diag/quarantine_patch.py`; harness build trees whose cases fit doctest are LISTED, not moved (week 2); (3) `MC2_MULTIPLAYER` CMake
option default OFF: `code/multplyr*.cpp` leave the target, `MPlayer` becomes a null pointer defined in one TU, headers and mp screens stay; (4) tier1
grades `validationErrors` from the VK health block (a non-zero count fails the VK arm); (5) `MC2_TRACY` CMake option (default ON) so a proof build
can compile Tracy out; (6) `STOCK-DATA-TRUTH` extended to `data/tgl`. Non-scope: amnesty, render code, anything lane 1 claims, deleting a row whose
replacement is not running (Q6). Prerequisites: merged nifty. Proof/exit: `tools/inventory.json` (tool -> caller) committed with a board row that fails
on an unlisted script; Python lines under `tools/scripts/tests` down by at least 20 % (168,246 -> at most 134k) with every deletion in the ledger
`.claude/cleanup/harness-cull-wave1.md` naming its caller check; `check-contracts.sh` green with the rows-deleted list; `build_all_targets.py` green with
`MC2_MULTIPLAYER=OFF` and ON; default build has no `multplyr*.obj`; tier1 5/5 on the lane install under GL and under VK with the validation row
present and zero; doctest unchanged (2,742); editor smoke 15/15. Hours: 5 + 5.

### T3 (lane 1, 10 h): B0 harness
Scope: `MC2_ASAN` in `cmake/Mc2Standalone.cmake` + the doctest tier built and run under it; `tests/unit/alloc_probe.cpp` with `ScopedLeakCheck`
(create/copy/move/destroy/double-destroy per migrated type, starting with EString); `tools/own_inventory.py` (type -> allocation sites, free sites,
owner) with `tests/unit/own_inventory_baseline.json` and a board row; pointer-ordering scan (`map<T*`, pointer `<`, pointer hashing) as a row with
baseline; the reload-delta size seam (opt-in portable TU, proof builds only); SIMID extended to buildings, turrets, ABL state and per-stream RNG call
counts, then the four baselines re-recorded ONCE with the reason in the commit; `[DT]` trace rebuilt from the `determinism-hunt/` patches as the
compile-time option `MC2_SIM_TRACE` (default OFF, no gate row, LG19-clean), patches deleted. Proof/exit: doctest green natively AND under ASan; the
two new board rows green; `--check` green on four missions after the re-record; `determinism-hunt/` gone; B1 batch notes (3 batches, file lists,
inventory rows, free sites, the `Description()`-class sites) written in `lane1-state.md` for RB1. Hours: 6 + 4.

### T4 (lane 2, 8 h): VK proof substrate
Scope: GFXReconstruct capture at the golden frame of mc2_01 day and mc2_10 night under `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`;
`gfxrecon-convert` of two runs at HEAD diffed = empty (the VK oracle, checklist item 1); a `vk_stream_diff` board row that names the first differing
command; RenderDoc `import renderdoc` probe recorded (pass or the exact failure); `visual_verify` region digests become the A/B verdict line
(`REGION DIFF <name> <value>`), the montage is attached only for regions over threshold; Tracy tool path wired into `tracy_capture_compare.py`.
Proof/exit: two HEAD captures diff empty on both missions; a deliberate one-line shader change produces a named differing draw; rows green; ledger of
what the home-grown VK present-dump path still proves (if nothing: delete it in T6). Hours: 4 + 4.

### T5 (lane 1, 10 h): step 4 B1
Scope: EString -> `std::string` for code 46, editor 16, mclib 3 files via `EString::str()`, three batches (code UI screens, code rest + mclib, editor),
commit per batch; the class survives for gui widgets until B6. Proof per batch: build + `build_all_targets.py`; doctest under ASan; `/analyze`
C26815/C26816 clean on touched files; `own_inventory` row diff committed; `--check` on mc2_03/10/24 unchanged; `saveload_roundtrip --check` mc2_16;
tier1 5/5; editor smoke for batch 3. Exit: LG03 down by the EString sites outside gui/, no EString owner outside gui/ in the inventory, B2 batch note
(EList sites with the iteration/insertion contract, SortList total order) in `lane1-state.md` for RB2. Hours: 6 + 4.

### T6 (lane 2, 8 h): gate amnesty wave 1 (render files)
Phase 1 (2 h, no edits): `.claude/cleanup/gate-amnesty-wave1.md` with every Feature and Trace row in GameOS/, RenderCore/, shaders/ and the named mclib
render files: default, last reader, live user of the other arm (game, editor, smoke parser, capture tool), verdict DELETE / FOLD / KEEP. Phase 2: one
commit per family (row + reader + dead arm + doc + `gate_gen.py --write`); VK sub-gates fold into `MC2_VK_SHARED_GBUFFER`. Keep by ruling:
`MC2_TERRAIN_AMBIENT_FLOOR_NIGHT`, cliff tessellation, `MC2_GPU_PARTICLES`, step 4 section 2 files. Proof/exit: Feature 195 -> at most 150, Trace
156 -> at most 120 (wave 2 continues); `gate_gen.py --check --rebuild` consistent; LG19/LG20 only down; `build_all_targets.py`; tier1 5/5; day + night
A/B per family under GL and VK decided by region digest (T4), montages for over-threshold regions in the state file; validation row zero. Hours: 4 + 4.

### E0 (lane 2, 6 h): editor proof floor
Scope: `tests/smoke/editor/` (untracked) adopted into `run_editor_smoke.py` or deleted with the reason; the smoke extended to every dialog reachable
without a human (open, default-accept, close; crash = fail) from the 15 cases; `tools/editor_inventory.py` generating the MFC inventory (dialog class,
DDX fields, menu entry, smoke case covering it) as a committed JSON + board row; a one-page `editor-arc-e1-brief.md` listing E1's shell steps in the
order the inventory gives. Non-scope: any `editor/*.cpp` edit (B1 batch 3 owns them this week). Proof/exit: editor smoke N/N with N > 15 stated; the
inventory row green; the brief names the panel order pending Q7. Hours: 6.

### Review slots (ruling: every step 4 batch reviewed before and after, read-only Fable)
RB1 Thu 15 evening: reads T3's batch notes only (not code), checks file lists against `register_lane`, free-site inventory rows present, teardown order
written, the `Description()`/COW sites listed, no `unordered_*` for an EList, proof commands named with numbers; verdict ACCEPT / ACCEPT-WITH-FIX /
REJECT in `cxx23-step4-b1-review.md`; lane 1 starts B1 Friday only on ACCEPT or with the fixes applied first. RA1 Sun 18: diffs of the three B1 commits
against the step 4 checklist (inventory diff, no new throw in game loops, no static-duration owner, pointer-ordering scan clean, baselines unchanged,
build_all_targets + editor smoke, no HANDLE/COM/SEH, multiplayer untouched); same sitting reviews the B2 note (RB2). A REJECT reverts the batch by
sha before Monday; nothing of a rejected batch is merged forward.

### Day by day
Mon 12: spot-check, T1 s1 (pairs, stream split), T2 s1 (audit, cull); overnight 6 pairs. Tue 13: T1 s2 (baselines, camera sites, roundtrip baseline),
T2 s2 (multiplayer, validation, Tracy option), 20:00 merge T2, overnight tier2 at the merged tip (Tracy-off build). Wed 14: both lanes re-run the board;
T3 s1, T4 s1. Thu 15: T3 s2 (batch notes), T4 s2, 20:00 merge T4, RB1. Fri 16: RB1 fixes (1 h cap), T5 s1 (batches 1-2), T6 s1 (audit + first family).
Sat 17: T5 s2 (batch 3, B2 note), T6 s2, 20:00 merge T6, overnight 12 pairs at HEAD. Sun 18: RA1 + RB2, E0, merges. Mon 19: M.

## 9. Fixes the orchestrator applies to the source docs (edits, not rewrites)
`program-plan-2026-10-12.md`: section 1 "about 113" -> 132; the `tests/saveload/baselines` sentence -> "mc2_01 recorded (untracked, lane 1 s3)";
tier3 row -> "parked by ruling; 7 mod + 2 stock rows"; section 3 table replaced by section 8 above (T4 removed, cull/multiplayer/validation/E0 added,
hours corrected); T1(b) exit "1279 -> 0" -> "re-save diff printed as diagnostic"; section 6: delete Q1-Q7 (all answered), replace with section 7 above;
section 4 "What replaces the Opus review" keeps (2) but states the BEFORE review explicitly; briefs replaced by section 10. Status header: reviewed
2026-10-12.
`target-architecture-2026-10-12.md`: 2.3 add "appearance/VFX RNG consumers split from the sim stream BEFORE headless"; 2.7 add "the component disable
table stays zeroed now and becomes data-driven (moddable) later; `[DT]` trace is a CMake facility, off by default"; 2.8 table: RGP/RGD "owner installs
RDP first", RenderDoc "python module import unverified", jscpd "owner install", Tracy "Downloads path"; 2.8 add "proof builds compile Tracy out
(`MC2_TRACY=OFF`)"; 2.9 and 7.8 `LINUX_BUILD`: "rename the misnomer, keep the arm (ruling 4)"; 7.8 drop "no file over 1,000 lines" (keep the ratchet);
4 attack order: T1 cull runs on lane 2 in parallel (not solo-first), S1 split into S1a (inside this week's T1/T3) and S1b (tick default, per-system
streams, headless, order queue: week 2+ after Q1/Q2), multiplayer compile-out = 4-6 h with the scope in section 1; 2.6 "PacketFile [unverified]" ->
"read by `save_inspect.py`/`saveload_roundtrip.py`, both rewritten with the schema"; the B7 extension packet (3c4e850d8) withdrawn pending Q8;
6 Q8 deleted; gap table: tier3 row "parked", mc2_15 row "not reproducible under ASan or page heap; Tracy-off proof builds; flake row".

## 10. Briefs for Monday morning (paste-ready)
### T1 brief (lane 1)
You are lane 1 on `A:/Games/mc2-main` (branch `claude/nifty-mendeleev`, install `mc2-win64-v0.4`, build `build64-ninja`). Read `ORCHESTRATOR-HANDOFF.md`,
`lane1-state.md`, `program-review-2026-10-12.md` sections 4 and 8 (T1), `cxx23-step4-ownership-plan.md` sections 5 and 11, `saveload-fidelity-2026-10-11.md`.
`register_lane` claiming `code/`, `mclib/` (not mlr/, gosfx/ or render files), `tests/`, `cmake/`, `tools/sim_determinism.py`, `tools/saveload_roundtrip.py`.
Session cap 6 h: at 6 h commit or revert, rewrite `lane1-state.md`, stop; session 2 continues from it. Lease hours 08:00-14:00; `--wait-timeout 3600`,
never `--no-lease`; launches background-only; never kill cl/ninja by name; never touch `code/multplyr*`, `editor/`, `gui/`, GameOS/, lane 2's files.
Task in order: (1) commit the untracked `tests/saveload/baselines/mc2_01.json` and the `tests/smoke/smoke_missions.txt` edit if they are the previous
session's (its state file says); (2) `py -3 tools/sim_determinism.py pairs-until-fail --mission mc2_24 --max 6 --duration 30 --out <scratch>`; on FAIL
apply `determinism-hunt/dt_mc24_bolt.patch` once to name the consumer that drew out of order, then revert the patch; (3) give appearance and VFX
consumers their own render-side LCG stream seeded from the mission seed (start at `mech3d_update.cpp:167` `RollDice(50)`; grep every `RandomNumber(`/
`gos_rand` reached only from `*Appearance::update` or effect code; sim consumers unchanged); re-run 6 pairs; (4) move the four `eye->projectForScreenXY`
sites (`mech_update.cpp:786,788`, `gvehicl_update.cpp:790,792`) to the HUD pass; (5) `write-baseline` for mc2_03, mc2_10, mc2_16 (`MC2_FIXTURE_ORDER_LIVENESS=1`),
mc2_24, adding `data_fingerprint` (sha of `data/tgl`, `data/fx`, mission fit) and `seed` to the file format; (6) camera-park pairs (`run --env-a/--env-b
MC2_CAMERA_PARK=...`) on mc2_24 and mc2_10; (7) `saveload_roundtrip.py run --baseline` mc2_16; print the re-save diff by packet class into the state file
as a diagnostic, fix nothing there. At session end start 6 more pairs in the background (`--max 6 --out <scratch>/overnight`) and name the path in the
state file. Every C++ commit: build, `py -3 tools/build_contract_harnesses.py --run --unit`, `bash scripts/check-contracts.sh`; commit by pathspec; commit
BEFORE deploy; comments WHAT and WHY. Exit = the T1 proof list in section 8 with every number in `lane1-state.md`, one `verified` line per commit batch.
End with "What would have made this faster or more efficient" (3-6 bullets).

### T2 brief (lane 2)
You are lane 2 on `A:/Games/mc2-lane2` (branch `claude/lane2-s3`, `build64-ninja`, install `mc2-win64-lane2`, deploy with `--allow-new-target`). Merge nifty
first. Read the handoff, `lane2-state.md`, `program-review-2026-10-12.md` sections 4-6 and 8 (T2), `harness-cull-audit.md` if it exists (else write it in
your first 2 h: for every file under tools/, scripts/, tests/ the rule "survives only if a tracked caller names it and that caller is live"; verdict
DELETE / KEEP(caller) / REPLACE(maintained tool); no deletion before the audit is committed). `register_lane` claiming `scripts/`, `tools/` (except
`sim_determinism.py`, `saveload_roundtrip.py`), `tests/smoke/` scripts, `CMakeLists.txt` (multiplayer and Tracy options only), `GameOS/gameos/vulkan_context.cpp`,
`docs/`. Session cap 6 h; lease hours 14:00-20:00; rules as T1; never touch `code/` beyond the one TU that defines `MPlayer = nullptr`, never `mclib/`, `editor/`.
Task in order: (1) cull wave 1 per the audit: scripts named by no tracked file, landed one-time codemods (`gate_codemod.py`, `comment_guard.py`,
`cite_guard.py` + their board rows), `nofocus.park_window_offscreen`, `lane3-diag/quarantine_patch.py`; one commit per group with the caller check in the
message; a row is deleted only when its replacement proof is named AND running (else KEEP, listed); (2) `tools/inventory.json` (script -> caller -> proof)
+ a board row that fails on an unlisted script; (3) `option(MC2_MULTIPLAYER "..." OFF)`: `code/multplyr*.cpp` leave the mc2 target, `MPlayer` defined as a
null pointer in one TU, headers and mp screens untouched; both settings build (`build_all_targets.py`); (4) tier1 reads the VK health `validationErrors`
counter and fails the VK arm when non-zero; (5) `option(MC2_TRACY ON)` so a proof build compiles Tracy out; (6) `STOCK-DATA-TRUTH` covers `data/tgl`.
Proof: `check-contracts.sh` green with a rows-deleted list in `.claude/cleanup/harness-cull-wave1.md`; Python lines under tools/scripts/tests at most 134k
(from 168,246); doctest unchanged; `build_all_targets.py` green OFF and ON; default build has no `multplyr*.obj`; tier1 5/5 on the lane install under GL
and under `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1` with the validation row at zero; editor smoke 15/15. Merge point is Tue 20:00: nothing of
this task merges earlier. Exit: counts in `lane2-state.md`, the ledger, the inventory. End with "What would have made this faster or more efficient".
