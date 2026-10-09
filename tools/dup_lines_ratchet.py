#!/usr/bin/env python3
"""dup_lines_ratchet - monotonic copy-paste counter for the owned source dirs.

GL<->VK twins and intra-backend clones are a recurring drift source: a fix lands in one
copy and the other silently keeps the bug. This counts source lines that sit inside a
run of >= N consecutive (normalized) lines occurring at least twice anywhere in the
tree -- across files and within a file -- and lets that count only go DOWN.

  py -3 tools/dup_lines_ratchet.py                      # report (totals, per-dir, top groups)
  py -3 tools/dup_lines_ratchet.py --top 50             # longer work queue
  py -3 tools/dup_lines_ratchet.py --write-baseline     # (re)write dup_lines_ratchet.baseline.json
  py -3 tools/dup_lines_ratchet.py --check              # exit 1 if total or any per-dir count rose
  py -3 tools/dup_lines_ratchet.py --check --tighten    # on success, lower the baseline to current

Normalization: comments removed, whitespace collapsed, blank lines dropped, so a "run" is
consecutive meaningful lines. A window only counts when at least half of it is
non-trivial (braces, #include, bare `break;` carry no logic) and half of it is distinct
non-trivial lines (tables of one repeated row are data, not copy-paste).

Source of truth: the COMMITTED tree (--rev, default HEAD), so other agents' uncommitted
work never flaps the gate. --staged scans the index, --worktree the dirty tree.

Excluded: generated files (*.gen.h, *.gen.hglsl, shaders/spv/, 3rdparty/, anything marked
linguist-generated in .gitattributes) plus globs in tools/dup_lines_ratchet.exclude.

Exit: 0 ok, 1 a count rose, 2 usage/baseline error.  Stdlib only.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter, defaultdict
from itertools import accumulate

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.abspath(os.path.join(HERE, ".."))
BASELINE = os.path.join(HERE, "dup_lines_ratchet.baseline.json")
EXCLUDE_FILE_REL = "tools/dup_lines_ratchet.exclude"

DEFAULT_DIRS = ["code", "mclib", "GameOS", "RenderCore", "GuiRuntime",
                "RenderWorld", "GameAdapters", "HostServices", "shaders"]
DEFAULT_N = 8
EXTS = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".inl",
        ".frag", ".vert", ".comp", ".tesc", ".tese", ".geom", ".glsl", ".hglsl")
# Always-on exclusions; the exclude file and .gitattributes add to these.
BUILTIN_EXCLUDES = ["3rdparty/", "*/3rdparty/", "shaders/spv/", "*.gen.h", "*.gen.hglsl",
                    "graphify-out/", "*/__pycache__/", "*/Tracy/"]

_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
_NOALNUM = re.compile(r"^[\W_]*$")
_TRIVIAL = frozenset(["#endif", "#else", "#pragma once", "break;", "else", "return;",
                      "default:", "continue;"])


# ---------------------------------------------------------------- normalization
def _blank(m):
    t = m.group(0)
    if t.startswith("//"):
        return ""
    if t.startswith("/*"):
        return "\n" * t.count("\n")           # keep line numbers stable
    return t


def normalize(text):
    """-> list of (line_number, normalized_text, is_trivial) for every non-blank code line."""
    out = []
    for no, raw in enumerate(_TOKEN.sub(_blank, text).split("\n"), 1):
        line = " ".join(raw.split())
        if not line:
            continue
        triv = (line in _TRIVIAL or line.startswith("#include") or _NOALNUM.match(line) is not None)
        out.append((no, line, triv))
    return out


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
        if len(parts) < 2:
            continue
        for a in parts[1:]:
            if a == "linguist-generated" or a == "linguist-generated=true":
                pats.append(parts[0])
    return pats


def is_excluded(rel, patterns):
    return any(_match(rel, p) for p in patterns)


def wanted(rel, dirs, patterns):
    if rel.split("/", 1)[0] not in dirs or not rel.endswith(EXTS):
        return False
    parts = rel.split("/")[:-1]
    if any(p.startswith("build") for p in parts):
        return False
    return not is_excluded(rel, patterns)


# ---------------------------------------------------------------- core scan
class Result:
    def __init__(self, n):
        self.n = n
        self.total = 0
        self.per_dir = {}
        self.per_file = {}
        self.groups = []          # [(score, run_lines, [(rel, first_line, last_line), ...])]
        self.files = 0
        self.lines = 0


def scan(files, n=DEFAULT_N, with_groups=True):
    """files: {rel: bytes|str}. Deterministic: files are visited in sorted order."""
    rels = sorted(files)
    ids_of = {}
    triv_ids = set()
    ids, nos = [], []
    nontriv = []
    for rel in rels:
        data = files[rel]
        text = data if isinstance(data, str) else data.decode("latin-1")
        rows = normalize(text)
        fi = []
        for _no, line, triv in rows:
            i = ids_of.get(line)
            if i is None:
                i = ids_of[line] = len(ids_of)
                if triv:
                    triv_ids.add(i)
            fi.append(i)
        ids.append(fi)
        nos.append([r[0] for r in rows])
        nontriv.append([0 if r[2] else 1 for r in rows])
    need = (n + 1) // 2

    # pass 1: hash every qualifying window, count occurrences
    wins = []
    counter = Counter()
    for fi, nt in zip(ids, nontriv):
        m = len(fi) - n + 1
        if m <= 0:
            wins.append([])
            continue
        hs = [hash(t) for t in zip(*[fi[k:k + m] for k in range(n)])]
        ps = [0] + list(accumulate(nt))
        for s in range(m):
            if ps[s + n] - ps[s] < need:
                hs[s] = -1
        wins.append(hs)
        counter.update(h for h in hs if h != -1)
    dupset = {h for h, c in counter.items() if c > 1}

    # pass 2: exact-compare the candidates; drop self-overlapping and low-variety windows
    cand = defaultdict(list)
    for f, hs in enumerate(wins):
        for s, h in enumerate(hs):
            if h in dupset:
                cand[(h, tuple(ids[f][s:s + n]))].append((f, s))
    wbucket = [dict() for _ in rels]
    bucket_occ = []
    for key in sorted(cand, key=lambda k: cand[k][0]):
        if len(set(key[1]) - triv_ids) < need:
            continue
        kept = []
        for f, s in cand[key]:                    # already (file, start) ordered
            if kept and kept[-1][0] == f and s - kept[-1][1] < n:
                continue
            kept.append((f, s))
        if len(kept) < 2:
            continue
        b = len(bucket_occ)
        bucket_occ.append(kept)
        for f, s in kept:
            wbucket[f][s] = b

    res = Result(n)
    res.files = len(rels)
    res.lines = sum(len(x) for x in ids)
    for f, rel in enumerate(rels):
        wb = wbucket[f]
        if not wb:
            continue
        cov = bytearray(len(ids[f]))
        for s in wb:
            cov[s:s + n] = b"\1" * n
        c = sum(cov)
        res.per_file[rel] = c
        d = rel.split("/", 1)[0]
        res.per_dir[d] = res.per_dir.get(d, 0) + c
        res.total += c

    if with_groups:
        claimed = set()
        for f, rel in enumerate(rels):
            for s in sorted(wbucket[f]):
                if (f, s) in claimed:
                    continue
                b = wbucket[f][s]
                copies = []
                for f2, s2 in bucket_occ[b]:
                    k = 1
                    while True:
                        b1 = wbucket[f].get(s + k)
                        if b1 is None or b1 != wbucket[f2].get(s2 + k) or (f2 == f and s2 == s):
                            break
                        k += 1
                    copies.append((f2, s2, k))
                kmax = max(k for _f, _s, k in copies)
                spans = []
                for f2, s2, k in copies:
                    kk = kmax if (f2, s2) == (f, s) else k
                    for j in range(kk):
                        claimed.add((f2, s2 + j))
                    spans.append((rels[f2], nos[f2][s2], nos[f2][s2 + kk + n - 2], kk + n - 1))
                spans.sort()
                res.groups.append((sum(sp[3] for sp in spans), kmax + n - 1,
                                   [(r, a, z) for r, a, z, _l in spans]))
        res.groups.sort(key=lambda g: (-g[0], g[2][0]))
    return res


# ---------------------------------------------------------------- git / fs sources
def _git(root, args, inp=None):
    return subprocess.run(["git"] + args, cwd=root, input=inp, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout


def _git_try(root, args):
    try:
        return _git(root, args)
    except subprocess.CalledProcessError:
        return None


def _blob_text(root, spec):
    raw = _git_try(root, ["show", spec])
    return raw.decode("utf-8", "replace") if raw is not None else ""


def load_sources(root, dirs, source, rev):
    """-> ({rel: bytes}, patterns). source: head | staged | worktree."""
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
        return out, patterns
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
    return out, patterns


def head_sha(root, rev):
    raw = _git_try(root, ["rev-parse", "--short=9", rev])
    return raw.decode().strip() if raw else "unknown"


# ---------------------------------------------------------------- baseline
def make_baseline(res, dirs, root, rev):
    counts = {d: res.per_dir.get(d, 0) for d in dirs}
    return {"version": 1, "head": head_sha(root, rev), "n": res.n, "dirs": list(dirs),
            "total": res.total, "counts": counts,
            "files": {k: v for k, v in sorted(res.per_file.items())}}


def write_json(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(obj, fh, indent=1, sort_keys=True)
        fh.write("\n")


def check(res, base, out=sys.stdout, samples=10):
    """Fail when the repo total or any baseline directory count rose."""
    bad = []
    if res.total > base["total"]:
        bad.append(("TOTAL", base["total"], res.total))
    for d in sorted(set(base["counts"]) | set(res.per_dir)):
        b, c = base["counts"].get(d, 0), res.per_dir.get(d, 0)
        if c > b:
            bad.append((d, b, c))
    if not bad:
        print("dup_lines_ratchet: OK (total %d <= baseline %d; %d fewer duplicated lines)"
              % (res.total, base["total"], base["total"] - res.total), file=out)
        return 0
    print("dup_lines_ratchet: FAIL - duplicated-line count rose vs baseline (%s, N=%d):"
          % (base.get("head", "?"), base.get("n", res.n)), file=out)
    for d, b, c in bad:
        print("  %-14s %6d -> %6d (+%d)" % (d, b, c, c - b), file=out)
    bf = base.get("files", {})
    rose = sorted(((c - bf.get(r, 0), r) for r, c in res.per_file.items() if c > bf.get(r, 0)), reverse=True)
    if rose:
        print("  files whose duplicated-line count rose:", file=out)
        for dl, r in rose[:samples]:
            print("    +%-5d %s" % (dl, r), file=out)
        rose_set = {r for _d, r in rose}
        shown = 0
        for score, run, spans in res.groups:
            if shown >= 5:
                break
            if any(s[0] in rose_set for s in spans):
                print("  copy-paste (%d copies x up to %d lines): %s" % (
                    len(spans), run, ", ".join("%s:%d-%d" % s for s in spans[:4])), file=out)
                shown += 1
    print("Share the code (one helper/header) instead of copying it, or pay the count down elsewhere.",
          file=out)
    return 1


# ---------------------------------------------------------------- reporting
def print_report(res, top, out=sys.stdout, label="", copies=6, only=None):
    print("dup_lines_ratchet report%s (N=%d, %d files, %d normalized lines)"
          % (label, res.n, res.files, res.lines), file=out)
    print("  total duplicated lines: %d" % res.total, file=out)
    print("  %-14s %8s" % ("dir", "dup lines"), file=out)
    for d, c in sorted(res.per_dir.items(), key=lambda kv: (-kv[1], kv[0])):
        print("  %-14s %8d" % (d, c), file=out)
    groups = [g for g in res.groups if not only or any(sp[0].startswith(only) for sp in g[2])]
    print("\ntop %d duplicate groups%s (copies x run lines; score = total lines in all copies):"
          % (min(top, len(groups)), " touching %s" % only if only else ""), file=out)
    for i, (score, run, spans) in enumerate(groups[:top], 1):
        print("%3d. score=%d  %d copies  longest run=%d" % (i, score, len(spans), run), file=out)
        for r, a, z in spans[:copies]:
            print("       %s:%d-%d (%d)" % (r, a, z, z - a + 1), file=out)
        if len(spans) > copies:
            print("       ... +%d more" % (len(spans) - copies), file=out)


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
    ap.add_argument("--min-run", type=int, default=None, help="run length N (default: baseline's, else %d)" % DEFAULT_N)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--only", default=None, metavar="PREFIX", help="report: only groups with a copy under this path prefix")
    ap.add_argument("--copies", type=int, default=6, help="report: copies listed per group")
    ap.add_argument("--dirs", nargs="*", default=None)
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--repo", default=DEFAULT_ROOT, help="repo root (tests)")
    a = ap.parse_args(argv)
    source = "worktree" if a.worktree else "staged" if a.staged else "head"
    dirs, n = a.dirs or DEFAULT_DIRS, a.min_run or DEFAULT_N
    base = None
    if a.check:
        if not os.path.exists(a.baseline):
            print("dup_lines_ratchet: no baseline at %s (run --write-baseline)" % a.baseline, file=sys.stderr)
            return 2
        with open(a.baseline, "r", encoding="utf-8") as fh:
            base = json.load(fh)
        if not a.dirs:
            dirs = base.get("dirs", dirs)
        if not a.min_run:
            n = base.get("n", n)
        elif a.min_run != base.get("n"):
            print("dup_lines_ratchet: --min-run %d differs from baseline N=%s; re-baseline instead"
                  % (a.min_run, base.get("n")), file=sys.stderr)
            return 2
    t0 = time.time()
    files, _pats = load_sources(a.repo, dirs, source, a.rev)
    res = scan(files, n, with_groups=not a.write_baseline)
    if a.check:
        rc = check(res, base)
        improved = res.total < base["total"] or any(
            res.per_dir.get(d, 0) < c for d, c in base["counts"].items())
        if rc == 0 and a.tighten and improved:
            write_json(a.baseline, make_baseline(res, dirs, a.repo, a.rev))
            print("dup_lines_ratchet: baseline tightened")
        return rc
    if a.write_baseline:
        nb = make_baseline(res, dirs, a.repo, a.rev)
        write_json(a.baseline, nb)
        print("dup_lines_ratchet: baseline written: %s (head %s, N=%d, total %d, %d files)"
              % (a.baseline, nb["head"], n, res.total, res.files))
        return 0
    print_report(res, a.top, copies=a.copies, only=a.only, label=" [%s %s, %.1fs]" % (source, a.rev if source == "head" else "", time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
