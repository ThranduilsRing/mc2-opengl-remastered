#!/usr/bin/env python3
"""cite_guard - fail-closed guard for a cheap model's doc cite repair and doc triage.

The giant source files were split, so docs now cite `path/file.cpp:1234` at the wrong file or line. A cheap model may repair
cites and stamp a status header, nothing else; this tool finds what is broken, proposes the exact replacement, and proves the
model's edit touched only cites.

  py -3 tools/cite_guard.py --check [paths]                  verify every cite against HEAD (default: every doc)
  py -3 tools/cite_guard.py --triage-report [paths]          one line per doc: cites, broken, stale, age, header, newer twin
  py -3 tools/cite_guard.py --fix-suggest DOC                per broken cite, a replacement the model applies verbatim
  py -3 tools/cite_guard.py --diff-guard --base REV DOC...   worktree vs REV: only cite spans and the status header may differ
  py -3 tools/cite_guard.py --selftest

Cite grammar (any .md/.txt/.rst doc; paths repo-relative, `/` or `\\`, bare basenames, `A:/Games/mc2-main/...` absolute):
  path:12  path:12-20  path:12,30  path#L12  path#L12-L20  path (line 12)  path:12 (symbol)  path:12:symbol  path::symbol
  and a bare path of a source file (.cpp .h .hpp .inl .py .sh .glsl .hglsl .vert .frag .comp .cmake .json).
A cite is checked against the tree at the base rev (HEAD): the path exists, the line exists, and a cited symbol (explicit, or
the nearest backticked identifier of the same sentence) sits within +-3 lines. A missing path is classified SPLIT / DELETED /
RENAMED / UNKNOWN; a wrong line or symbol is STALE-LINE with the current location. A line cite into a split file that names no
symbol is compared by content with the file as it was before the split (the split moved whole units verbatim).

Exempt (INFO, never a finding): cites in a fenced block tagged text/log/console/output or holding `[X v1]`, PASS, FAIL or a
sha; any line holding `(quoted history)`; the block after an `<!-- history -->` line. The first line may be a status header
`<!-- doc-status: current|stale|superseded-by:<path> | reviewed:<YYYY-MM-DD> -->`.

Exit: 0 ok, 1 finding, 2 usage / environment error.  Stdlib only.
"""
from __future__ import annotations

import argparse
import atexit
import datetime
import json
import os
import posixpath
import re
import subprocess
import sys
import tempfile

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))

BARE_EXTS = {"cpp", "h", "hpp", "inl", "py", "sh", "glsl", "hglsl", "vert", "frag", "comp", "cmake", "json", "c", "cc"}
LINE_EXTS = BARE_EXTS | {"md", "txt", "rst", "yaml", "yml", "cfg", "ini", "fit", "ps1", "bat", "js", "ts", "html", "css",
                         "csv", "xml", "tga", "hxx", "cxx", "def", "rc"}
DOC_EXTS = (".md", ".txt", ".rst")
DOC_DIRS = ("docs", ".claude", "tests", "tools", "scripts")
# Data lists the checkers read, not prose: scanning them would only mislead the model.
DATA_TXT = re.compile(r"(allowlist|baseline|inventory|requirements|debt|^CMakeLists|^build_inputs)", re.I)
ABS_PREFIXES = ("a:/games/mc2-main/", "a:/games/mc2-opengl-src/")
HISTORY_INFOS = {"text", "log", "console", "output", "txt", "history"}
FINDING_CLASSES = ("SPLIT", "DELETED", "RENAMED", "UNKNOWN", "STALE-LINE", "MISSING-SYMBOL")
WINDOW = 3
STOP_WORDS = {"true", "false", "void", "null", "nullptr", "this", "self", "none", "bool", "int", "char", "float", "double",
              "const", "static", "struct", "class", "enum", "return", "else", "default", "size", "type", "name", "data",
              "static_assert", "sizeof", "define", "include", "ifdef", "ifndef", "pragma", "typedef", "using", "namespace"}

_SYM = r"[A-Za-z_~][\w:~]{2,}(?:\(\))?"
CITE_RE = re.compile(r"""
    (?<![\w/\\.~@+$%<>{}*=-])
    (?P<path>(?:[A-Za-z]:[\\/])?(?:[\w.+~@-]+[\\/])*[\w.+~@-]+\.(?P<ext>[A-Za-z][A-Za-z0-9]{0,7}))(?![\w])
    (?:
        (?P<hash>\#L\d+(?:-L?\d+)?)
      | (?P<colon>:\d+(?:-\d+)?(?:,\d+(?:-\d+)?)*)(?P<tail1>`?\s*\((?P<s1>[A-Za-z_~][\w:~]{2,}(?:\(\))?)\)|:(?P<s2>[A-Za-z_]\w*(?:::\w+)*))?
      | (?P<dcolon>::(?P<s3>[A-Za-z_~][\w:~]*))
      | (?P<par>\s*\((?:lines?|ln|L)\s*\d+(?:\s*-\s*\d+)?\))
    )?
""", re.X)
HEADER_RE = re.compile(r"^<!-- doc-status: (?:current|stale|superseded-by:(?P<sup>[^\s|]+)) \| reviewed:(?P<date>\d{4}-\d{2}-\d{2}) -->$")
HEADER_PREFIX = "<!-- doc-status:"
IDENT_RE = re.compile(r"^~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*(?:\([^`]*\))?$")
SHA_RE = re.compile(r"\b(?=[0-9a-f]*\d)(?=[0-9a-f]*[a-f])[0-9a-f]{7,40}\b")
EVENT_RE = re.compile(r"\[[A-Z0-9_]+ v\d+\]")
OUTPUT_RE = re.compile(r"\b(?:PASS|FAIL)\b")
SENT_RE = re.compile(r"(?<=[.!?])\s+(?=[A-Z`\[(])|\|")
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})\s*([\w+.-]*)")


# ------------------------------------------------------------------ text normalisation (same convention as comment_guard)
def prep(data):
    """bytes -> (text, bom, eol style). Latin-1 keeps every byte distinct so identity checks stay honest."""
    bom = data.startswith(b"\xef\xbb\xbf")
    if bom:
        data = data[3:]
    text = data.decode("latin-1")
    crlf = text.count("\r\n")
    lf = text.count("\n") - crlf
    style = "none" if not (crlf or lf) else "crlf" if not lf else "lf" if not crlf else "mixed"
    return text.replace("\r\n", "\n"), bom, style


def split_lines(text):
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def out(s=""):
    enc = getattr(sys.stdout, "encoding", None) or "ascii"
    sys.stdout.write(s.encode(enc, "replace").decode(enc, "replace") + "\n")


# ------------------------------------------------------------------ git
class Git:
    def __init__(self, root):
        self.root = root
        self._batch = None

    def run(self, args):
        r = subprocess.run(["git", "-c", "core.quotepath=false"] + list(args), cwd=self.root, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, **console_child.child_kwargs(redirected=True))
        return r.returncode, r.stdout

    def blob(self, spec):
        """Bytes of `rev:path` through one long-lived cat-file process, or None when the object does not exist."""
        if self._batch is None:
            self._batch = subprocess.Popen(["git", "cat-file", "--batch"], cwd=self.root, stdin=subprocess.PIPE,
                                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                           **console_child.child_kwargs(redirected=True))
            atexit.register(self.close)
        p = self._batch
        try:
            p.stdin.write(spec.encode("utf-8", "replace") + b"\n")
            p.stdin.flush()
            head = p.stdout.readline().split()
            if len(head) != 3 or head[1] != b"blob":
                return None
            size = int(head[2])
            data = p.stdout.read(size)
            p.stdout.read(1)
            return data
        except (OSError, ValueError):
            return None

    def close(self):
        p, self._batch = self._batch, None
        if p is not None:
            try:
                p.stdin.close()
                p.wait(timeout=5)
            except Exception:
                pass


