<!-- doc-status: stale | reviewed:2026-10-11 -->
# check-contracts rows and lane/lease machinery: single-lane cleanup plan

Read-only analysis (no builds, launches or check runs). Classification is from the row names, the wiring comments and script headers. Rows marked "candidate" were not diffed line by line.

## 1. check-contracts.sh (1476 lines, 151 `run_check`/`run_check_tri` rows)

- 46 rows are selftest/ratchet rows. They are counted under the row's own class below.
- Measured in the file's own comments, on the multi-lane board: ~340-360 s for ~130 rows, median 0.24 s. `no_raw_gl_from_game` takes ~122 s and `include_firewall` ~96 s, which together are ~60% of the run. Eight rows breach the 5 s budget (line 105-128).
- Sub-agent estimate for 151 rows: ~6-7 min cold, ~2.5 min once the two grep rows are fixed.
- I did not run the script. It claims to be read-only, but the selftests build temp hardlinks, and it also runs pytest and git. `--selftest` is the cheap probe.

Totals (approximate): KEEP ~128, OBSOLETE 7, REDUNDANT-candidate ~11, SLOW 5 (SLOW rows are KEEP-but-fix, not removal).

### OBSOLETE (multi-lane / worktree era)
| row (line) | why |
|---|---|
| deploy_lane_parity (935) | Compares deploy_lease.DEPLOY_FOLDERS against check-deploy-target.GOOD_DIRS. Two registries only exist because of many lanes. Dies with the lease. |
| deploy_lane_ratchet (937) | Selftest of the row above. |
| link_guard_selftest (1116) | check-hardlink-isolation.py (645 lines). Guards hardlinks shared between lane deploy dirs (LINK-GUARD-1). One deploy dir means nothing to share. Keep only if the cheap-install hardlink to the game data stays. |
| iso_deploy_selftest (1117) | make-isolated-deploy.py (618 lines). Builds per-lane isolated deploys. |
| beauty_sidecars (1143) and beauty_sidecars_selftest (1144) | The `--no-lanes` half still checks engine source, so trim the lane-audit code, keep the rows. Candidate only. |
| (script, not a row) check-worktree-context.sh | Stale-root-edit guard. Not wired into the board. Delete. |

### REDUNDANT candidates (verify before cutting)
- raw_gl_depthfunc, raw_gl_depthmask, raw_gl_colormask, raw_gl_blendfunc, raw_gl_fbobind (802-806) overlap raw_gl_capstone (807) and no_raw_gl_from_game (742). Six per-state scripts could be one.
- colormask_ownership (798) vs raw_gl_colormask (804).
- binding_slots (746), vulkan_bindings (815) and sampler_bindings (765) overlap. The selftest pair can be one row.
- pass_attachment (799) and pass_coverage (1013). Also render_ctx_parity (1014) and editor_pass_parity (1015).
- gpu_buffer_owners (812), buffer_lifetime (811), drawbuffer_ownership (801), mdi_submission (810).
- delivery_manifest (967) and delivery_installs (1012). delivery_selftest (968) probably covers both.
- smoke_fingerprint_ratchet (898) vs smoke_fingerprint_honesty (894). Different modes, so merge into one invocation.
- capture_determinism_ratchet vs capture_determinism, capture_env_ratchet vs capture_env_declared. The same pairing pattern applies to the other 22 selftest pairs. Each is a separate `py -3` start (~0.2 s), so merging into a single `--quiet --selftest` call would save time without losing coverage.
- unified_projection_retirement (1324). Retirement checks can be dropped once the thing is gone. Candidate.

### SLOW (keep, but fix)
no_raw_gl_from_game (~122 s), include_firewall (~96 s), flag_registry_truth (repo-wide getenv sweep), contract_coverage, doc_links. Also watch scripts_tests (pytest, ~2.3 s, fine).

### KEEP
Everything else guards real invariants (shader/SPIR-V/pipeline, render-resource ownership, env/flag registries, sim identity, stock mission data, asset manifests, doc links, capture/oracle honesty, deploy coherence). The `disk_guard_selftest` row (1095) is borderline: ENOSPC was caused by sibling worktrees, but the guard is generic, so keep it.

## 2. Lane/lease machinery

