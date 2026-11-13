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
At 69ecfebc2: lint HEAD PASS (20s), doc-link delta PASS, docs-only dry run selects
doc_links only, sim-touch dry run selects five source rows and all six sim rows.
Native core: one RelWithDebInfo Ninja build/run PASS, 4,081 cases passed and
14 skipped, 33s including build. Default slow groups are excluded; nightly is
covered by the runner-selection tests. No game, deploy or remote run needed.
Final delta also scopes merge-time ratchets and semantic inventory checks to
the same table (explicit lint/standalone proof still runs the requested checks).
Merge-delta tests: 238 passed in 68.70s. Final receipts (lint and docs/sim dry runs)
are refreshed after committing this note, then archived under
E:/mc2-archive-2026-10/proofs/checks-path-scoped-1/; the hand-back names that tip.
