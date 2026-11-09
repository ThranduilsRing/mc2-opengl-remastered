"""Frozen merge candidates, fail-closed promotion and red-batch bisection."""
from __future__ import annotations

import json
import csv
import hashlib
import io
import os
from pathlib import Path
import re
import sys
import time
import tarfile

import _git_util
from lane_precheck import Stop

NIFTY = "claude/nifty-mendeleev"
STAGE_PREFIX = "claude/stage-"
STAGE_HELP = """merge queue (bors-style staging; nifty only ever fast-forwards to a proven candidate):
  1. py -3 tools/lane_merge.py --stage W --batch A B C     freeze tips A B C, build claude/stage-W from nifty and merge them there
  2. py -3 scripts/merge_gate.py --full ...                gate the checked-out candidate; writes build64-ninja/gate-verdict.json
     py -3 tools/laptop_gate.py W --sim                   add all six sim PASS rows to the pulled window summary
  3. py -3 tools/lane_merge.py --promote W --verdict <pulled>/gate-verdict.json
                                                         fast-forward nifty to the candidate: refused unless the verdict is a
                                                           complete PASS for the candidate HEAD (no skipped row) and nifty is its
                                                           ancestor and not checked out elsewhere; summary sim and payload identities must match;
                                                           a red leaves nifty untouched
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


NIGHTLY_PROOFS = "E:/mc2-archive-2026-10/proofs/merge-gate"


def nightly_unmet(verdict: dict) -> list[str]:
    """Required stages of the verdict's nightly/weekly tier that did not execute green. A PARTIAL with no recorded tier proves nothing."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
    import merge_gate
    if "tier" not in verdict:
        return []   # W130 review P0: verdicts written before merge_gate recorded `tier` keep the pre-p31 acceptance (PASS/PARTIAL, nothing failed)
    tier = verdict.get("tier") or 0
    if tier < 1:
        return ["(verdict records no nightly tier)"]
    executed = verdict.get("executed") or {}
    required = [n for n, _c, _e in merge_gate.plan(1) if n not in merge_gate.NON_BLOCKING_STAGES
                and (n == "deploy" or 1 <= merge_gate.stage_tier(n) <= tier)]
    return [n for n in required if not merge_gate.stage_passed(str((executed.get(n) or {}).get("status", "")))]


COMPARE_BOOKMARKS = ("mc2_01_saved_205644", "mc2_10_saved_204523")   # merge_gate's day/night capture pair


def missing_visual_evidence(verdict: dict, vpath: Path) -> list[str]:
    """p31: promotion read only JSON, so a window could land without the day/night images the owner is told to view. A verdict
    that required compare-captures must come with its evidence: before/after PNG and the A/B montage per bookmark, either pulled
    beside the verdict (laptop_gate: <pulled>/evidence) or in the gate's own proof dir (artifacts.proof)."""
    if "compare-captures" not in (verdict.get("required") or []):
        return []
    roots = [vpath.parent / "evidence", Path(str((verdict.get("artifacts") or {}).get("proof") or vpath.parent))]
    root = next((r for r in roots if (r / "ab").is_dir()), None)
    if root is None:
        return ["no capture evidence (before/after/ab) beside the verdict or in its proof dir: " + ", ".join(map(str, roots))]
    need = [p for b in COMPARE_BOOKMARKS
            for p in (root / "before" / b / f"{b}.png", root / "after" / b / f"{b}.png", root / "ab" / b / "montage.jpg")]
    gone = [str(p.relative_to(root)) for p in need if not (p.is_file() and p.stat().st_size)]
    if gone:
        return [f"capture evidence incomplete in {root}: missing or empty " + ", ".join(gone)]
    print("[promote] day/night evidence to view: " + ", ".join(str(root / "ab" / b / "montage.jpg") for b in COMPARE_BOOKMARKS))
    return []