| file | lines | what | |
|---|---|---|---|
| scripts/smoke_lib/deploy_lease.py | 419 | LRU deploy-folder lease: `auto_acquire`, `acquire_lease`, `release_lease`, `touch_lease`, `_registry_lock`, `DEPLOY_FOLDERS`, `_short_name`. Env: `MC2_SMOKE_LEASE_TTL_SECS`. Registry `A:/Games/mc2-opengl/.smoke_leases.json`. | remove |
| scripts/test_deploy_lease.py | 380 | Tests for the above (`Harness` class). Not on the contracts board. | delete |
| tools/repo_intel/lane_registry.py | 341 | `register_lane`, `release_lane`, `list_lanes`, `check_conflict`; `files_claimed` overlap. Env: `MC2_LANE_REGISTRY_PATH`, `MC2_LANE_TTL_SECS`. | delete |
| tools/repo_intel/test_anti_churn.py | n/a | Tests lane_registry (`lr.` calls) and mission_facts/gate_status. | trim lane tests only |
| scripts/mcp/repo_intel_server.py | 773 | MCP tools `register_lane`, `list_lanes`, `check_conflict` (line ~573), `deploy_status`; imports `lane_registry as lreg`. | drop 3 tools |
| .mcp.json | 20 | `MC2_LANE_REGISTRY_PATH`, and `REPO_INTEL_ROOT` points at the `nifty-mendeleev` worktree inside opengl-src, not at mc2-main. Stale. | fix |
| scripts/run_smoke.py | 3575 (96 lane/lease hits) | `--no-lease`, `--deploy`, `--guard`; `_lane_lock_token` (316); lease block 575-750; per-lane lock file `smoke.<lane>.lock` (952-1010); `touch_lease` (1221); release (3558). | simplify |
| scripts/check-deploy-target.py | 500 | Guard: broken tree, `.mc2_deploy_lane.json` contention, fingerprint, manifest coherence. | trim |
| tools/repo_intel/deploy_status.py (266), link_group.py, mission_facts.py | n/a | Import `deploy_lease.DEPLOY_FOLDERS`; mission_facts has a fallback copy. | edit |
| scripts/deploy_payload.py (1404) | n/a | Mentions `.mc2_deploy_lane.json` as a file it must not hardlink. | minor |

### What removal entails
1. Callers of `--no-lease` (must drop the flag): check-brain-opord.py:104, check-combat-soak.py:81, check-file-resolve-miss.py:115. Anything else passing `--deploy` or `--guard` must also change.
2. In run_smoke.py, `--exe` becomes required. Keep a single `smoke.lock` (or reuse the one lock) instead of per-lane tokens. Delete `_lane_lock_token` and its unit test.
3. Env vars to retire: `MC2_SMOKE_LEASE_TTL_SECS`, `MC2_LANE_REGISTRY_PATH`, `MC2_LANE_TTL_SECS`. Also drop them from the env registry/allowlist, otherwise `env_registry` and `env_drive_guard` go red.
4. Rows to remove: deploy_lane_parity and ratchet (and scripts/check-deploy-lane-parity.py, 245 lines); link_guard_selftest and iso_deploy_selftest (plus check-hardlink-isolation.py and make-isolated-deploy.py unless hardlinked installs stay).
5. Docs: BUILD-WIN.md, docs/deploy-isolation.md, docs/build-parallel-and-tooling.md, .claude/cleanup/scout-*.md. `doc_links` will catch dangling links.
6. check-contract-coverage.py will flag any orphaned check script. Check it after deletion.

### Still protects the single lane (do not delete)
- DEPLOY_FINGERPRINT check in run_smoke.py (1113-1133, 3101-3344) plus scripts/exe_fingerprint.py and smoke_lib/fingerprint.py. This catches stale or dirty exes. It matters more with one lane because a stale exe is easy to smoke by mistake. Rows: smoke_fingerprint_honesty and ratchet. Tests: scripts/tests/test_fingerprint.py and test_deploy_staleness.py.
- check-deploy-target.py's broken-tree check and manifest coherence (`check_broken_tree`, `check_manifest_coherence`, `check_fingerprint`). Only the lane-contention half is obsolete.
- Same-path game-running protection in run_smoke.py: `_same_path_mc2` (276), `_taskkill_mc2` (343), the tasklist PID check, and the single lock. This prevents two smoke runs or an interactive game from clobbering the exe. Keep one lock.
- disk-space guard, deploy_data_parity, deploy_coherence, check-background-launch.
- The lane lock file `.mc2_deploy_lane.json` is also the only record of "who deployed this dir"; replace with the deploy manifest before deleting it.
- Adversarial note: `include_firewall` and `no_raw_gl_from_game` are slow but are real invariants. Do not delete them just because they are slow.

## 3. Open risks
- Another worktree still exists (`A:/Games/mc2-opengl-src`, branch `claude/community-knowledge-pack-1`), and `.mcp.json` still points to it. Confirm it is dead before removing the shared registry.
- Line counts for REDUNDANT rows are unverified; diff the scripts before merging them.
