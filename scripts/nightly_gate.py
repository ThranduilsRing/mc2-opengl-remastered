"""Scheduled nightly/weekly merge gate against the PUBLISHED nifty in mc2-main.

    py -3 scripts/nightly_gate.py [--dry-run] [--weekly]

Runs `gate_wait.py --timeout 7200 -- merge_gate.py --nightly` (Sundays: --weekly), restricted to the tier-1/2 stages plus
deploy, at BELOW_NORMAL, never --kill-existing. Proof dir: E:/mc2-archive-2026-10/proofs/merge-gate/nightly-<date>/
(gate-verdict.json inside; never written to mc2-main's build64-ninja, which holds the stage window's verdict). Refuses (rc 2) when mc2-main is not on claude/nifty-mendeleev or is dirty. While another gate holds
A:/Games/mc2-ops/GATE_QUIET.json, gate_wait waits for it (up to --timeout) instead of skipping the night. tools/lane_merge_stage.py --promote refuses unless the newest
verdict here is under 36 h old, failed == [] and not INFRA (FAIL needs a triage* file). The gate must run in mc2-main, so a stage
branch checkout still refuses the night (the gate itself is orchestrator-only in mc2-main).
"""
import argparse
import datetime
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import console_child  # noqa: E402  (BELOW_NORMAL + CREATE_NO_WINDOW for every child)

PROOF_ROOT = "E:/mc2-archive-2026-10/proofs/merge-gate"
NIFTY = "claude/nifty-mendeleev"
TMP_DIR = "G:/mc2-tmp"


def git(*a):
    return subprocess.run(["git", "-C", ROOT, *a], capture_output=True, text=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


def refusal():
    if git("branch", "--show-current") != NIFTY:
        return f"mc2-main is not on {NIFTY}"
    if git("status", "--porcelain", "--untracked-files=no"):
        return "mc2-main is dirty"
    return None


def current_window():
    """The newest merge window number named in nifty's recent subjects ('merge ... w128'), or None."""
    import re
    nums = [int(m) for m in re.findall(r"\bw(\d{2,4})\b", git("log", "-40", "--format=%s"))]
    return max(nums) if nums else None


RECEIPT_MAX_AGE_H = 8 * 24


def receipt_key(weekly, stages):
    """Hash of everything a nightly's result depends on: the published tree, the mode and the stage list."""
    import hashlib
    blob = "\n".join([git("rev-parse", "HEAD^{tree}"), "weekly" if weekly else "nightly", *stages])
    return hashlib.sha256(blob.encode()).hexdigest()


def _load(path):
    import json
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _clean_pass(v):
    return v.get("verdict") == "PASS" and v.get("complete") and not v.get("failed")


def find_receipt(proof_root, key, now=None):
    """The newest earlier PASS verdict dir whose receipt has this key and is recent enough, else None."""
    import glob
    import time
    now = time.time() if now is None else now
    for d in sorted(glob.glob(os.path.join(proof_root, "nightly-*")), reverse=True):
        r = _load(os.path.join(d, "receipt.json"))
        v = _load(os.path.join(d, "gate-verdict.json"))
        if r and v and r.get("key") == key and _clean_pass(v) \
                and (now - float(r.get("written") or 0)) / 3600 <= RECEIPT_MAX_AGE_H:
            return d
    return None


def reuse_receipt(src_dir, proof, key, now=None):
    """Carry an unchanged-input PASS forward: same verdict, fresh timestamp, honest reused_from marker."""
    import json
    import time
    now = time.time() if now is None else now
    v = _load(os.path.join(src_dir, "gate-verdict.json"))
    v.update(written=now, reused_from=src_dir)
    for name, body in (("gate-verdict.json", v), ("receipt.json", {"key": key, "written": now, "reused_from": src_dir})):
        with open(os.path.join(proof, name), "w", encoding="utf-8") as f:
            json.dump(body, f, indent=1)


def write_receipt(proof, key):
    import json
    import time
    v = _load(os.path.join(proof, "gate-verdict.json")) or {}
    if _clean_pass(v):
        with open(os.path.join(proof, "receipt.json"), "w", encoding="utf-8") as f:
            json.dump({"key": key, "written": time.time()}, f, indent=1)


def stage_names(weekly):
    import merge_gate
    keep = [n for n, _c, _e in merge_gate.plan(1)
            if n == "deploy" or merge_gate.stage_tier(n) >= 1]
    return [n for n in keep if weekly or merge_gate.stage_tier(n) < 2]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--weekly", action="store_true", help="default: only on Sundays")
    ap.add_argument("--force", action="store_true", help="rerun even when an identical-input PASS receipt exists")
    args = ap.parse_args(argv)
    why = refusal()
    if why:
        print(f"[nightly_gate] REFUSED: {why}")
        return 2
    today = datetime.date.today()
    weekly = args.weekly or today.weekday() == 6
    window = current_window()
    if weekly and window is None:   # the mods battery refuses without --window; say so up front instead of failing hours in
        print("[nightly_gate] REFUSED: the weekly run needs the merge-window number and nifty's log names none")
        return 2
    proof = f"{PROOF_ROOT}/nightly-{today.isoformat()}"
    os.makedirs(proof, exist_ok=True)
    py = [sys.executable] if os.name != "nt" else ["py", "-3"]
    cmd = py + [f"{ROOT}/scripts/gate_wait.py", "--timeout", "7200", "--log", f"{proof}/gate_wait.log", "--"] + py + [
        f"{ROOT}/scripts/merge_gate.py", "--weekly" if weekly else "--nightly", "--proofs-dir", proof, "--isolated-verdict",
        *(["--window", str(window)] if window else []), "--stages"] + stage_names(weekly)
    print("[nightly_gate] " + " ".join(cmd))
    if args.dry_run:
        return 0
    key = receipt_key(weekly, stage_names(weekly))
    prior = None if args.force else find_receipt(PROOF_ROOT, key)
    if prior:   # p29 cut 5: same tree, mode and stages already passed; do not burn another hour of the box
        reuse_receipt(prior, proof, key)
        print(f"[nightly_gate] inputs unchanged since {prior}; reused its PASS (--force reruns)")
        return 0
    env = dict(os.environ, TMP=TMP_DIR, TEMP=TMP_DIR, TMPDIR=TMP_DIR)  # never write to C: (lane rules)
    os.makedirs(TMP_DIR, exist_ok=True)
    rc = subprocess.run(cmd, cwd=ROOT, env=env, **console_child.child_kwargs()).returncode
    if rc == 0:
        write_receipt(proof, key)
    return rc


if __name__ == "__main__":
    sys.exit(main())