def untriaged_nightly_fail(proofs: str, now: float | None = None, max_age_h: float = 36.0) -> list[str]:
    """Fail-closed: refusal reasons unless the newest nightly-<date>/gate-verdict.json is fresh (< max_age_h), has failed == [] and
    is PASS or PARTIAL (an operator stage filter is fine when every row it ran passed). INFRA, FAIL (unless a triage* file sits
    beside it), unreadable and missing verdicts all refuse."""
    dirs = sorted(Path(proofs).glob("nightly-*/gate-verdict.json"), key=lambda p: p.parent.name)
    if not dirs:
        return [f"no nightly verdict under {proofs} (the nightly has not run); run scripts/nightly_gate.py"]
    newest = dirs[-1]
    try:
        v = json.loads(newest.read_text(encoding="utf-8"))
        written = float(v.get("written") or newest.stat().st_mtime)
    except (OSError, ValueError, AttributeError):
        return [f"newest nightly verdict is unreadable ({newest.parent})"]
    why = []
    age_h = ((time.time() if now is None else now) - written) / 3600
    if age_h > max_age_h:
        why.append(f"newest nightly verdict is {age_h:.0f} h old (limit {max_age_h:.0f} h; {newest.parent})")
    verdict = v.get("verdict")
    if verdict == "INFRA":
        why.append(f"newest nightly verdict is INFRA, not a result ({newest.parent}); rerun the nightly")
    elif verdict == "PARTIAL" and not v.get("failed"):
        # A stage filter is the nightly's design, but it is only a nightly if it still ran every stage of ITS tier green.
        why += [f"newest nightly PARTIAL did not run/pass its required stage {n} ({newest.parent})" for n in nightly_unmet(v)]
    elif verdict not in ("PASS", "PARTIAL") or v.get("failed"):
        if not list(newest.parent.glob("triage*")):
            why.append(f"newest nightly verdict is {verdict} with failed rows and untriaged ({newest.parent}); add a triage file there")
    return why


def sim_evidence_problems(vpath: Path, candidate: str) -> list[str]:
    """Bind the six explicit sim PASS rows to this candidate and the pulled payload."""
    try:
        summary = json.loads((vpath.parent / "summary.json").read_text(encoding="utf-8"))
        sim = summary.get("sim", {})
        why = []
        for label, head in (("summary", summary.get("head")), ("sim", sim.get("head"))):
            if head != candidate:
                why.append(f"{label} head {head or '(missing)'} is not candidate {candidate}")
        if sim.get("verdict") != "PASS" or sim.get("rc") != 0 or sim.get("error"):
            why.append("sim verdict must be PASS with rc=0 and no error")
        required = [*("determinism-" + m for m in ("mc2_01", "mc2_03", "mc2_10", "mc2_24")),
                    "saveload-mc2_03", "saveload-mc2_10"]
        for name in required:
            status = sim.get("rows", {}).get(name, {}).get("status")
            # A perf_counter-only note never fails a row (lane rules; merge_gate.passed); REUSED stays refused here.
            if status != "PASS" and not str(status).startswith("PASS (perf_counter-only WARN"):
                why.append(f"sim row {name}: {status or 'MISSING'} (explicit PASS required)")
        payload = vpath.parent / "payload.tar"
        expected = (vpath.parent / "payload.sha256").read_text().split()[0]
        with payload.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != expected:
            why.append("payload sha256 mismatch")
        with tarfile.open(payload) as archive:
            manifest = archive.extractfile(".deployed_manifest.csv").read()
            rows = list(csv.DictReader(io.StringIO(manifest.decode("utf-8-sig").split('\n', 1)[1])))
            hashes = {r["relpath"]: r["sha256"] for r in rows}
            for member in archive.getmembers():
                if member.name.startswith("files/"):
                    rel = member.name[len("files/"):]
                    with archive.extractfile(member) as stream:
                        actual = hashlib.file_digest(stream, "sha256").hexdigest()
                    if actual != hashes.get(rel):
                        why.append(f"payload byte sha256 mismatch: {rel}")
        if hashlib.sha256(manifest).hexdigest() != sim.get("manifest_sha256"):
            why.append("sim payload manifest sha256 mismatch or missing identity")
        exe = next((r for r in rows if r["relpath"] == "mc2.exe"), {})
        if exe.get("src_commit") != candidate:
            why.append("payload mc2.exe candidate head mismatch")
        return why
    except (OSError, ValueError, KeyError, IndexError, AttributeError, TypeError, tarfile.TarError) as exc:
        return [f"missing or unreadable sim/payload evidence beside {vpath}: {exc}"]


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
    # A window verdict that ran --nightly/--weekly (tier >= 1): complete PASS, nothing failed or skipped, for this candidate IS the nightly proof.
    own_nightly = (verdict.get("tier", 0) >= 1 and verdict.get("verdict") == "PASS" and verdict.get("complete")
                   and not verdict.get("failed") and not verdict.get("skipped") and verdict.get("head") == cand)
    if not own_nightly:
        why.extend(untriaged_nightly_fail(NIGHTLY_PROOFS))
    if verdict.get("skipped"):
        why.append("skipped rows: " + ", ".join(sorted(verdict["skipped"])))
    why.extend(missing_visual_evidence(verdict, vpath))
    why.extend(sim_evidence_problems(vpath, cand))
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
