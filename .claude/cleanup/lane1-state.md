<!-- doc-status: current | reviewed:2026-10-12 -->
# Lane 1 state (builder on A:/Games/mc2-main, branch claude/nifty-mendeleev). Hand-over file for the next agent; keep current at every report.

Session 2026-10-12 late evening (lane-1 Sonnet, short fix session, cap 3 h): the six findings of `reviews/lane1-2026-10-12-cleanup.md` (ACCEPT-WITH-FIX), in the
review's order. Never pushed. Install `mc2-win64-v0.4`. Lane 2 untouched (no edit under GameOS/, RenderCore/, shaders/, Gates/, tests/unit/test_gate.cpp); no B1 code.
Previous session (ABL string safety, determinism, B1 notes, checkpoint): `git show 6391a5e0b:.claude/cleanup/lane1-state.md`.

## Commits this session (oldest first)
- 948d3ffbf F1. `getRelativePositionToObject` tests its output pointer unconditionally (not behind `MC2_ABL_ARG_GUARD`) and logs one `[ABL v1] event=bad_argument`
  line. The two relative-position builtins moved from `code/ablmc2_misc.cpp` to `code/ablmc2_position.cpp` so the unit tier can link the REAL binding: new
  `tests/unit/abl_exec_shim.{h,cpp}` answers the interpreter stack (`ABLi_popInteger/Real/RealPtr`), `CurObject`/`ObjectManager` and the mover geometry from a
  scripted queue; `tests/unit/test_abl_position.cpp` runs `execGetRelativePositionToObject` for a non-mover (zero vector over garbage), an unknown id, and a null
  output with the guard off (stderr captured: one line, stack balanced). A raw-memory `GameObject` stands in for a non-mover (a real one needs the object pool); the
  mover branch cannot be run in the unit tier (needs a constructed Mover). The commit alone leaves one LG06 (`const char*` in a local helper) that dd56665c4 removes.
- dd56665c4 F2. Executor writers bounded: `dest = "text"` (`copyBounded` + `assign_truncated`; unterminated target fixed), by-value array parameter (copy the smaller
  declared size, zero the rest, `array_param_truncated` if cut), array formal of the entry module in `ABLi_execute` (the old code copied from the stack top: the block
  starts zeroed, one `bad_argument` line, `pushAddress` instead of overwriting the previous cell). One reporter for all `[ABL v1]` reject/clamp lines:
  `mclib/abl_script_events.{h,cpp}` (16 lines per event, whole event literals, no per-call-site counters). `routine()` copied a state name into the 128-byte
  `StateHandleInfo::name` unchecked: now an `ABL_Fatal` (compile-time, modder content). **Writer census** `tools/abl_writer_census.py` + `abl_writer_census.baseline.json`
  + `tools/test_abl_writer_census.py` + board row `abl_writer_census`: **114 rows / 178 sites = bounded 13, fixed 26, open 16, read 37, engine 83, frame 3**.
  `abl-string-audit.md` section 1 rewritten to the census (it had said "the only writes were the two above" and understated the int/real out-array class: 16 open sites,
  14 builtins/rows, listed there). Log tag census re-frozen (one emitter added: `bad_argument`). Callers inventory + allowlist updated.
- 011a2d29c F3. Doctests: exact fit, capacity 1, empty item, `concat(s, s)` aliasing; `ABLi_popCharBuf`'s accept test is the pure `abl::charBufOf` (count 0, -1, scalar,
  other element type, null address). The helper-only zero-vector case was removed (the real binding is covered by F1).
- 08c172454 F4/F5. `appendBounded` = clamp the text end + `copyBounded` on the tail; `mclib/abl_out_params.h` deleted (one caller, a 3-assignment wrapper; the site comment
  says the zero vector is a chosen answer). Literal event names, CurFSM ternary and the no-op `static_cast` landed in dd56665c4.
- 5b127ac85 F6. `cxx23-step4-b1-notes.md` corrected (nine rows, 67 files in B1.1-B1.7 = 2+19+2+15+11+10+8, `mechcmd2_frame.cpp` touched but not an EString-token file, B1.4 15
  files, "15 batches" -> B1.2-B1.7, H11 `alloc_probe.cpp:89`, objective.{h,cpp} 175 word occurrences (179 substring hits incl. 4 `CEStringList`), 5 other includers) and
  `cxx23-step4-b1-files.json` (file -> batch, asserted equal to `git grep -lw EString 642019a64`). NOTE: `dbb25604a` (Opus before-review, APPROVE-WITH-CHANGES) landed
  while this ran; its changes to the notes are NOT applied here.
- ecf1a57e5 board: `docs/split-map.md` lists the new TU (the one red row after the checkpoint run).

## Checkpoint (HEAD ecf1a57e5, exe built and deployed from it, fingerprint `ecf1a57e536d`)
- ninja `mc2` + `mc2_launcher`: rc 0. `build_contract_harnesses.py --run --unit`: 20 harnesses PASS; doctest 2757 test cases passed, 1 skipped (the one "failed" assertion in the totals line is the
  `should_fail` self-test of the printf corpus; status SUCCESS). The previous session reported 2750 passed / 1 skipped.
