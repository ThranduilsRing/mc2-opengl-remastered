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
import shutil
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


EX_BUSY = 75   # tree or queue busy / infra failure: nothing was judged, retry later
INFRA_RCS = (3, 4)   # lane_merge: MERGE_HEAD present, staged files in the way
INFRA_TEXT = ("tree busy", "REFUSED", "PARTIAL")


class Busy(Exception):
    pass


class QueueLock:
    """Exclusive OS file lock shared by submit, land and gate (the OS drops it if the holder dies)."""
    def __init__(self, queue_path, wait_s=0, sleep=time.sleep, clock=time.monotonic):
        self.path, self.wait_s, self.sleep, self.clock, self.fh = queue_path + ".lock", wait_s, sleep, clock, None

    def _try(self):
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(self.fh.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(self.fh, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def __enter__(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.fh = open(self.path, "a+")
        deadline = self.clock() + self.wait_s
        while True:
            try:
                self._try()
                return self
            except OSError:
                if self.clock() >= deadline:
                    self.fh.close()
                    raise Busy(self.path)
                self.sleep(0.2)

    def __exit__(self, *exc):
        self.fh.close()


def _read(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def load(path):
    """Queue state. A corrupt file is moved to .corrupt-<ts> and the .bak is used; with no usable .bak, Busy."""
    try:
        return _read(path)
    except FileNotFoundError:
        return {"queue": [], "suspect": []}
    except ValueError:
        os.replace(path, f"{path}.corrupt-{int(time.time())}")
        try:
            return _read(path + ".bak")
        except (OSError, ValueError):
            raise Busy(path + " is corrupt and has no usable .bak")


def save(path, state):
    """Atomic write: tmp file, .bak of the previous state, os.replace retried (Windows PermissionError)."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)
    if os.path.exists(path):
        shutil.copyfile(path, path + ".bak")
    for i in range(5):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == 4:
                raise
            time.sleep(0.2)


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


def submit(path, branch, sha, wait_s=30):
    """Read-modify-write under the queue lock, so a submit during a land is never lost."""
    try:
        with QueueLock(path, wait_s=wait_s):
            state = load(path)
            state["queue"] = [e for e in state["queue"] if e["branch"] != branch]
            state["queue"].append({"branch": branch, "sha": sha, "status": "queued", "reason": "", "submitted": time.time()})
            save(path, state)
    except Busy as exc:
        print(f"land_queue: BUSY ({exc})")
        return EX_BUSY
    print(f"land_queue: queued {branch} {sha[:12]} (position {len([e for e in state['queue'] if e['status'] == 'queued'])})")
    return 0


def is_infra(rc, out):
    return rc in INFRA_RCS or any(t in out for t in INFRA_TEXT)


def nifty_problem(root, runner):
    """Why the nifty tree cannot take a tip right now, or None. Checked before every tip."""
    _, br = git(root, "rev-parse", "--abbrev-ref", "HEAD", runner=runner)
    if br != NIFTY:
        return f"{root} is on {br!r}, not {NIFTY}"
    _, dirty = git(root, "status", "--porcelain", "--untracked-files=no", runner=runner)
    if dirty:
        return "tracked changes in the nifty tree: " + dirty.splitlines()[0]
    if git(root, "rev-parse", "-q", "--verify", "MERGE_HEAD", runner=runner)[0] == 0:
        return "a merge is in progress (MERGE_HEAD)"
    return None


def restore(root, pre, runner):
    """Back to the pre-merge head after any red: abort a half merge, then hard-reset a merge that was kept."""
    if git(root, "rev-parse", "-q", "--verify", "MERGE_HEAD", runner=runner)[0] == 0:
        git(root, "merge", "--abort", runner=runner)
    _, head = git(root, "rev-parse", "HEAD", runner=runner)
    if head != pre:
        git(root, "reset", "--hard", pre, runner=runner)
        return head
    return None


def land(path, root, runner=None, dry_run=False):
    """Land every queued tip in order: lint, precheck, then `--batch <sha> --verify full` (regenerate, ratchets, build,
    doctest, board). A tip red is rejected with the step and tail and nifty is reset to the pre-merge head; an infra
    failure re-queues the tip and stops the loop with 75. Returns 0 all landed, 1 some rejected, 75 busy/infra."""
    runner = runner or default_runner
    py = [sys.executable, "-B", os.path.join(root, "tools", "lane_merge.py"), "--root", root]
    try:
        with QueueLock(path):
            return _land_locked(path, root, runner, py, dry_run)
    except Busy as exc:
        print(f"land_queue: BUSY, {exc}")
        return EX_BUSY


def _land_locked(path, root, runner, py, dry_run):
    state = load(path)
    if dry_run:
        for e in state["queue"]:
            if e["status"] == "queued":
                print(f"land_queue: would land {e['branch']} {e['sha'][:12]}: lint, precheck --no-link, --batch --verify full")
        return 0
    landed = rejected = 0
    for e in state["queue"]:
        if e["status"] != "queued":
            continue
        why = nifty_problem(root, runner)
        if why:
            print(f"land_queue: BUSY, {why}; no entry touched")
            return EX_BUSY
        _, pre = git(root, "rev-parse", "HEAD", runner=runner)
        e["status"], e["pre_head"] = "landing", pre
        save(path, state)
        steps = (("lint", py + ["--lint", e["sha"]]), ("precheck", py + ["--precheck", e["sha"], "--no-link"]),
                 ("merge", py + ["--batch", e["sha"], "--verify", "full"]))
        outcome = None
        for step, cmd in steps:
            try:
                rc, out = runner(cmd, root)
            except OSError as exc:
                rc, out = 4, f"OSError: {exc}"
            if rc:
                outcome = (step, rc, out)
                break
        if outcome is None:
            _, head = git(root, "rev-parse", "HEAD", runner=runner)
            e["status"], e["reason"], e["landed_head"] = "landed", "", head
            landed += 1
            print(f"land_queue: LANDED {e['branch']} {e['sha'][:12]} -> {head[:12]}")
            save(path, state)   # a crash between tips keeps what already landed recorded
            continue
        step, rc, out = outcome
        undone = restore(root, pre, runner)
        if undone:
            e["reset_from"] = undone
        if is_infra(rc, out):
            e["status"], e["reason"] = "queued", f"infra at {step} rc={rc}: {out[-TAIL:]}"
            save(path, state)
            print(f"land_queue: INFRA at {step} for {e['branch']} (rc={rc}); tip stays queued, stopping")
            return EX_BUSY
        e["status"], e["reason"] = "rejected", f"{step} rc={rc}: {out[-TAIL:]}"
        rejected += 1
        print(f"land_queue: REJECT {e['branch']} {e['sha'][:12]} at {step}" + (f" (nifty reset from {undone[:12]})" if undone else ""))
        save(path, state)
    print(f"land_queue: land done, landed={landed} rejected={rejected}")
    return 1 if rejected else 0


def gate(path, root, publish=False, runner=None):
    try:
        with QueueLock(path):
            return _gate_locked(path, root, publish, runner or default_runner)
    except Busy as exc:
        print(f"land_queue: BUSY, {exc}")
        return EX_BUSY


def _gate_locked(path, root, publish, runner):
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
    ln = sub.add_parser("land")
    ln.add_argument("--dry-run", action="store_true")
    g = sub.add_parser("gate")
    g.add_argument("--publish", action="store_true")
    sub.add_parser("status")
    a = ap.parse_args(argv)
    if a.cmd == "submit":
        return submit(a.queue, a.branch, a.sha)
    if a.cmd == "land":
        return land(a.queue, a.root, dry_run=a.dry_run)
    if a.cmd == "gate":
        return gate(a.queue, a.root, a.publish)
    return status(a.queue, a.root)


if __name__ == "__main__":
    sys.exit(main())
