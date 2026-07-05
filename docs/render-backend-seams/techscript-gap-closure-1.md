# TECHSCRIPT-GAP-CLOSURE-1 — five-slice closure of the TechScript v1.5 (discussion #18) gap

**Full ledger:** `.claude/TECHSCRIPT-GAP-CLOSURE-1.md` (every spec feature → status, spec-deltas, conflict resolutions, remaining slices). This file is the arc's render-backend-seams pointer.

Branch `claude/techscript-gap` off `c18618c4`; one commit per slice; every slice default-OFF, gate-OFF byte-identical, relaxed-guard checker green (6 call-sites, unchanged), harness suite green both intent-queue gates, `mc2` builds green each commit.

| Slice | Gate | SHA | One-liner |
|---|---|---|---|
| HARNESS-STUB-REPAIR-1 (prep) | — | `9ce8ba27` | Restored the offline dispatch harness build (BRAIN-ENGAGE-1's mover/dcontact/contact includes had broken it) |
| BRAINSPECIAL-ALIAS-1 | `MC2_BRAIN_ALIAS` | `9bd8ad17` | Data-driven alias registry: built-in seeds + `Aliases { a = "canonical" }` block + per-block `alias=` key resolution + case-insensitive catalog shorthand |
| BRAINSPECIAL-SCOPE-GLOBAL-1 | `MC2_BRAIN_SCOPE_GLOBAL` | `a5147373` | `global_specials.fit` merged as a shared Call-target library (mission-local wins; globals never provide the entry body); fixed RawScan double-commit quirk |
| BRAINSPECIAL-VARIANTOF-1 | `MC2_BRAIN_VARIANTOF` | `089f1099` | `variantOf=` inheritance after global merge (empty Body inherits; re-declared Body overrides; depth-8 + cycle guards) |
| BRAINSPECIAL-FLOW-WAIT-1 | `MC2_BRAIN_FLOW` | `f9a3a67f` | WAIT / WAIT_UNTIL / STOP as LATCHED SEQUENCE GATES over the every-tick re-execution model (spec-delta: not VM blocking; GOTO/LABEL declined per determinism ruling); `getBrainTimeMs()` sim-time base; per-verb-index refire guards replace the class-level once-guard pre-set for flow-bearing bodies |
| FITBLOCK-WRITER-1 | (pure utility) | `364fbbcc` | `code/fitblockwriter.{h,cpp}` — raw `Brain{}`/`TechSpecial{}` brace-block writer (the editable-editor keystone); 13/13 round-trip self-test against the real scanner |

**Proof machinery added:** 9 new fixtures (manifest 27→36), harness `flow_sequential` mode (3 passes with settable stub sim-time), `--fitwriter-selftest`, stub headers for the ENGAGE-1 dependency set.

**Spec-deltas (documented for the author, ledger §Spec-name map):** WAIT = re-queue/latched-gate semantics; variantOf override granularity = whole Body; GlobalSpecials are library-only (never entry bodies); GOTO/LABEL replaced by Call + SetState + latched gates; converter stays external tooling; internal label remains BrainSpecial.