# ------------------------------------------------------------------ cites
class Cite:
    """One cite span inside one doc line. text == pathtxt + spec + tail, always, so a replacement can be rebuilt part by part."""
    __slots__ = ("line_no", "col", "start", "end", "text", "pathtxt", "spec", "tail", "ranges", "sym", "kind",
                 "implicit", "line_text")

    def numbers(self):
        return [n for a, b in self.ranges for n in (a, b)]


def _ranges(spec):
    out_ = []
    for m in re.finditer(r"(\d+)(?:\s*-\s*L?(\d+))?", spec):
        a = int(m.group(1))
        out_.append((a, int(m.group(2)) if m.group(2) else a))
    return out_


def norm_path(raw):
    """Repo-relative forward-slash path, or None for a path outside the repo (other drives, installs, build dirs)."""
    p = raw.replace("\\", "/")
    if re.match(r"^[A-Za-z]:/", p):
        low = p.lower()
        for pref in ABS_PREFIXES:
            if low.startswith(pref):
                p = p[len(pref):]
                break
        else:
            return None
    p = re.sub(r"(?i)^\.claude/worktrees/[^/]+/", "", p)
    while p.startswith("./"):
        p = p[2:]
    for comp in p.split("/"):
        lc = comp.lower()
        if lc.startswith("build64") or lc == "node_modules" or lc.startswith("mc2-win64"):
            return None
    return p


def is_weak(sym):
    """A plain lowercase word in parens may be prose ("(old)"); it only counts as a symbol when the cited file contains it."""
    return re.fullmatch(r"[a-z0-9]+", sym.replace("()", "")) is not None


def sym_name(sym):
    """The last `::` component without call parens: what is searched for."""
    return sym.replace("()", "").rstrip(":").split("::")[-1]


def has_symbol(text_lines, name):
    rx = re.compile(r"(?<![\w])" + re.escape(name) + r"(?![\w])")
    return any(rx.search(ln) for ln in text_lines)


def _implicit_symbol(line_text, c, spans):
    cuts = [0] + [m.end() if m.group(0) != "|" else m.start() for m in SENT_RE.finditer(line_text)] + [len(line_text)]
    lo = max(x for x in cuts if x <= c.start)
    hi = min(x for x in cuts if x >= c.end)
    best = None
    for m in re.finditer(r"`([^`\n]+)`", line_text):
        if m.start() < lo or m.end() > hi or any(s < m.end() and m.start() < e for s, e in spans):
            continue
        tok = m.group(1).strip()
        if not IDENT_RE.match(tok):
            continue
        name = sym_name(tok.split("(")[0])
        if len(name) < 4 or name.lower() in STOP_WORDS or re.match(r"^MC2_[A-Z0-9_]+$", name):
            continue
        gap = c.start - m.end() if m.end() <= c.start else m.start() - c.end
        if best is None or gap < best[0]:
            best = (gap, name)
    return best[1] if best else None


def parse_line(line_text, line_no):
    cites, spans = [], []
    for m in CITE_RE.finditer(line_text):
        ext = m.group("ext").lower()
        has_spec = bool(m.group("hash") or m.group("colon") or m.group("par") or m.group("dcolon"))
        if ext not in (LINE_EXTS if has_spec else BARE_EXTS):
            continue
        c = Cite()
        c.line_no, c.col, c.start, c.end, c.text = line_no, m.start() + 1, m.start(), m.end(), m.group(0)
        c.pathtxt = m.group("path")
        rest = c.text[len(c.pathtxt):]
        c.sym = m.group("s1") or m.group("s2") or m.group("s3")
        if m.group("colon"):
            c.spec, c.tail = m.group("colon"), rest[len(m.group("colon")):]
        elif m.group("hash"):
            c.spec, c.tail = m.group("hash"), ""
        elif m.group("par"):
            c.spec, c.tail = m.group("par"), ""
        else:
            c.spec, c.tail = "", rest
        c.ranges = _ranges(c.spec)
        c.kind = "line" if c.ranges else ("sym" if m.group("dcolon") else "path")
        c.implicit, c.line_text = None, line_text
        cites.append(c)
        spans.append((c.start, c.end))
    for c in cites:
        if c.sym is None:
            c.implicit = _implicit_symbol(line_text, c, spans)
    return cites


def exempt_lines(lines):
    """line_no -> reason for lines whose cites are quoted history."""
    ex = {}
    blocks, i = [], 0
    while i < len(lines):
        m = FENCE_RE.match(lines[i])
        if m:
            fence, info = m.group(1), m.group(2).lower()
            j = i + 1
            while j < len(lines) and not (lines[j].lstrip().startswith(fence[0] * len(fence))):
                j += 1
            blocks.append((i, j, info))
            i = j + 1
        else:
            i += 1
    for a, b, info in blocks:
        body = "\n".join(lines[a + 1:b])
        why = None
        if info in HISTORY_INFOS:
            why = "fence:" + info
        elif EVENT_RE.search(body) or OUTPUT_RE.search(body) or SHA_RE.search(body):
            why = "quoted-output"
        if why:
            for n in range(a + 1, b + 2):
                ex[n] = why
    for idx, ln in enumerate(lines):
        n = idx + 1
        if "(quoted history)" in ln:
            ex[n] = "quoted-history-marker"
        if ln.strip() == "<!-- history -->":
            k = idx + 1
            while k < len(lines) and not lines[k].strip():
                k += 1
            if k < len(lines):
                if FENCE_RE.match(lines[k]):
                    for a, b, _info in blocks:
                        if a == k:
                            for q in range(a + 1, b + 2):
                                ex[q] = "history-marker"
                else:
                    while k < len(lines) and lines[k].strip():
                        ex[k + 1] = "history-marker"
                        k += 1
    return ex


class Doc:
    __slots__ = ("rel", "lines", "header", "cites", "exempt")


def parse_doc(rel, text):
    d = Doc()
    d.rel, d.lines = rel, split_lines(text)
    d.header = d.lines[0] if d.lines and d.lines[0].startswith(HEADER_PREFIX) else None
    d.exempt = exempt_lines(d.lines)
    d.cites = []
    for i, ln in enumerate(d.lines):
        if i == 0 and d.header is not None:
            continue
        d.cites.extend(parse_line(ln, i + 1))
    return d


def parse_cite_text(s):
    """The cite when the whole string is exactly one cite, else None."""
    cs = parse_line(s, 1)
    return cs[0] if len(cs) == 1 and cs[0].text == s else None


# ------------------------------------------------------------------ repo knowledge
class Unit:
    def __init__(self, src):
        self.src, self.stems, self.members, self.base, self.split_commit = src, set(), set(), None, None


class Result:
    __slots__ = ("status", "detail", "sugg", "path")

    def __init__(self, status, detail="", sugg=None, path=None):
        self.status, self.detail, self.sugg, self.path = status, detail, sugg, path


