#!/usr/bin/env python3
"""Land-as-ready queue (throughput study J2): green tips land continuously, a full gate runs about every 3 h.

No daemon: the orchestrator (or cron) invokes one subcommand per call.

  land_queue.py submit <branch> <sha>   add a tip to the queue (re-submitting a branch replaces its entry)
  land_queue.py land                    queued tips in order: --lint, --precheck, then the single-tip lane_merge path
                                        (merge + derived follow-ups + verify). Red -> stays in the queue as "rejected" + reason
  land_queue.py gate [--publish]        full merge_gate on the nifty tip. Green -> tag nifty-green-<n> (lanes branch from it);
                                        red -> the range since the last green tag is recorded as suspect
  land_queue.py status                  print the queue, the green tags and the suspect ranges

--root is the nifty worktree (default: this checkout); --queue is the queue file (default $MC2_LAND_QUEUE, else
A:/Games/mc2-ops/land_queue.json). See docs/land-as-ready.md.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(HERE, "..", "scripts"))
import console_child  # noqa: E402  (windowless children)
DEFAULT_ROOT = os.path.abspath(os.path.join(HERE, ".."))
DEFAULT_QUEUE = os.environ.get("MC2_LAND_QUEUE", "A:/Games/mc2-ops/land_queue.json")
NIFTY = "claude/nifty-mendeleev"
TAG_PREFIX = "nifty-green-"
TAIL = 600


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {"queue": [], "suspect": []}


def save(path, state):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, path)


def default_runner(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace",
                       **console_child.child_kwargs(redirected=True))
    return r.returncode, ((r.stdout or "") + (r.stderr or "")).strip()


def git(root, *args, runner=None):
    rc, out = (runner or default_runner)(["git", *args], root)
    return rc, out


def green_tags(root, runner=None):
    rc, out = git(root, "tag", "--list", TAG_PREFIX + "*", runner=runner)
    nums = []
    for t in out.split():
        tail = t[len(TAG_PREFIX):]
        if rc == 0 and tail.isdigit():
            nums.append(int(tail))
    return sorted(nums)


def submit(path, branch, sha):
    state = load(path)
    state["queue"] = [e for e in state["queue"] if e["branch"] != branch]
    state["queue"].append({"branch": branch, "sha": sha, "status": "queued", "reason": "", "submitted": time.time()})
    save(path, state)
    print(f"land_queue: queued {branch} {sha[:12]} (position {len([e for e in state['queue'] if e['status'] == 'queued'])})")
    return 0


def land(path, root, runner=None):
    """Land every queued tip in order. A red tip is rejected with the failing step and tail, then the next is tried."""
    runner = runner or default_runner
    state = load(path)
    py = [sys.executable, "-B", os.path.join(root, "tools", "lane_merge.py"), "--root", root]
    landed = 0
    for e in state["queue"]:
        if e["status"] != "queued":
            continue
        for step, cmd in (("lint", py + ["--lint", e["sha"]]), ("precheck", py + ["--precheck", e["sha"]]),
                          ("merge", py + [e["sha"]])):
            rc, out = runner(cmd, root)
            if rc:
                e["status"], e["reason"] = "rejected", f"{step} rc={rc}: {out[-TAIL:]}"
                print(f"land_queue: REJECT {e['branch']} {e['sha'][:12]} at {step}")
                break
        else:
            _, head = git(root, "rev-parse", "HEAD", runner=runner)
            e["status"], e["reason"], e["landed_head"] = "landed", "", head
            landed += 1
            print(f"land_queue: LANDED {e['branch']} {e['sha'][:12]} -> {head[:12]}")
        save(path, state)   # a crash between tips keeps what already landed recorded
    print(f"land_queue: land done, landed={landed} rejected={sum(1 for e in state['queue'] if e['status'] == 'rejected')}")
    return 0


def gate(path, root, publish=False, runner=None):
    runner = runner or default_runner
    state = load(path)
    nums = green_tags(root, runner)
    last = (TAG_PREFIX + str(nums[-1])) if nums else None
    _, head = git(root, "rev-parse", "HEAD", runner=runner)
    nxt = (nums[-1] + 1) if nums else 1
    window = str(nxt)
    gate_cmd = [sys.executable, "-B", os.path.join(root, "scripts", "merge_gate.py"), "--full", "--window", window,
                "--expect-head", head]
    rc, out = runner(gate_cmd, root)
    if rc:
        _, commits = git(root, "log", "--format=%h %s", f"{last}..{head}" if last else head, runner=runner)
        state["suspect"].append({"from": last, "to": head, "commits": commits.splitlines(), "gate_rc": rc, "when": time.time()})
        save(path, state)
        print(f"land_queue: GATE RED at {head[:12]}; suspect range {last or 'root'}..{head[:12]} ({len(commits.splitlines())} commits)")
        return 1
    tag = TAG_PREFIX + window
    trc, tout = git(root, "tag", tag, head, runner=runner)
    if trc:
        print(f"land_queue: gate green but tagging {tag} failed: {tout}")
        return 1
    state["suspect"] = []   # a green gate at HEAD clears every earlier suspect range
    save(path, state)
    print(f"land_queue: GATE GREEN {head[:12]} tagged {tag}")
    if publish:
        prc, pout = runner([sys.executable, "-B", os.path.join(root, "scripts", "merge_gate.py"), "--publish", "--expect-head", head], root)
        print(f"land_queue: publish {'OK' if prc == 0 else 'FAILED rc=' + str(prc) + ': ' + pout[-TAIL:]}")
        return int(prc != 0)
    print("land_queue: not published (pass --publish after the montage has been viewed)")
    return 0


def status(path, root):
    state = load(path)
    for e in state["queue"]:
        print(f"{e['status']:9} {e['branch']} {e['sha'][:12]} {e.get('reason', '')[:120]}")
    print("green tags:", ", ".join(TAG_PREFIX + str(n) for n in green_tags(root)) or "none")
    for s in state["suspect"]:
        print(f"suspect: {s['from']}..{s['to'][:12]} ({len(s['commits'])} commits)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--queue", default=DEFAULT_QUEUE)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("submit")
    s.add_argument("branch")
    s.add_argument("sha")
    sub.add_parser("land")
    g = sub.add_parser("gate")
    g.add_argument("--publish", action="store_true")
    sub.add_parser("status")
    a = ap.parse_args(argv)
    if a.cmd == "submit":
        return submit(a.queue, a.branch, a.sha)
    if a.cmd == "land":
        return land(a.queue, a.root)
    if a.cmd == "gate":
        return gate(a.queue, a.root, a.publish)
    return status(a.queue, a.root)


if __name__ == "__main__":
    sys.exit(main())
