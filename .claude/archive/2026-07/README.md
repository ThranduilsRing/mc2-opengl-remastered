# DOCS-RETIREMENT-SWEEP-1 — census + retirement record (2026-07-01)

Archive convention: superseded/stale `.claude/` working docs get `git mv`'d here with a
2-line tombstone header (why retired, what supersedes). Conservative: archive, never delete.

## Census result — this worktree (`claude/controlmap-sample-1`)

`.claude/` (32 docs): **ACTIVE 26** (16 tracked terrain-v2/skybox/water recons dated 2026-07-01,
9 untracked arch-review/moddability/markings/smoke-latency docs dated 2026-07-01,
unit-kind-classcheck-audit — unit-profile arc in flight), **REFERENCE 6**
(HANDOFF-frame-graph-arc, FIXB-MVP-CURRENCY-1-SCOPE, PROJZ-GL-UNIFICATION-RECON-1,
LOW-CAMERA-GROUND-MODE-RECON-1, FRAME-RESOURCE-LEDGER-1 — post-slice version, newer than root's,
LINEAR-COLOR-AUDIT-1), **SUPERSEDED 0, STALE 0**. → Nothing archived from this branch.

`docs/` (915 docs): topic-tree set (critical_inline_rules, tier1_env_vars, known_issues,
active_campaigns, renderworld_arc_status, asset-pipeline, disciplines, cxx17-coding-rules)
verified byte-identical to canonical nifty except active_campaigns.md — 5 stale claims
corrected in place 2026-07-01 (CSM gate, H1c/H2 status, drawPass Slice B, shadow cherry-pick).
Historical plans/specs corpus (docs/plans, docs/superpowers) uses **in-place SUPERSEDED banners**
by convention (~8 whole-file, ~20 partial) — left in place: banners are self-tombstoning and
moving them breaks relative cross-links cited as "amendment trail".

## Root-repo-only retirement candidates (`A:/Games/mc2-opengl-src/.claude/`) — FOLLOW-UP, do not cross-commit

A root-lane (branch `claude/animated-prop-cook-recon-1`) sweep should archive these to its
own `.claude/archive/2026-07/`:

1. `FILE-AUDIT-SCORECARD-1.md` — banner'd "SUPERSEDED for code-action by FILE-AUDIT-CORRECTIONS-1.md" (`74a38efc`).
2. `ENGINE-SCORECARD-1.md` — same SUPERSEDED banner (`74a38efc`).
3. `PORTFOLIO-RECON-LANES.md` — banner'd "SUPERSEDED BY VERIFICATION" (raw pre-verification scout output; PORTFOLIO-TARGETS-VERIFIED-1 is authoritative).
4. `FRAME-RESOURCE-LEDGER-1.md` (root, untracked) — **STALE**: still claims `sceneColorCopyTex_` MISSING ("the keystone gap"); VFX-SCENECOLOR-GRAB-1 shipped it (`docs/VFX-SCENECOLOR-GRAB-1.md`; post-slice ledger version lives on `claude/controlmap-sample-1` `.claude/FRAME-RESOURCE-LEDGER-1.md`). Replace root copy with the post-slice version, then keep ONE.
5. `launcher-import-design.md` — explicitly superseded by `launcher-import-design-v2.md` ("Supersedes launcher-import-design.md").
6. `docs/active_campaigns.md` (root checkout) — has an UNCOMMITTED reconcile (CSM/H2/Slice-B corrections) duplicating what this branch committed 2026-07-01; land or discard to avoid divergence.
7. Full census of the ~87 untracked root `.claude/` docs (recons/scopes/plans from many finished arcs) still needed — untracked files can't be `git mv`'d with history; commit-then-move or move-as-new.

NOT retirement candidates (checked): `STALE-PREMISES.md` (active tombstone ledger),
`GPU-BUFFER-OWNER-NEXT-SLICES-1/2.md` (both feed the in-flight GPU-buffer-owner arc).