class Repo:
    def __init__(self, root, rev="HEAD"):
        self.root, self.rev = os.path.abspath(root), rev
        self.git = Git(self.root)
        rc, outb = self.git.run(["ls-tree", "-r", "-z", "--name-only", rev])
        if rc != 0:
            raise RuntimeError("cannot read tree %s in %s" % (rev, self.root))
        self.tree = {p.decode("utf-8", "replace") for p in outb.split(b"\0") if p}
        self.by_base, self.by_lower = {}, {}
        for p in self.tree:
            self.by_base.setdefault(posixpath.basename(p), []).append(p)
            self.by_lower.setdefault(p.lower(), []).append(p)
        self._lines, self._hist, self._suffix = {}, None, {}
        self._sym_cache, self._win_index, self._grep_cache = {}, {}, {}
        self.units, self.unit_of = {}, {}
        self._load_units()

    # -- blobs
    def blob_lines(self, path, rev=None):
        key = (rev or self.rev, path)
        if key not in self._lines:
            data = self.git.blob("%s:%s" % key)
            self._lines[key] = None if data is None else split_lines(prep(data)[0])
        return self._lines[key]

    # -- split units
    def _load_units(self):
        for p in sorted(x for x in self.tree if x.startswith("tools/split/allow/") and x.endswith(".json")):
            try:
                d = json.loads(prep(self.git.blob("%s:%s" % (self.rev, p)) or b"{}")[0])
            except ValueError:
                continue
            src, stem = d.get("src"), d.get("stem")
            if not src or not stem:
                continue
            u = self.units.setdefault(src, Unit(src))
            u.stems.add(stem)
            for k in ("base", "split_commit"):
                if d.get(k) and d[k] != "PENDING":
                    setattr(u, k, d[k])
            dirn = posixpath.dirname(src)
            for t in self.tree:
                b = posixpath.basename(t)
                if posixpath.dirname(t) == dirn and b.startswith(stem + "_") and b.rsplit(".", 1)[-1] in ("cpp", "h", "hpp", "inl"):
                    u.members.add(t)
            if src in self.tree:
                u.members.add(src)
        sm = self.blob_lines("docs/split-map.md") or []
        cur = None
        for ln in sm:
            m = re.match(r"^## `([^`]+)`", ln)
            if m:
                cur = self.units.get(m.group(1))
                continue
            m = re.match(r"^\| `([^`]+)` \|", ln)
            if m and cur is not None:
                cand = posixpath.join(posixpath.dirname(cur.src), m.group(1))
                if cand in self.tree:
                    cur.members.add(cand)
        for u in self.units.values():
            for t in u.members | {u.src}:
                self.unit_of[t] = u

    def unit_files(self, path):
        u = self.unit_of.get(path)
        return sorted(u.members, key=lambda x: (x != u.src, x)) if u else [path]

    # -- history of removed paths
    def history(self):
        if self._hist is None:
            self._hist = {}
            rc, outb = self.git.run(["log", "--diff-filter=DR", "-M", "--name-status", "--format=%x01%h%x09%cs", self.rev])
            cur = None
            for ln in outb.decode("utf-8", "replace").splitlines():
                if ln.startswith("\x01"):
                    cur = ln[1:].split("\t")
                elif ln and cur and len(cur) == 2:
                    f = ln.split("\t")
                    if f[0] == "D" and len(f) == 2:
                        self._hist.setdefault(f[1], ("DELETED", cur[0], cur[1], None))
                    elif f[0].startswith("R") and len(f) == 3:
                        self._hist.setdefault(f[1], ("RENAMED", cur[0], cur[1], f[2]))
        return self._hist

    def removed(self, path):
        """(kind, sha, date, new path) for a path that left the tree; renames are followed to the surviving file."""
        h, seen = self.history().get(path), 0
        while h and h[0] == "RENAMED" and h[3] not in self.tree and seen < 8:
            nxt = self.history().get(h[3])
            if not nxt:
                return ("DELETED", h[1], h[2], None)
            h, seen = nxt, seen + 1
        return h

    # -- path resolution
    def resolve(self, raw, doc_rel):
        """('ok'|'missing'|'skip'|'ambiguous', path)."""
        p = norm_path(raw)
        if p is None:
            return ("skip", raw)
        if p in self.tree:
            return ("ok", p)
        rel = posixpath.normpath(posixpath.join(posixpath.dirname(doc_rel), p))
        if rel in self.tree:
            return ("ok", rel)
        if p.startswith("../"):
            if rel.startswith(".."):
                return ("skip", raw)
            p = rel
        low = self.by_lower.get(p.lower(), [])
        if len(low) == 1:
            return ("ok", low[0])
        if "/" not in p:
            c = self.by_base.get(p, [])
            if len(c) == 1:
                return ("ok", c[0])
            if len(c) > 1:
                return ("ambiguous", p)
        else:
            if p not in self._suffix:
                self._suffix[p] = [t for t in self.tree if t.endswith("/" + p) or t.lower().endswith("/" + p.lower())]
            if len(self._suffix[p]) == 1:
                return ("ok", self._suffix[p][0])
        return ("missing", p)

    # -- symbol search
    def find_symbol(self, files, name, prefer_path=None, prefer_line=0):
        key = (tuple(files), name)
        if key not in self._sym_cache:
            rx = re.compile(r"(?<![\w])" + re.escape(name) + r"(?![\w])")
            dfn = re.compile(r"^[A-Za-z_~].*(?<![\w])" + re.escape(name) + r"(?![\w])\s*\(")
            hits = []
            for f in files:
                for i, ln in enumerate(self.blob_lines(f) or [], 1):
                    if rx.search(ln):
                        isdef = bool(dfn.match(ln)) and not ln.rstrip().endswith(";") and not ln.lstrip().startswith("return")
                        hits.append((0 if isdef else 1, f, i))
            self._sym_cache[key] = hits
        ranked = sorted((d, 0 if f == prefer_path else 1, abs(i - prefer_line), f, i) for d, f, i in self._sym_cache[key])
        return [(f, i, d == 0) for d, _b, _c, f, i in ranked]

    def repo_grep(self, name):
        if name not in self._grep_cache:
            self._grep_cache[name] = self._repo_grep(name)
        return self._grep_cache[name]

    def _repo_grep(self, name):
        rc, outb = self.git.run(["grep", "-n", "-w", "-F", "-I", "-e", name, self.rev, "--", "*.cpp", "*.h", "*.hpp", "*.inl"])
        hits = []
        for ln in outb.decode("utf-8", "replace").splitlines():
            parts = ln.split(":", 3)
            if len(parts) >= 3 and parts[2].isdigit():
                hits.append((parts[1], int(parts[2])))
        return hits

    # -- content relocation for line cites into a split source file
    def _window(self, lines, n):
        g = lambda k: lines[k - 1].strip() if 1 <= k <= len(lines) else ""  # noqa: E731
        return (g(n - 1), g(n), g(n + 1))

    def content_state(self, path, n):
        """'same' | 'moved' | None (no pre-split text to compare with) for line n of a split source file."""
        u = self.unit_of.get(path)
        if not u or path != u.src or not u.base:
            return None
        old = self.blob_lines(path, u.base)
        new = self.blob_lines(path)
        if old is None or new is None or n > len(old):
            return None
        return "same" if n <= len(new) and self._window(old, n) == self._window(new, n) else "moved"

    def _window_index(self, path):
        u = self.unit_of[path]
        if u.src not in self._win_index:
            full, cur = {}, {}
            for f in self.unit_files(path):
                ls = self.blob_lines(f) or []
                for i in range(1, len(ls) + 1):
                    w = self._window(ls, i)
                    full.setdefault(w, []).append((f, i))
                    cur.setdefault(w[1], []).append((f, i))
            self._win_index[u.src] = (full, cur)
        return self._win_index[u.src]

    def content_relocate(self, path, n):
        u = self.unit_of.get(path)
        old = self.blob_lines(path, u.base) if u and u.base else None
        if not old or n > len(old):
            return None
        want = self._window(old, n)
        full, cur = self._window_index(path)
        exact = full.get(want, [])
        if len(exact) == 1:
            return exact[0]
        loose = cur.get(want[1], []) if len(want[1]) >= 20 else []
        if not exact and len(loose) == 1:
            return loose[0]
        return None


# ------------------------------------------------------------------ checking
def _near(lines, c, name):
    rx = re.compile(r"(?<![\w])" + re.escape(name) + r"(?![\w])")
    for a, b in c.ranges:
        if any(rx.search(ln) for ln in lines[max(1, a - WINDOW) - 1:b + WINDOW]):
            return True
    return False


