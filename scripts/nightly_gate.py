"""Scheduled nightly/weekly merge gate against the PUBLISHED nifty, in its own worktree (A:/Games/mc2-nightly).

    py -3 scripts/nightly_gate.py [--dry-run] [--weekly]

Runs `gate_wait.py --timeout 7200 -- merge_gate.py --nightly` (Sundays: --weekly), restricted to the tier-1/2 stages plus
deploy (nightly: the non-blocking mods-slim check; weekly: the full mods battery), at BELOW_NORMAL, never --kill-existing. Proof dir: E:/mc2-archive-2026-10/proofs/merge-gate/nightly-<date>/
(gate-verdict.json inside; never written to mc2-main's build64-ninja, which holds the stage window's verdict). mc2-main's branch and dirty state do not matter:
the gate runs in A:/Games/mc2-nightly (MC2_NIGHTLY_ROOT), created once with `git worktree add --detach`, each night `git checkout --detach claude/nifty-mendeleev`
and built in its own build64-ninja. Refuses (rc 2) only when that worktree has tracked changes. While another gate holds
A:/Games/mc2-ops/GATE_QUIET.json, gate_wait waits for it (up to --timeout) instead of skipping the night. A host lease still held after
LEASE_WAIT_S (60 min) at start drops the game rows (NOT RUN line) and runs the CPU rows. tools/lane_merge_stage.py --promote refuses unless the newest
verdict here is under 36 h old, failed == [] and not INFRA (FAIL needs a triage* file).
"""
import argparse
import datetime
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN_ROOT = os.path.dirname(HERE)   # the repo this script lives in (mc2-main); only used for refs, never built or gated
NIGHTLY_ROOT = os.environ.get("MC2_NIGHTLY_ROOT", "A:/Games/mc2-nightly")   # merge_gate.NIGHTLY_ROOT reads the same variable
ROOT = NIGHTLY_ROOT   # where merge_gate.py / gate_wait.py / the build run
sys.path.insert(0, HERE)
import console_child  # noqa: E402  (BELOW_NORMAL + CREATE_NO_WINDOW for every child)

PROOF_ROOT = "E:/mc2-archive-2026-10/proofs/merge-gate"
NIFTY = "claude/nifty-mendeleev"
TMP_DIR = "G:/mc2-tmp"
LEASE_WAIT_S = 60 * 60
CPU_ROWS = ("deploy", "contracts", "harnesses")   # no game launch: these still run when the host lease stays held


def git(*a, cwd=None):
    """git in `cwd` (default: the main repo, whose refs are the same ones the nightly worktree sees)."""
    return subprocess.run(["git", "-C", cwd or MAIN_ROOT, *a], capture_output=True, text=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


def refusal():
    """The nightly worktree is the only tree that can block the night: tracked changes there. mc2-main's branch and state are irrelevant."""
    if os.path.isdir(ROOT) and git("status", "--porcelain", "--untracked-files=no", cwd=ROOT):
        return f"{ROOT} (the nightly worktree) is dirty"
    return None


def run_child(cmd, label):
    """One BELOW_NORMAL child in the nightly worktree; its rc (a failure is printed)."""
    env = dict(os.environ, TMP=TMP_DIR, TEMP=TMP_DIR, TMPDIR=TMP_DIR)
    os.makedirs(TMP_DIR, exist_ok=True)
    rc = subprocess.run(cmd, cwd=ROOT, env=env, **console_child.child_kwargs()).returncode
    if rc:
        print(f"[nightly_gate] {label} failed rc={rc}")
    return rc


def prepare_worktree():
    """Create (once), move to nifty, configure (first use) and build (keeps the tree warm). -> reason string on failure, else None."""
    py = [sys.executable] if os.name != "nt" else ["py", "-3"]
    if not os.path.isdir(ROOT):
        out = subprocess.run(["git", "-C", MAIN_ROOT, "worktree", "add", "--detach", ROOT, NIFTY], capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True))
        if out.returncode:
            return f"git worktree add {ROOT} failed: {out.stderr.strip()}"
    else:
        why = refusal()
        if why:
            return why
        if run_child(["git", "checkout", "--detach", NIFTY], "git checkout --detach"):
            return f"git checkout --detach {NIFTY} failed in {ROOT}"
    if not os.path.isfile(os.path.join(ROOT, "build64-ninja", "build.ninja")) and run_child(py + ["scripts/configure_ninja.py"], "configure_ninja"):
        return "configure_ninja failed"
    if run_child(py + ["scripts/ninja_build.py"], "ninja_build"):   # the gate never builds; deploy refuses a stale exe
        return "ninja_build failed"
    return None


def wait_for_lease(timeout_s=LEASE_WAIT_S, acquire=None, release=None):
    """True when the host lease could be taken (so it is free now) within timeout_s; host_lease.acquire does the waiting.
    The probe lease is released at once: the gate's own rows take it themselves."""
    from smoke_lib import host_lease
    acquire, release = acquire or host_lease.acquire, release or host_lease.release
    print(f"[nightly_gate] checking the host lease (waits up to {timeout_s / 60:.0f} min if held)", flush=True)
    try:
        acquire("nightly-gate-probe", timeout_s=timeout_s)
    except host_lease.HostLeaseBusy as exc:
        print(f"[nightly_gate] host lease still held after {timeout_s / 60:.0f} min: {exc}")
        return False
    release()
    return True


def cpu_only(names):
    """(the rows that need no game, the game rows that are NOT RUN without the lease)."""
    return [n for n in names if n in CPU_ROWS], [n for n in names if n not in CPU_ROWS]


