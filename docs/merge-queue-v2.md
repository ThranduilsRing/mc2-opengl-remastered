# Merge queue v2 (design note, not built)

v1 (tools/lane_merge.py --stage/--promote/--bisect, scripts/merge_gate.py durable verdict) stages frozen tips on
claude/stage-W, gates the candidate, and fast-forwards nifty only on a complete PASS. v2 picks up what v1 left out:

- astra-review-r7 changes 4 and 5 (out of v1 scope).
- Informational inventories as gate artifacts: vk-ownership-ledger.md, gated-layout-macros.md and
  binding-slot-occupancy.json are still committed because their contract rows (vk_ownership_ledger --check,
  gated_layout_macros, binding_slots) compare against the committed copy. Moving them out means turning those rows
  into "generator runs clean" checks and publishing the output under the verdict's artifacts; do it as one change so
  coverage never drops.
- Automatic bisect: v1 prints the next --stage command; v2 can drive stage -> gate -> bisect without the orchestrator.
- Speculative pipelining: gate the second half of a red batch on a parallel candidate while the first half retests.

## Diff-scoped gate (built, scripts/gate_scope.py)

`merge_gate.py --full` decides the `mods` and `video-decode` stages from the candidate diff: merge-base of the head in
`build64-ninja/last-full-gate.json` to HEAD. A stage whose inputs the diff does not touch is recorded in
gate-verdict.json under `scope_skipped` as `SKIP(scope: <reason>)` and in the row status; it is never PASS, but it does not
make the verdict PARTIAL, so `--promote` still works. Unknown paths (not docs, tooling, shaders, art, or known C++ roots)
count as touching everything.

| Stage | Runs when the diff touches |
|---|---|
| `mods` | `data/`, `mods/`, `examples/mods/`, `tests/mods/`, `tools/mod_smoke.py`, `tools/mod_*`, `tools/validate_mod_package.py` ONLY (direct mod content; engine C++ incl. VFS/loader/mission code, shaders and unknown paths never trigger mods; `--weekly` runs it regardless) |
| `video-decode` | `scripts/video_decode_smoke.py`, `code/mc2video*`, `code/mc2movie*`, `code/controlgui_chat*`, `code/tick_commit*`, `gameos/sim_overlap*`, C++ whose name says video, movie, decode, mve, bink, ffmpeg, avsync, audio; any movie file (`.bik .avi .mp4 .mve .webm .ogv .mkv`) |

The real-decode `video-decode` launches run only on an explicit `--stages video-decode`, never in a daytime or `--nightly` gate (owner 2026-11-06: play-throughs cover video); every gate covers the decoder-open deferral offline (`tests/unit/test_playmovie_deferral.cpp`). Under `tests/`, `scripts/` and `tools/` only the video files above force it; other paths there never do.

Other stages (tier1, player-path, determinism, saveload) always run.

- `--nightly` forces every stage, mods included (implies `--full`). Run it overnight.
- The mark: a scope-skipped gate may publish, but `last-full-gate.json` only moves when every stage ran, so it always
  names the last all-stage gate (and the scoped diff and `dup_check` base stay honest).
- 30 h rule: if the mark is older than 30 h (or missing, or the diff cannot be computed), the next gate runs every stage.
- Determinism rows run 2 at a time (worker A on the stock install, worker B on a stock copy, `MC2_HOST_GAME_SLOTS=2`,
  each row takes its own FIFO host slot) whenever mods is active or at least two determinism rows are live. Saveload rows
  stay on worker A (private `--save-dir`; not proven safe on the clone).
