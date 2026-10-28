#!/usr/bin/env python3
"""List determinism and saveload baselines whose recorded exe is far behind HEAD; re-pin them at the merged HEAD on request.

  py -3 tools/repin_stale.py [--max-behind 50]                         # list; exit 1 when any is stale
  py -3 tools/repin_stale.py --repin [--only mc2_01,mc2_03] --install NAME_OR_DIR   # re-record the stale ones with the baseline's own settings

WHY: baselines left 120 commits behind made each lane bundle an unrelated re-pin. The merge window re-pins them once, at the merged HEAD,
from an install deployed from that HEAD (tools/lane_merge.py's merge flow, .claude/agents/mc2-merge.md). Re-pin = the same recording
commands a lane would run: saveload `--baseline` inherits save_at/load_at/sim_speed/mask from the file; determinism re-runs the pair for
`--det-duration` seconds and write-baseline keeps the file's frames and env.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DET_DIR = os.path.join(ROOT, "tests", "smoke", "sim_determinism")
SAVE_DIR = os.path.join(ROOT, "tests", "saveload", "baselines")


def git(*args: str) -> str:
    r = subprocess.run(["git", "-C", ROOT, *args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)  # background-launch: allow git query, console tool
    return r.stdout.strip() if r.returncode == 0 else ""


def commits_behind(sha: str | None) -> int | None:
    """Commits in HEAD that the baseline's exe sha does not have; None when the sha is unknown to this repo."""
    if not sha or not git("cat-file", "-t", sha):
        return None
    out = git("rev-list", "--count", f"{sha}..HEAD")
    return int(out) if out.isdigit() else None


def scan(det_dir: str = DET_DIR, save_dir: str = SAVE_DIR) -> list[dict]:
    rows = []
    for kind, folder, pick in (("determinism", det_dir, lambda d: d.get("sha")),
                               ("saveload", save_dir, lambda d: (d.get("exe") or {}).get("sha"))):
        for path in sorted(glob.glob(os.path.join(folder, "mc2_*.json"))):
            try:
                with open(path, encoding="utf-8") as fh:
                    data = json.load(fh)
            except (OSError, ValueError):
                continue
            if not isinstance(data, dict):
                continue
            sha = pick(data)
            rows.append({"kind": kind, "mission": data.get("mission") or os.path.basename(path)[:-5], "path": path,
                         "sha": sha, "behind": commits_behind(sha), "data": data})
    return rows


def stale(rows: list[dict], max_behind: int) -> list[dict]:
    """Unknown shas count as stale: a baseline pinned to a commit this repo cannot see cannot be reasoned about."""
    return [r for r in rows if r["behind"] is None or r["behind"] > max_behind]


def repin_command(row: dict, install: str, det_duration: int, tmp: str) -> list[list[str]]:
    py = [sys.executable, "-B"]
    if row["kind"] == "saveload":
        return [py + [os.path.join(ROOT, "tools", "saveload_roundtrip.py"), "--baseline", "--mission", row["mission"], "--install", install]]
    m = row["mission"]
    out = os.path.join(tmp, "repin_" + m)
    sd = os.path.join(ROOT, "tools", "sim_determinism.py")
    env = [x for k, v in (row["data"].get("env") or {}).items() for x in ("--env", f"{k}={v}")]
    return [py + [sd, "run", m, "--duration", str(det_duration), "--install", install, "--out", out, "--baseline"] + env,
            py + [sd, "write-baseline", "@LOG@", "--frames", str(row["data"].get("frames", 0)), "--mission", m, "--install", install] + env]


def repin(row: dict, install: str, det_duration: int, tmp: str) -> bool:
    for cmd in repin_command(row, install, det_duration, tmp):
        if "@LOG@" in cmd:
            logs = glob.glob(os.path.join(tmp, "repin_" + row["mission"], "**", row["mission"] + ".log"), recursive=True)
            if not logs:
                print(f"  no run log for {row['mission']}; not re-pinned")
                return False
            cmd = [logs[0] if c == "@LOG@" else c for c in cmd]
        print("  " + " ".join(cmd[2:]))
        if subprocess.call(cmd, cwd=ROOT) != 0:  # background-launch: allow runs the repo baseline tools (saveload_roundtrip/run_smoke) printed above
            return False
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--max-behind", type=int, default=50, help="stale when the recorded exe is more than N commits behind HEAD (default 50)")
    ap.add_argument("--repin", action="store_true", help="re-record the stale baselines now (needs --install: an install deployed from HEAD)")
    ap.add_argument("--only", help="comma-separated missions to restrict the listing / re-pin to")
    ap.add_argument("--install", help="install name or dir for --repin")
    ap.add_argument("--det-duration", type=int, default=30, help="seconds per determinism re-pin run (default 30)")
    ap.add_argument("--tmp", default="G:/mc2-tmp")
    args = ap.parse_args(argv)
    rows = scan()
    if args.only:
        keep = {s.strip() for s in args.only.split(",")}
        rows = [r for r in rows if r["mission"] in keep]
    bad = stale(rows, args.max_behind)
    for r in rows:
        mark = "STALE" if r in bad else "ok"
        behind = "unknown sha" if r["behind"] is None else f"{r['behind']} commits behind"
        print(f"{mark:5} {r['kind']:11} {r['mission']:8} {r['sha'] or '-':14} {behind}")
    print(f"repin_stale: {len(bad)} stale of {len(rows)} (limit {args.max_behind})")
    if not (args.repin and bad):
        return 1 if bad else 0
    if not args.install:
        print("repin_stale: --repin needs --install")
        return 2
    failed = [r for r in bad if not (print(f"re-pin {r['kind']} {r['mission']}") or repin(r, args.install, args.det_duration, args.tmp))]
    print("repin_stale: " + ("FAILED " + ", ".join(f"{r['kind']}:{r['mission']}" for r in failed) if failed else "re-pinned all"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
