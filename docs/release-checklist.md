<!-- doc-status: current | reviewed:2026-10-11 -->
# Release checklist (publishing the public zips)

The release artifacts must be self-contained: a user who downloads the zips and
extracts them gets a working install, with no dependency on the dev deploy. The
**release gate** (`scripts/test_release_zip_install.py`, S11) proves that from
the zips alone and emits a machine-readable verdict.

## Before publishing zips

```
1. Build the zips from a verified deploy:
     bash scripts/build_release.sh
2. Run the release gate FROM THE ZIPS ONLY (no dev deploy on PATH):
     py -3 scripts/test_release_zip_install.py --zip-dir <OUTDIR>
   It extracts engine+gamedata (+optional overlays) into a temp tree, verifies
   the required-file/dir tree, smokes mc2_01 against the assembled mc2.exe, and
   writes:
     release_install_manifest.csv   (relpath, sha256, bytes, zip_source)
     release_install_report.json    (S12 mc2-manifest/1 identity + verdict)
3. Confirm report verdict == PASS (exit 0). A FAIL exit (1 tree / 2 smoke) blocks
   the release.
4. Archive release_install_manifest.csv + release_install_report.json ALONGSIDE
   the published zips (provenance: which build, which commit, which exe sha).
```

## Why each step

- **Zips-only install** catches "works on my machine because the dev deploy had
  the file" — the #1 release-integrity bug class. The gate has no access to the
  dev deploy dir.
- **report.json carries the unified identity block** (`scripts/manifest_schema.py`):
  `identity.exe.sha256`, `identity.git.commit`, `identity.zip_set`,
  `identity.deploy_target` (the temp install), `report.verdict`. It joins with
  smoke run manifests and the visual golden sets on the same fields — one schema,
  one trust gate. Validate with `py -3 scripts/check-manifest-schema.py <dir>`.
- **Archive with the zips** so a published build is forever traceable to the exe
  hash + commit that produced it, killing the stale-artifact ambiguity class.

## Gate exit codes

```
0  tree verify + smoke PASS (or --no-smoke tree-only PASS)
1  tree verification failure (missing required file/dir, empty shaders/, no .fst)
2  smoke failure against the assembled install
```
