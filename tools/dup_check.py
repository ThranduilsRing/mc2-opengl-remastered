#!/usr/bin/env python3
"""dup_check - copy-paste may not rise: PMD CPD duplicate count at the tip vs the merge base. No baseline file.

  py -3 tools/dup_check.py --check [--base REF] [--rev REV] [--min-tokens N] [--report]

Both trees are the COMMITTED content of the owned C++ dirs (git archive, so a dirty working tree cannot flap it), generated *.gen.h
files dropped. CPD (`pmd cpd --language cpp`) runs where PMD is: on the box (Linux, JRE + /workspace/tools/pmd), or from the PC by
streaming the two trees to the box over ssh. Metric: total duplicated lines (sum of the `lines` column over CPD matches). Exit 0 = no
rise, 1 = rose (the verdict prints the delta), 3 = INFRA (no PMD/JRE locally and the box is unreachable): never a silent pass.

N = 60 tokens: the old tool counted runs of 8 normalized non-trivial lines (~7 tokens a line); on the same tree CPD at 60 reports
36.4k duplicated lines against the old tool's 38.9k (100 -> 22k, 150 -> 15k), so 60 keeps the old sensitivity. Shaders are not scanned
(CPD has no GLSL language); the old shader window count is gone with the old tool.
"""
from __future__ import annotations

import argparse
import csv
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402

DIRS = ["code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld", "GameAdapters", "HostServices"]
EXTS = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".inl", ".inc")
GENERATED = (".gen.h", ".gen.hpp")
MIN_TOKENS = 60
DEFAULT_BASE = "claude/nifty-mendeleev"
PMD_LOCAL = os.environ.get("MC2_PMD", "/workspace/tools/pmd/pmd-bin-7.28.0/bin/pmd")
KEY = os.environ.get("MC2_BOX_KEY", "C:/SPB_Data/.ssh/mc2farm_ed25519")
HOST = os.environ.get("MC2_BOX_HOST", "root@14.227.95.149")
PORT = os.environ.get("MC2_BOX_PORT", "30598")
SSH = ["ssh", "-i", KEY, "-p", PORT, "-o", "LogLevel=ERROR", "-o", "BatchMode=yes", HOST]


def keep_member(name: str) -> bool:
    n = name.replace("\\", "/")
    return n.lower().endswith(EXTS) and not n.lower().endswith(GENERATED) and n.split("/", 1)[0] in DIRS


def filtered_tar(rev: str, repo: str = ROOT) -> bytes:
    """git archive of the owned dirs at rev, reduced to the scanned C++ files, as tar bytes."""
    top = subprocess.run(["git", "-C", repo, "ls-tree", "--name-only", rev], capture_output=True, text=True,
                         **console_child.child_kwargs(redirected=True)).stdout.split()
    dirs = [d for d in DIRS if d in top]  # a dir absent at an old base is simply empty there
    ga = subprocess.run(["git", "-C", repo, "archive", "--format=tar", rev, "--"] + dirs, capture_output=True,
                        **console_child.child_kwargs(redirected=True))
    if ga.returncode:
        raise RuntimeError("git archive %s failed: %s" % (rev, ga.stderr.decode(errors="replace").strip()[:200]))
    out = io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(ga.stdout)) as src, tarfile.open(fileobj=out, mode="w") as dst:
        for m in src:
            if m.isfile() and keep_member(m.name):
                dst.addfile(m, src.extractfile(m))
    return out.getvalue()


def cpd_args(min_tokens: int) -> list:
    return ["cpd", "--language", "cpp", "--minimum-tokens", str(min_tokens), "--format", "csv", "--no-fail-on-violation", "--no-fail-on-error", "--dir", "."]


def parse_csv(text: str) -> tuple:
    """-> (matches, duplicated lines). CPD csv columns: lines,tokens,occurrences,file...; the header row names them."""
    rows = list(csv.DictReader(io.StringIO(text)))
    return len(rows), sum(int(r["lines"]) for r in rows)


