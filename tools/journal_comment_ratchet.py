#!/usr/bin/env python3
"""journal_comment_ratchet - stops NEW plan/process-provenance comments; legacy ones are baselined.

A journal comment cites where a change came from (slice/ticket tag, phase, revision, review
round, plan doc) instead of saying what the code does or why. Legacy ones are cleaned in
bulk later; this ratchet only keeps the repo-wide total from rising (moves between files pass).

  py -3 tools/journal_comment_ratchet.py                   # report (per-dir, top files)
  py -3 tools/journal_comment_ratchet.py --write-baseline  # (re)write journal_comment_ratchet.baseline.json
  py -3 tools/journal_comment_ratchet.py --check           # exit 1 if the total rose
  py -3 tools/journal_comment_ratchet.py --check --tighten # on success, lower the baseline
  py -3 tools/journal_comment_ratchet.py --matches 30      # sample matched comment lines

Scans comments only (C/C++/GLSL line and block comments, Python '#'), counting comment LINES
with at least one match. Source: the COMMITTED tree (--rev, default HEAD); --worktree scans
the dirty tree. Skips 3rdparty/, shaders/spv/, build*/, *.gen.*, linguist-generated paths.
Exit: 0 ok, 1 a count rose, 2 usage/baseline error.  Stdlib only.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys
import tokenize

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import file_size_ratchet as fsr  # noqa: E402  (git helpers + exclusion globs)

DEFAULT_ROOT = os.path.abspath(os.path.join(HERE, ".."))
BASELINE = os.path.join(HERE, "journal_comment_ratchet.baseline.json")
DIRS = ["GameOS", "mclib", "code", "RenderCore", "GuiRuntime", "Gates", "RenderWorld",
        "editor", "shaders", "scripts", "tools", "tests"]
C_EXTS = (".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".glsl", ".hglsl", ".vert", ".frag",
          ".comp", ".tesc", ".tese", ".geom")
EXCLUDES = fsr.BUILTIN_EXCLUDES + ["build*/", "*/build*/", "*/node_modules/"]

# Two or more ALL-CAPS words then a number, or a capital+digits then a lowercase+digit suffix.
# UTF-8, SSE-2, IEEE-754, CVE-2021-1234 and C++23 fit neither shape.
_TAG = r"\b[A-Z][A-Z0-9]+(?:-[A-Z][A-Z0-9]+)+-\d+[a-z]?\b|\b[A-Z]\d{1,2}-[a-z]\d+[a-z]?\b"
JOURNAL_RE = re.compile(
    "|".join([_TAG,
              r"\bPhase [0-9]+\b", r"\bSlice [A-Z0-9]{1,3}\b", r"\brev ?[0-9]{1,2}\b",
              r"\bStep [0-9]+\b", r"\bAstra correction [0-9]+\b", r"\breview correction\b",
              r"\b[\w-]*plan\.md\b", r"docs/superpowers/plans/"]))

MSG = ("journal_comment_ratchet: FAIL - new plan/slice/review provenance in comments.\n"
       "Describe what/why instead of the plan/slice tag.")


def _c_comments(text):
    """Yield (lineno, comment_text) for C-family comments; skips string/char literals."""
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if c == "\n":
            line += 1
            i += 1
        elif two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            yield line, text[i + 2:j]
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j
            for k, part in enumerate(text[i + 2:j].split("\n")):
                yield line + k, part
            line += text.count("\n", i, j)
            i = j + 2
        elif c == '"' or c == "'":
            i += 1
            while i < n and text[i] != c and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            i += 1
        else:
            i += 1


def _py_comments(text):
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.COMMENT:
                yield tok.start[0], tok.string[1:]
    except (tokenize.TokenError, IndentationError, SyntaxError):
        pass  # a malformed file keeps the comments seen so far


def matches(rel, text):
    """-> [(lineno, comment_line)] of journal comments in one file (one entry per line)."""
    gen = _py_comments(text) if rel.endswith(".py") else _c_comments(text)
    seen, out = set(), []
    for ln, body in gen:
        if ln not in seen and JOURNAL_RE.search(body):
            seen.add(ln)
            out.append((ln, body.strip()))
    return out


def wanted(rel, patterns):
    if not rel.endswith(C_EXTS + (".py",)):
        return False
    if any(p.startswith("build") for p in rel.split("/")[:-1]):
        return False
    return not any(fsr._match(rel, p) for p in patterns)


def load(root, source, rev):
    """-> {rel: text} for owned comment-bearing files."""
    if source == "head":
        attrs = fsr._blob_text(root, rev + ":.gitattributes")
    else:
        with open(os.path.join(root, ".gitattributes"), encoding="utf-8", errors="replace") as fh:
            attrs = fh.read()
    pats = EXCLUDES + fsr.gitattr_generated(attrs)
    if source == "worktree":
        names = fsr._git(root, ["ls-files", "-z", "--cached", "--others", "--exclude-standard", "--"] + DIRS)
    else:
        names = fsr._git(root, ["ls-tree", "-r", "-z", "--name-only", rev, "--"] + DIRS)
    rels = sorted(x.decode("utf-8", "replace") for x in names.split(b"\0") if x)
    rels = [r for r in rels if wanted(r, pats)]
    out = {}
    if source == "worktree":
        for r in rels:
            try:
                with open(os.path.join(root, r), "rb") as fh:
                    out[r] = fh.read().decode("utf-8", "replace")
            except OSError:
                pass
        return out
    raw = fsr._git(root, ["cat-file", "--batch"], "".join(rev + ":" + r + "\n" for r in rels).encode("utf-8"))
    pos = 0
    for r in rels:
        eol = raw.index(b"\n", pos)
        hdr = raw[pos:eol].split()
        pos = eol + 1
        if len(hdr) == 3 and hdr[1] == b"blob":
            size = int(hdr[2])
            out[r] = raw[pos:pos + size].decode("utf-8", "replace")
            pos += size + 1
    return out


def count(files):
    """-> {rel: n} for files with n > 0."""
    res = {}
    for rel, text in files.items():
        n = len(matches(rel, text))
        if n:
            res[rel] = n
    return res


def per_dir(counts):
    d = {}
    for rel, n in counts.items():
        top = rel.split("/", 1)[0]
        d[top] = d.get(top, 0) + n
    return d


def check(counts, base, out=None):
    """Fail only when the repo total rises; per-file deltas are the diagnostic (moves between files pass)."""
    out = out or sys.stdout
    bf = base.get("files", {})
    total, btotal = sum(counts.values()), sum(bf.values())
    rose = sorted(((n - bf.get(r, 0), r, bf.get(r, 0), n) for r, n in counts.items() if n > bf.get(r, 0)), reverse=True)
    fell = sum(b - counts.get(r, 0) for r, b in bf.items() if b > counts.get(r, 0))
    if total <= btotal:
        print("journal_comment_ratchet: OK (%d journal comment lines, baseline %d)" % (total, btotal), file=out)
        return 0
    print(MSG, file=out)
    print("  total %d -> %d (+%d)" % (btotal, total, total - btotal), file=out)
    for dl, r, b, c in rose:
        print("  +%-3d %s (%d -> %d)" % (dl, r, b, c), file=out)
    print("  (%d lines removed elsewhere)" % fell, file=out)
    print("Matched: slice/ticket tags, Phase N, Slice X, revN, Step N, review/Astra correction, "
          "plan docs. Run --matches to list lines.", file=out)
    return 1


def make_baseline(counts):
    return {"version": 1, "total": sum(counts.values()), "per_dir": dict(sorted(per_dir(counts).items())),
            "files": dict(sorted(counts.items()))}


def write_json(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(obj, fh, indent=1, sort_keys=True)
        fh.write("\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--write-baseline", action="store_true")
    g.add_argument("--check", action="store_true")
    ap.add_argument("--tighten", action="store_true")
    ap.add_argument("--matches", type=int, default=0, metavar="N", help="print ~N evenly spaced matches")
    ap.add_argument("--rev", default="HEAD")
    ap.add_argument("--worktree", action="store_true", help="scan the dirty working tree")
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--repo", default=DEFAULT_ROOT)
    a = ap.parse_args(argv)
    files = load(a.repo, "worktree" if a.worktree else "head", a.rev)
    counts = count(files)
    if a.matches:
        allm = [(r, ln, b) for r in sorted(counts) for ln, b in matches(r, files[r])]
        step = max(1, len(allm) // a.matches)
        for r, ln, b in allm[::step][:a.matches]:
            print("%s:%d: %s" % (r, ln, b[:160]))
        return 0
    if a.check:
        if not os.path.exists(a.baseline):
            print("journal_comment_ratchet: no baseline at %s (run --write-baseline)" % a.baseline, file=sys.stderr)
            return 2
        with open(a.baseline, encoding="utf-8") as fh:
            base = json.load(fh)
        rc = check(counts, base)
        if rc == 0 and a.tighten and sum(counts.values()) < base.get("total", 0):
            write_json(a.baseline, make_baseline(counts))
            print("journal_comment_ratchet: baseline tightened")
        return rc
    if a.write_baseline:
        write_json(a.baseline, make_baseline(counts))
        print("journal_comment_ratchet: baseline written (%d lines in %d files)" % (sum(counts.values()), len(counts)))
        return 0
    print("journal_comment_ratchet report (%d lines in %d files)" % (sum(counts.values()), len(counts)))
    for d, n in sorted(per_dir(counts).items(), key=lambda kv: -kv[1]):
        print("  %-12s %6d" % (d, n))
    for r, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:20]:
        print("  %5d  %s" % (n, r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