def _sugg(path, line, n, how):
    return {"path": path, "line": line, "n": n, "how": how}


def _from_hits(hits, how):
    """A suggestion from ranked symbol hits; n counts the candidates that compete (definitions beat mere mentions)."""
    pool = [h for h in hits if h[2]] or hits
    return _sugg(pool[0][0], pool[0][1], len(pool), how) if pool else None


def relocate(repo, c, path):
    """Best current (path, line) for a cite, trying its symbol, then content at the split base; None when unknown."""
    names = [sym_name(c.sym)] if c.sym else ([c.implicit] if c.implicit else [])
    first = c.ranges[0][0] if c.ranges else 0
    for name in names:
        sg = _from_hits(repo.find_symbol(repo.unit_files(path), name, path, first), "symbol `%s`" % name)
        if sg:
            return sg
    if c.kind == "line" and first and len(c.ranges) == 1:
        hit = repo.content_relocate(path, first)
        last = c.ranges[0][1]
        if hit and last != first:
            end = repo.content_relocate(path, last)
            if end != (hit[0], hit[1] + last - first):
                hit = None
        if hit:
            return _sugg(hit[0], hit[1], 1, "content at split base")
    if c.sym:
        hits = repo.repo_grep(sym_name(c.sym))
        if hits and len({f for f, _ in hits}) == 1:
            return _sugg(hits[0][0], hits[0][1], len(hits), "symbol `%s` (repo-wide)" % sym_name(c.sym))
    return None


def check_cite(repo, doc_rel, c, exempt=None):
    if exempt:
        return Result("INFO", "quoted history (%s)" % exempt)
    kind, path = repo.resolve(c.pathtxt, doc_rel)
    if kind == "skip":
        return Result("INFO", "path outside the repo")
    if kind == "ambiguous":
        return Result("INFO", "ambiguous basename")
    if kind == "missing":
        return _classify_missing(repo, path, c)
    lines = repo.blob_lines(path)
    if lines is None:
        return Result("OK", path=path)
    if c.kind == "path":
        return Result("OK", path=path)
    sym = c.sym
    if sym and c.kind == "line" and is_weak(sym) and not has_symbol(lines, sym_name(sym)):
        sym = None
    if c.kind == "sym":
        if has_symbol(lines, sym_name(c.sym)):
            return Result("OK", path=path)
        return Result("MISSING-SYMBOL", "`%s` not in %s" % (c.sym, path), relocate(repo, c, path), path)
    bad = [n for n in c.numbers() if n < 1 or n > len(lines)]
    if bad:
        return Result("STALE-LINE", "line %d beyond end of %s (%d lines)" % (bad[0], path, len(lines)),
                      relocate(repo, c, path), path)
    if sym:
        name = sym_name(sym)
        if _near(lines, c, name):
            return Result("OK", path=path)
        return Result("STALE-LINE", "`%s` not within +-%d lines of %s:%d" % (name, WINDOW, path, c.ranges[0][0]),
                      relocate(repo, c, path), path)
    if c.implicit and _near(lines, c, c.implicit):
        return Result("OK", path=path)
    state = repo.content_state(path, c.ranges[0][0])
    if state == "same":
        return Result("OK", "line unchanged since the split", path=path)
    if state == "moved":
        return Result("STALE-LINE", "line %d of %s differs from the pre-split file" % (c.ranges[0][0], path),
                      relocate(repo, c, path), path)
    if c.implicit and has_symbol(lines, c.implicit):
        return Result("STALE-LINE", "`%s` not within +-%d lines of %s:%d" % (c.implicit, WINDOW, path, c.ranges[0][0]),
                      relocate(repo, c, path), path)
    return Result("OK", "line not verifiable (no symbol)", path=path)


def _classify_missing(repo, path, c):
    base = posixpath.basename(path)
    units = [u for u in repo.units.values() if u.src == path or ("/" not in path and posixpath.basename(u.src) == path)]
    if units:
        u = units[0]
        pieces = sorted(u.members - {path})
        sug = relocate_missing(repo, c, pieces)
        return Result("SPLIT", "pieces: %s" % ", ".join(pieces) if pieces else "no pieces found", sug)
    h = repo.removed(path)
    if h is None and "/" not in path:
        for k, v in repo.history().items():
            if posixpath.basename(k) == base:
                h = repo.removed(k)
                break
    if h is None:
        if "/" not in path:
            return Result("INFO", "unresolved basename")
        if any(posixpath.splitext(seg)[1][1:].lower() in LINE_EXTS for seg in path.split("/")[:-1]):
            return Result("INFO", "slash-separated list of file names")
        same = sorted(repo.by_base.get(base, []))[:3]
        return Result("UNKNOWN", "not in the tree and no deleting commit found" + ("; same basename at " + ", ".join(same) if same else ""))
    if h[0] == "RENAMED":
        sug = _sugg(h[3], c.ranges[0][0] if c.ranges else 0, 1, "rename") if h[3] in repo.tree else None
        return Result("RENAMED", "renamed to %s in %s (%s)" % (h[3], h[1], h[2]), sug)
    return Result("DELETED", "deleted in %s (%s)" % (h[1], h[2]))


def relocate_missing(repo, c, pieces):
    names = [sym_name(c.sym)] if c.sym else ([c.implicit] if c.implicit else [])
    for name in names:
        sg = _from_hits(repo.find_symbol(pieces, name), "symbol `%s`" % name)
        if sg:
            return sg
    return None


def evaluate(repo, doc):
    return [(c, check_cite(repo, doc.rel, c, doc.exempt.get(c.line_no))) for c in doc.cites]


# ------------------------------------------------------------------ suggestions
def build_replacement(repo, c, sugg):
    """The new cite text for a suggestion, in the old cite's own form, verified; None if nothing valid can be built."""
    prefix = ""
    m = re.match(r"^[A-Za-z]:[\\/].*?mc2-[a-z-]+[\\/]", c.pathtxt)
    if m and norm_path(c.pathtxt) is not None:
        prefix = c.pathtxt[:m.end()].replace("\\", "/")
    newpath = prefix + sugg["path"]
    if c.kind == "path":
        return newpath
    if c.kind == "sym":
        return newpath + c.tail
    first = c.ranges[0][0]
    delta = (sugg["line"] or first) - first
    shifted = re.sub(r"\d+", lambda m_: str(max(1, int(m_.group()) + delta)), c.spec)
    simple = {"#": "#L%d" % sugg["line"], ":": ":%d" % sugg["line"]}.get(c.spec[:1], " (line %d)" % sugg["line"])
    for spec in (shifted, simple) if len(c.ranges) == 1 and c.ranges[0][0] == c.ranges[0][1] else (shifted,):
        text = newpath + spec + c.tail
        nc = parse_cite_text(text)
        if nc is not None:
            nc.implicit = c.implicit
            if check_cite(repo, "", nc).status == "OK":
                return text
    return None


def _suggestible(repo, c, r):
    """A symbol hit gives one line, so it can only replace a one-line cite; ranges need the content or rename proof of their ends.
    A content match inside the same split source file cannot be told from a stale cite afterwards, so it is not offered."""
    sg = r.sugg
    if c.kind != "line":
        return True
    u = repo.unit_of.get(r.path) if r.path else None
    if sg["how"].startswith("content") and u is not None and sg["path"] == u.src == r.path:
        return False
    if len(c.ranges) != 1 or not sg["line"]:
        return False
    return c.ranges[0][0] == c.ranges[0][1] or sg["how"].startswith(("content", "rename"))


def suggest_for(repo, doc):
    """[(cite, result, new_text or None)] for every finding of a doc."""
    rows = []
    for c, r in evaluate(repo, doc):
        if r.status not in FINDING_CLASSES:
            continue
        new = None
        if r.sugg and r.sugg["n"] == 1 and _suggestible(repo, c, r):
            new = build_replacement(repo, c, r.sugg)
        rows.append((c, r, new))
    return rows