- `check-contracts.sh`: 169 passed / 1 failed (`split_map`, fixed by ecf1a57e5 and re-run green); `abl_writer_census`, `log_tag_census`, `script_callers`, `tools_tests`
  (108 modules) PASS. `cxx17_ratchet --check` (HEAD) OK, nothing raised, 0 fewer.
- tier1: `2026-10-12T18-32-25` = 5/5 PASS on exe `ecf1a57e536d` (mc2_01 149 fps, mc2_03 84, mc2_10 107, mc2_17 135, mc2_24 47 with p1 2 fps and 5606 of 8192 frames:
  the host was loaded, the verdict is PASS, no rerun needed). No smoke ran after the last doc commit; the install is re-deployed from the final HEAD (code identical to the tested exe).

## NEXT for the next owner, in order
1. Apply `dbb25604a`'s changes to `cxx23-step4-b1-notes.md` if the orchestrator rules so; B1.0 code only after that (bridge, pins, `/analyze` recipe with a seeded negative, C4840).
2. ABL int/real out-array slice: the 16 `open` census sites (`ABLi_popIntBuf` / `ABLi_popRealBuf` + clamp + `<name>_truncated`); each row turns `bounded` in the baseline. The census
   `--check` is the guard that no new writer slips in.
3. Data pass for the 8 tight debug strings (`abl-string-audit.md` section 3; needs the owner to say who edits `.abl` content).
4. mc2_07 tier2 on an idle host (two `heartbeat_freeze_play` at 3-5k frames on 2026-10-12, host at 100 % CPU).
5. Save round trip baselines (`tests/saveload/baselines/` do not exist); B0 remainder: alloc_probe into a proof build's gos_Malloc, mc2_15 under ASan.
6. Open design note from the review (F5): the zero vector for a non-mover is a valid world position, so a script that feeds it to a move order now walks to the origin where it
   kept a stale point; only the 4 determinism missions and mc2_15 were run. For the Opus / orchestrator to rule (keep zero, or a sentinel).

## Tool notes
- `grep -c $'\r' file` in this Bash tool matches EVERY line (the escape is lost): use Python (`b.count(b"\r\n")`) or `git ls-files --eol`. The index stores LF for most files; some
  working copies are CRLF (`ablxstmt.cpp`, most of `ablrtn.cpp` which is MIXED, the `.claude/cleanup/*.md` files). `tools/dev/apply_edit.py` keeps a file's own ending but treats a
  MIXED file as LF: for `ablrtn.cpp` apply byte-level edits with explicit `\r\n`.
- **The Bash tool eats backslashes in heredocs** (`\\b` -> a backspace byte, `'\\0'` -> a NUL byte inside Python-in-heredoc edits). Write regex/escape-carrying text with the Write tool
  or `apply_edit.py` JSON, then grep the result for `\x08` / `\x00`.
- `cxx17_ratchet --check` reads the COMMITTED tree by default: use `--worktree` / `--staged` before committing, or a new `const char*` shows up only after the commit.
- `git add -N` a new file before `log_tag_census.py --check` / `callers.py` (both read `git ls-files`).
- The doctest tier compiles an engine binding by giving only that TU the extra include dirs (`set_source_files_properties(... INCLUDE_DIRECTORIES ...)` in `tests/unit/CMakeLists.txt`)
  and answering what it links against in `tests/unit/abl_exec_shim.cpp`; add to the shim only what the linker demands.
- `sim_determinism.py` / `run_smoke.py` cap `--duration` at 30 s; mc2_03 needs the full 30 s on a loaded host to pass the 1000-frame baseline.
- `run_smoke.py --keep-logs` keeps `tests/smoke/artifacts/<id>/<mission>.log` on a PASS.
- A foreground `until grep -q "^rc=" log; do sleep N; done` works for waiting (a bare `sleep N` is blocked); the tool moves it to the background after 600 s and still notifies.
- Fingerprint: committing ANY file (even a doc) after the build makes the deployed exe stale against HEAD (`DEPLOY_FINGERPRINT`); relink + deploy before a smoke.
- tier1 on a loaded host: a `heartbeat_freeze_play` with p1 2-3 fps is the host; rerun that mission alone and record both report ids.

## Rules (unchanged)
- Commit by explicit pathspec after `git diff --cached --name-only`; `git commit -F <file>`; trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
  Commit BEFORE build + deploy. Launches only through `scripts/run_smoke.py --deploy 0.4` / `tools/sim_determinism.py` / `tools/saveload_roundtrip.py`,
  background-only, never `--no-lease`. Never kill cl/ninja by name (`tools/kill_build_tree.py`). Never contort code for a ratchet (rewrite the baseline with
  the reason). Never touch GameOS/, RenderCore/, shaders/, Gates/ beyond a task's need, nor tests/unit/test_gate.cpp, nor gos_static_prop*, nor code/multplyr*.
