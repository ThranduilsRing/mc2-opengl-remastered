#!/usr/bin/env python3
"""file_size_ratchet - monotonic file-length counter for the owned source dirs.

Giant files are expensive twice: an agent (or human) must read thousands of lines to
touch one function, and every edit rebuilds the whole TU. The file-size program splits
owned sources by responsibility toward ~800-1500 lines; this ratchet stops the count of
oversized files from ever growing while that work is in flight.

  py -3 tools/file_size_ratchet.py                      # report (per-dir table + top 30)
  py -3 tools/file_size_ratchet.py --top 60             # longer list
  py -3 tools/file_size_ratchet.py --write-baseline     # (re)write file_size_ratchet.baseline.json
  py -3 tools/file_size_ratchet.py --check              # exit 1 if a per-dir metric rose
  py -3 tools/file_size_ratchet.py --check --tighten    # on success, lower the baseline to current

Metrics, per top-level dir (and repo-wide):
  files_over_1500   files with more than 1500 lines
  files_over_3000   files with more than 3000 lines
  max_lines         the longest file's length
Check fails when any of those rises above the baseline, or when a file that was not in
the baseline's >1500 set enters it (a new oversized file, or a split piece that is still
too big). A baselined file that merely grew is a WARNING (listed, exit 0): the per-dir
max and counts already bound the damage, and the warning keeps the growth visible.

Line count = PHYSICAL lines (newline count, plus one for an unterminated last line).
Blank and comment lines count on purpose: the metric is the read cost of opening the
file, not the amount of logic in it.

Source of truth: the COMMITTED tree (--rev, default HEAD), so other agents' uncommitted
work never flaps the gate. --staged scans the index, --worktree the dirty tree.

Excluded: generated files (*.gen.*, anything marked linguist-generated in .gitattributes),
3rdparty/, shaders/spv/, build*/ dirs, plus globs in tools/file_size_ratchet.exclude.

Exit: 0 ok, 1 a metric rose / new oversized file, 2 usage/baseline error.  Stdlib only.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import subprocess
import sys
import time
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.abspath(os.path.join(HERE, ".."))
BASELINE = os.path.join(HERE, "file_size_ratchet.baseline.json")
EXCLUDE_FILE_REL = "tools/file_size_ratchet.exclude"

DEFAULT_DIRS = ["code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld",
                "GameAdapters", "HostServices", "shaders", "editor", "gui", "tools"]
# Explicit tooling debt registration; keep unrelated Python outside this change.
REGISTERED_TOOLING = {"tools/scifi_kit/art/intent_military.py"}
EXTS = (".cpp", ".cc", ".h", ".hpp", ".inl",
        ".glsl", ".hglsl", ".vert", ".frag", ".comp", ".tesc", ".tese")
BUILTIN_EXCLUDES = ["3rdparty/", "*/3rdparty/", "shaders/spv/", "*.gen.*",
                    "graphify-out/", "*/__pycache__/", "*/Tracy/"]
T1, T2 = 1500, 3000
TOP_DEFAULT = 30


# ---------------------------------------------------------------- exclusion
def _match(rel, pat):
    if pat.endswith("/"):
        return rel.startswith(pat) or ("/" + pat) in ("/" + rel)
    if "/" not in pat:
        return fnmatch.fnmatchcase(rel.rsplit("/", 1)[-1], pat)
    return fnmatch.fnmatchcase(rel, pat)


def parse_patterns(text):
    return [l.strip() for l in text.splitlines() if l.strip() and not l.lstrip().startswith("#")]


def gitattr_generated(text):
    """Patterns carrying `linguist-generated` (set, not `-` / `=false`)."""
    pats = []
    for line in text.splitlines():
        parts = line.split("#")[0].split()
        for a in parts[1:]:
            if a in ("linguist-generated", "linguist-generated=true"):
                pats.append(parts[0])
    return pats


def wanted(rel, dirs, patterns):
    if rel.startswith("tools/") and rel not in REGISTERED_TOOLING:
        return False
    if rel.split("/", 1)[0] not in dirs or not (rel.endswith(EXTS) or rel in REGISTERED_TOOLING):
        return False
    if any(p.startswith("build") for p in rel.split("/")[:-1]):
        return False
    return not any(_match(rel, p) for p in patterns)


# ---------------------------------------------------------------- measurement
def count_lines(data):
    """Physical line count of bytes/str."""
    if isinstance(data, str):
        data = data.encode("utf-8", "replace")
    if not data:
        return 0
    return data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


class Result:
    def __init__(self):
        self.lines = {}        # rel -> physical line count
        self.per_dir = {}      # dir -> {files, files_over_1500, files_over_3000, max_lines}
        self.total = {}


def measure(files):
    """files: {rel: bytes|str} -> Result. Deterministic."""
    res = Result()
    for rel in sorted(files):
        res.lines[rel] = count_lines(files[rel])
    groups = {}
    for rel, n in res.lines.items():
        groups.setdefault(rel.split("/", 1)[0], []).append(n)
        groups.setdefault("*", []).append(n)
    for d, ns in groups.items():
        m = {"files": len(ns),
             "files_over_1500": sum(1 for n in ns if n > T1),
             "files_over_3000": sum(1 for n in ns if n > T2),
             "max_lines": max(ns)}
        if d == "*":
            res.total = m
        else:
            res.per_dir[d] = m
    return res


def over(res, threshold=T1):
    return {r: n for r, n in res.lines.items() if n > threshold}


def top_files(res, k):
    return sorted(res.lines.items(), key=lambda kv: (-kv[1], kv[0]))[:k]


# ---------------------------------------------------------------- git / fs sources
def _git(root, args, inp=None):
    return subprocess.run(["git"] + args, cwd=root, input=inp, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True, **console_child.child_kwargs(redirected=True)).stdout


def _git_try(root, args):
    try:
        return _git(root, args)
    except subprocess.CalledProcessError:
        return None


def _blob_text(root, spec):
    raw = _git_try(root, ["show", spec])
    return raw.decode("utf-8", "replace") if raw is not None else ""


def load_sources(root, dirs, source, rev):
    """-> {rel: bytes}. source: head | staged | worktree."""
    if source == "worktree":
        def rd(rel):
            try:
                with open(os.path.join(root, rel), "r", encoding="utf-8", errors="replace") as fh:
                    return fh.read()
            except OSError:
                return ""
        attrs, excl = rd(".gitattributes"), rd(EXCLUDE_FILE_REL)
    else:
        pre = ":" if source == "staged" else rev + ":"
        attrs, excl = _blob_text(root, pre + ".gitattributes"), _blob_text(root, pre + EXCLUDE_FILE_REL)
    patterns = BUILTIN_EXCLUDES + gitattr_generated(attrs) + parse_patterns(excl)

    if source == "worktree":
        names = _git(root, ["ls-files", "-z", "--cached", "--others", "--exclude-standard", "--"]
                     + list(dirs)).split(b"\0")
    elif source == "staged":
        names = _git(root, ["ls-files", "-z", "--"] + list(dirs)).split(b"\0")
    else:
        names = _git(root, ["ls-tree", "-r", "-z", "--name-only", rev, "--"] + list(dirs)).split(b"\0")
    rels = sorted(x.decode("utf-8", "replace") for x in names if x)
    rels = [r for r in rels if wanted(r, dirs, patterns)]
    if source == "worktree":
        out = {}
        for r in rels:
            try:
                with open(os.path.join(root, r), "rb") as fh:
                    out[r] = fh.read()
            except OSError:
                pass
        return out
    spec = (lambda r: ":" + r) if source == "staged" else (lambda r: rev + ":" + r)
    raw = _git(root, ["cat-file", "--batch"], "".join(spec(r) + "\n" for r in rels).encode("utf-8"))
    out, pos = {}, 0
    for r in rels:
        eol = raw.index(b"\n", pos)
        hdr = raw[pos:eol].split()
        pos = eol + 1
        if len(hdr) == 3 and hdr[1] == b"blob":
            size = int(hdr[2])
            out[r] = raw[pos:pos + size]
            pos += size + 1
    return out


def head_sha(root, rev):
    raw = _git_try(root, ["rev-parse", "--short=9", rev])
    return raw.decode().strip() if raw else "unknown"


# ---------------------------------------------------------------- baseline
METRICS = ("files_over_1500", "files_over_3000", "max_lines")


def make_baseline(res, dirs, root, rev):
    return {"version": 1, "head": head_sha(root, rev), "dirs": list(dirs),
            "thresholds": [T1, T2],
            "total": res.total,
            "per_dir": {d: res.per_dir[d] for d in sorted(res.per_dir)},
            "files_over_1500": {r: n for r, n in sorted(over(res).items())},
            "top": [{"file": r, "lines": n} for r, n in top_files(res, TOP_DEFAULT)]}


def write_json(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(obj, fh, indent=1, sort_keys=True)
        fh.write("\n")


def check(res, base, out=None):
    """Fail when a per-dir metric rose or a file newly entered the >1500 set."""
    out = out or sys.stdout
    bad = []
    zero = {m: 0 for m in METRICS}
    for d in sorted(set(base["per_dir"]) | set(res.per_dir)):
        b, c = base["per_dir"].get(d, zero), res.per_dir.get(d, zero)
        for m in METRICS:
            if c[m] > b[m]:
                bad.append((d, m, b[m], c[m]))
    bfiles = base.get("files_over_1500", {})
    new = sorted(((n, r) for r, n in over(res).items() if r not in bfiles), reverse=True)
    grew = sorted(((n - bfiles[r], r, bfiles[r], n) for r, n in over(res).items()
                   if r in bfiles and n > bfiles[r]), reverse=True)
    if grew:
        print("file_size_ratchet: WARNING - baselined oversized files grew:", file=out)
        for dl, r, b, c in grew[:10]:
            print("  +%-5d %s (%d -> %d)" % (dl, r, b, c), file=out)
    if not bad and not new:
        print("file_size_ratchet: OK (%d files >%d, baseline %d; max %d, baseline %d)"
              % (res.total["files_over_1500"], T1, base["total"]["files_over_1500"],
                 res.total["max_lines"], base["total"]["max_lines"]), file=out)
        return 0
    print("file_size_ratchet: FAIL - file sizes rose vs baseline (%s):" % base.get("head", "?"), file=out)
    for d, m, b, c in bad:
        print("  %-14s %-16s %6d -> %6d (+%d)" % (d, m, b, c, c - b), file=out)
    if new:
        print("  files newly over %d lines (not in baseline's oversized set):" % T1, file=out)
        for n, r in new:
            print("    %6d  %s" % (n, r), file=out)
    print("Split by responsibility (see .claude/cleanup/giant-file-split-plan.md), keeping new "
          "pieces <= %d lines." % T1, file=out)
    return 1


def improved(res, base):
    zero = {m: 0 for m in METRICS}
    if any(res.per_dir.get(d, zero)[m] < b[m] for d, b in base["per_dir"].items() for m in METRICS):
        return True
    bfiles = base.get("files_over_1500", {})
    return set(over(res)) < set(bfiles) or any(n < bfiles[r] for r, n in over(res).items() if r in bfiles)


# ---------------------------------------------------------------- reporting
def print_report(res, top, out=None, label=""):
    out = out or sys.stdout
    print("file_size_ratchet report%s (%d files)" % (label, res.total.get("files", 0)), file=out)
    print("  %-14s %7s %9s %9s %9s" % ("dir", "files", ">%d" % T1, ">%d" % T2, "max"), file=out)
    for d in sorted(res.per_dir, key=lambda d: (-res.per_dir[d]["max_lines"], d)):
        m = res.per_dir[d]
        print("  %-14s %7d %9d %9d %9d" % (d, m["files"], m["files_over_1500"],
                                         m["files_over_3000"], m["max_lines"]), file=out)
    t = res.total
    print("  %-14s %7d %9d %9d %9d" % ("TOTAL", t["files"], t["files_over_1500"],
                                     t["files_over_3000"], t["max_lines"]), file=out)
    print("\ntop %d files by physical lines:" % top, file=out)
    for i, (r, n) in enumerate(top_files(res, top), 1):
        print("%3d. %6d  %s" % (i, n, r), file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--report", action="store_true")
    g.add_argument("--write-baseline", action="store_true")
    g.add_argument("--check", action="store_true")
    ap.add_argument("--tighten", action="store_true", help="check: on success rewrite baseline lower")
    ap.add_argument("--rev", default="HEAD", help="git rev to scan (default HEAD)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--worktree", action="store_true", help="scan the dirty working tree")
    src.add_argument("--staged", action="store_true", help="scan the git index")
    ap.add_argument("--top", type=int, default=TOP_DEFAULT)
    ap.add_argument("--dirs", nargs="*", default=None)
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--repo", default=DEFAULT_ROOT, help="repo root (tests)")
    a = ap.parse_args(argv)
    source = "worktree" if a.worktree else "staged" if a.staged else "head"
    dirs = a.dirs or DEFAULT_DIRS
    base = None
    if a.check:
        if not os.path.exists(a.baseline):
            print("file_size_ratchet: no baseline at %s (run --write-baseline)" % a.baseline, file=sys.stderr)
            return 2
        with open(a.baseline, "r", encoding="utf-8") as fh:
            base = json.load(fh)
        if not a.dirs:
            dirs = base.get("dirs", dirs)
    t0 = time.time()
    res = measure(load_sources(a.repo, dirs, source, a.rev))
    if not res.lines:
        print("file_size_ratchet: no source files found under %s" % ", ".join(dirs), file=sys.stderr)
        return 2
    if a.check:
        rc = check(res, base)
        if rc == 0 and a.tighten and improved(res, base):
            write_json(a.baseline, make_baseline(res, dirs, a.repo, a.rev))
            print("file_size_ratchet: baseline tightened")
        return rc
    if a.write_baseline:
        nb = make_baseline(res, dirs, a.repo, a.rev)
        write_json(a.baseline, nb)
        print("file_size_ratchet: baseline written: %s (head %s, %d files, %d over %d)"
              % (a.baseline, nb["head"], res.total["files"], res.total["files_over_1500"], T1))
        return 0
    print_report(res, a.top, label=" [%s %s, %.1fs]" % (source, a.rev if source == "head" else "", time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
