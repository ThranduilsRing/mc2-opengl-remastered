"""Sim/save-sensitive path classification and the saveload + sim_determinism rows a precheck runs for such a tip (G3).

WHY: W114 -- a render-only tip leaked a draw delta into a saved field; sim_determinism stayed green and only the gate's
saveload stage caught it. Commands come from merge_gate.plan verbatim (--mask included); this module only classifies and runs them.
Split out of lane_merge.py to keep that file under the 1500-line ratchet.
"""
from __future__ import annotations

import csv
import fnmatch
import json
import os
import re
import subprocess
import sys

import console_child
import _git_util

HERE = os.path.dirname(os.path.abspath(__file__))


def git(root, *args):
    return _git_util.git(root, *args, merge_stderr=True)


def lines(text):
    return [ln for ln in text.splitlines() if ln]


def run(root, cmd, env=None):
    return subprocess.run(cmd, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          encoding="utf-8", errors="replace", **console_child.child_kwargs(redirected=True))


SIM_PATHS_FILE = os.path.join(HERE, "sim_sensitive_paths.json")
#: The gate's own stage names (scripts/merge_gate.plan); the precheck reuses those commands and the --mask verbatim.
SIM_ROWS = ("saveload-mc2_01", "saveload-mc2_10", "determinism-mc2_10")
SIM_NOT_RUN_RCS = (4, 75)   # host lease busy / GATE QUIET refusals of the launchers: infra, not a verdict on the tip
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from smoke_lib.paths import INSTALL_ROOT, PROOF_ROOT

SIM_PROOF_BASE = (PROOF_ROOT + '/precheck-sim')


def sim_patterns(path: str = SIM_PATHS_FILE) -> list[str]:
    with open(path, encoding="utf-8") as f:
        return list(json.load(f)["patterns"])


def sim_sensitive_paths(changed, patterns=None) -> list[str]:
    """The changed paths that match a sim/save-sensitive glob (tools/sim_sensitive_paths.json)."""
    pats = sim_patterns() if patterns is None else patterns
    return sorted(p for p in changed if any(fnmatch.fnmatchcase(p.replace("\\", "/"), g) for g in pats))


def deployed_commit(install: str) -> str | None:
    """src_commit of the mc2.exe row in the install's .deployed_manifest.csv."""
    try:
        with open(os.path.join(install, ".deployed_manifest.csv"), newline="", encoding="utf-8") as f:
            for row in csv.reader(f):
                if row and row[0] == "mc2.exe" and len(row) > 3:
                    return row[3]
    except OSError:
        pass
    return None


def stale_install(root: str, install: str, sha: str) -> str | None:
    """None when the install's exe was deployed from the tip (or from a commit with no build-input change to it), else why not."""
    if not os.path.isfile(os.path.join(install, "mc2.exe")):
        return f"no install at {install}"
    src = deployed_commit(install)
    if not src:
        return f"{install} has no deploy manifest row for mc2.exe"
    if src == sha:
        return None
    r = git(root, "diff", "--name-only", src, sha)
    if r.returncode:
        return f"install exe commit {src[:9]} is unknown to this repo"
    from smoke_lib import fingerprint
    changed = fingerprint.build_inputs_changed(lines(r.stdout))
    return None if not changed else f"install exe was built from {src[:9]}, not the tip {sha[:9]} ({changed[0]} changed): deploy the tip first"


def first_diff_line(text: str) -> str:
    """The first line that names a divergence (saveload: 'differs at packet..', sim_determinism: first divergent frame), else the last line."""
    rows = [l.strip() for l in text.splitlines() if l.strip()]
    for l in rows:
        if re.search(r"differs|DIVERG|diverg|first (differing|divergent)|packet \d+", l, re.I):
            return l
    return rows[-1] if rows else "(no output)"


def default_sim_install(root: str) -> str | None:
    m = re.fullmatch(r"mc2-(lane\d+)", os.path.basename(os.path.abspath(root)))
    return (INSTALL_ROOT + '/') + m.group(1) if m else None


def gate_quiet_reason() -> str | None:
    from smoke_lib import host_lease
    try:
        host_lease.check_gate_quiet(lambda m: None)
    except host_lease.HostLeaseBusy as exc:
        return str(exc)
    return None


def run_sim_rows(root: str, sha: str, install: str | None, proof: str | None = None, runner=None, quiet=None) -> dict:
    """Run the gate's saveload mc2_01/mc2_10 and sim_determinism mc2_10 (merge_gate.plan's own commands, --mask included)
    against `install`. -> {row: 'PASS' | 'FAIL: <first differing line>' | 'NOT RUN: <why>'}; NOT RUN is never a pass."""
    runner = runner or run
    verdicts = {r: "NOT RUN: not reached" for r in SIM_ROWS}
    why = ("no lane install (pass --sim-install)" if not install else stale_install(root, install, sha)) or (quiet or gate_quiet_reason)()
    if why:
        return {r: "NOT RUN: " + why for r in SIM_ROWS}
    import merge_gate
    steps = {n: (c, e) for n, c, e in merge_gate.plan(1, proof or os.path.join(SIM_PROOF_BASE, sha[:12]))}
    for row in SIM_ROWS:
        command, extra = steps[row]
        command = [a for a in command if a != "--allow-owner-install"]  # the gate's flag; a lane install never needs it (farm_stage strips it too)
        command[command.index("--install") + 1] = install
        r = runner(root, command, env=dict(os.environ, **extra))
        out = r.stdout or ""
        print(f"precheck: sim row {row}: rc={r.returncode}", flush=True)
        if r.returncode == 0:
            verdicts[row] = "PASS"
        elif r.returncode in SIM_NOT_RUN_RCS:
            verdicts[row] = "NOT RUN: " + first_diff_line(out)
            break
        else:
            verdicts[row] = "FAIL: " + first_diff_line(out)
            print("\n".join(out.splitlines()[-6:]), flush=True)
    return verdicts


def tip_changed_paths(root: str, tip: str, base: str = "claude/nifty-mendeleev") -> list[str]:
    """Paths the tip changes against its merge-base with nifty."""
    def out(*a):
        r = git(root, *a)
        if r.returncode:
            raise RuntimeError(f"git {' '.join(a)} failed: {r.stdout.strip()}")
        return r.stdout.strip()
    return lines(out("diff", "--name-only", out("merge-base", base, tip), tip + "^{commit}"))


def sim_rows_satisfied(root: str, sha: str, receipt: dict) -> bool:
    """A receipt covers the tip's sim paths: every required row PASS, or the explicit --no-sim-rows override (recorded)."""
    rec = receipt.get("sim_rows")
    if rec is not None:
        return bool(rec.get("override")) or not rec.get("required") or all(rec.get("rows", {}).get(r) == "PASS" for r in SIM_ROWS)
    try:
        return not sim_sensitive_paths(tip_changed_paths(root, sha))   # a pre-G3 receipt cannot vouch for a sim-touching tip
    except RuntimeError:
        return True
