"""Box health probe (~5 s): runs /workspace/bin/box-health on the vast box and turns it into an exit code.

  py -3 tools/box_health.py                 summary: problems + warnings, one line each
  py -3 tools/box_health.py --json          the full JSON (GPUs, filler, supervisor, disk, cpu, toolchain, last rows)
  py -3 tools/box_health.py --floor-gb 30   stricter free-disk floor (default 15 = box_run's DISK_GUARD)
Exit 0 = healthy, 1 = unhealthy (the problems are printed), 3 = box unreachable or no valid JSON (ssh fails fast: 5 s connect timeout).
Use it before a --gate run: a red box makes a gate verdict meaningless (BOX_INFRA), so stop and fix the box first.
"""
import argparse, json, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import box_run

RC_OK, RC_BAD, RC_UNREACHABLE = 0, 1, 3
REMOTE = "/workspace/bin/box-health"


def ssh_cmd(remote):
    # fail fast: no prompts, 5 s connect, the session dies after ~10 s of silence
    return ["ssh", "-n", "-i", box_run.KEY, "-p", str(box_run.PORT), "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "LogLevel=ERROR",
            "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2", "-o", "StrictHostKeyChecking=accept-new", box_run.HOST, remote]


def probe(floor_gb, rows, timeout=30, runner=None):
    """(doc or None, error text)"""
    runner = runner or box_run.run
    try:
        r = runner(ssh_cmd(f"{REMOTE} --floor-gb {floor_gb:g} --rows {int(rows)}"), capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, f"box-health timed out after {timeout}s"
    except OSError as ex:
        return None, f"ssh failed: {ex}"
    try:
        doc = json.loads(r.stdout)
    except ValueError:
        err = (r.stderr or r.stdout or "").strip().splitlines()
        return None, f"no JSON from box-health (ssh rc={r.returncode}): {err[0][:200] if err else 'no output'}"
    if not isinstance(doc, dict) or "ok" not in doc:
        return None, "box-health JSON has no 'ok' field"
    return doc, ""


def summary(doc):
    d, c = doc.get("disk") or {}, doc.get("cpu") or {}
    g = " ".join(f"gpu{e.get('index')}:{e.get('util_pct')}%{'/L' if e.get('locked') else ''}" for e in doc.get("gpus") or [])
    w = (doc.get("filler") or {}).get("workers") or {}
    lines = [f"[box_health] {'OK' if doc['ok'] else 'UNHEALTHY'} {doc.get('time', '')} free={d.get('free_gb')}GB (floor {d.get('floor_gb')}) "
             f"load={(c.get('load') or ['?'])[0]}/{c.get('quota_cpus')} {g} workers={sum(1 for v in w.values() if v.get('alive'))}/{len(w)}"]
    q = doc.get("build_queue")
    if q:
        fmt = lambda t: f"{t['lane']} {t['age_s'] // 60}m"
        lines.append(f"  builds  slots={q.get('slots')} holders=[{', '.join(fmt(t) for t in q.get('holders') or [])}] "
                     f"waiting={len(q.get('waiters') or [])}[{', '.join(fmt(t) for t in q.get('waiters') or [])}] orphans={len(q.get('orphans') or [])}")
    lines += [f"  PROBLEM {p}" for p in doc.get("problems") or []]
    lines += [f"  warn    {p}" for p in doc.get("warnings") or []]
    return "\n".join(lines)


def main(argv=None, runner=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--floor-gb", type=float, default=15.0)
    ap.add_argument("--rows", type=int, default=12)
    ap.add_argument("--json", action="store_true", help="print the full JSON")
    ap.add_argument("--timeout", type=int, default=30)
    a = ap.parse_args(argv)
    doc, err = probe(a.floor_gb, a.rows, a.timeout, runner)
    if doc is None:
        print(f"[box_health] UNREACHABLE: {err}")
        return RC_UNREACHABLE
    print(json.dumps(doc, indent=1) if a.json else summary(doc))
    return RC_OK if doc["ok"] else RC_BAD


if __name__ == "__main__":
    sys.exit(main())
