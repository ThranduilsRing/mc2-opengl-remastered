<!-- doc-status: current | reviewed:2026-10-11 -->
# Orchestrator handoff — LIVE STATE (modernization / tech-debt paydown)

Owner intent: **pay down most/all tech debt before building features**. No time pressure; thorough + verified beats fast. Then build.
WHAT this is: the rules, the lanes, the ordered queue and the open owner rulings — nothing else, kept ≤ 120 lines. The milestone record (every "state at
handoff" bullet, newest first, plus the pre-restructure queue and rule narratives) is `.claude/cleanup/HANDOFF-LOG.md`; append with `handoff_append.py --bullet`.
RESUME HERE: read this file, then `lane1-state.md` / `lane2-state.md`, then the newest 5 entries of the log. Verify any "green" claim with `git log` and a fresh run.

## Ground rules (owner-set, non-negotiable)
- Work ONLY in `A:/Games/mc2-main`, branch `claude/nifty-mendeleev` (lane 2: worktree `A:/Games/mc2-lane2`, branch `claude/lane2-s3`). Local commits only — **never push**.
- **One agent edits C++/shaders/builds per lane.** Read-only scouts and Python/doc-only agents may run in parallel (they must not touch C++ or build).
- Concurrency (owner 2026-10-09): **max 5 subagents; one Opus at a time**. **Lane cap (owner 2026-10-11): a lane session lives at most 6 hours** — long sessions relax proofs and take shortcuts; rotate from the lane state file at 6 h or at the program boundary, whichever first. A lane never changes a proof standard or default on its own; it reports the failure and the orchestrator rules. Capture/smoke A/B sets run with animations frozen once that mode exists (owner ruling: sim determinism is a requirement). Reviews: Opus per step; a Fable program-level meta-review every 3–4 Opus reviews (owner 2026-10-11; ~2% weekly Fable usage each). Two builder lanes, file-disjoint (lane 2: own `build64-ninja`, install `mc2-win64-lane2`); the orchestrator merges lane 2 into nifty with `lane_merge.py`.
- Model tiers: **Haiku** mechanical/doc/memory edits (<100k context, exact file:line + replacement); **Sonnet** code and design; **Opus** adversarial reviews only. Co-Authored-By trailer: Fable 5.1 on orchestrator commits (lane files say Sonnet 5.5; see Q7).
- Every C++ commit in any lane: build + harnesses+doctest (`py -3 tools/build_contract_harnesses.py --run --unit`) + check-contracts + smoke-lite. Doctest is NOT optional (7cc80c5e3 broke the `mc2_tests` link unseen).
- **Ratchets:** never contort code to satisfy a ratchet; rewrite the baseline with the reason. Counts only go down by intent.
- **Pathspec rule:** `git diff --cached --name-only` first; commit with explicit pathspec `git commit -m ... -- <paths>`; never `git add -A`/`.`. Two sweep accidents on 2026-10-09 (252dcfbe3, 9b8e02f81).
- **Smoke / lease / launch:** ONE smoke/capture on the host at a time, lease-serialized; agents never pass `--no-lease` (owner-only `MC2_OWNER_NO_LEASE=1`). Game/editor launches background-only (no focus, mouse or keyboard) unless the owner asks. tier1 needs an explicit `--deploy`/`--exe` (never the golden install; exe_guard in `nofocus.spawn` refuses exes lacking `MC2_WINDOW_OFFSCREEN`), and test trees run with `MC2_CURSOR_GUARD_OFF=1`. Incident 2026-10-10 08:34–09:10 (two concurrent runs took the owner's mouse for 20 min) — full text in the log archive.
- **Console children** from scripts: `scripts/console_child.py` `child_kwargs()` only. A bare `CREATE_NO_WINDOW` spawns a conhost per child and crashed the owner's PIA VPN 5× on 2026-10-09 (fix 8d39799d5 + 604562b39; `check-background-launch.py` enforces); narrative in the log archive.
- Commit BEFORE build+deploy (DEPLOY_FINGERPRINT gate); deploy with `deploy_payload.py --target game --ninja` before smoke. Captures: `MC2_CURSOR_NO_PARK=1`.
- **Look at captures yourself** before accepting any visual/render change (`tools/visual_verify/ab.py`; day AND night: `mc2_01_saved_205644`, `mc2_10_saved_204523`). MAD is NOT a pass signal.
- Adversarial audit (read-only agent) before deleting features/data; mechanical modernization needs none. "Does X exist" path checks: a **script**, not an agent. Text edits to code/shaders: exact, line-anchored, re-read after editing.
- Comments say WHAT and WHY, never HOW; every edited region loses `file:NNN` refs and "how" narration; no separate comment-only sweeps (see Q6).
- **Budget mode (owner 2026-10-11 evening): spawn ONLY Fable subagents from now on, far fewer and much larger tasks (whole arcs with their own proofs, 4-10 h), no Sonnet/Haiku/Opus; per-step Opus reviews are replaced per the week plan `program-plan-2026-10-12.md` (Fable planner writes it; the owner kicks off from it after the morning reset).**
- **Questions to the owner go inline, numbered, at the end of the reply. Never use popup (AskUserQuestion) questions.**
- Parked by owner: `MC2_NO_LAUNCHER` pre-main launcher_env gap; G0 `MC2_SHADOW_ENABLE` (unify toward VK direct-CSM default ON, VK capture viewed).

### Orchestrator tools
- `py -3 tools/handoff_append.py [--lanes T] [--queue-prefix N T] [--queue-replace N OLD NEW] [--queue-append T] [--bullet T]... [--message S] [--dry-run]` — `--bullet` appends to `HANDOFF-LOG.md` (newest first, date-stamped); `--lanes`/`--queue-*` edit this file. Commits only the file(s) edited; refuses mid-merge (exit 3); `--dry-run` previews both diffs.
- `py -3 tools/lane_merge.py <sha-or-branch> [--message S] [--dry-run]` — merge --no-ff, baseline conflicts auto-resolved, ratchets re-tightened, follow-up commit of the baselines; fail-closed (4 dirty overlap/staged, 5 conflict, 6 ratchet FAIL).
- `py -3 tools/visual_verify/montage_compact.py <before> <after> | --montage <x_before_after_diff.png> [--mech-box x,y,w,h] [--out f] [--scale 0.25]` — red-boxed 1/4-scale AFTER plus one `DIFF outside_box=...` line (exit 1 over `--threshold` 200).

## Lanes now
- Lanes (2026-10-11 17:09): lane 1 third session (started 17:30, cap 23:30) = mc2_24 verdict (8+ pairs at HEAD; wound-table overread 6876d17fd is the candidate), step 4 prerequisite 2 (XData{} zero-init, static_asserts, re-save byte identity, roundtrip --baseline, savegame backup helper), then fingerprint-vs-build-inputs, lease-aware deploy, pairs-until-fail. Lane 2 third session (started 16:50, cap 22:30, expected ~3 h) = checkpoint at merged tip, rewriter batch-of-one fallback + msbuild gate error text + check_batch_allowed.py, then dstd.h/inifile.h/txmmgr.h/gameos.hpp one at a time. Lane 3 CLOSED 16:45 (merged 018c09a06, worktree removed); mc2_15 heap-corruption writer open, blocked on the owner (Application Verifier). Owner decisions pending: testgame.ims restore, jaegermech .tgl vs loose ini, Application Verifier, lane 3 install disk.
- State files: `lane1-state.md` (mc2-main) and `lane2-state.md` (authoritative copy in `A:/Games/mc2-lane2/.claude/cleanup/`, mirrored here at each rotation/merge); each lane refreshes its file at every report.
- Status snapshot (ratchet baselines at 830b29c6e, meta-review §H; re-run before claiming green): LG01 693, LG02 691, LG03 2397, LG12a 493, LG12b 624, LG13 2817, LG14 321, LG19 3, LG20 136, LG09 6685; files >1500 = 50; board 166/166 (lane 1); doctest 2,626 `TEST_CASE(`.

## Ordered queue (one line per item; history in the log archive)
1. S1 loop-speed / Ninja dev build — DONE 2026-10-09 ee4fcb1bf.
2. D1/D2 offline provers (vk_ubo_layout, glsl_port_diff, vk_shaders_compile) — DONE 2026-10-09 f54687963. Open: `visualDisplaceFar` has no PORT markers (add, then re-add two registry rows).
3. S2 typed gate registry G0–G8 + logging P2 — DONE 2026-10-11 (G8 batches f875520f1; Opus G8 review 0ada1fc81 ACCEPT-WITH-FIX, fixes landed). LG19 baseline is 3, not 1 (meta-review, G8 review F5).
4. D3/D4 + seam #2 (gpu_abi) shader/UBO de-copy — DONE 2026-10-09 e3769f754 (f1be2a63f, 253cf6d1d).
5. S3 narrowing + `/std:c++23preview` + warnings-as-errors per dir — DONE 2026-10-09 eb1a949d1 (slices 1–3 merged).
6. clang-tidy safe set — DONE as C++23 step 1 (item 12) 2026-10-11 c023b4533..8334acdf2.
7. D5–D11 — DONE 2026-10-10: D5 519f9815f, D7 baf2bba49, D8 9fe2ec722, D11 17838e310 (D6/D9/D10 per `opus-consistency-review.md`: not re-verified here).
8. S4 — PARTIAL: file-size program (50 files >1500 at 1e2fa987d; remaining giants listed in `giant-file-split-plan.md`; function-level giants `finalizeGeometry`/`flush` accepted); VK RAII in one island and offline mission-load test (`mission-load-test-design.md`, low priority) NOT started.
9. Gate deletions B0–B6 — DONE 2026-10-09 7d535fc58 (47 gates, ≈ −4,300 lines).
10. S5 perf — baseline DONE 2026-10-11 f358efc3a (`docs/perf-baseline-2026-10-11.md`); perf changes only with Tracy deltas; nothing further started.
11. Legacy supersession deletions + repairs (Opus audit `legacy-subsystem-audit.md`) — DONE 2026-10-11 39df08046; review fixes landed. Rulings in force below.
12. C++23 idiom program — step 1 CLOSED 2026-10-11 (LG01 693, LG02 691, PCH 395a37af0); step 2 (lane 2, `cxx23-step2-define-enum-plan.md`) IN PROGRESS; step 3 (lane 1, `cxx23-step3-logging-plan.md`) B0–B6 DONE, review `cxx23-step3-review.md` 16936e6ab ACCEPT-WITH-FIX, remainder in `step3-lg12b-ledger.md`; step 4 (ownership) PLAN drafted (`cxx23-step4-ownership-plan.md`, untracked) — blocked on the Opus design review, the meta-review §E bar and Q9–Q12; step 5 (`Stuff::`→glm) last.
13. GPU particles parity → retire MLR (after C++23) — NOT started: bring `MC2_GPU_PARTICLES` to parity (effects-heavy captures, GL+VK), flip default ON, then delete the MLR clipper/lights/light map (~−3.5k lines).
- Meta-review ordering changes (§D) and ranked recommendations (§B) are not yet ruled on. Parked visual work (after debt paydown): VK mech shadows (`vk-mech-shadows-plan.md`), mc2_10 hi-res recook, road ground colour, parity items H2/H4/M1/M6/M7, dead shaders gos_grass.*/overlay_alpha_clear.frag (audit first).

## Rulings in force (owner)
- 2026-10-11 legacy: HostServices → DELETE; MLR in full = KEEP until GPU particles reach parity (lights/light map stay, DrawLightMaps on the phase-helper list); `Stuff::` math untouched until step 5, which opens with a bit-identity harness (doctest `Stuff::` vs glm over a randomized corpus + a sim checksum in the smoke tier) — sim migrates only if bit-identical, else render-side only; glm vendored into `3rdparty/` at step 5; EString migrates with the gui widgets in step 4.
- Step 4 lens (owner): not "wrap today's pointer flows in unique_ptr" — per subsystem state (a) the ownership model we would choose, (b) the path there, (c) what legacy shape must stay and why. Hard constraints are only on-disk/binary formats, gameplay determinism and mod CONTENT; in-repo tools (mc2fx, asset_viewer, mech_import, tg_import_dump, launcher, data_tools) are rebuilt in the batch that changes the API (`tools/build_all_targets.py` is the proof); breaking an EXTERNAL tool we do not build is a decision point, not a design input.
- 2026-10-09 gate rulings: `project-gate-deletion-decisions-2026-10-09` (memory); VK umbrella canonical for sub-gates. Delete superseded test infra and dead code after audit, tag first.

## Owner rulings 2026-10-11 (the former open questions, answered)
1. Sim determinism is a REQUIREMENT; also freeze mech/building animations in capture/smoke mode.
2. Saves load within a release line; across the ownership step only once the round-trip test exists.
3. Multiplayer (`code/multplyr*`) FROZEN out of steps 2–4; revisit later.
4. Windows-only is TEMPORARY: Linux and macOS playable later — keep portability seams, do not delete the Linux arm, stop claiming it is proven (rename the `LINUX_BUILD` misnomer later).
5. Readers may reject/clamp malformed mod content with a log line; never crash.
6. Codebase-wide comment pass = later, cheap OpenRouter agents, outside the main program.
7. Commit trailer model name: no preference.
8. Gate amnesty target stays < 30.
Step 4 decision points ANSWERED 2026-10-11: no external modder tools on the C++ surface; .fx parsed by their own parsers; ALLOC_STATS proof builds only; multiplayer drifts as a named exception (compile out behind a CMake option if a batch needs it; redesign later); SortList gets an explicit total order.

## Pointers
- Log: `.claude/cleanup/HANDOFF-LOG.md`. Reviews: `meta-review-2026-10-11.md` (ordering, proof gaps, step 4/5 bar), `opus-modernization-review.md` + `opus-consistency-review.md` (roadmap S1–S5, D1–D11), `cxx23-step3-review.md`, `legacy-deletions-review.md`, `legacy-subsystem-audit.md`, `split-batch-review-2026-10-10.md`, `typed-gate-registry-g*-review.md`, `tier1-flake-log.md`.
- Plans (all `.claude/cleanup/`): `giant-file-split-plan.md`, `cxx17-strangler-plan.md`, `seam2-gpu-structs-plan.md`, `logging-facade-plan.md`, `offline-checks-plan.md`, `checks-and-lanes-plan.md`, `from-scratch-review.md`, `hires-heightmap-plan.md`.
- Memory: `C:/Users/Joe/.claude/projects/A--Games-mc2-opengl-src/memory/MEMORY.md` (its RESUME line must point at this file); rule files `project-session-rules-2026-10-09`, `project-gate-deletion-decisions-2026-10-09`.
- Pre-restructure handoff (full prose): `git show 204898a0b:.claude/cleanup/ORCHESTRATOR-HANDOFF.md`.

## Session exit bar
Ratchet baseline tightened (counts only down), check-contracts all green, tier1 smoke 5/5 (or smoke-lite inner loop + full tier1 at milestones), A/B viewed, memory handoff updated (`MEMORY.md` RESUME line).
