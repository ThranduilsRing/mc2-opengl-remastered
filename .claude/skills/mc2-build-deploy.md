---
name: mc2-build-deploy
description: Commit, build mc2.exe, then deploy to the dev install with verification -- the full cycle
---

# MC2 Build + Deploy

Commit -> build -> deploy, all in `A:/Games/mc2-main`.

## Steps

1. **Commit first** (local only). The exe fingerprint embeds the git sha; a dirty-tree deploy is unattributable.
2. **Invoke `/mc2-build`** (RelWithDebInfo, `--target mc2 mc2_launcher`).
3. **If build fails, stop** -- don't deploy a stale exe.
4. **Deploy** (sha256-verified, writes the manifest, hard-fails if the exe is locked -- close the game yourself; never taskkill):
```bash
py -3 A:/Games/mc2-main/scripts/deploy_payload.py --target game
```
   The only deploy target is `A:/Games/mc2-opengl/mc2-win64-v0.4`. The golden `mc2-win64-v0.4c` must never be deployed to. See `/mc2-deploy` for details.
5. **Staleness check anytime** (read-only): `py -3 A:/Games/mc2-main/scripts/deploy_payload.py --target game --verify-only`

(No editor deploy line: the `--target editor` preset in `deploy_payload.py` defaults to the golden 0.4c, so it is not documented here.)
