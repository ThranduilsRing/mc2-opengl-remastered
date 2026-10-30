<!-- doc-status: current | reviewed:2026-10-11 -->
# Release process (maintainer runbook)

End-to-end recipe for cutting a public release. Wraps
[release-checklist.md](release-checklist.md) (the zip-integrity gate) with the
surrounding steps: version bump, changelog, tagging, packaging, verification,
and upload.

## 1. Where the version lives (bump policy)

`project(mc2 VERSION ...)` in `CMakeLists.txt` is the semantic version source.
Match the release tag to that value; directory names are not version authority.
The executable's commit fingerprint supplies exact build identity:

| Place | What it carries | Bump action |
|---|---|---|
| `cmake/GenerateBuildFingerprint.cmake` → `build64/generated/build_fingerprint.h` | git SHA + dirty flag, embedded in the exe and printed as the `[BUILD_FINGERPRINT v1]` startup banner | automatic — but **commit before building**; a `-dirty` fingerprint is unattributable and `scripts/deploy_payload.py` warns on it |
| Release lane / deploy folder name (e.g. `A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1`) and the `DEPLOY` env consumed by `scripts/build_release.sh` | the human-facing version string | create a new lane per release; point `DEPLOY` at it |
| `CMakeLists.txt` project VERSION + GitHub Release tag/name | what users see | bump the project VERSION; tag `v<version>` |

Bump policy: patch (`v0.5.0` → `v0.5.1`) for fix rounds on the same feature
set; minor for a new feature wave; `-rc1` suffixes for candidate lanes that
may still be respun.

## 2. Per-release changelog template

Draft release notes as `.claude/RELEASE-NOTES-DRAFT-v<version>.md` while the
merge wave is in flight, then finalize into the GitHub Release body. Worked
example: the v0.5.1 draft (`.claude/RELEASE-NOTES-DRAFT-v0.5.1.md`) uses this
shape:

```markdown
# MC2 OpenGL v<version> — release notes

## Fixed GitHub issues
- **#NN** — one line: symptom -> fix (note any opt-in gate + default).

## Display / UI
## World rendering
## Vulkan backend            (drop sections that had no changes)
## Fixtures / tooling (modder- and dev-facing)

## Not in this release
- deferred items, with the branch they sit on and why.
```

Rules: every fixed issue gets its number; anything shipped default-OFF states
its gate name and default; staged-but-unmerged work goes under "Not in this
release" — never listed as shipped until re-verified against the merged tree.

## 3. End-to-end runbook

```
0. Land the merge wave. Re-verify every release-note bullet against the
   actual merged tree (a drafted bullet is a claim, not evidence).
1. Commit everything, THEN build (RelWithDebInfo) — commit-first keeps
   build_fingerprint.h attributable. Deploy to the release lane with
   scripts/deploy_payload.py (it verifies exe+shaders lockstep and refuses
   dirty fingerprints under --require-clean).
2. Smoke the deployed lane: py -3 scripts/run_smoke.py --tier tier1
   --duration 30 --keep-logs  (plus any release-specific click-throughs).
2a. Capture a Tracy perf baseline for the release (see §3a below).
3. Update RELEASE_README.md (version header, known-issues table) and finalize
   the changelog from the draft.
4. Tag: git tag v<version> && push the tag (user-gated).
5. Package: DEPLOY=<release lane> bash scripts/build_release.sh
   -> release_assets/*.zip (engine, gamedata, burnins-4x pt1+pt2, art, tgl,
   movies; the script verifies burnins are 4x and nothing leaked between zips).
   Start mc2.exe directly; the retired mc2-launcher.exe must not ship.
   Example mods come from examples/mods/ (mod.json plus data/ overlays),
   never from the deploy's locally installed mods. The in-game picker and
   tools/mc2_import/import_tool.py replace launcher workflows.
6. Gate: py -3 scripts/test_release_zip_install.py --zip-dir <OUTDIR>
   -> must PASS (exit 0). This is the zips-only install + smoke proof; see
   release-checklist.md for exit codes and why.
7. Archive release_install_manifest.csv + release_install_report.json
   alongside the published zips (provenance: exe sha + commit + zip set).
8. GitHub Release: create release on the tag, paste the changelog, upload the
   zips + the two manifest files. Include RELEASE_README.md content as the
   install instructions.
```

Steps 4 and 8 (tag push, publish) are user-gated — agents prepare, the
maintainer pulls the trigger.

## 3a. Per-release Tracy perf baseline

Every release gets a Tracy baseline so later releases can be regression-checked
against it. Capture Vulkan at the canonical pose and store the
result under `release_assets/<version>/tracy/`:

```
py -3 tools/tracy_capture_compare.py baseline \
    --install <deployed release lane, e.g. A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1> \
    --name v<version> --out-dir release_assets/v<version>/tracy
```

This writes `.tracy` traces plus `baseline.json` + `summary.md`
(frames, fps, per-zone means) into that dir. `release_assets/` is gitignored, so
archive the `summary.md` + `baseline.json` alongside the published zips (step 7)
as the perf provenance for the release — the multi-MB `.tracy` files stay local.

Hygiene is baked in: uncapped (`MC2_FPS_CAP=0`), free-timestep, no
`MC2_VK_PRESENT_VERIFY`. Vulkan is mandatory; there is no GL fallback.
Full details and the `regress` cross-check against a
prior release's baseline: [tracy-harness.md](tracy-harness.md).

## 4. References

- [release-checklist.md](release-checklist.md) — the zip gate in detail
  (why zips-only, manifest schema, exit codes).
- `scripts/build_release.sh` — packaging recipe (reads `DEPLOY`, `SRCDATA`,
  `OUTDIR`).
- `scripts/test_release_zip_install.py` — the release gate.
- `scripts/deploy_payload.py` — canonical deploy (exe+shader lockstep,
  manifest, dirty-fingerprint guard).
- [data-provenance.md](data-provenance.md) — what game data ships and where
  it came from.
- [tracy-harness.md](tracy-harness.md) — the Tracy perf harness and the
  per-release baseline command (step 2a / §3a).