# ------------------------------------------------------------------ diff guard
class Guard:
    def __init__(self, rel):
        self.rel, self.findings, self.notes = rel, [], []

    @property
    def ok(self):
        return not self.findings

    def fail(self, kind, msg):
        self.findings.append((kind, msg))


def _mask(doc):
    rows = []
    for i, ln in enumerate(doc.lines):
        if i == 0 and doc.header is not None:
            continue
        for c in sorted((c for c in doc.cites if c.line_no == i + 1), key=lambda c: -c.start):
            ln = ln[:c.start] + "\x00" + ln[c.end:]
        rows.append((i + 1, ln))
    return rows


def _header_problem(repo, line):
    m = HEADER_RE.match(line)
    if not m:
        return "malformed status header"
    try:
        datetime.date.fromisoformat(m.group("date"))
    except ValueError:
        return "status header date is not a date"
    if m.group("sup") and repo.resolve(m.group("sup"), "")[0] != "ok":
        return "superseded-by target %s is not a tracked file" % m.group("sup")
    return None


def guard_texts(repo, rel, base_bytes, new_bytes):
    g = Guard(rel)
    base_t, bbom, bstyle = prep(base_bytes)
    new_t, nbom, nstyle = prep(new_bytes)
    if bbom != nbom:
        g.fail("bytes", "BOM added or removed")
    if bstyle != nstyle and "none" not in (bstyle, nstyle):
        g.notes.append("note: line endings normalised (CRLF<->LF)")
    if base_t.endswith(chr(10)) != new_t.endswith(chr(10)):
        g.fail("bytes", "final newline added or removed")
    bd, nd = parse_doc(rel, base_t), parse_doc(rel, new_t)
    if nd.header is not None:
        why = _header_problem(repo, nd.header)
        if why:
            g.fail("header", why)
    elif bd.header is not None:
        g.fail("header", "status header removed")
    brows, nrows = _mask(bd), _mask(nd)
    for k in range(max(len(brows), len(nrows))):
        b = brows[k] if k < len(brows) else (None, "<end of file>")
        n = nrows[k] if k < len(nrows) else (None, "<end of file>")
        if b[1] != n[1]:
            g.fail("prose", "first divergent line pair\n          base L%s: %s\n          new  L%s: %s" % (
                b[0], b[1].replace("\x00", "<cite>")[:160], n[0], n[1].replace("\x00", "<cite>")[:160]))
            return g
    for cb, cn in zip(bd.cites, nd.cites):
        if cb.text == cn.text:
            continue
        why = replacement_problem(repo, rel, bd, cb, cn)
        if why:
            g.fail("cite", "L%d: `%s` -> `%s`: %s" % (cn.line_no, cb.text, cn.text, why))
    return g


def _unit_of_old(repo, rel, cb):
    """(path or None, unit or None, rename target or None) for the old cite's file."""
    kind, p = repo.resolve(cb.pathtxt, rel)
    if kind == "ok":
        return p, repo.unit_of.get(p), None
    if kind == "missing":
        for u in repo.units.values():
            if u.src == p or ("/" not in p and posixpath.basename(u.src) == p):
                return None, u, None
        h = repo.removed(p)
        if h and h[0] == "RENAMED":
            return None, None, h[3]
    return None, None, None


def replacement_problem(repo, rel, bdoc, cb, cn):
    if bdoc.exempt.get(cb.line_no):
        return "quoted history is immutable"
    rn = check_cite(repo, rel, cn)
    if rn.status != "OK":
        return "new cite does not check out (%s %s)" % (rn.status, rn.detail)
    if cb.kind != cn.kind:
        return "cite shape changed (%s -> %s)" % (cb.kind, cn.kind)
    if cb.sym:
        if not cn.sym or sym_name(cb.sym) != sym_name(cn.sym):
            return "symbol changed (%s -> %s)" % (cb.sym, cn.sym)
        if cb.tail != cn.tail:
            return "text after the line spec changed (%r -> %r)" % (cb.tail, cn.tail)
        return None
    ro = check_cite(repo, rel, cb)
    if cb.kind == "line" and not cn.sym and ro.status == "OK":
        return "old cite was valid and names no symbol; nothing to repair"
    kindn, newp = repo.resolve(cn.pathtxt, rel)
    sg = ro.sugg
    if sg and sg["n"] == 1 and not cn.sym and cn.kind == "line" and (newp, cn.ranges[0][0]) != (sg["path"], sg["line"]):
        return "the only candidate location is %s:%d (%s)" % (sg["path"], sg["line"], sg["how"])
    oldp, unit, renamed = _unit_of_old(repo, rel, cb)
    if cn.sym and cb.kind == "line":
        return None
    if oldp is not None and oldp == newp:
        return None
    if unit is not None and repo.unit_of.get(newp) is unit:
        return None
    if renamed is not None and renamed == newp:
        return None
    return "no symbol ties the new cite to the old; it must stay in the same file, split unit or rename target"


# ------------------------------------------------------------------ doc selection
def in_scope(rel):
    if not rel.lower().endswith(DOC_EXTS):
        return False
    parts = rel.split("/")
    if rel.startswith("docs/archive/") or any(p == "node_modules" or p.startswith("build64") for p in parts):
        return False
    if len(parts) == 1:
        return rel.lower().endswith(".md")
    if parts[0] not in DOC_DIRS:
        return False
    return not (rel.lower().endswith(".txt") and DATA_TXT.search(parts[-1]))


def _ls(repo, extra, spec=None):
    rc, o = repo.git.run(["ls-files", "-z"] + extra + (["--", spec] if spec else []))
    return [x.decode("utf-8", "replace") for x in o.split(b"\0") if x]


def list_docs(repo, paths, include_untracked):
    untracked = ["-o", "--exclude-standard"]
    if not paths:
        files = _ls(repo, []) + (_ls(repo, untracked) if include_untracked else [])
        return sorted({f for f in files if in_scope(f)})
    found = []
    for p in paths:
        ap = os.path.abspath(p if os.path.isabs(p) or os.path.exists(p) else os.path.join(repo.root, p))
        rel = os.path.relpath(ap, repo.root).replace("\\", "/")
        if os.path.isdir(ap):
            files = _ls(repo, [], rel) + (_ls(repo, untracked, rel) if include_untracked else [])
            found += [f for f in files if in_scope(f)]
        else:
            found.append(rel)
    return sorted(set(found))


def read_doc(repo, rel):
    try:
        with open(os.path.join(repo.root, rel), "rb") as fh:
            return fh.read()
    except OSError:
        return None


def load_docs(repo, rels):
    docs = []
    for rel in rels:
        data = read_doc(repo, rel)
        if data is not None:
            docs.append(parse_doc(rel, prep(data)[0]))
    return docs


def last_commit_info(repo, rels):
    """rel -> (date, age rank): rank 0 is the newest commit, so equal dates still order."""
    want, got, cur, seq = set(rels), {}, None, -1
    rc, o = repo.git.run(["log", "--format=%x01%cs", "--name-only", "--no-renames", repo.rev, "--", "*.md", "*.txt", "*.rst"])
    for ln in o.decode("utf-8", "replace").splitlines():
        if ln.startswith("\x01"):
            cur, seq = ln[1:], seq + 1
        elif ln in want and ln not in got:
            got[ln] = (cur, seq)
    return got


# ------------------------------------------------------------------ commands
def tally():
    return {k: 0 for k in ("cites", "OK", "INFO") + FINDING_CLASSES}


