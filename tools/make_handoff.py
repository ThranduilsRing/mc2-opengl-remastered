#!/usr/bin/env python3
"""DEV-EFFICIENCY-BOOTSTRAP-1 #4 - standard slice handoff generator.

Emits the consistent handoff block agents have been hand-writing as giant prose
every slice. Auto-fills the mechanical parts from git (commits, files changed,
mergeable branches / prunable worktrees); leaves the judgment parts as labeled
placeholders to fill in. Feeds straight into the final message and the memory file.

Usage:
  py -3 tools/make_handoff.py --slice ICON-ATLAS-HARNESS-1 --since HEAD~3
  py -3 tools/make_handoff.py --slice X --base claude/nifty-mendeleev
  py -3 tools/make_handoff.py --slice X --since HEAD~2 --nifty claude/nifty-mendeleev

CLI-only (compose later into MCP if it proves useful). Read-only - no mutations.
"""

import argparse
import os
import subprocess
import sys
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(args):
    r = subprocess.run(["git"] + args, capture_output=True, text=True, cwd=REPO_ROOT, **console_child.child_kwargs(redirected=True))
    return r.stdout.rstrip(), r.returncode


def main():
    ap = argparse.ArgumentParser(description="Generate a standard slice handoff block.")
    ap.add_argument("--slice", required=True, help="slice name, e.g. ICON-ATLAS-HARNESS-1")
    ap.add_argument("--since", default=None, help="rev to diff from (e.g. HEAD~3)")
    ap.add_argument("--base", default=None, help="explicit base ref (overrides --since)")
    ap.add_argument("--nifty", default="claude/nifty-mendeleev", help="mainline ref")
    args = ap.parse_args()

    base = args.base or args.since or "HEAD~3"
    rng = f"{base}..HEAD"

    branch, _ = git(["rev-parse", "--abbrev-ref", "HEAD"])
    head, _ = git(["rev-parse", "--short", "HEAD"])
    commits, _ = git(["log", "--oneline", rng])
    stat, _ = git(["diff", "--stat", rng])
    names, _ = git(["diff", "--name-only", rng])
    nfiles = len([l for l in names.splitlines() if l.strip()])

    # Prunable: local claude/* branches already merged into nifty (minus current).
    merged, mrc = git(["branch", "--merged", args.nifty, "--format=%(refname:short)"])
    prunable = []
    if mrc == 0:
        for b in merged.splitlines():
            b = b.strip()
            if b and b != branch and b.startswith("claude/") and b != args.nifty:
                prunable.append(b)

    # Worktrees (porcelain) for the prune list.
    wt_out, _ = git(["worktree", "list", "--porcelain"])
    worktrees = []
    cur = {}
    for line in wt_out.splitlines():
        if line.startswith("worktree "):
            if cur:
                worktrees.append(cur)
            cur = {"path": line.split(" ", 1)[1]}
        elif line.startswith("branch "):
            cur["branch"] = line.split(" ", 1)[1].replace("refs/heads/", "")
    if cur:
        worktrees.append(cur)
    prunable_wt = [w for w in worktrees
                   if w.get("branch", "").replace("refs/heads/", "") in set(prunable)]

    md = []
    md.append(f"## {args.slice} - handoff")
    md.append("")
    md.append(f"**branch** `{branch}` @ `{head}`  |  **range** `{rng}`")
    md.append("")
    md.append("> _AUTO (git): commits, files, prune list. MANUAL (you must write):"
              " Proven / NOT-proven / Next - never infer proof from git data alone._")
    md.append("")
    md.append("### Commits")
    md.append("```")
    md.append(commits or "(none)")
    md.append("```")
    md.append("### Files changed " + f"({nfiles})")
    md.append("```")
    md.append(stat or "(none)")
    md.append("```")
    md.append("### Tests run")
    md.append("- [ ] contract runner: `py -3 tools/build_contract_harnesses.py --run`  -> _result_")
    md.append("- [ ] tier1 smoke (if production touched): _5/5 ?_")
    md.append("### Proven")
    md.append("- _what this slice demonstrably establishes_")
    md.append("### NOT proven / out of scope")
    md.append("- _what was deferred or needs a human (e.g. visual confirm)_")
    md.append("### Next recommended slice")
    md.append("- _the next target_")
    md.append("### Prune after merge")
    if prunable:
        for b in prunable:
            wt = next((w["path"] for w in prunable_wt if w.get("branch") == b), None)
            md.append(f"- branch `{b}`" + (f"  +worktree `{wt}`" if wt else ""))
    else:
        md.append("- _(no merged claude/* branches detected)_")
    md.append("")
    print("\n".join(md))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