def have_local_pmd():
    pmd = shutil.which("pmd") or (PMD_LOCAL if os.path.exists(PMD_LOCAL) else None)
    return pmd if pmd and shutil.which("java") else None


def cpd_local(tar_bytes: bytes, pmd: str, min_tokens: int) -> str:
    with tempfile.TemporaryDirectory(prefix="dup_check_") as d:
        with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as t:
            t.extractall(d, filter="data")
        r = subprocess.run([pmd] + cpd_args(min_tokens), cwd=d, capture_output=True, text=True, errors="replace",
                           **console_child.child_kwargs(redirected=True))
    if r.returncode:
        raise RuntimeError("pmd cpd exited %d: %s" % (r.returncode, r.stderr.strip()[-300:]))
    return r.stdout


def cpd_remote(tar_bytes: bytes, min_tokens: int) -> str:
    script = ("d=$(mktemp -d) && tar -x -C \"$d\" && cd \"$d\" && %s %s; rc=$?; cd /; rm -rf \"$d\"; exit $rc"
              % ("/workspace/tools/pmd/pmd-bin-7.28.0/bin/pmd", " ".join(cpd_args(min_tokens))))
    r = subprocess.run(SSH + [script], input=tar_bytes, capture_output=True, **console_child.child_kwargs(redirected=True))
    if r.returncode:
        raise RuntimeError("box cpd exited %d: %s" % (r.returncode, r.stderr.decode(errors="replace").strip()[-300:]))
    return r.stdout.decode(errors="replace")


def measure(rev: str, min_tokens: int, pmd, repo: str = ROOT) -> tuple:
    tar = filtered_tar(rev, repo)
    return parse_csv(cpd_local(tar, pmd, min_tokens) if pmd else cpd_remote(tar, min_tokens))


def verdict(base: tuple, tip: tuple) -> tuple:
    """-> (exit code, one-line message). Fails only when the duplicated-line total rises."""
    delta = tip[1] - base[1]
    msg = "dup_check: base %d lines/%d matches, tip %d lines/%d matches, delta %+d lines" % (base[1], base[0], tip[1], tip[0], delta)
    return (1, msg + " -- RISE: copy-paste grew; factor the new duplicate out") if delta > 0 else (0, msg + " -- ok")


def resolve_base(base, rev, repo=ROOT):
    ref = base or DEFAULT_BASE
    r = subprocess.run(["git", "-C", repo, "merge-base", rev, ref], capture_output=True, text=True,
                       **console_child.child_kwargs(redirected=True))
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="PMD CPD duplicate count at the tip vs the merge base")
    ap.add_argument("--check", action="store_true", help="the default; kept so rows read like the other ratchets")
    ap.add_argument("--report", action="store_true", help="print the numbers and exit 0")
    ap.add_argument("--base", help="ref to compare against (default: merge-base with %s)" % DEFAULT_BASE)
    ap.add_argument("--rev", default="HEAD", help="tip to measure (committed content)")
    ap.add_argument("--min-tokens", type=int, default=MIN_TOKENS)
    args = ap.parse_args(argv)
    base = resolve_base(args.base, args.rev)
    if not base:
        print("dup_check: INFRA: no merge base between %s and %s" % (args.rev, args.base or DEFAULT_BASE), file=sys.stderr)
        return 3
    pmd = have_local_pmd()
    try:
        b = measure(base, args.min_tokens, pmd)
        t = measure(args.rev, args.min_tokens, pmd)
    except (RuntimeError, OSError) as e:
        print("dup_check: INFRA: %s (no JRE/PMD here and the box is unreachable, or CPD failed): not a pass" % e, file=sys.stderr)
        return 3
    code, msg = verdict(b, t)
    print(msg + " (N=%d, base %s)" % (args.min_tokens, base[:9]))
    return 0 if args.report else code


if __name__ == "__main__":
    sys.exit(main())