def run_check(repo, args):
    docs = load_docs(repo, list_docs(repo, args.paths, args.include_untracked))
    t, bad_docs = tally(), 0
    for d in docs:
        rows = evaluate(repo, d)
        bad = False
        for c, r in rows:
            t["cites"] += 1
            t[r.status] += 1
            if r.status in FINDING_CLASSES:
                bad = True
                s = ""
                if r.sugg:
                    s = " -> %s:%s (%s)" % (r.sugg["path"], r.sugg["line"] or "", r.sugg["how"])
                if not args.quiet:
                    out("%s:%d:%d: %s %s  %s%s" % (d.rel, c.line_no, c.col, r.status, c.text, r.detail, s))
            elif r.status == "INFO" and args.verbose:
                out("%s:%d:%d: INFO %s  %s" % (d.rel, c.line_no, c.col, c.text, r.detail))
        bad_docs += bad
    broken = sum(t[k] for k in ("SPLIT", "DELETED", "RENAMED", "UNKNOWN"))
    out("cite_guard: docs=%d cites=%d ok=%d broken=%d (SPLIT=%d DELETED=%d RENAMED=%d UNKNOWN=%d) stale-line=%d "
        "missing-symbol=%d info=%d docs_with_findings=%d" % (
            len(docs), t["cites"], t["OK"], broken, t["SPLIT"], t["DELETED"], t["RENAMED"], t["UNKNOWN"],
            t["STALE-LINE"], t["MISSING-SYMBOL"], t["INFO"], bad_docs))
    return 1 if broken or t["STALE-LINE"] or t["MISSING-SYMBOL"] else 0


def header_value(doc):
    if doc.header is None:
        return "none"
    m = HEADER_RE.match(doc.header)
    if not m:
        return "malformed"
    return doc.header[len(HEADER_PREFIX):-4].strip().replace(" | ", "@")


def cite_set(repo, doc):
    items = set()
    for c in doc.cites:
        kind, p = repo.resolve(c.pathtxt, doc.rel)
        if kind in ("ok", "missing"):
            items.add("p:" + p)
        if c.sym:
            items.add("s:" + sym_name(c.sym))
    return items


def run_triage(repo, args):
    docs = load_docs(repo, list_docs(repo, args.paths, args.include_untracked))
    info = last_commit_info(repo, [d.rel for d in docs])
    sets = {d.rel: cite_set(repo, d) for d in docs}
    out("# path | cites | broken | stale-line | last-commit-date | status-header | superseded-hint")
    for d in docs:
        rows = evaluate(repo, d)
        n = len(rows)
        broken = sum(1 for _c, r in rows if r.status in ("SPLIT", "DELETED", "RENAMED", "UNKNOWN"))
        stale = sum(1 for _c, r in rows if r.status in ("STALE-LINE", "MISSING-SYMBOL"))
        mine, hint = sets[d.rel], "-"
        if len(mine) >= 3:
            best = (0.0, None)
            for o in docs:
                if o.rel == d.rel or o.rel not in info or (d.rel in info and info[o.rel][1] >= info[d.rel][1]):
                    continue
                theirs = sets[o.rel]
                if len(theirs) < 3:
                    continue
                j = len(mine & theirs) / float(len(mine | theirs))
                if j > best[0]:
                    best = (j, o.rel)
            if best[0] >= 0.6:
                hint = "%s (%.2f)" % (best[1], best[0])
        out("%s | %d | %d | %d | %s | %s | %s" % (d.rel, n, broken, stale, info[d.rel][0] if d.rel in info else "untracked", header_value(d), hint))
    return 0


def run_fix_suggest(repo, args):
    if len(args.paths) != 1:
        out("--fix-suggest takes exactly one doc")
        return 2
    rel = list_docs(repo, args.paths, True)[0]
    data = read_doc(repo, rel)
    if data is None:
        out("cannot read %s" % rel)
        return 2
    doc = parse_doc(rel, prep(data)[0])
    rows = suggest_for(repo, doc)
    out("fix-suggest %s: %d cite(s) need repair" % (rel, len(rows)))
    for c, r, new in rows:
        where = "%s:%d:%d: %s |" % (rel, c.line_no, c.col, r.status)
        if new is not None:
            out("%s REPLACE `%s` WITH `%s` | %s; %d hit(s)" % (where, c.text, new, r.sugg["how"], r.sugg["n"]))
        else:
            note = r.detail
            if r.sugg and r.sugg["n"] > 1:
                note += "; %d candidates, first %s:%s (%s)" % (r.sugg["n"], r.sugg["path"], r.sugg["line"], r.sugg["how"])
            elif r.sugg and r.sugg["n"] == 1:
                note += "; start is at %s:%s (%s), the cited span needs a hand check" % (r.sugg["path"], r.sugg["line"], r.sugg["how"])
            out("%s KEEP `%s` | %s" % (where, c.text, note))
    return 0


def run_diff_guard(repo, args):
    if not args.paths:
        out("--diff-guard needs at least one doc")
        return 2
    bad = 0
    for p in args.paths:
        rel = os.path.relpath(os.path.abspath(p if os.path.exists(p) else os.path.join(repo.root, p)), repo.root).replace("\\", "/")
        base = repo.git.blob("%s:%s" % (args.base, rel))
        new = read_doc(repo, rel)
        if new is None:
            out("FAIL  %s\n      - [file] cannot read the worktree file" % rel)
            bad += 1
            continue
        if base is None:
            out("FAIL  %s\n      - [file] not in %s" % (rel, args.base))
            bad += 1
            continue
        g = guard_texts(repo, rel, base, new)
        if not g.ok or not args.quiet:
            out("%-5s %s" % ("OK" if g.ok else "FAIL", rel))
            for k, m in g.findings:
                out("      - [%s] %s" % (k, m))
            for n in g.notes:
                out("      " + n)
        bad += 0 if g.ok else 1
    return 1 if bad else 0


# ------------------------------------------------------------------ selftest fixtures
def _git_in(root, *a):
    r = subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.autocrlf=false"] + list(a),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, **console_child.child_kwargs(redirected=True))
    if r.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(a), r.stderr.decode("utf-8", "replace")))
    return r.stdout.decode("utf-8", "replace").strip()


def commit_files(root, files, msg, remove=()):
    for rel, text in files.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(text.encode("latin-1") if isinstance(text, str) else text)
    for rel in remove:
        _git_in(root, "rm", "-q", "-f", rel)
    _git_in(root, "add", "-A")
    _git_in(root, "commit", "-q", "--allow-empty", "-m", msg)
    return _git_in(root, "rev-parse", "--short", "HEAD")


def _src(n, marks):
    ls = ["// line %d" % i for i in range(1, n + 1)]
    for ln, txt in marks.items():
        ls[ln - 1] = txt
    return "\n".join(ls) + "\n"


def make_world(root):
    """A temp repo holding every situation the checks classify: valid, split (kept and removed), deleted, renamed."""
    _git_in(root, "init", "-q")
    base = commit_files(root, {
        "src/a.cpp": _src(30, {5: "void alpha() {", 12: "int beta() {", 20: "struct Gamma {"}),
        "src/big.cpp": _src(40, {3: "int big_head() {", 25: "int big_tail_unique_line_here() {"}),
        "src/gone.cpp": _src(20, {10: "void moved_fn() {"}),
        "src/old_name.cpp": _src(10, {}),
        "src/ren_old.cpp": _src(15, {4: "void ren_fn() {"}),
        "docs/target.md": "# target\n",
    }, "base")
    commit_files(root, {
        "src/big.cpp": _src(10, {3: "int big_head() {"}),
        "src/big_part.cpp": "\n".join(["// piece"] + ["// line %d" % i for i in range(11, 41)]).replace(
            "// line 25", "int big_tail_unique_line_here() {") + "\n",
        "src/big_internal.h": "// shared\n",
        "src/gone_a.cpp": "// piece\n// x\nvoid moved_fn() {\n}\n",
        "src/gone_b.cpp": "// piece b\n",
        "tools/split/allow/big.json": json.dumps({"base": base, "src": "src/big.cpp", "stem": "big", "split_commit": "PENDING"}),
        "tools/split/allow/gone.json": json.dumps({"base": base, "src": "src/gone.cpp", "stem": "gone"}),
    }, "split", remove=("src/gone.cpp",))
    commit_files(root, {}, "delete old_name", remove=("src/old_name.cpp",))
    _git_in(root, "mv", "src/ren_old.cpp", "src/ren_new.cpp")
    _git_in(root, "commit", "-q", "-m", "rename")
    return root


