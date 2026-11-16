proof-plan-check-1: tests/tools only; base 434a42c8d, assigned branch verified.
Scope: reject via-menu Tracy without in-window readiness; explicit intro skip;
300 s smoke via-menu default/cap; accurate nightly-only zero-row hint.
Sites: tools/tracy_capture_compare.py capture_trace/game_env/main;
scripts/run_smoke.py duration validation/via-menu call;
scripts/check-contracts.sh run_nightly/zero-row summary/selftest.
Done: items 1-3 implemented; intro remains on unless --skip-intro is explicit.
Proof: run_scripts_tests.py --no-cache --report with test_tracy_capture_compare,
test_via_menu: green; test_run_smoke_early_validation: green after correcting
the new test's expected owner-only exit code (6). Contracts --selftest: PASS
using C:/Program Files/Git/usr/bin/bash.exe (bare bash resolved to WSL).
Receipts: E:/mc2-archive-2026-10/proofs/proof-plan-check-1/.
Next: local commit, new_drive_paths.py --base claude/play-1113 --head HEAD once.
No build, deploy, game, or proof_plan.py. No scratch installs created.
