#!/usr/bin/env python3
"""Run the dev scripted player through the off-screen/headless mission harness."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INSTALL = "E:/mc2-installs/claude-player-brain-l1"


def parse_report(text):
    rows = {}
    verdict = None
    for line in text.splitlines():
        if "[MISSION_OBJECTIVES v1]" in line:
            fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
            rows[int(fields["id"])] = fields
        if "[MISSION_VERDICT v1]" in line:
            verdict = line[line.index("[MISSION_VERDICT v1]"):]
    return verdict, rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mission", required=True)
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--minutes", type=float, default=5)
    args = ap.parse_args(argv)
    if args.minutes <= 0 or not args.minutes < float("inf"):
        ap.error("--minutes must be finite and positive")
    seconds = max(1, round(args.minutes * 60))
    env = dict(os.environ, MC2_PLAY_MISSION="1", MC2_PLAY_MISSION_INVULNERABLE="1",
               MC2_PLAY_MISSION_SKIP_CINEMATICS="1", MC2_PLAY_MISSION_SECONDS=str(seconds),
               MC2_RNG_SEED="42", MC2_SMOKE_SEED="42", MC2_DETERMINISTIC_RNG="1",
               MC2_SMOKE_FIXED_TIMESTEP="1")
    # Headless ticks run without a GPU or host-game slot. The harness still
    # owns install leasing, process priority, timeouts, logging and cleanup.
    cmd = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--exe",
           str(Path(args.install) / "mc2.exe"), "--mission", args.mission,
           "--headless", "--ticks", str((seconds + 2) * 30),
           "--duration", str(seconds + 30), "--keep-logs", "--wait-timeout", "1200"]
    sys.path.insert(0, str(ROOT / "scripts"))
    import console_child
    result = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace",
                            **console_child.child_kwargs(redirected=True))
    text = result.stdout
    artifacts = re.findall(r"^\[runner\] artifacts: (.+)$", text, re.MULTILINE)
    if artifacts:
        log = Path(artifacts[-1].strip()) / (args.mission + ".log")
        if log.exists():
            text += "\n" + log.read_text(encoding="utf-8", errors="replace")
    verdict, rows = parse_report(text)
    print(verdict or "[MISSION_VERDICT v1] verdict=ERROR reason=no_engine_verdict")
    print("id  text_key  state    primary hidden")
    for ident, row in sorted(rows.items()):
        print(f"{ident:<3} {row['text_key']:<9} {row['state']:<8} {row['primary']:<7} {row['hidden']}")
    for path in artifacts:
        print("log:", Path(path.strip()) / (args.mission + ".log"))
    if verdict is None:
        print("\n".join(result.stdout.splitlines()[-8:]), file=sys.stderr)
    # Match the existing determinism harness: headless smoke can return 1
    # for presentation gates, but must reach its budget without a crash.
    clean = result.returncode == 0 or (result.returncode == 1
            and "[SMOKE v1] event=tick_budget" in text and "Crash Report" not in text)
    if not clean:
        print("harness failed; inspect the retained log", file=sys.stderr)
    return 0 if verdict and "verdict=PASS " in verdict and clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
