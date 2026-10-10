#!/usr/bin/env python3
# scripts/write-deploy-manifest.py
"""Write a .deploy-manifest.json into a deploy target directory.

Opt-in: only writes when explicitly invoked as part of a deploy flow
(see .claude/skills/mc2-deploy.md). Never runs automatically.

Examples:
  py -3 scripts/write-deploy-manifest.py A:/Games/mc2-opengl/mc2-win64-v0.4 \
      mc2.exe mc2.pdb --glob "shaders/*.vert" --glob "shaders/*.frag" \
      --glob "shaders/include/*"
  py -3 scripts/write-deploy-manifest.py <deploy_dir> --glob "*.dll"

File arguments and --glob patterns are relative to the deploy dir and
describe the files that were JUST deployed there.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import subprocess
import sys
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

MANIFEST_NAME = ".deploy-manifest.json"


def _git(worktree: Path, *args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(worktree), *args],
            text=True, stderr=subprocess.DEVNULL, **console_child.child_kwargs(redirected=True)).strip()
    except Exception:
        return ""


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def collect_files(deploy_dir: Path, names: list[str], globs: list[str]) -> list[Path]:
    files: list[Path] = []
    seen: set[Path] = set()
    for name in names:
        p = (deploy_dir / name).resolve()
        if p.is_file() and p not in seen:
            files.append(p)
            seen.add(p)
        elif not p.is_file():
            print(f"[deploy-manifest] WARN: listed file missing, skipped: {name}",
                  file=sys.stderr)
    for pattern in globs:
        for p in sorted(deploy_dir.glob(pattern)):
            rp = p.resolve()
            if rp.is_file() and rp not in seen:
                files.append(rp)
                seen.add(rp)
    return files


def build_manifest(deploy_dir: Path, files: list[Path], worktree: Path) -> dict:
    entries = []
    for p in files:
        st = p.stat()
        entries.append({
            "path": p.relative_to(deploy_dir.resolve()).as_posix(),
            "size": st.st_size,
            "sha256": _sha256(p),
            "mtime": st.st_mtime,
        })
    return {
        "schema": 1,
        "written_at": dt.datetime.now().astimezone().isoformat(),
        "source_worktree": str(worktree),
        "git_branch": _git(worktree, "rev-parse", "--abbrev-ref", "HEAD"),
        "git_head": _git(worktree, "rev-parse", "HEAD"),
        "git_dirty": bool(_git(worktree, "status", "--porcelain")),
        "files": entries,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("deploy_dir", help="deploy target directory (e.g. v0.4)")
    ap.add_argument("files", nargs="*",
                    help="file names just deployed, relative to deploy_dir")
    ap.add_argument("--glob", action="append", default=[],
                    help="glob pattern relative to deploy_dir (repeatable)")
    ap.add_argument("--worktree", default=str(Path(__file__).resolve().parents[1]),
                    help="source worktree for git metadata (default: this repo)")
    ap.add_argument("--merge", action="store_true",
                    help="merge entries into an existing manifest instead of "
                         "replacing it (game + editor share one deploy dir)")
    args = ap.parse_args()

    deploy_dir = Path(args.deploy_dir)
    if not deploy_dir.is_dir():
        print(f"[deploy-manifest] ERROR: not a directory: {deploy_dir}",
              file=sys.stderr)
        return 1
    if not args.files and not args.glob:
        print("[deploy-manifest] ERROR: no files or --glob given; nothing to record",
              file=sys.stderr)
        return 1

    files = collect_files(deploy_dir, args.files, args.glob)
    if not files:
        print("[deploy-manifest] ERROR: no matching files found in deploy dir",
              file=sys.stderr)
        return 1

    manifest = build_manifest(deploy_dir, files, Path(args.worktree))
    out = deploy_dir / MANIFEST_NAME
    if args.merge and out.is_file():
        try:
            old = json.loads(out.read_text(encoding="utf-8"))
            new_paths = {e["path"] for e in manifest["files"]}
            kept = [e for e in old.get("files", [])
                    if e.get("path") not in new_paths]
            manifest["files"] = kept + manifest["files"]
        except Exception as e:
            print(f"[deploy-manifest] WARN: could not merge existing manifest "
                  f"({e}); replacing it", file=sys.stderr)
    out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[deploy-manifest] wrote {out} ({len(files)} files, "
          f"HEAD {manifest['git_head'][:8]}{' dirty' if manifest['git_dirty'] else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
