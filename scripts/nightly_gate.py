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


def receipt_key(weekly, stage):
    """Hash of everything one stage's result depends on: the published tree, the mode and the stage."""
    import hashlib
    blob = "\n".join([git("rev-parse", "HEAD^{tree}"), "weekly" if weekly else "nightly", stage])
    return hashlib.sha256(blob.encode()).hexdigest()


def _load(path):
    import json
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def passed_stages(verdict, required):
    """The required stages that executed green in this verdict. A scheduled run passes --stages, so its overall verdict is PARTIAL by
    design; per-stage status is the only honest signal. A verdict with failed rows issues nothing."""
    import merge_gate
    if not verdict or verdict.get("failed"):
        return []
    ex = verdict.get("executed") or {}
    return [n for n in required if merge_gate.stage_passed(str((ex.get(n) or {}).get("status", "")))]


def find_stage_receipts(proof_root, weekly, required, now=None):
    """stage -> newest recent nightly dir holding a receipt for that stage's key whose own verdict shows the stage green."""
    import glob
    import time
    now = time.time() if now is None else now
    found = {}
    for d in sorted(glob.glob(os.path.join(proof_root, "nightly-*")), reverse=True):
        r = _load(os.path.join(d, "receipt.json"))
        v = _load(os.path.join(d, "gate-verdict.json"))
        if not r or not v or (now - float(r.get("written") or 0)) / 3600 > RECEIPT_MAX_AGE_H:
            continue
        ok = passed_stages(v, required)
        for n in required:
            if n not in found and n in ok and (r.get("stages") or {}).get(n) == receipt_key(weekly, n):
                found[n] = d
    return found


def write_receipts(proof, weekly, required):
    """One receipt per stage, issued on that stage's own PASS (not on the overall verdict, which is PARTIAL for a --stages run)."""
    import json
    import time
    ok = passed_stages(_load(os.path.join(proof, "gate-verdict.json")), required)
    if ok:
        with open(os.path.join(proof, "receipt.json"), "w", encoding="utf-8") as f:
            json.dump({"written": time.time(), "stages": {n: receipt_key(weekly, n) for n in ok}}, f, indent=1)
    return ok


def reuse_receipts(found, proof, weekly, required, now=None):
    """Every required stage already has a same-tree receipt: carry them into a fresh verdict with honest REUSED statuses."""
    import json
    import time
    now = time.time() if now is None else now
    first = _load(os.path.join(found[required[0]], "gate-verdict.json"))
    executed = {n: dict((_load(os.path.join(found[n], "gate-verdict.json")).get("executed") or {})[n],
                        status=f"REUSED: {found[n]}") for n in required}
    v = dict(first, written=now, executed=executed, failed=[], reused_from=sorted(set(found.values())))
    receipt = {"written": now, "stages": {n: receipt_key(weekly, n) for n in required}, "reused_from": v["reused_from"]}
    for name, body in (("gate-verdict.json", v), ("receipt.json", receipt)):
        with open(os.path.join(proof, name), "w", encoding="utf-8") as f:
            json.dump(body, f, indent=1)


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
    required = stage_names(weekly)
    found = {} if args.force else find_stage_receipts(PROOF_ROOT, weekly, required)
    if required and len(found) == len(required):   # same tree, mode and every stage already passed; do not burn another hour of the box
        reuse_receipts(found, proof, weekly, required)
        print(f"[nightly_gate] every required stage already passed on this tree ({', '.join(sorted(set(found.values())))}); reused (--force reruns)")
        return 0
    env = dict(os.environ, TMP=TMP_DIR, TEMP=TMP_DIR, TMPDIR=TMP_DIR)  # never write to C: (lane rules)
    os.makedirs(TMP_DIR, exist_ok=True)
    rc = subprocess.run(cmd, cwd=ROOT, env=env, **console_child.child_kwargs()).returncode
    issued = write_receipts(proof, weekly, required)   # per stage, whatever the overall rc: a red stage must not void the green ones
    print(f"[nightly_gate] receipts issued for {len(issued)}/{len(required)} stages: {', '.join(issued) or 'none'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
