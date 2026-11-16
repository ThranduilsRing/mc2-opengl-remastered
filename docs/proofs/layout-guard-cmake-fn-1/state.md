# Layout guard CMake function parsing

- Assigned base: 0db3181b3b440021d87eb4f52f3b0645356d3745.
- Before: checker --quiet exits 1 (function-local MC2_VULKAN missed).
- Change: scripts/check-gated-layout-macros.py expands single-parameter helpers.
- Regression: scripts/tests/test_check_gated_layout_macros.py covers literal calls.
- PASS: --write --quiet and subsequent --quiet both exit 0; no violations remain.
- PASS: run_scripts_tests.py test_check_gated_layout_macros --jobs 4 (1 module).
- Contracts: default -j4 exits 0, selects 0 rows; explicit --nightly --rows
  '^gated_layout_macros$' exits 0 (1 PASS). No complete-board coverage claimed.
- Evidence: E:/mc2-archive-2026-10/proofs/layout-guard-cmake-fn-1-*.log.
- Complete; next action is orchestrator review of the local commit.
- Tooling only: no build, game, deploy, CMake or README changes.
