"""Frozen merge candidates, fail-closed promotion and red-batch bisection."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import _git_util
import console_child
from lane_precheck import Stop

NIFTY = "claude/nifty-mendeleev"
STAGE_PREFIX = "claude/stage-"
STAGE_HELP = """merge queue (bors-style staging; nifty only ever fast-forwards to a proven candidate):
  1. py -3 tools/lane_merge.py --stage W --batch A B C     freeze tips A B C, build claude/stage-W from nifty and merge them there
  2. py -3 scripts/merge_gate.py --full ...                gate the checked-out candidate; writes build64-ninja/gate-verdict.json
  3. py -3 tools/lane_merge.py --promote W                 fast-forward nifty to the candidate: refused unless the verdict is a
                                                           complete PASS for the candidate HEAD (no skipped row) and nifty is its
                                                           ancestor and not checked out elsewhere; a red leaves nifty untouched
  4. on a red: py -3 tools/lane_merge.py --bisect W --red  halve the frozen tips (first half retested, rest queued) and print the
                                                           next --stage command; a red single tip is GUILTY and goes back to its lane
     after a green half: --promote W, then --bisect W --green queues the rest on the new nifty
  state: build64-ninja/stage-W.json (base, frozen tips, testing, queue, rejected, history)"""


def refuse_direct_nifty(root: str, hotfix: bool) -> None:
    """Only --promote moves nifty. A merge run with nifty checked out is refused (stage instead) unless --hotfix, which is logged."""
    if git_out(root, "branch", "--show-current") != NIFTY:
        return
    if hotfix:
        print(f"HOTFIX direct merge into {NIFTY} (bypasses stage/gate/promote; recorded in merge-window.json and the next gate verdict)")
        return
    raise Stop(5, f"DIRECT MERGE REFUSED: {NIFTY} only moves by --promote. Run: py -3 tools/lane_merge.py --stage W --batch <tips>, "
                  "gate the candidate, then --promote W (--hotfix is the logged escape)")


def stage_preflight(root: str, window: str) -> list[str]:
    """Cheap gate rows (staged-hygiene --range, dup_check, background_launch) on the checked-out candidate, before any build.
    A red marks the stage refused in its state file; --promote then refuses. Returns the red lines."""
    proc = subprocess.run([sys.executable, "scripts/merge_gate.py", "--cheap-rows"], cwd=root, capture_output=True, text=True,
                          **console_child.child_kwargs(redirected=True))
    if proc.returncode == 0:
        return []
    reds = [ln for ln in (proc.stdout + proc.stderr).splitlines() if "cheap row FAIL" in ln] or [f"merge_gate --cheap-rows rc={proc.returncode}"]
    state = load_stage(root, window)
    state["refused"] = reds
    save_stage(root, window, state)
    return reds


def git_out(root: str, *args: str) -> str:
    try:
        return _git_util.git_out(root, *args)
    except _git_util.GitError as exc:
        raise Stop(2, str(exc)) from exc


def stage_state_path(root: str, window: str) -> Path:
    return Path(root) / "build64-ninja" / f"stage-{window}.json"


def load_stage(root: str, window: str) -> dict:
    try:
        return json.loads(stage_state_path(root, window).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        raise Stop(2, f"no staging state for window {window} ({stage_state_path(root, window)}); run --stage {window} --batch ... first")


def save_stage(root: str, window: str, state: dict) -> None:
    path = stage_state_path(root, window)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(state, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def bisect_step(state: dict, passed: bool) -> tuple[dict, str]:
    """Pure queue step after a candidate's verdict. -> (new state, action): "promote" (green: the tested tips may land; any queued
    rest is tested next on the new nifty), "guilty" (a red single tip is rejected; the queue is tested next), "retest" (a red batch
    is halved: the first half is tested, the second goes to the front of the queue), "done" (nothing left)."""
    s = dict(state, testing=list(state.get("testing", [])), queue=list(state.get("queue", [])),
             rejected=list(state.get("rejected", [])), history=list(state.get("history", [])))
    tested = s["testing"]
    s["history"].append(dict(tested=tested, passed=passed))
    if passed:
        s["landed"] = list(state.get("landed", [])) + tested
        s["testing"], s["queue"] = s["queue"], []
        return s, "promote"
    if len(tested) <= 1:
        s["rejected"] += tested
        s["testing"], s["queue"] = s["queue"], []
        return s, "guilty" if tested else "done"
    half = (len(tested) + 1) // 2
    s["testing"], s["queue"] = tested[:half], tested[half:] + s["queue"]
    return s, "retest"


def checked_out_elsewhere(root: str, branch: str) -> list[str]:
    """Worktrees that have `branch` checked out (update-ref under a checkout would leave it with a moved branch and a stale tree)."""
    out, path = [], None
    for line in git_out(root, "worktree", "list", "--porcelain").splitlines():
        if line.startswith("worktree "):
            path = line[len("worktree "):]
        elif line == f"branch refs/heads/{branch}":
            out.append(path)
    return out


def stage(root: str, window: str, tips: list[str]) -> dict:
    """Freeze the tips and check out claude/stage-<window> at the current nifty; the --batch merge then runs on that branch."""
    if not re.fullmatch(r"[A-Za-z0-9._-]+", window):
        raise Stop(2, f"bad window name {window!r}")
    if _git_util.status_entries(root):
        raise Stop(4, "staging needs a clean worktree (git status is not empty)")
    base = git_out(root, "rev-parse", f"{NIFTY}^{{commit}}")
    frozen = [git_out(root, "rev-parse", f"{t}^{{commit}}") for t in tips]
    branch = STAGE_PREFIX + window
    git_out(root, "checkout", "-q", "-B", branch, base)
    try:
        prev = load_stage(root, window)
    except Stop:
        prev = {}
    state = dict(prev, window=window, branch=branch, base=base, tips=dict(zip(frozen, tips)), testing=frozen,
                 queue=[q for q in prev.get("queue", []) if q not in frozen], staged=time.time())
    save_stage(root, window, state)
    print(f"STAGE {branch} base={base[:9]} tips={len(frozen)} (" + " ".join(f[:9] for f in frozen) + ")")
    return state


def promote(root: str, window: str, verdict_path: str | None = None) -> int:
    """Fast-forward nifty to the staged candidate, fail-closed. Every refusal leaves nifty exactly where it was."""
    state = load_stage(root, window)
    branch = state.get("branch", STAGE_PREFIX + window)
    started = time.monotonic()
    vpath = Path(verdict_path or Path(root) / "build64-ninja" / "gate-verdict.json")
    try:
        verdict = json.loads(vpath.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise Stop(6, f"PROMOTE REFUSED: no readable gate verdict at {vpath} ({exc})")
    cand = git_out(root, "rev-parse", f"{branch}^{{commit}}")
    old = git_out(root, "rev-parse", f"{NIFTY}^{{commit}}")
    why = []
    if state.get("refused"):
        why.append("stage refused by its preflight: " + "; ".join(state["refused"]))
    if verdict.get("verdict") != "PASS" or not verdict.get("complete"):
        why.append(f"verdict {verdict.get('verdict')} complete={verdict.get('complete')}")
    if verdict.get("skipped"):
        why.append("skipped rows: " + ", ".join(sorted(verdict["skipped"])))
    if verdict.get("head") != cand:
        why.append(f"verdict head {str(verdict.get('head'))[:9]} is not the candidate {cand[:9]}")
    if _git_util.git(root, "merge-base", "--is-ancestor", old, cand, merge_stderr=True).returncode != 0:
        why.append(f"nifty {old[:9]} is not an ancestor of the candidate (not a fast-forward)")
    elsewhere = checked_out_elsewhere(root, NIFTY)
    if elsewhere:
        why.append("nifty is checked out in " + ", ".join(elsewhere))
    if why:
        raise Stop(6, "PROMOTE REFUSED (nifty untouched): " + "; ".join(why))
    git_out(root, "update-ref", "-m", f"merge queue: promote {branch}", f"refs/heads/{NIFTY}", cand, old)
    tag = f"nifty-green-{window}-{cand[:9]}"
    git_out(root, "tag", "-a", "-f", "-m", f"gate PASS (complete) for {cand}", tag, cand)   # local only, never pushed
    if git_out(root, "branch", "--show-current") == branch and not _git_util.status_entries(root):
        git_out(root, "checkout", "-q", NIFTY)   # same commit: the tree does not change
    seconds = round(time.monotonic() - started, 1)
    verdict.setdefault("timings", {})["promotion"] = seconds
    tmp = vpath.with_name(vpath.name + ".tmp")
    tmp.write_text(json.dumps(verdict, indent=2), encoding="utf-8")
    os.replace(tmp, vpath)
    state["promoted"] = dict(sha=cand, old=old, seconds=seconds, at=time.time(), tag=tag)
    save_stage(root, window, state)
    print(f"PROMOTED {NIFTY} {old[:9]} -> {cand[:9]} ({seconds}s) tag={tag}")
    return 0


def bisect(root: str, window: str, passed: bool) -> int:
    state, action = bisect_step(load_stage(root, window), passed)
    save_stage(root, window, state)
    names = state.get("tips", {})
    if action == "guilty":
        print(f"GUILTY {names.get(state['rejected'][-1], state['rejected'][-1])} ({state['rejected'][-1][:9]}): back to its lane")
    if state["testing"]:
        print(f"NEXT py -3 tools/lane_merge.py --stage {window} --batch " + " ".join(state["testing"]))
    else:
        print(f"QUEUE EMPTY window={window} landed={len(state.get('landed', []))} rejected={len(state['rejected'])}")
    return 0