def new_repo(prefix="cite_guard_"):
    tmp = tempfile.TemporaryDirectory(prefix=prefix)
    make_world(tmp.name)
    return tmp, Repo(tmp.name)


def _ev(repo, text, rel="docs/d.md"):
    return evaluate(repo, parse_doc(rel, text))


def _statuses(repo, text, rel="docs/d.md"):
    return [r.status for _c, r in _ev(repo, text, rel)]


SELFTEST_CASES = []


def case(fn):
    SELFTEST_CASES.append(fn)
    return fn


def _guard(repo, base, new, rel="docs/d.md"):
    return guard_texts(repo, rel, base.encode("latin-1"), new.encode("latin-1"))


@case
def st_grammar_forms(repo):
    forms = ["src/a.cpp:5", "src/a.cpp:5-9", "src/a.cpp:5,9", "src/a.cpp#L5", "src/a.cpp#L5-L9", "src/a.cpp (line 5)",
             "src/a.cpp:5 (alpha)", "src/a.cpp:5:alpha", "src/a.cpp::alpha", "src/a.cpp"]
    for f in forms:
        cs = parse_line("see `%s` here" % f, 1)
        assert len(cs) == 1 and cs[0].text == f, (f, [c.text for c in cs])
    c = parse_line(r"at src\a.cpp:5 and A:/Games/mc2-main/src/a.cpp:12 and [x](src/a.cpp#L3)", 1)
    assert len(c) == 3 and [x.kind for x in c] == ["line"] * 3
    assert not parse_line("host localhost:8080 and 12:30 and e.g.:3 and v1.2:4", 1)


@case
def st_valid_cite(repo):
    assert _statuses(repo, "`src/a.cpp:5` (alpha) and src/a.cpp:12 (beta) and src/a.cpp:21 near `Gamma`.\n") == ["OK", "OK", "OK"]
    assert _statuses(repo, "see src/a.cpp:5 (`alpha`)\n") == ["OK"]


@case
def st_stale_line_symbol_elsewhere(repo):
    rows = _ev(repo, "The beta body, src/a.cpp:25 (beta).\n")
    r = rows[0][1]
    assert r.status == "STALE-LINE" and r.sugg["path"] == "src/a.cpp" and r.sugg["line"] == 12, (r.status, r.sugg)
    beyond = _ev(repo, "src/a.cpp:99 (alpha)\n")[0][1]
    assert beyond.status == "STALE-LINE" and "beyond" in beyond.detail and beyond.sugg["line"] == 5


@case
def st_implicit_symbol_is_soft(repo):
    assert _statuses(repo, "`alpha` is at src/a.cpp:5.\n") == ["OK"]
    assert _statuses(repo, "`alpha` is at src/a.cpp:25.\n") == ["STALE-LINE"]
    assert _statuses(repo, "`nosuchthing` is at src/a.cpp:25.\n") == ["OK"]


@case
def st_split_classification(repo):
    rows = _ev(repo, "Called from src/gone.cpp:10 (moved_fn).\n")
    r = rows[0][1]
    assert r.status == "SPLIT" and "src/gone_a.cpp" in r.detail, (r.status, r.detail)
    assert r.sugg["path"] == "src/gone_a.cpp" and r.sugg["line"] == 3


@case
def st_split_core_line_by_content(repo):
    assert _statuses(repo, "src/big.cpp:3\n") == ["OK"]
    r = _ev(repo, "src/big.cpp:25\n")[0][1]
    assert r.status == "STALE-LINE" and r.sugg["path"] == "src/big_part.cpp", (r.status, r.sugg)
    assert _statuses(repo, "src/big.cpp:9 (big_head)\n") == ["STALE-LINE"]


@case
def st_deleted_and_renamed(repo):
    d = _ev(repo, "gone: src/old_name.cpp:3\n")[0][1]
    assert d.status == "DELETED" and re.search(r"deleted in [0-9a-f]{7,} \(\d{4}-\d{2}-\d{2}\)", d.detail), d.detail
    rn = _ev(repo, "was src/ren_old.cpp:4 (ren_fn)\n")[0][1]
    assert rn.status == "RENAMED" and "src/ren_new.cpp" in rn.detail
    assert _statuses(repo, "src/never/there.cpp:3 and src/nope.h\n") == ["UNKNOWN", "UNKNOWN"]
    assert _statuses(repo, "see foo.cpp\n") == ["INFO"]


@case
def st_quoted_history_exempt(repo):
    txt = ("```text\nsrc/old_name.cpp:3 [CONFIG v1]\n```\n"
           "```cpp\nsrc/old_name.cpp:4 PASS\n```\n"
           "src/old_name.cpp:5 (quoted history)\n"
           "<!-- history -->\n```\nsrc/old_name.cpp:6\n```\n"
           "<!-- history -->\nsrc/old_name.cpp:7\nsrc/old_name.cpp:8\n\n"
           "src/old_name.cpp:9\n"
           "```bash\npy src/old_name.cpp:10\n```\n")
    st = _statuses(repo, txt)
    assert st == ["INFO"] * 6 + ["DELETED", "DELETED"], st


@case
def st_diff_guard_accepts_cite_replacement(repo):
    base = "# T\n\nSee `src/a.cpp:25` (beta) for the body.\nAlso src/gone.cpp:10 (moved_fn).\n"
    new = "# T\n\nSee `src/a.cpp:12` (beta) for the body.\nAlso src/gone_a.cpp:3 (moved_fn).\n"
    g = _guard(repo, base, new)
    assert g.ok, g.findings


@case
def st_diff_guard_rejects_prose_edit(repo):
    base = "# T\n\nSee `src/a.cpp:25` (beta) for the body.\n"
    g = _guard(repo, base, "# T\n\nSee `src/a.cpp:12` (beta) for the BODY.\n")
    assert not g.ok and g.findings[0][0] == "prose" and "base L3" in g.findings[0][1]
    assert not _guard(repo, base, base + "extra\n").ok
    assert not _guard(repo, base, "# T\n\nSee `src/a.cpp:12` (beta) for the body. src/a.cpp:5\n").ok
    assert not _guard(repo, base, "# T\n\nSee `src/a.cpp:12` (beta) for the body.\n```\nx\n```\n").ok


@case
def st_diff_guard_rejects_other_symbol(repo):
    base = "See src/a.cpp:25 (beta) now.\n"
    g = _guard(repo, base, "See src/a.cpp:5 (alpha) now.\n")
    assert not g.ok and "symbol changed" in g.findings[0][1]
    g = _guard(repo, base, "See src/a.cpp:5 now.\n")
    assert not g.ok
    g = _guard(repo, base, "See src/a.cpp:99 (beta) now.\n")
    assert not g.ok and "does not check out" in g.findings[0][1]
    g = _guard(repo, "See src/a.cpp:5 now.\n", "See src/big.cpp:5 now.\n")
    assert not g.ok


