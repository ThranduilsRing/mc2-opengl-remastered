checks-path-scoped-1 state (2026-11-12)

Base 3bfbe445a; assigned slot08 / claude/checks-path-scoped-1. Read lane rules,
CLAUDE.md and prune slices A/B/C. Scope: six requested items only; preserve box
tooling, identity/deploy isolation, tier1, capture compare, core doctest and ABI.
Implemented all six items with the existing sim rules and input table. Full Python
suites and slow doctest groups remain nightly. Capture-after seeds the existing
cache, keyed on executable/content/options/poses/helpers/effective device/GPU.
The digest tests now use fixed rules input; digest budgets are unchanged.
Only the 1,979-line light-band study and its registrations/reference are removed.

Green: 274 tests in the broad pass plus the 20-test validation delta; the two
remaining red digest tests in that pass now use SAMPLE (delta green). The three
reported clean-tip failures are fixed. Owning test_contract_coverage and
test_contracts_cache suites pass; cxx17 --check --worktree passes. Caller,
log-tag and sim-firewall rows pass. Doc-link row found the retired study
reference; removed it. Logs: G:/mc2-tmp/slot08/checks-path-scoped-*.log.
Next: local commit, lint HEAD, doc-link delta, unit core build/run, dry-run docs
and sim synthetic diffs; archive receipts and record final SHA in hand-back.
