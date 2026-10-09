---
name: mc2-deploy
description: Deploy mc2.exe and all shaders from A:/Games/mc2-main to the dev install (mc2-win64-v0.4), with diff verification
---

# MC2 Deploy

Deploy the built exe + PDB + ffmpeg DLLs + all shaders from `A:/Games/mc2-main` to the dev install, with per-file hash verification and a deploy manifest. Also ships source-tracked support payload (launch `.bat`s) so `build → deploy` is self-complete with no manual copy. Commit first: the exe fingerprint embeds the git sha.

**Canonical path: `scripts/deploy_payload.py`.** One command per target. It hard-fails on every documented stale-deploy trap (locked exe, silently-not-overwritten shaders, stale PDB, wrong target) and writes `<target>/.deployed_manifest.csv` automatically.

## One-command deploy

```bash
# Dev install (A:/Games/mc2-opengl/mc2-win64-v0.4, mc2.exe, build64/RelWithDebInfo):
py -3 A:/Games/mc2-main/scripts/deploy_payload.py --target game
```

The golden `mc2-win64-v0.4c` must NEVER be a deploy target. The `--target editor` preset (`Mission Editor.exe`) also deploys to the dev install `mc2-win64-v0.4` (reset 2026-10-05), not 0.4c.

## What the script guarantees

- **Lock refusal, never taskkill**: if the target exe is held by a running process, the deploy HARD FAILS (exit 2) and tells you to close the game/editor yourself. It will never kill processes.
- **Per-file copy + post-copy sha256 diff**: every file is copied individually (`cp -r` stale-shader trap eliminated) and re-hashed; any mismatch is a hard fail.
- **PDB freshness**: deployed PDB must hash-match the build's PDB (stale PDB = wrong Tracy symbols). Override only with `--allow-stale-pdb`.
- **Manifest written automatically** as the last step: `<target>/.deployed_manifest.csv` (relpath, sha256, bytes, src_commit, timestamp).
- **Refuses to create target dirs** (wrong-target trap): target must already exist.

## Staleness check (read-only)

```bash
py -3 "A:/Games/mc2-main/scripts/deploy_payload.py" --target game --verify-only
```

Re-hashes the target against its manifest, reports `N match, N stale, N missing`. No copying. If the target has no manifest yet (never deployed via this tool) it reports that gracefully and exits 0. Add `--strict` to make drift / missing manifest a nonzero exit (for gating).

## Manifest truth (TWO-MANIFEST TRAP — read this)

There are two manifest files in a deploy target and they are NOT interchangeable:

- **`.deployed_manifest.csv` is the VERIFIER TRUTH** — the manifest consumed by
  `deploy_payload.py --verify-only`. This is the one a staleness gate checks.
- **`.deploy-manifest.json` is legacy/auxiliary** unless explicitly consumed
  elsewhere (e.g. `check-deploy-coherence.py`). It does NOT drive `--verify-only`.

Rule: **any out-of-band deploy path** that copies `mc2.exe` / `mc2.pdb` / shaders
into a target (manual `cp`, a one-off script) MUST refresh
the CSV via:

```bash
py -3 "A:/Games/mc2-main/scripts/deploy_payload.py" "<target>" \
    --source-root "A:/Games/mc2-main" --build-dir "<build>" --write-manifest-only
```

Do **NOT** "fix" a `--verify-only` STALE report by editing the JSON — that is the
wrong manifest. The CSV is what `--verify-only` re-hashes against; editing the JSON
changes nothing the verifier reads. (Verified 2026-06-12: append a byte to a
deployed `mc2.exe` → `--verify-only --strict` exits 6; `--write-manifest-only`
re-hashes in place → `--verify-only --strict` clean again.)

## Extras not covered by deploy_payload.py

- **Mod tools** (gosFX effect tools, if built with `-DENABLE_MC2FX=ON -DENABLE_MC2FX_PREVIEW=ON`): deploy via `bash A:/Games/mc2-main/scripts/deploy-mc2fx-tools.sh`, but only to v0.4; do not let it touch 0.4c (override with `DEPLOY=...`).

## Critical Rules

- **NEVER taskkill** to free a locked exe — close the game/editor manually and re-run.
- Deploy target: ONLY `A:/Games/mc2-opengl/mc2-win64-v0.4`. `mc2-win64-v0.4c` is the golden — never deploy to it.
- Verify deployed exe mtime/manifest commit ≥ your fix commit before re-testing a "fixed" bug.

---

## Appendix: manual per-file recipe (FALLBACK ONLY)

Use only if `deploy_payload.py` is unavailable. This knowledge is still valid — it is what the script automates.

### Paths
- **Source**: `A:/Games/mc2-main` (never the stale root `A:/Games/mc2-opengl-src`)
- **Deploy target**: `A:/Games/mc2-opengl/mc2-win64-v0.4`

### Steps

1. **Deploy exe, PDB, DLLs, shaders** (sanctioned command from `BUILD-WIN.md`; run from `A:/Games/mc2-main`, never copy by hand):
```bash
py -3 scripts/deploy_payload.py "A:/Games/mc2-opengl/mc2-win64-v0.4" --source-root . --build-dir build64 --exe-name mc2.exe
```

2. **Verify ALL deployed files**: Run `diff -q` on every DLL and shader file (source vs deployed). Report:
   - Files that were deployed successfully (match confirmed)
   - **ANY mismatches — flag loudly as errors**
   - Files that exist in source but not in deploy (new files that need copying)
   - Files that exist in deploy but not in source (stale files from old branches)

3. **Write BOTH deploy manifests** (LAST step, after all copies verified).
   There are two manifest files and both must be refreshed or one goes
   stale behind the other:
   - `.deploy-manifest.json` — advisory coherence manifest (carries git
     metadata; read by `check-deploy-coherence.py`).
   - `.deployed_manifest.csv` — the manifest `deploy_payload.py --verify-only`
     checks. A copy that skips this leaves `--verify-only` reporting STALE.

```bash
# JSON coherence manifest:
py -3 "A:/Games/mc2-main/scripts/write-deploy-manifest.py" "A:/Games/mc2-opengl/mc2-win64-v0.4" \
    mc2.exe mc2.pdb --merge --worktree "A:/Games/mc2-main" \
    --glob "*.dll" --glob "shaders/*.vert" --glob "shaders/*.frag" \
    --glob "shaders/*.tesc" --glob "shaders/*.tese" --glob "shaders/include/*"

# CSV manifest that --verify-only checks (re-hashes files already deployed;
# no copy). Without this the manual recipe leaves the CSV stale:
py -3 "A:/Games/mc2-main/scripts/deploy_payload.py" "A:/Games/mc2-opengl/mc2-win64-v0.4" \
    --source-root "A:/Games/mc2-main" --write-manifest-only
```
   **Better: just use `py -3 scripts/deploy_payload.py --target game` —
   the canonical one-command path copies AND writes the CSV manifest in one
   shot, so it never drifts. The manual recipe above is a fallback only.**
