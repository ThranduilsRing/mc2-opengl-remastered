#!/usr/bin/env python3
"""stress_overlap -- shake the G7b sim/render overlap with jitter and the abort-on-violation audit.

WHAT: launches each mission N times through scripts/run_smoke.py with MC2_SIM_OVERLAP_AUDIT=2+<jitter ms> (abort on the
first violation, render-owns fence and private-RNG jitter), under the determinism clock
(MC2_SMOKE_FIXED_TIMESTEP=1, MC2_SIM_IDENTITY_TRACE=1). Each run's SIMID chain is hashed over the frames every run of that
mission reached; one serial reference run per mission (MC2_SIM_OVERLAP=0, no jitter) must give the same hash. A run is red
when it prints a `[SIM_OVERLAP v1] VIOLATION` line, aborts, or its hash differs from the serial reference.

  py -3 tools/stress_overlap.py --exe E:/mc2-installs/<lane>/mc2.exe --runs 8 --out E:/mc2-archive-2026-10/proofs/<lane>/stress
  py -3 tools/stress_overlap.py --deploy <lane> --missions mc2_10 --runs 2 --out <dir>

Farm (the seeded harness has this file once reseeded; `{EXE}`/`{ROOT}` are replaced by host.py):
  {"argv": ["python", "C:/mc2farm/harness/tools/stress_overlap.py", "--exe", "{EXE}", "--ref-exe", "{EXE}",
            "--runs", "8", "--out", "{ROOT}/results/stress"], "cwd": "C:/mc2farm/harness", "timeout": 7200,
   "env": {"MC2_HOST_LEASE_PATH": "C:/mc2farm/host_game_lease.json", "MC2_HARNESS_GPU": "any", ...}}
  python tools/farm/host.py submit job.json --exe build64-ninja/mc2.exe

The summary is UTF-8 JSON at <out>/stress_overlap.json; exit 0 only when every run is green.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

SIMID = "[SIMID v1] simframe="
VIOLATION = "[SIM_OVERLAP v1] VIOLATION"
# Cosmetic (appearance/weather) RNG draw counts are not simulation state (tools/sim_determinism.py simulation_fields).
_RNG_COUNTS = re.compile(r" rng=(\d+)/\d+/\d+(?=\s|$)")
DETERMINISM = {"MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_SIM_IDENTITY_TRACE": "1"}
MIN_FRAMES = 300


def stress_env(jitter_ms: int) -> dict:
    return {**DETERMINISM, "MC2_SIM_OVERLAP": "1", "MC2_SIM_OVERLAP_AUDIT": str(2 + max(0, min(50, jitter_ms)))}


def serial_env() -> dict:
    return {**DETERMINISM, "MC2_SIM_OVERLAP": "0", "MC2_SIM_OVERLAP_AUDIT": "1"}


# The sim worker and the render thread share the console, so another thread's line can land inside a SIMID line (an
# unbuffered fwrite writes its last byte separately). Find each record anywhere; stop at the next "[" (sim_determinism.load_simid).
_SIMID_RECORD = re.compile(re.escape(SIMID) + r"[^\r\n\[]*")


def simid_lines(text: str) -> list[str]:
    return [_RNG_COUNTS.sub(r" rng=\1", m.group().rstrip()) for m in _SIMID_RECORD.finditer(text)]


def chain(lines: list[str], frames: int) -> str:
    h = hashlib.sha256()
    for ln in lines[:frames]:
        h.update(ln.encode("utf-8", "replace"))
    return h.hexdigest()[:16]


def first_violation(text: str) -> str:
    return next((ln.strip() for ln in text.splitlines() if VIOLATION in ln), "")


def target_args(args) -> list[str]:
    t = ["--exe", args.exe] if args.exe else ["--deploy", args.deploy]
    if args.ref_exe:
        t += ["--ref-exe", args.ref_exe]
    elif args.ref_commit:
        t += ["--ref-commit", args.ref_commit]
    return t


def run_once(args, mission: str, env_extra: dict, tag: str) -> dict:
    env = dict(os.environ, **env_extra)
    t0 = time.monotonic()
    r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py"), "--mission", mission,
                        "--duration", str(args.duration), *target_args(args), "--keep-logs", "--sim-determinism"],
                       env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                       errors="replace", **console_child.child_kwargs(redirected=True))
    arts = [ln.removeprefix("[runner] artifacts: ").strip() for ln in r.stdout.splitlines()
            if ln.startswith("[runner] artifacts: ")]
    log_path = os.path.join(arts[-1], mission + ".log") if arts else ""
    log = ""
    if log_path and os.path.exists(log_path):
        with open(log_path, encoding="utf-8", errors="replace") as fh:
            log = fh.read()
    text = r.stdout + "\n" + log
    lines = simid_lines(log)
    row = {"mission": mission, "tag": tag, "rc": r.returncode, "wall_s": round(time.monotonic() - t0, 1),
           "artifacts": arts[-1] if arts else "", "frames": len(lines), "violation": first_violation(text),
           "aborted": "abort=1" in text, "_lines": lines}
    with open(os.path.join(args.out, f"{mission}_{tag}.stdout.txt"), "w", encoding="utf-8") as fh:
        fh.write(r.stdout)
    print(f"[stress_overlap] {mission} {tag}: rc={r.returncode} frames={len(lines)}"
          + (f" RED {row['violation']}" if row["violation"] else ""), flush=True)
    return row


def judge(rows: list[dict]) -> dict:
    """Hash every run of one mission over the shortest run's frames and compare with the serial reference."""
    ref = next(r for r in rows if r["tag"] == "serial")
    frames = min(r["frames"] for r in rows)
    out = {"frames_compared": frames, "serial_hash": chain(ref["_lines"], frames), "runs": []}
    for r in rows:
        h = chain(r["_lines"], frames)
        red = []
        if r["violation"] or r["aborted"]:
            red.append("violation")
        if frames < MIN_FRAMES:
            red.append(f"frames<{MIN_FRAMES}")
        if h != out["serial_hash"]:
            red.append("simid_mismatch")
        out["runs"].append({k: v for k, v in r.items() if not k.startswith("_")} | {"simid_hash": h, "red": red})
    out["green"] = all(not r["red"] for r in out["runs"])
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--exe", help="exe to launch (run_smoke --exe)")
    g.add_argument("--deploy", help="registered lane install (run_smoke --deploy)")
    r = p.add_mutually_exclusive_group()
    r.add_argument("--ref-exe", help="run_smoke fingerprint reference exe")
    r.add_argument("--ref-commit", help="run_smoke fingerprint reference commit")
    p.add_argument("--missions", default="mc2_10,mc2_24,mc2_01")
    p.add_argument("--runs", type=int, default=8)
    p.add_argument("--duration", type=int, default=20)
    p.add_argument("--jitter", type=int, default=6, help="audit jitter in ms (0..50)")
    p.add_argument("--out", required=True)
    args = p.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    summary = {"tool": "stress_overlap", "jitter_ms": args.jitter, "duration_s": args.duration, "runs_per_mission": args.runs,
               "target": target_args(args), "missions": {}}
    for mission in [m.strip() for m in args.missions.split(",") if m.strip()]:
        rows = [run_once(args, mission, serial_env(), "serial")]
        rows += [run_once(args, mission, stress_env(args.jitter), f"jitter{i + 1}") for i in range(args.runs)]
        summary["missions"][mission] = judge(rows)
        print(f"[stress_overlap] {mission}: {'GREEN' if summary['missions'][mission]['green'] else 'RED'} "
              f"frames={summary['missions'][mission]['frames_compared']} serial={summary['missions'][mission]['serial_hash']}", flush=True)
    summary["green"] = all(m["green"] for m in summary["missions"].values())
    path = os.path.join(args.out, "stress_overlap.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2, ensure_ascii=False)
    print(f"[stress_overlap] {'GREEN' if summary['green'] else 'RED'} -> {path}")
    return 0 if summary["green"] else 1


if __name__ == "__main__":
    sys.exit(main())