def current_window():
    """The newest merge window number named in nifty's recent subjects ('merge ... w128', 'merge fix W137:', 'W138a'), or None."""
    import re
    nums = [int(m) for m in re.findall(r"\bw(\d{2,4})[a-z]?\b", git("log", "-40", "--format=%s", NIFTY), re.IGNORECASE)]
    return max(nums) if nums else None


def nightly_base():
    """merge_gate's --base: the previous published window (second-newest nifty-green-<W>-<sha> tag on HEAD's history).

    merge_gate refuses without a comparison base unless build64-ninja/merge-window.json names HEAD (it names the last
    stage window, so it never does for the published nifty). The previous window's tip makes the diff span the whole
    newest window, so its runtime rows are not skipped as unaffected. None when fewer than two tags are reachable."""
    import re
    tags = {}
    for name in git("tag", "-l", "nifty-green-*", "--merged", NIFTY).split():
        m = re.fullmatch(r"nifty-green-(\d+)([a-z]?)-[0-9a-f]+", name)   # 138b, 138c are windows too: order (number, suffix)
        if m:
            tags[(int(m[1]), m[2])] = name
    if len(tags) < 2:
        return None
    return git("rev-parse", "--verify", "--quiet", tags[sorted(tags)[-2]] + "^{commit}") or None


RECEIPT_MAX_AGE_H = 8 * 24


def receipt_key(weekly, stage):
    """Hash of everything one stage's result depends on: the published tree, the mode and the stage."""
    import hashlib
    blob = "\n".join([git("rev-parse", NIFTY + "^{tree}"), "weekly" if weekly else "nightly", stage])
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
    """deploy plus every tier-1 (nightly) or tier-1/2 (weekly) stage. Nightly carries the slim, non-blocking mods-slim check;
    weekly carries the full mods battery instead."""
    import merge_gate
    tier = 2 if weekly else 1
    return [n for n, _c, _e in merge_gate.plan(1)
            if n == "deploy" or (merge_gate.stage_tier(n) >= 1 and merge_gate.tier_includes(n, tier))]


def required_stages(names):
    """The stages a receipt (and promotion) needs green: non-blocking rows report but never gate."""
    import merge_gate
    return [n for n in names if n not in merge_gate.NON_BLOCKING_STAGES]


def make_plan(weekly, today, names=None):
    """-> (plan, None) or (None, reason). plan = names, base, window, proof, cmd. Reads refs only (mc2-main's branch and state are irrelevant)."""
    window = current_window()
    if weekly and window is None:   # the mods battery refuses without --window; say so up front instead of failing hours in
        return None, "the weekly run needs the merge-window number and nifty's log names none"
    names = list(stage_names(weekly) if names is None else names)
    if window is None and "mods-slim" in names:   # the battery refuses without --window; the slim check is advisory, so drop it
        names.remove("mods-slim")
        print("[nightly_gate] mods-slim dropped tonight: nifty's log names no merge window (mod_smoke needs --window)")
    base = nightly_base()
    if base is None:
        return None, f"no previous nifty-green-<W> tag on {NIFTY}'s history (merge_gate needs --base)"
    proof = f"{PROOF_ROOT}/nightly-{today.isoformat()}"
    return dict(names=names, base=base, window=window, proof=proof, cmd=gate_cmd(weekly, names, base, window, proof)), None


def gate_cmd(weekly, names, base, window, proof):
    py = [sys.executable] if os.name != "nt" else ["py", "-3"]
    return py + [f"{ROOT}/scripts/gate_wait.py", "--timeout", "7200", "--log", f"{proof}/gate_wait.log", "--"] + py + [
        f"{ROOT}/scripts/merge_gate.py", "--weekly" if weekly else "--nightly", "--proofs-dir", proof, "--isolated-verdict",
        "--base", base, *(["--window", str(window)] if window else []), "--stages"] + names


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
    plan, why = make_plan(weekly, today)
    if why:
        print(f"[nightly_gate] REFUSED: {why}")
        return 2
    names, proof = plan["names"], plan["proof"]
    print("[nightly_gate] " + " ".join(plan["cmd"]))
    if args.dry_run:
        return 0
    os.makedirs(proof, exist_ok=True)
    required = required_stages(names)
    found = {} if args.force else find_stage_receipts(PROOF_ROOT, weekly, required)
    if required and len(found) == len(required):   # same tree, mode and every stage already passed; do not burn another hour of the box
        reuse_receipts(found, proof, weekly, required)
        print(f"[nightly_gate] every required stage already passed on this tree ({', '.join(sorted(set(found.values())))}); reused (--force reruns)")
        return 0
    why = prepare_worktree()
    if why:
        print(f"[nightly_gate] REFUSED: {why}")
        return 2
    if not wait_for_lease():
        cpu, game = cpu_only(names)
        print(f"[nightly_gate] NOT RUN: {', '.join(game) or 'none'} (host lease held for {LEASE_WAIT_S // 60} min); running the CPU rows: {', '.join(cpu) or 'none'}")
        names = cpu
        required = required_stages(names)
        plan["cmd"] = gate_cmd(weekly, names, plan["base"], plan["window"], proof)
        if not names:
            return 3
    env = dict(os.environ, TMP=TMP_DIR, TEMP=TMP_DIR, TMPDIR=TMP_DIR)  # never write to C: (lane rules)
    os.makedirs(TMP_DIR, exist_ok=True)
    rc = subprocess.run(plan["cmd"], cwd=ROOT, env=env, **console_child.child_kwargs()).returncode
    issued = write_receipts(proof, weekly, required)   # per stage, whatever the overall rc: a red stage must not void the green ones
    print(f"[nightly_gate] receipts issued for {len(issued)}/{len(required)} stages: {', '.join(issued) or 'none'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
