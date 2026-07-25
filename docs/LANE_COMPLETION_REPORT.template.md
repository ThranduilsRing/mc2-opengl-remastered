---
# LANE COMPLETION REPORT
# Fill every field. Parser: scripts/parse-done-reports.py
# Required fields: branch, commit, base_commit, files_changed, risk_class,
#                  build_run, smoke_run, deploy_state, merge_recommendation
# Copy this file to LANE_COMPLETION_REPORT.md at your worktree root.

branch: claude/<slug>-1
commit: <full sha>
base_commit: <full sha of the nifty commit this forked from>
files_changed: 0
risk_class: low          # low | medium | high
build_run: PASS          # PASS | FAIL | skipped
smoke_run: PASS          # PASS | FAIL | skipped
deploy_state: deployed   # deployed | not-deployed | N/A
merge_recommendation: READY   # READY | HOLD | NEEDS-REVIEW
---

## What landed

<!-- One paragraph. What the lane did. No bullet soup — a future session
     should be able to read this cold and understand the change. -->

## Files changed

<!-- Key files only, one line each, with a note on what changed.
     The parser uses `files_changed` for the count; this section is prose. -->

## Gates run

<!-- List every gate command + result verbatim, e.g.:
     - tier1 5/5 PASS (2026-06-13 sha abc1234)
     - oracle_report --strict exit 0
     - py -3 scripts/deploy_payload.py --target game --verify-only exit 0
-->

## Restore notes

<!-- How to undo this if it needs to be reverted. Name the kill-switch env var
     if one exists, or the specific commits to revert. If "just revert the merge
     commit", say so. If state was mutated outside git (deploy dirs, manifests),
     list what needs manual cleanup. -->

## Known follow-ups

<!-- Deferred items that this lane deliberately left open. Each item should
     have enough context for a fresh session to pick it up. -->

## Merge recommendation

<!-- Expand on the frontmatter merge_recommendation field.
     READY     — tier1 clean, oracle clean, no open blockers, safe to merge.
     HOLD      — passing but waiting on something (state it explicitly).
     NEEDS-REVIEW — specific concern that must be resolved before merge. -->

## Bookend reviews (STANDING USER RULE, 2026-07-25 — both are mandatory)

Full checklist: `docs/disciplines.md`. A lane may not report done without §2.

### META REVIEW (done before starting)

<!-- What state did you inherit, and what did you find WRONG about it?
     Reader-source gate defaults; what a DEFAULT LAUNCH executes; -dirty or
     unattributable prior evidence; orphaned/uncommitted work + live lane claims;
     superseding/regressing commits; and any of the BRIEF'S OWN PREMISES that
     turned out to be false. Say "no corrections" only if you actually checked. -->

premises_corrected:        # none | <list what the brief got wrong>

### ADVERSARIAL SELF-REVIEW (done before reporting done)

<!-- Try to refute your own claim. Answer the first question in plain words. -->

user_sees_it_on_default_launch: NO   # YES | NO | N/A
<!-- If NO: this work is NOT DELIVERED. Say so explicitly and name the gate
     plus who has to approve flipping it. Do not report it as fixed. -->

gate_registered:               # yes | no | N/A
gate_smoke_allowlisted:        # yes | no | N/A   (no => both A/B arms ran the
                               #                   same path; verification void)
clean_fingerprint_build:       # yes | no  (MC2_BUILD_GIT_DIRTY 0, md5-verified)
gate_string_in_deployed_exe:   # yes | no | N/A  (a rebuild can fail silently)
verified_where_defect_manifests: # yes | no — see the Standing Capture Rule in
                               #   docs/visual-verify.md (full-frame wide verdict
                               #   shot, >=3 locations incl. a control area,
                               #   day AND night, mc2_01/mc2_03/mc2_10/mc2_24)
regressed_a_user_approval:     # no | <what>

residual_risk:
<!-- Plainly. What could still be wrong. -->

not_done:
<!-- Plainly. What you did NOT do, and why. -->
