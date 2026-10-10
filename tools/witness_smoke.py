#!/usr/bin/env python3
"""witness_smoke.py — deploy + smoke + log-grep verdict in one call.

Wraps scripts/deploy_payload.py and scripts/run_smoke.py (subprocess — does
NOT reimplement either), then greps the newest smoke artifact logs for
expected / forbidden regexes. Compact verdict for agents.

    py -3 tools/witness_smoke.py --install A:/Games/mc2-opengl/mc2-win64-v0.3 \
        [--deploy [--source-root . --build-dir build64]] \
        --mission mc2_01 [--mission mc2_24] [--duration 30] \
        [--env K=V ...] \
        --expect "<regex>" [--expect ...] [--forbid "<regex>" ...]

Prints: smoke result table line(s), then per --expect FOUND (+ first matching
log line) or MISSING, per --forbid CLEAN or HIT (+ first matching line).
Exit 0 only if smoke passed AND all expects found AND no forbids hit.
Exit 1 = smoke passed but grep verdict failed; smoke's own nonzero exit is
propagated as-is (typically 1); 2 = setup error (no artifacts, bad args).

Notes: uses run_smoke --keep-logs and --exe <install>/mc2.exe, so
it targets exactly the install you name (the folder is leased, never auto-selected).
Never uses --kill-existing. Env vars are passed to the smoke process env.
"""
import argparse
import os
import re
import subprocess
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "tests" / "smoke" / "artifacts"


def newest_artifact_dirs(n=1):
    if not ARTIFACTS.is_dir():
        return []
    dirs = [d for d in ARTIFACTS.iterdir() if d.is_dir()]
    return sorted(dirs, key=lambda d: d.stat().st_mtime, reverse=True)[:n]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--install", required=True, help="install dir containing mc2.exe")
    ap.add_argument("--deploy", action="store_true", help="run deploy_payload.py first")
    ap.add_argument("--source-root", default=str(ROOT))
    ap.add_argument("--build-dir", default="build64")
    ap.add_argument("--exe-name", default="mc2.exe")
    ap.add_argument("--mission", action="append", default=[], required=True)
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--env", action="append", default=[], metavar="K=V")
    ap.add_argument("--expect", action="append", default=[], metavar="REGEX")
    ap.add_argument("--forbid", action="append", default=[], metavar="REGEX")
    args = ap.parse_args()

    install = Path(args.install)
    exe = install / args.exe_name
    py = sys.executable

    if args.deploy:
        cmd = [py, str(ROOT / "scripts" / "deploy_payload.py"), str(install),
               "--source-root", args.source_root, "--build-dir", args.build_dir,
               "--exe-name", args.exe_name]
        print("[witness] deploy:", " ".join(cmd), flush=True)
        rc = subprocess.call(cmd, cwd=str(ROOT), **console_child.child_kwargs())
        if rc != 0:
            print(f"[witness] DEPLOY FAILED rc={rc}")
            return rc

    if not exe.is_file():
        print(f"[witness] setup error: {exe} not found")
        return 2

    env = dict(os.environ)
    # This tool always targets an EXPLICIT --install exe, so the worktree-HEAD
    # deploy-fingerprint gate would hard-fail whenever the install wasn't
    # built from this HEAD (e.g. probing v0.3/v0.4c). Default it off; a caller
    # can re-enable with --env MC2_SMOKE_REQUIRE_FINGERPRINT=1.
    env.setdefault("MC2_SMOKE_REQUIRE_FINGERPRINT", "0")
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    before = {d.name for d in newest_artifact_dirs(10**6)}

    cmd = [py, str(ROOT / "scripts" / "run_smoke.py"),
           "--exe", str(exe), "--keep-logs",
           "--duration", str(args.duration)]
    for m in args.mission:
        cmd += ["--mission", m]
    print("[witness] smoke:", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, cwd=str(ROOT), env=env,
                          capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    smoke_rc = proc.returncode
    smoke_out = (proc.stdout or "") + (proc.stderr or "")

    # Surface the compact result table (mission verdict lines) only.
    for line in smoke_out.splitlines():
        if re.search(r"\b(PASS|FAIL|pass|fail|verdict|crash)\b", line):
            print("[smoke]", line.rstrip())
    print(f"[smoke] exit={smoke_rc}")

    new_dirs = [d for d in newest_artifact_dirs(10**6) if d.name not in before]
    scan_dirs = new_dirs or newest_artifact_dirs(1)
    if not scan_dirs:
        print("[witness] setup error: no smoke artifact dirs found")
        return 2
    print(f"[witness] scanning logs in: {', '.join(d.name for d in scan_dirs)}")

    # Gather text lines from all logs in the artifact dir(s).
    lines = []
    for d in scan_dirs:
        for f in sorted(d.rglob("*")):
            if f.is_file() and f.suffix.lower() in (".log", ".txt", ".jsonl", ".json"):
                try:
                    lines += f.read_text(errors="replace").splitlines()
                except OSError:
                    pass

    grep_ok = True
    for pat in args.expect:
        rx = re.compile(pat)
        hit = next((ln for ln in lines if rx.search(ln)), None)
        if hit:
            print(f"[expect] FOUND   {pat!r}: {hit.strip()[:200]}")
        else:
            print(f"[expect] MISSING {pat!r}")
            grep_ok = False
    for pat in args.forbid:
        rx = re.compile(pat)
        hit = next((ln for ln in lines if rx.search(ln)), None)
        if hit:
            print(f"[forbid] HIT     {pat!r}: {hit.strip()[:200]}")
            grep_ok = False
        else:
            print(f"[forbid] CLEAN   {pat!r}")

    if smoke_rc != 0:
        print("[witness] VERDICT: FAIL (smoke)")
        return smoke_rc
    if not grep_ok:
        print("[witness] VERDICT: FAIL (grep)")
        return 1
    print("[witness] VERDICT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