@case
def st_diff_guard_rejects_unneeded_and_history_edits(repo):
    # a valid symbol-less cite has nothing to repair; naming a symbol that really sits there is a verified claim
    assert not _guard(repo, "src/a.cpp:6\n", "src/a.cpp:7\n").ok
    assert _guard(repo, "src/a.cpp:6\n", "src/a.cpp:7 (alpha)\n").ok
    assert _guard(repo, "src/a.cpp:6 (alpha)\n", "src/a.cpp:7 (alpha)\n").ok
    assert not _guard(repo, "`src/a.cpp:25` (beta)\n", "`src/a.cpp:12`(beta)\n").ok
    h = "```text\nsrc/a.cpp:25 (beta)\n```\n"
    assert not _guard(repo, h, h.replace("25", "12")).ok


@case
def st_diff_guard_split_unit_move(repo):
    r = _ev(repo, "src/big.cpp:25\n")[0][1]
    want = "src/big_part.cpp:%d" % r.sugg["line"]
    g = _guard(repo, "src/big.cpp:25\n", want + "\n")
    assert g.ok, g.findings
    assert not _guard(repo, "src/big.cpp:25\n", "src/a.cpp:25\n").ok
    assert not _guard(repo, "src/big.cpp:25\n", "src/big_part.cpp:1\n").ok


@case
def st_status_header(repo):
    body = "# T\n\ntext src/a.cpp:5 (alpha)\n"
    hdr = "<!-- doc-status: stale | reviewed:2026-10-10 -->\n"
    assert _guard(repo, body, hdr + body).ok
    assert _guard(repo, hdr + body, hdr.replace("stale", "current") + body).ok
    assert _guard(repo, body, "<!-- doc-status: superseded-by:docs/target.md | reviewed:2026-10-10 -->\n" + body).ok
    assert not _guard(repo, body, "<!-- doc-status: superseded-by:docs/missing.md | reviewed:2026-10-10 -->\n" + body).ok
    assert not _guard(repo, body, "<!-- doc-status: fresh | reviewed:2026-10-10 -->\n" + body).ok
    assert not _guard(repo, body, "<!-- doc-status: stale | reviewed:2026-13-40 -->\n" + body).ok
    assert not _guard(repo, hdr + body, body).ok
    assert not _guard(repo, body, hdr + "\n" + body).ok
    assert not _guard(repo, body, "# T\n" + hdr + "\ntext src/a.cpp:5 (alpha)\n").ok


@case
def st_crlf_and_bom(repo):
    base = "# T\r\n\r\nSee src/a.cpp:25 (beta).\r\n"
    g = _guard(repo, base, "# T\r\n\r\nSee src/a.cpp:12 (beta).\r\n")
    assert g.ok, g.findings
    g = _guard(repo, base, "# T\n\nSee src/a.cpp:12 (beta).\n")
    assert g.ok and g.notes
    assert not guard_texts(repo, "docs/d.md", base.encode(), b"\xef\xbb\xbf" + base.encode()).ok
    assert parse_doc("d.md", prep(base.encode())[0]).cites[0].line_no == 3


@case
def st_fix_suggest_shape(repo):
    text = "Body: src/a.cpp:25 (beta).\nGone: src/old_name.cpp:3\nIn `moved_fn` src/gone.cpp:10.\nok src/a.cpp:5 (alpha)\n"
    doc = parse_doc("docs/d.md", text)
    rows = suggest_for(repo, doc)
    assert [r.status for _c, r, _n in rows] == ["STALE-LINE", "DELETED", "SPLIT"], [r.status for _c, r, _n in rows]
    assert rows[0][2] == "src/a.cpp:12 (beta)" and rows[1][2] is None and rows[2][2] == "src/gone_a.cpp:3"
    for c, r, new in rows:
        if new:
            base = text
            fixed = base.replace(c.text, new, 1)
            assert _guard(repo, base, fixed).ok


@case
def st_cli_fix_suggest_lines(repo):
    sys_out = []
    doc = "Body: src/a.cpp:25 (beta).\nGone: src/old_name.cpp:3\n"
    with open(os.path.join(repo.root, "docs", "f.md"), "w", encoding="latin-1", newline="") as fh:
        fh.write(doc)
    ns = argparse.Namespace(paths=["docs/f.md"], include_untracked=True, base="HEAD", quiet=False, verbose=False)
    import io
    buf, old = io.StringIO(), sys.stdout
    sys.stdout = buf
    try:
        rc = run_fix_suggest(repo, ns)
    finally:
        sys.stdout = old
    lines = buf.getvalue().splitlines()
    sys_out.extend(lines)
    assert rc == 0 and lines[0] == "fix-suggest docs/f.md: 2 cite(s) need repair", lines
    assert re.match(r"^docs/f\.md:1:7: STALE-LINE \| REPLACE `src/a\.cpp:25 \(beta\)` WITH `src/a\.cpp:12 \(beta\)` \|", lines[1]), lines[1]
    assert re.match(r"^docs/f\.md:2:7: DELETED \| KEEP `src/old_name\.cpp:3` \| deleted in ", lines[2]), lines[2]


@case
def st_scope_rules(repo):
    assert in_scope("docs/a.md") and in_scope("README.md") and in_scope(".claude/cleanup/x.md") and in_scope("tools/x/README.md")
    assert not in_scope("docs/archive/a.md") and not in_scope("code/a.md") and not in_scope("tools/CMakeLists.txt")
    assert not in_scope("scripts/foo-allowlist.txt") and not in_scope("top.txt") and not in_scope("docs/x/build64-ninja/a.md")


def run_selftest():
    tmp, repo = new_repo()
    failed = 0
    try:
        for fn in SELFTEST_CASES:
            try:
                fn(repo)
            except AssertionError as e:
                failed += 1
                out("FAIL  %s: %s" % (fn.__name__, e))
    finally:
        repo.git.close()
        try:
            tmp.cleanup()
        except OSError:
            pass
    n = len(SELFTEST_CASES)
    out("cite_guard selftest: %d/%d ok" % (n - failed, n))
    return 1 if failed else 0


# ------------------------------------------------------------------ CLI
def build_parser():
    ap = argparse.ArgumentParser(description="Fail-closed guard for doc cite repair and doc triage.")
    ap.add_argument("paths", nargs="*", help="docs or directories (default: every doc in scope)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="verify every cite (default mode)")
    mode.add_argument("--diff-guard", action="store_true", help="prove the worktree docs differ from --base only in cites and status header")
    mode.add_argument("--triage-report", action="store_true", help="one line per doc (read-only)")
    mode.add_argument("--fix-suggest", action="store_true", help="suggested replacement per broken cite of one doc (read-only)")
    mode.add_argument("--selftest", action="store_true", help="run the temp-repo fixtures")
    ap.add_argument("--base", default="HEAD", help="base rev for --diff-guard (default HEAD)")
    ap.add_argument("--repo", help="repo root (default: the repo this tool lives in)")
    ap.add_argument("--include-untracked", action="store_true", help="also scan untracked docs")
    ap.add_argument("-v", "--verbose", action="store_true", help="--check: also list INFO cites")
    ap.add_argument("-q", "--quiet", action="store_true", help="print failures / counts only")
    return ap


def main(argv=None):
    args = build_parser().parse_intermixed_args(argv)
    if args.selftest:
        return run_selftest()
    try:
        repo = Repo(args.repo or ROOT)
    except (RuntimeError, OSError) as e:
        out("cite_guard: %s" % e)
        return 2
    try:
        if args.diff_guard:
            return run_diff_guard(repo, args)
        if args.triage_report:
            return run_triage(repo, args)
        if args.fix_suggest:
            return run_fix_suggest(repo, args)
        return run_check(repo, args)
    finally:
        repo.git.close()


if __name__ == "__main__":
    try:
        rc = main()
        sys.stdout.flush()
    except (BrokenPipeError, OSError):
        # A closed pipe (`| head`) is the reader's choice, not a failure; keep the interpreter from flushing into it again.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        rc = 0
    sys.exit(rc)
