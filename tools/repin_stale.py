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
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DET_DIR = os.path.join(ROOT, "tests", "smoke", "sim_determinism")
SAVE_DIR = os.path.join(ROOT, "tests", "saveload", "baselines")


def git(*args: str) -> str:
    r = subprocess.run(["git", "-C", ROOT, *args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)  # background-launch: allow git query, console tool
    return r.stdout.strip() if r.returncode == 0 else ""


def commits_behind(sha: str | None) -> int | None:
    """First-parent commits in HEAD past the baseline's exe sha (a merge window counts once, not as its lane originals plus the "merge lane:" copies);
    None when the sha is unknown to this repo."""
    if not sha or not git("cat-file", "-t", sha):
        return None
    out = git("rev-list", "--first-parent", "--count", f"{sha}..HEAD")
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


GATE_DROP_WITH_VALUE = ("--install", "--out", "--save-dir", "--mission")


def gate_form(kind: str, mission: str) -> list[str]:
    """The merge gate's own flags for this row (scripts/merge_gate.plan): headless-both/ticks/sim-speed/single for determinism,
    mask/first-save-only/headless for saveload. Taken from the plan, never restated, so a re-pin cannot record in another form.
    The command word, mission, --install, --out and --save-dir are the caller's."""
    for extra in (os.path.join(ROOT, "tools"), os.path.join(ROOT, "scripts")):
        if extra not in sys.path:
            sys.path.insert(0, extra)
    import merge_gate
    name = ("determinism-" if kind == "determinism" else "saveload-") + mission
    for step, cmd, _env in merge_gate.plan(1, "PROOF"):
        if step == name:
            args = list(cmd[4:] if kind == "determinism" else cmd[3:])  # drop py, script, check, mission (saveload: py, script, check)
            flags, i = [], 0
            while i < len(args):
                if args[i] in GATE_DROP_WITH_VALUE:
                    i += 2
                    continue
                flags.append(args[i])
                i += 1
            return flags
    raise SystemExit(f"repin_stale: the merge gate has no row {name}; cannot record in the gate's form")


def scratch_root(explicit: str | None = None) -> str:
    """Parent of the re-pin run dirs: --out-root, else a fresh dir under the slot scratch ($MC2_LANE_SCRATCH), else a fresh tempdir.
    Never a fixed shared path."""
    if explicit:
        return explicit
    lane = os.environ.get("MC2_LANE_SCRATCH")
    if lane:
        os.makedirs(lane, exist_ok=True)
        return tempfile.mkdtemp(prefix="repin_", dir=lane)
    return tempfile.mkdtemp(prefix="repin_")


def fresh_out_dir(path: str) -> str:
    """A run dir must not hold an earlier run (the stale log would be re-pinned): refuse loudly, rc 2."""
    if os.path.exists(path) and (not os.path.isdir(path) or os.listdir(path)):
        raise SystemExit(f"repin_stale: out dir {path} already exists and is not empty; pass another --out-root (rc 2)")
    return path


def repin_command(row: dict, install: str, det_duration: int, tmp: str) -> list[list[str]]:
    py = [sys.executable, "-B"]
    m = row["mission"]
    if row["kind"] == "saveload":
        out = os.path.join(tmp, "repin_save_" + m)
        return [py + [os.path.join(ROOT, "tools", "saveload_roundtrip.py"), "baseline", "--mission", m, "--install", install,
                      "--out", out, "--save-dir", os.path.join(out, "savegame")] + gate_form("saveload", m)]
    out = os.path.join(tmp, "repin_" + m)
    sd = os.path.join(ROOT, "tools", "sim_determinism.py")
    env = [x for k, v in (row["data"].get("env") or {}).items() for x in ("--env", f"{k}={v}")]
    form = [f for f in gate_form("determinism", m) if f != "--single"]  # --single is check-only: a re-pin records from a pair
    if ("--headless-both" in form or "--headless" in form) and "--ticks" not in form:
        frames = row["data"].get("frames")
        if type(frames) is not int or frames <= 0:
            raise SystemExit(f"repin_stale: {m} needs a positive recorded frame count for --ticks")
        form += ["--ticks", str(frames)]  # run does not infer ticks from the baseline, unlike check
    return [py + [sd, "run", m, "--duration", str(det_duration), "--install", install, "--out", out, "--baseline"] + form + env,
            py + [sd, "write-baseline", "@LOG@", "--frames", str(row["data"].get("frames", 0)), "--mission", m, "--install", install] + env]


def repin(row: dict, install: str, det_duration: int, tmp: str) -> bool:
    fresh_out_dir(os.path.join(tmp, "repin_" + row["mission"]))
    fresh_out_dir(os.path.join(tmp, "repin_save_" + row["mission"]))
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
    keep_unknown_keys(row)
    return True


def keep_unknown_keys(row: dict) -> None:
    """A re-pin rewrites the file from scratch; restore top-level keys the old file had (e.g. schema_migration) that the new one lacks."""
    try:
        with open(row["path"], encoding="utf-8") as fh:
            new = json.load(fh)
    except (OSError, ValueError):
        return
    lost = {k: v for k, v in (row.get("data") or {}).items() if k not in new}
    if isinstance(new, dict) and lost:
        new.update(lost)
        with open(row["path"], "w", encoding="utf-8") as fh:
            json.dump(new, fh, indent=2)
            fh.write("\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--max-behind", type=int, default=50, help="stale when the recorded exe is more than N commits behind HEAD (default 50)")
    ap.add_argument("--repin", action="store_true", help="re-record the stale baselines now (needs --install: an install deployed from HEAD)")
    ap.add_argument("--only", help="comma-separated missions to restrict the listing / re-pin to")
    ap.add_argument("--install", help="install name or dir for --repin")
    ap.add_argument("--det-duration", type=int, default=30, help="seconds per determinism re-pin run (default 30)")
    ap.add_argument("--out-root", help="parent of the re-pin run dirs (default: a fresh dir under $MC2_LANE_SCRATCH, else a tempdir)")
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
    root = scratch_root(args.out_root)
    failed = [r for r in bad if not (print(f"re-pin {r['kind']} {r['mission']}") or repin(r, args.install, args.det_duration, root))]
    print("repin_stale: " + ("FAILED " + ", ".join(f"{r['kind']}:{r['mission']}" for r in failed) if failed else "re-pinned all"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
