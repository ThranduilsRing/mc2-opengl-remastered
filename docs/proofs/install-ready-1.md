# install-ready-1 state

Base: 434a42c8d96514bb4b8496049a1a7663c36ee6e9; branch claude/install-ready-1.
Scope: Astra review edits; temporary fixture tests only, no engine build/deploy/start.

## Findings

- Stale mc2.exe/runtime.dll bytes reproduce manifest rejection before copying
  (scripts/make-isolated-deploy.py:294, exit 2), or after verification during
  copying (:687, exit 4). Both are required integrity refusals, not a reason
  to accept changed bytes. No completion manifest or lane receipt is published.
  Recovery requires restoring bytes matching the manifest or recreating the
  seed from a valid deployed source; refreshing hash caches cannot authorize drift.
- A receipt-bearing lane with five shared links per executable, DLL and manifest
  already succeeds: prepare_seed_delta (scripts/deploy_payload.py:2668) detaches
  changed write destinations, then the guard passes and deploy preserves every
  seed/peer byte. The actual call order is :2858-2864, including the manifest.
  No production change needed. The new fixture uses the real isolation guard.
- Without a receipt or explicit legacy eligibility, shared writes still refuse.
  The fringe cont1 receipt names an unprepared /workspace/slot1/install on the
  box; :2859-2860 only allows legacy PC installs under the configured lane root.
  This fixture reproduces that intentional refusal; eligibility is not broadened.
- Nonempty resume remains deferred: make-isolated-deploy.py:946 refuses it;
  .lane-seed.json has no lane ownership and is written after copying (:1033).
  Aggregate lane_slot readiness remains deferred by the review edits.

## Offline proof

New cases: test_isolated_deploy.py:216 (four cases),
test_deploy_payload_papercuts.py:19 (two cases); all use tmp_path fixtures.
Command (PYTHONUTF8=1; TEMP/TMP/TMPDIR and cook cache set to lane scratch):

```text
py -3 scripts/run_scripts_tests.py test_isolated_deploy test_deploy_payload_papercuts --jobs 4 --no-cache
scripts_tests: 2 modules, 2 green, 0 failing
```

Initial attempt failed in the base runner's cp1252 subprocess decoder, before
reporting results. UTF-8 mode resolved the environment issue without code changes.
No engine build, installed-game deploy, game start or full nightly row run.
Next: local path-scoped commit, one final new_drive_paths lint, remove lane scratch.
