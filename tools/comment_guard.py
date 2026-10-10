#!/usr/bin/env python3
"""comment_guard - fail-closed proof that an edit touched comments and nothing else.

A cheap model may de-bloat comments (delete narration, dead history, commented-out code)
only if a bad edit cannot land. This tool is the gate: it passes a file only when ALL hold.

  a. Comment-only   Base and new text lex to the same code. Comments are removed with real
                    C/C++ lexing (strings, chars, raw strings, digit separators, line
                    continuations, `#include <a//b>`), whitespace is collapsed per logical
                    line, blank lines are ignored. The first divergent line pair is reported.
  b. Why survives   Comments that carry rationale or machine-read meaning are protected and must
                    reappear in the new text verbatim (whitespace-normalised, moves allowed):
                    license header, event-format notes, registered gate names (only names in
                    Gates/gate_table.json; macros such as MC2_VERIFY and slice tags such as
                    FOO-BAR-1 are history, not protective, unless a checker reads them), lint pragmas,
                    rationale words, dates, shas, TODO tags, doxygen tags, pragma/static_assert
                    neighbours. Checker needles (see below), FOO-BEGIN/FOO-END and @@/===-fenced markers
                    and the first-lines comments of split pieces are protected too. Everything under
                    Gates/ is wholly protected. A protected comment may instead be KEPT
                    as a rewrite candidate: `// TODO(rewrite): <original text>`. A protected
                    comment may be deleted only if a reviewer-written --allow file lists it.
  c. Only shrinks   New comment bytes may not exceed base comment bytes (plus 16 B per new
                    TODO(rewrite) tag).

  py -3 tools/comment_guard.py --check code/a.cpp code/b.h        # worktree vs HEAD (default)
  py -3 tools/comment_guard.py --check --staged                   # index vs HEAD, files from the index
  py -3 tools/comment_guard.py --check --base abc123 code/a.cpp   # vs another rev (--rev is an alias)
  py -3 tools/comment_guard.py --check --base-file orig.cpp copy.cpp   # vs a file outside git
  py -3 tools/comment_guard.py --check --allow reviewed.json code/a.cpp   # reviewer-approved deletions
  py -3 tools/comment_guard.py --rewrite-candidates [paths]       # list every TODO(rewrite) comment
  py -3 tools/comment_guard.py --needles                          # list the checker needles (needle<TAB>first defining file)
  py -3 tools/comment_guard.py --selftest

Checker needles: a comment a checker script reads is a sentinel, whatever Rule A says about tags. At startup the
guard scans scripts/ (*.py *.sh and the *.json/*.txt/*.allowlist lists), tools/ (*.py, not tests), tests/, cmake/ and
.claude/hooks/ and collects (a) tag-shaped tokens (FOO-BAR-1) from string literals and non-comment text and (b) marker
words (>= 6 chars: two uppercase segments joined by `-`, a trailing `:`, or >= 12 chars with `-` and a capital; never prose,
lowercase stems or the NEEDLE_STOPLIST) from grep/rg/Select-String arguments, re.* patterns and `in text` tests. A comment containing a needle verbatim is protected; `--needles` prints them. Over-protecting is accepted.

Optional object proof (a build, which the caller runs, sits between the two steps):
  py -3 tools/comment_guard.py --hash-objs build64-ninja code/a.cpp > before.json
  ... edit comments, rebuild ...
  py -3 tools/comment_guard.py --compare-objs before.json

The object hash covers the COFF sections that define behaviour (code, data, relocations,
symbols, linker directives) and ignores the header timestamp and every .debug* section,
because comment deletion legitimately moves debug line tables. Files that use
__LINE__/__DATE__/__TIME__ downgrade a mismatch to WARN; assert-style macros defined in
headers expand __LINE__ invisibly, so a DIFF there is expected after a line-count change -
treat --check as the primary proof and the object compare as a bonus. A toolchain switch
that stabilises __DATE__/__TIME__ (for example /d1nodatetime or /Brepro) may be needed.

Exit: 0 ok, 1 finding, 2 usage / environment error.  Stdlib only.
"""
from __future__ import annotations

import argparse
import ast
import datetime
import glob
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)
import gate_table_reader  # noqa: E402  (Gates/gate_table.json reader; scripts/gate_table_reader.py)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))

SCOPE_EXTS = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".inl",
              ".hglsl", ".glsl", ".vert", ".frag", ".comp", ".tesc", ".tese", ".geom")
LINE_SENSITIVE = re.compile(r"\b(?:__LINE__|__DATE__|__TIME__|__TIMESTAMP__)\b")

# ------------------------------------------------------------------ protection rules
# (label, compiled regex). Case-sensitive entries are machine-read formats; the rest are
# rationale words, matched case-insensitively so "Why:", "MUST" and "must" all count.
_CS = [
    ("event-format", r"[A-Z_]+ v[0-9]+\]"),
    ("clang-format", r"clang-format"),
    ("NOLINT", r"NOLINT"),
    ("NOSONAR", r"NOSONAR"),
    ("Co-Authored", r"Co-Authored"),
    ("SPDX", r"SPDX"),
    ("date", r"20[0-9]{2}-[0-9]{2}"),
    ("sha", r"(?<![0-9A-Za-z])(?=[0-9a-fA-F]*[0-9])[0-9a-fA-F]{7,40}(?![0-9A-Za-z])"),
    ("tag", r"\b(?:TODO|FIXME|HACK|XXX)\b"),
    ("doxygen-tag", r"(?<![\w.])@[A-Za-z]+"),
]
_CI = [
    ("why", r"\bwhy\b"), ("because", r"\bbecause\b"), ("must", r"\bmust"),
    ("never", r"\bnever\b"), ("do-not", r"\bdo not\b"), ("dont", r"\bdon'?t\b"),
    ("race", r"\b(?:race|races|racy)\b"), ("contract", r"\bcontract"),
    ("invariant", r"\binvariant"), ("bug", r"\bbug"), ("workaround", r"\bworkaround"),
    ("deliberate", r"\bdeliberate"), ("intentional", r"\bintentional"),
]
PROTECT = ([(n, re.compile(p)) for n, p in _CS] +
           [(n, re.compile(p, re.I)) for n, p in _CI])
_PRAGMA_LINE = re.compile(r"^\s*#\s*pragma\b|\bstatic_assert\b|\b_Static_assert\b")

# Gate names are protective only when the compiled gate table registers them: `MC2_VERIFY` and
# friends are macros. Slice/phase tags (BUILDING-GLASS-1) are history, so they are masked out
# before the keyword rules run (a date such as FOO-2026-10-09 is not masked).
_MC2_TOKEN = re.compile(r"(?<![A-Za-z0-9_])MC2_[A-Z0-9_]*[A-Z0-9]")
_SLICE_TAG = re.compile(r"(?<![A-Za-z0-9_-])[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+-\d+(?![A-Za-z0-9_])")
_DATE_IN_TAG = re.compile(r"20[0-9]{2}-[0-9]{2}")
_GATE_CACHE = []        # [set | None]; None = table unreadable -> every MC2_ token protects (fail closed)

# `TODO(rewrite): ` right after the comment opener keeps a protected comment alive as a to-do.
REWRITE_TAG = "TODO(rewrite):"
TAG_GROWTH = 16
_OPENER = re.compile(r"^(//[/!]?|/\*[*!]?)\s*(.*)$", re.S)
_TAGGED = re.compile(r"^(//[/!]?|/\*[*!]?)\s*TODO\(rewrite\):\s*(.*)$", re.S)


def registered_gates():
    """Gate names from Gates/gate_table.json (loaded once); None when the table cannot be read."""
    if not _GATE_CACHE:
        try:
            _GATE_CACHE.append(set(gate_table_reader.gates(ROOT)))
        except gate_table_reader.GateTableError:
            _GATE_CACHE.append(None)
    return _GATE_CACHE[0]


def set_gate_names(names):
    """Override the registered set (tests); None restores lazy loading, False simulates an unreadable table."""
    del _GATE_CACHE[:]
    if names is not None:
        _GATE_CACHE.append(None if names is False else set(names))


def mask_slice_tags(txt):
    return _SLICE_TAG.sub(lambda m: m.group(0) if _DATE_IN_TAG.search(m.group(0)) else " " * len(m.group(0)), txt)


def _canon_line(line):
    m = _OPENER.match(line)
    return m.group(1) + " " + m.group(2) if m else line


def _detag_line(line):
    """-> (canonical line without the rewrite tag, True if it carried the tag)."""
    m = _TAGGED.match(line)
    return (m.group(1) + " " + m.group(2), True) if m else (_canon_line(line), False)


def count_tags(lx):
    return sum(1 for c in lx.comments if _TAGGED.match(c.text))


# ------------------------------------------------------------------ checker needles
# A comment can be a SENTINEL a checker script reads (`grep 'FOO-AUDIT-END'`), which Rule A would otherwise treat as
# a history tag. The needle set is built from the repo's own checker sources, once per run, and any comment that
# contains a needle verbatim (case-sensitive) is protected. Over-protecting is fine; missing a needle is not.
NEEDLE_MIN_TAG = 5
NEEDLE_MIN_WORD = 6
_NEEDLE_PREFIX = 5                  # NeedleSet indexes needles by their first five characters
_NEEDLE_TAG = re.compile(r"(?<![A-Za-z0-9_])[A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+")
_NEEDLE_WORD = re.compile(r"[A-Za-z0-9_-]+:?")
_UPPER_SEGMENTS = re.compile(r"[A-Z][A-Z0-9]*(?:-[A-Z0-9]*[A-Z][A-Z0-9]*)+")
_LOWER_STEM = re.compile(r"^[a-z0-9_]+$")
NEEDLE_STOPLIST = frozenset(["byte-identical", "gos_terrain", "static_prop", "MATERIAL", "MC2_VK"])
_NEEDLE_ESCAPE = re.compile(r"\\[A-Za-z]|\$\{?[A-Za-z_][A-Za-z_0-9]*\}?")
_GREP_CMD = re.compile(r"(?<![\w-])(?:[ef]?grep|rg|Select-String)(?![\w-])(.*)$")
_QUOTED = re.compile(r"'([^']*)'" r'|"((?:[^"\\]|\\.)*)"')
_RE_FUNCS = frozenset(["search", "match", "fullmatch", "compile", "findall", "finditer", "sub", "subn", "split"])
_STR_FUNCS = frozenset(["find", "rfind", "count", "index", "startswith", "endswith"])
_NEEDLE_SOURCES = (                 # (dir, recursive, suffixes); scanned in this order, files sorted
    ("scripts", True, (".py", ".sh")),
    ("scripts", False, (".json", ".txt", ".allowlist")),
    ("tools", True, (".py",)),
    ("tests", True, (".py", ".sh")),
    ("cmake", True, (".cmake",)),
    (".claude/hooks", False, (".py",)),
)
_NEEDLE_SKIP_DIRS = frozenset(["__pycache__", ".git", "node_modules", ".venv", "venv"])
NEEDLE_MAX_BYTES = 2 * 1024 * 1024
PIECE_HEADER_LINES = 14             # a split piece's description is read from its first 12 lines, a shared banner's from 14
_FENCED = re.compile(r"@@[^\n]*@@|={3,}[^\n=][^\n]*={3,}")
_BEGIN_END = re.compile(r"(?<![A-Za-z0-9_-])[A-Z0-9]+(?:-[A-Z0-9]+)*-(?:BEGIN|END)(?![A-Za-z0-9_])")


def _qtext(m):
    return m.group(1) if m.group(1) is not None else m.group(2)


def looks_like_marker(tok):
    """A word needle protects a comment only when it reads as a sentinel, not as prose or an identifier.

    Marker: two uppercase segments joined by `-` (TIER2-EXCLUDED), a trailing `:` (TEX-CLASS:), or >= 12 chars with a
    hyphen and an uppercase letter. Lowercase file stems and the known prose/identifier hits never qualify.
    """
    if tok in NEEDLE_STOPLIST or _LOWER_STEM.match(tok):
        return False
    return bool(_UPPER_SEGMENTS.search(tok) or tok.endswith(":")
                or (len(tok) >= 12 and "-" in tok and any(c.isupper() for c in tok)))


def _needle_words(text):
    """Marker-like words a grep/regex/membership test looks for (>= 6 chars; see looks_like_marker)."""
    out = []
    for tok in _NEEDLE_WORD.findall(_NEEDLE_ESCAPE.sub(" ", text)):
        colon = ":" if tok.endswith(":") else ""
        tok = tok.rstrip(":").strip("-_") + colon
        if len(tok) >= NEEDLE_MIN_WORD and any(c.isalpha() for c in tok) and looks_like_marker(tok):
            out.append(tok)
    return out


def _needle_tags(text):
    return [t for t in _NEEDLE_TAG.findall(text) if len(t) >= NEEDLE_MIN_TAG]


def _grep_words(text):
    """Words searched by grep/rg/Select-String on one line: quoted arguments, and bare words that are not paths or options."""
    out = []
    for line in text.split("\n"):
        m = _GREP_CMD.search(line)
        if not m:
            continue
        rest = m.group(1)
        for q in _QUOTED.finditer(rest):
            out += _needle_words(_qtext(q))
        for tok in _QUOTED.sub(" ", rest).split():
            if not tok.startswith("-") and not re.search(r"[/\\.$]", tok):
                out += _needle_words(tok)
    return out


def _strip_hash_comments(text):
    return "\n".join(l for l in text.split("\n") if not l.lstrip().startswith("#"))


def _py_consts(node):
    for n in ast.walk(node):
        if isinstance(n, ast.Constant) and isinstance(n.value, (str, bytes)):
            yield n.value.decode("latin-1") if isinstance(n.value, bytes) else n.value


def _py_needles(text):
    """-> (tags, words) from a Python checker; strings only, so its comments and docstrings add nothing."""
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        body = _strip_hash_comments(text)
        return _needle_tags(body), _grep_words(body)
    tags, words = [], []
    docs = set()
    for n in ast.walk(tree):
        if isinstance(n, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and n.body:
            f = n.body[0]
            if isinstance(f, ast.Expr) and isinstance(f.value, ast.Constant) and isinstance(f.value.value, str):
                docs.add(id(f.value))
    for n in ast.walk(tree):
        if isinstance(n, ast.Constant) and isinstance(n.value, (str, bytes)) and id(n) not in docs:
            s = n.value.decode("latin-1") if isinstance(n.value, bytes) else n.value
            tags += _needle_tags(s)
            if _GREP_CMD.search(s):
                words += _grep_words(s)
    wanted, names = [], set()           # expressions whose constants are search words; names to resolve afterwards

    def want(e):
        if isinstance(e, ast.Name):
            names.add(e.id)
        else:
            wanted.append(e)

    for n in ast.walk(tree):
        if isinstance(n, ast.Call) and n.args:
            f = n.func
            attr = f.attr if isinstance(f, ast.Attribute) else None
            if attr in _RE_FUNCS and isinstance(f.value, ast.Name) and f.value.id == "re":
                want(n.args[0])
            elif attr in _STR_FUNCS:
                want(n.args[0])
        elif isinstance(n, ast.Compare) and any(isinstance(o, (ast.In, ast.NotIn)) for o in n.ops):
            want(n.left)
    for n in ast.walk(tree):
        if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in names for t in n.targets):
            wanted.append(n.value)
        elif isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name) and n.target.id in names and n.value:
            wanted.append(n.value)
        elif isinstance(n, (ast.For, ast.AsyncFor, ast.comprehension)) and isinstance(n.target, ast.Name) and n.target.id in names:
            wanted.append(n.iter)
    for e in wanted:
        for s in _py_consts(e):
            words += _needle_words(s)
    return tags, words


def needles_from_source(rel, text):
    """-> {needle: kind} for one checker file (kind is 'tag' or 'word'). Pure; the directory walk lives in collect_needles."""
    low = rel.lower()
    if low.endswith(".py"):
        tags, words = _py_needles(text)
    elif low.endswith(".json"):
        tags, words = _needle_tags(text), []
    elif low.endswith(".sh"):
        body = _strip_hash_comments(text)
        tags, words = _needle_tags(body), _grep_words(body)
    elif low.endswith(".cmake"):
        body = _strip_hash_comments(text)
        tags = _needle_tags(body)
        words = [w for l in body.split("\n") if re.search(r"MATCHES|FIND|REGEX", l)
                 for q in _QUOTED.finditer(l) for w in _needle_words(_qtext(q))]
    else:
        tags, words = _needle_tags(_strip_hash_comments(text)), []
    out = {}
    for w in words:
        out[w] = "word"
    for t in tags:
        out[t] = "tag"
    return out


def _is_test_source(rel):
    base = os.path.basename(rel)
    return base.startswith("test_") or base.endswith("_test.py") or base == "conftest.py"


def checker_sources(root):
    """-> repo-relative checker files, each source group sorted."""
    seen, out = set(), []
    for sub, recursive, exts in _NEEDLE_SOURCES:
        top = os.path.join(root, *sub.split("/"))
        hits = []
        for d, dirs, files in os.walk(top):
            dirs[:] = sorted(x for x in dirs if x not in _NEEDLE_SKIP_DIRS and not x.startswith("build"))
            for f in files:
                if f.lower().endswith(exts):
                    hits.append(os.path.relpath(os.path.join(d, f), root).replace("\\", "/"))
            if not recursive:
                break
        for rel in sorted(hits):
            if rel in seen or rel == "tools/comment_guard.py":
                continue
            if sub == "tools" and _is_test_source(rel):
                continue
            seen.add(rel)
            out.append(rel)
    return out


def collect_needles(root):
    """-> ({needle: first defining file}, files scanned)."""
    found, files = {}, checker_sources(root)
    for rel in files:
        p = os.path.join(root, rel)
        try:
            if os.path.getsize(p) > NEEDLE_MAX_BYTES:
                continue
        except OSError:
            continue
        data = read_file(p)
        if data is None:
            continue
        for needle in needles_from_source(rel, data.decode("utf-8", "replace")):
            found.setdefault(needle, rel)
    return found, len(files)


EXTERNAL_NEEDLES_REL = "tools/comment_guard_needles.txt"
EXTERNAL_LABEL = EXTERNAL_NEEDLES_REL + " (external)"
_NEEDLE_LINE = re.compile(r"^(.*\S)\s+#\s*(\S.*)$")
_AFX_MARKER = re.compile(r"//\s*[{}]{2}AFX_")      # MFC ClassWizard parses //{{AFX_...  and //}}AFX_...


def parse_needle_file(text, name=EXTERNAL_NEEDLES_REL):
    """-> ({literal: label}, [(compiled regex, 're:...', label)]); ValueError names the first malformed line.

    One needle per line, `needle  # reason`; a `re:` prefix makes it a regex. Blank lines and `#` lines are skipped.
    """
    lits, regexes = {}, []
    for i, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = _NEEDLE_LINE.match(line)
        if not m:
            raise ValueError("%s:%d: expected `needle  # reason`, got: %s" % (name, i, show(line, 60)))
        needle = m.group(1)
        if needle.startswith("re:"):
            try:
                rx = re.compile(needle[3:])
            except re.error as e:
                raise ValueError("%s:%d: bad regex: %s" % (name, i, e))
            if not needle[3:] or rx.search(""):
                raise ValueError("%s:%d: regex matches the empty string" % (name, i))
            regexes.append((rx, needle, EXTERNAL_LABEL))
        elif len(needle) < NEEDLE_MIN_TAG:
            raise ValueError("%s:%d: literal needle shorter than %d characters" % (name, i, NEEDLE_MIN_TAG))
        else:
            lits[needle] = EXTERNAL_LABEL
    return lits, regexes


def load_external_needles(root):
    data = read_file(os.path.join(root, *EXTERNAL_NEEDLES_REL.split("/")))
    return parse_needle_file(data.decode("utf-8", "replace")) if data is not None else ({}, [])


class NeedleSet:
    def __init__(self, mapping, regexes=()):
        self.map = dict(mapping)
        self.regexes = list(regexes)
        self.by_prefix = {}
        for n in self.map:
            if len(n) >= _NEEDLE_PREFIX:
                self.by_prefix.setdefault(n[:_NEEDLE_PREFIX], []).append(n)

    def __len__(self):
        return len(self.map) + len(self.regexes)

    def find_regex(self, text):
        """-> [(matched text, label)] for the external regex needles."""
        out = []
        for rx, _src, label in self.regexes:
            m = rx.search(text)
            if m:
                out.append((m.group(0), label))
        return out

    def find(self, text):
        """Needles occurring verbatim in text, longest first."""
        hits = set()
        if self.by_prefix:
            for i in range(len(text) - _NEEDLE_PREFIX + 1):
                for n in self.by_prefix.get(text[i:i + _NEEDLE_PREFIX], ()):
                    if text.startswith(n, i):
                        hits.add(n)
        return sorted(hits, key=lambda n: (-len(n), n))


def split_pieces(root):
    """-> [(dir, stem)] from tools/split/allow/*.json: files `<stem>.*` and `<stem>_*.*` beside `src` are split pieces."""
    out = []
    for p in sorted(glob.glob(os.path.join(glob.escape(root), "tools", "split", "allow", "*.json"))):
        try:
            with open(p, "r", encoding="utf-8") as fh:
                d = json.load(fh)
        except (OSError, ValueError):
            continue
        if isinstance(d, dict) and isinstance(d.get("src"), str) and isinstance(d.get("stem"), str):
            out.append((os.path.dirname(_norm_rel(d["src"])), d["stem"]))
    return out


class Env:
    """Repo-derived protection inputs: the needle set and the split-piece stems (both loaded once per run)."""

    def __init__(self, needles, pieces, regexes=()):
        self.needles = needles if isinstance(needles, NeedleSet) else NeedleSet(needles, regexes)
        self.pieces = list(pieces)

    def is_piece(self, rel):
        rel = _norm_rel(rel)
        d, f = os.path.dirname(rel), os.path.splitext(os.path.basename(rel))[0]
        return any(d == pd and (f == st or f.startswith(st + "_")) for pd, st in self.pieces)


_ENV = {"root": ROOT, "env": None}


def use_repo(root):
    """Point the lazily loaded needle set and piece stems at another repo root."""
    root = os.path.abspath(root)
    if _ENV["root"] != root:
        _ENV.update(root=root, env=None)


def set_env(env):
    """Pin an Env (tests); None restores lazy loading from the current root."""
    _ENV["env"] = env


def current_env():
    if _ENV["env"] is None:
        mapping, _n = collect_needles(_ENV["root"])
        lits, regexes = load_external_needles(_ENV["root"])
        for n, label in lits.items():
            mapping.setdefault(n, label)
        _ENV["env"] = Env(mapping, split_pieces(_ENV["root"]), regexes)
    return _ENV["env"]


def run_needles(args):
    repo = os.path.abspath(args.repo or ROOT)
    mapping, nfiles = collect_needles(repo)
    try:
        lits, regexes = load_external_needles(repo)
    except ValueError as e:
        print("comment_guard: %s" % e, file=sys.stderr)
        return 2
    for n, label in lits.items():
        mapping.setdefault(n, label)
    rows = sorted(list(mapping.items()) + [(src, label) for _rx, src, label in regexes])
    for n, where in rows:
        print("%s\t%s" % (n, where))
    print("comment_guard: %d checker needle(s) from %d file(s)" % (len(rows), nfiles), file=sys.stderr)
    return 0


# ------------------------------------------------------------------ lexer
_WS = re.compile(r"[ \t\r\f\v]+")
_IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
_NUM = re.compile(r"(?:[0-9]|\.[0-9])(?:[eEpP][+-]|'[A-Za-z0-9_]|[A-Za-z0-9_.])*")
_INCL = re.compile(r"#[ \t]*(?:include_next|include|import)[ \t]*<[^>\n]*>")
_RAWOPEN = re.compile(r'"([^ ()\\\t\v\f\n]{0,16})\(')
_CHAR = re.compile(r"'(?:\\[^\n]|[^'\\\n])*'")
_RAW_PREFIX = frozenset(["R", "LR", "uR", "UR", "u8R"])


def collapse(s):
    return _WS.sub(" ", s).strip()


def _enc(s, newlines=False):
    """Protect whitespace inside a literal from collapsing: a changed literal is a code change."""
    s = s.replace(" ", "\x02").replace("\t", "\x03")
    return s.replace("\n", "\x01") if newlines else s


def show(s, limit=140):
    s = s.replace("\x02", " ").replace("\x03", "\t").replace("\x01", "\\n")
    return s if len(s) <= limit else s[:limit - 3] + "..."


class Comment:
    __slots__ = ("kind", "start", "end", "text", "trailing", "code_after", "pre_code")

    @property
    def full_line(self):
        return not self.trailing and not self.code_after

    @property
    def nbytes(self):
        return len(self.text)


class Lexed:
    def __init__(self):
        self.logical = []       # [(first physical line, collapsed code text)], non-blank only
        self.comments = []
        self.code_by_line = {}  # physical line -> code text on it (for pragma adjacency)
        self.errors = []


class _Lexer:
    def __init__(self, t):
        self.t = t
        self.n = len(t)
        self.i = 0
        self.line = 1
        self.cur = []
        self.cur_line = 1
        self.cur_has_code = False
        self.seen_code = False
        self.out = Lexed()

    def emit(self, s):
        self.cur.append(s)
        self.out.code_by_line.setdefault(self.line, []).append(s)
        if s.strip(" \t\r\f\v"):
            if not self.cur_has_code:
                self.cur_line = self.line
            self.cur_has_code = True
            self.seen_code = True
            for c in reversed(self.out.comments):
                if c.end != self.line:
                    break
                if c.kind == "block":
                    c.code_after = True

    def end_logical(self):
        txt = collapse("".join(self.cur))
        if txt:
            self.out.logical.append((self.cur_line, txt))
        self.cur = []
        self.cur_has_code = False

    def add_comment(self, kind, text):
        c = Comment()
        c.kind, c.text, c.start = kind, text, self.line
        c.end = self.line + text.count("\n")
        c.trailing = "".join(self.out.code_by_line.get(self.line, [])).strip() != ""
        c.code_after = False
        c.pre_code = not self.seen_code
        self.out.comments.append(c)
        self.line = c.end
        self.emit(" ")

    def string_end(self, i):
        t, n = self.t, self.n
        j = i + 1
        while j < n:
            c = t[j]
            if c == "\\":
                j += 2
                continue
            if c == '"':
                return j + 1
            if c == "\n":
                return None
            j += 1
        return None

    def run(self):
        t, n = self.t, self.n
        while self.i < n:
            i = self.i
            c = t[i]
            if c == "\n":
                self.end_logical()
                self.line += 1
                self.cur_line = self.line
                self.i += 1
            elif c == "\\" and t.startswith("\n", i + 1):
                self.line += 1
                self.i += 2
            elif c in " \t\r\f\v":
                self.emit(" ")
                self.i += 1
            elif c == "/" and t.startswith("/", i + 1):
                j = i
                while j < n and not (t[j] == "\n" and t[j - 1] != "\\"):
                    j += 1
                self.add_comment("line", t[i:j])
                self.i = j
            elif c == "/" and t.startswith("*", i + 1):
                j = t.find("*/", i + 2)
                if j < 0:
                    self.out.errors.append("unterminated /* comment starting at line %d" % self.line)
                    j = n
                else:
                    j += 2
                self.add_comment("block", t[i:j])
                self.i = j
            elif c == "#" and not self.cur_has_code and _INCL.match(t, i):
                m = _INCL.match(t, i)
                self.emit(m.group(0))
                self.i = m.end()
            elif c == '"':
                self.literal_string(i)
            elif c == "'":
                m = _CHAR.match(t, i)
                if m:
                    self.emit_literal(m.group(0), False)
                    self.i = m.end()
                else:
                    self.emit(c)
                    self.i += 1
            elif c in "0123456789" or (c == "." and i + 1 < n and t[i + 1] in "0123456789"):
                m = _NUM.match(t, i)
                self.emit(m.group(0))
                self.i = m.end()
            elif c == "_" or c.isalpha():
                m = _IDENT.match(t, i)
                if m and m.group(0) in _RAW_PREFIX and t.startswith('"', m.end()):
                    self.emit(m.group(0))
                    self.raw_string(m.end())
                elif m:
                    self.emit(m.group(0))
                    self.i = m.end()
                else:
                    self.emit(c)
                    self.i += 1
            else:
                self.emit(c)
                self.i += 1
        self.end_logical()
        return self.out

    def emit_literal(self, text, newlines):
        body = text.replace("\\\n", "")           # a spliced newline is not part of the literal
        self.emit(_enc(body, newlines))
        self.line += text.count("\n")

    def literal_string(self, i):
        j = self.string_end(i)
        if j is None:
            self.emit('"')
            self.i = i + 1
            return
        self.emit_literal(self.t[i:j], False)
        self.i = j

    def raw_string(self, i):
        m = _RAWOPEN.match(self.t, i)
        if not m:
            self.emit('"')
            self.i = i + 1
            return
        close = ")" + m.group(1) + '"'
        j = self.t.find(close, m.end())
        if j < 0:
            self.out.errors.append("unterminated raw string starting at line %d" % self.line)
            j = self.n
        else:
            j += len(close)
        start_line = self.line
        text = self.t[i:j]
        self.emit(_enc(text, True))
        self.line = start_line + text.count("\n")
        self.i = j


def lex(text):
    return _Lexer(text).run()


# ------------------------------------------------------------------ comment units
class Unit:
    __slots__ = ("lines", "start", "end", "header", "reasons")

    def text(self):
        return "\n".join(self.lines)


def _norm_lines(text):
    return tuple(l for l in (collapse(x) for x in text.split("\n")) if l)


def build_units(lx, env=None, rel=""):
    """Group comments into the smallest pieces that must be kept or dropped whole.

    A block comment is one unit; consecutive full-line `//` comments form one unit, split at
    bare `//` separator lines so a paragraph of narration can go without touching its neighbour.
    """
    units, run = [], None

    def close():
        nonlocal run
        if run:
            units.append(run)
            run = None

    def new(c):
        u = Unit()
        u.lines = _norm_lines(c.text)
        u.start, u.end, u.header, u.reasons = c.start, c.end, c.pre_code, []
        return u

    for c in lx.comments:
        if c.kind == "line" and c.full_line:
            if not c.text[2:].strip():
                close()
                continue
            if run and run.end + 1 == c.start and run.header == c.pre_code:
                run.lines += _norm_lines(c.text)
                run.end = c.end
            else:
                close()
                run = new(c)
        else:
            close()
            units.append(new(c))
    close()
    for u in units:
        mark_protected(u, lx, env, rel)
    return units


def mark_protected(u, lx, env=None, rel=""):
    txt = u.text()
    if u.header:
        u.reasons.append("file header")
    if env is not None:
        for n in env.needles.find(txt)[:3]:
            u.reasons.append("checker-needle `%s` read by %s" % (n, env.needles.map.get(n) or "a checker script"))
        for hit, label in env.needles.find_regex(txt)[:3]:
            u.reasons.append("checker-needle `%s` read by %s" % (show(hit, 60), label))
        if env.is_piece(rel) and u.start <= PIECE_HEADER_LINES:
            u.reasons.append("split-piece header (read by split_map)")
    if _AFX_MARKER.search(txt):
        u.reasons.append("MFC ClassWizard marker")
    if _BEGIN_END.search(txt):
        u.reasons.append("begin/end marker")
    if _FENCED.search(txt):
        u.reasons.append("fenced marker")
    gates = registered_gates()
    for tok in _MC2_TOKEN.findall(txt):
        if gates is None or tok in gates:
            u.reasons.append("gate-name")
            break
    masked = mask_slice_tags(txt)
    for name, rx in PROTECT:
        if rx.search(masked):
            u.reasons.append(name)
    for ln in range(u.start - 1, u.end + 2):
        code = "".join(lx.code_by_line.get(ln, []))
        if _PRAGMA_LINE.search(code):
            u.reasons.append("pragma/static_assert neighbour")
            break


def _contains(hay, needle):
    k = len(needle)
    return any(hay[i:i + k] == needle for i in range(len(hay) - k + 1))


def missing_protected(base_units, new_units):
    """Protected base units with no verbatim home in the new text -> [Unit].

    A unit also survives as a rewrite candidate: the same text with `TODO(rewrite): ` inserted right
    after the opener of its first line (whitespace after the opener is not significant).
    """
    pool = Counter(u.lines for u in new_units)
    spare = [u.lines for u in new_units]
    detagged = []                   # per new unit: ([canonical lines], [line carried the tag])
    for lines in spare:
        pairs = [_detag_line(l) for l in lines]
        detagged.append(([p[0] for p in pairs], [p[1] for p in pairs]))
    used_tags = set()
    lost = []
    for u in base_units:
        if not u.reasons:
            continue
        if pool[u.lines] > 0:
            pool[u.lines] -= 1
        elif any(_contains(s, u.lines) for s in spare):
            continue
        elif not _take_tagged(u.lines, detagged, used_tags):
            lost.append(u)
    return lost


def _take_tagged(lines, detagged, used):
    want = [_canon_line(l) for l in lines]
    k = len(want)
    for ui, (dl, tg) in enumerate(detagged):
        for i in range(len(dl) - k + 1):
            if tg[i] and (ui, i) not in used and dl[i:i + k] == want:
                used.add((ui, i))
                return True
    return False


# ------------------------------------------------------------------ per-file check
class Report:
    def __init__(self, rel):
        self.rel = rel
        self.findings = []       # [(kind, message)]
        self.base_cb = None
        self.new_cb = None
        self.allowed = []        # [(base line, reason)] protected comments deleted under --allow
        self.notes = []          # informational lines for the reviewer; never a finding

    @property
    def ok(self):
        return not self.findings

    def add(self, kind, msg):
        self.findings.append((kind, msg))


def in_scope(rel):
    return rel.lower().endswith(SCOPE_EXTS)


def is_gates(rel):
    return "gates" in [p.lower() for p in rel.replace("\\", "/").split("/")[:-1]]


def prep(data):
    """bytes -> (text, bom, eol style). Latin-1 keeps every byte distinct and sizes honest."""
    bom = data.startswith(b"\xef\xbb\xbf")
    if bom:
        data = data[3:]
    text = data.decode("latin-1")
    crlf = text.count("\r\n")
    lf = text.count("\n") - crlf
    style = "none" if not (crlf or lf) else "crlf" if not lf else "lf" if not crlf else "mixed"
    return text.replace("\r\n", "\n"), bom, style


ALLOW_TOLERANCE = 2


def _norm_rel(p):
    p = str(p).replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


class Allowlist:
    """Reviewer-written permission to delete named protected comments (never written by the editor).

    Each entry covers at most one deleted comment: its file plus its base line, +-ALLOW_TOLERANCE.
    """

    def __init__(self, entries):
        self.entries = [{"file": _norm_rel(e["file"]), "line": e["base_line"], "reason": e["reason"],
                         "used": False} for e in entries]

    @classmethod
    def load(cls, path):
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        if not isinstance(doc, list):
            raise ValueError("allowlist must be a JSON list")
        for i, e in enumerate(doc):
            if (not isinstance(e, dict) or not isinstance(e.get("file"), str) or not e["file"]
                    or isinstance(e.get("base_line"), bool) or not isinstance(e.get("base_line"), int)
                    or not isinstance(e.get("reason"), str) or not e["reason"].strip()):
                raise ValueError("entry %d needs file (str), base_line (int), reason (non-empty str)" % i)
        return cls(doc)

    def take(self, rel, line):
        rel = _norm_rel(rel)
        best = None
        for e in self.entries:
            if e["used"] or e["file"] != rel or abs(e["line"] - line) > ALLOW_TOLERANCE:
                continue
            if best is None or abs(e["line"] - line) < abs(best["line"] - line):
                best = e
        if best:
            best["used"] = True
        return best

    def stale(self, checked_rels):
        """Unmatched entries for files that were part of this run (entries for other files are not judged)."""
        checked = set(_norm_rel(r) for r in checked_rels)
        return [e for e in self.entries if not e["used"] and e["file"] in checked]


def check_texts(rel, base, new, allow=None, env=None):
    """Grade one file. base/new are bytes (None = absent). Returns a Report."""
    rep = Report(rel)
    if not in_scope(rel):
        rep.add("scope", "out of scope (only C/C++/GLSL sources are guarded)")
        return rep
    if base is None:
        rep.add("no-base", "no base text (new or untracked file): a comment-only edit needs a base")
        return rep
    if new is None:
        rep.add("no-new", "file is absent in the new text (deleted?)")
        return rep
    if is_gates(rel):
        if base != new:
            rep.add("gates", "Gates/ files are wholly protected; any change is rejected")
        return rep

    bt, bbom, bstyle = prep(base)
    nt, nbom, nstyle = prep(new)
    if bbom != nbom:
        rep.add("format", "UTF-8 BOM %s" % ("added" if nbom else "removed"))
    if "none" not in (bstyle, nstyle) and bstyle != nstyle:
        if "mixed" in (bstyle, nstyle):
            rep.add("format", "line endings are mixed within a file (%s -> %s)" % (bstyle, nstyle))
        else:
            rep.notes.append(EOL_NOTE)      # autocrlf artifact: uniform on both sides, code compared after CRLF -> LF

    bl, nl = lex(bt), lex(nt)
    for side, lx in (("base", bl), ("new", nl)):
        for e in lx.errors:
            rep.add("lex", "%s: %s" % (side, e))

    rep.base_cb = sum(c.nbytes for c in bl.comments)
    rep.new_cb = sum(c.nbytes for c in nl.comments)

    a = [x for x in bl.logical]
    b = [x for x in nl.logical]
    for k in range(max(len(a), len(b))):
        x = a[k] if k < len(a) else None
        y = b[k] if k < len(b) else None
        if x is None or y is None or x[1] != y[1]:
            rep.add("code", "code differs: base %s | new %s" % (
                ("line %d `%s`" % (x[0], show(x[1]))) if x else "<end of file>",
                ("line %d `%s`" % (y[0], show(y[1]))) if y else "<end of file>"))
            break

    env = env or current_env()
    lost = missing_protected(build_units(bl, env, rel), build_units(nl, env, rel))
    if allow is not None:
        kept = []
        for u in lost:
            hit = allow.take(rel, u.start)
            if hit:
                rep.allowed.append((u.start, hit["reason"]))
            else:
                kept.append(u)
        lost = kept
    for u in lost[:6]:
        rep.add("protected", "protected comment (%s) missing at base line %d: %s" % (
            ", ".join(sorted(set(u.reasons))), u.start, show(u.text().replace("\n", " / "), 120)))
    if len(lost) > 6:
        rep.add("protected", "... and %d more protected comments missing" % (len(lost) - 6))

    new_tags = max(0, count_tags(nl) - count_tags(bl))
    if rep.new_cb > rep.base_cb + TAG_GROWTH * new_tags:
        rep.add("grew", "comment bytes grew %d -> %d (this tool only shrinks; %d new TODO(rewrite) tag(s) allow +%d)" % (
            rep.base_cb, rep.new_cb, new_tags, TAG_GROWTH * new_tags))
    return rep


# ------------------------------------------------------------------ git / fs sources
def _git(repo, args):
    r = subprocess.run(["git"] + args, cwd=repo, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       **console_child.child_kwargs(redirected=True))
    return r.returncode, r.stdout


def git_blob(repo, spec):
    rc, out = _git(repo, ["cat-file", "blob", spec])
    return out if rc == 0 else None


def read_file(path):
    try:
        with open(path, "rb") as fh:
            return fh.read()
    except OSError:
        return None


def rel_of(repo, path):
    ap = os.path.abspath(path)
    try:
        r = os.path.relpath(ap, repo)
    except ValueError:
        return None
    return None if r.startswith("..") else r.replace("\\", "/")


def changed_files(repo, base, staged):
    args = ["diff", "--name-only", "-z"] + (["--cached"] if staged else []) + [base, "--"]
    rc, out = _git(repo, args)
    if rc != 0:
        return None
    return [p.decode("utf-8", "replace") for p in out.split(b"\0") if p]


EOL_NOTE = "note: line endings normalised (CRLF↔LF)"


def _note_lines(rep):
    enc = getattr(sys.stdout, "encoding", None) or "ascii"
    out = []
    for n in rep.notes:
        try:
            n.encode(enc)
        except (UnicodeEncodeError, LookupError):
            n = n.replace("↔", "<->")
        out.append("      " + n)
    return out


def fmt_report(rep):
    if rep.base_cb is not None:
        pct = "%+.1f%%" % ((rep.new_cb - rep.base_cb) * 100.0 / rep.base_cb) if rep.base_cb else "n/a"
        size = "  comments %d -> %d B (%s)" % (rep.base_cb, rep.new_cb, pct)
    else:
        size = ""
    lines = ["%-5s %s%s" % ("OK" if rep.ok else "FAIL", rep.rel, size)]
    lines += ["      - [%s] %s" % f for f in rep.findings]
    lines += _note_lines(rep)
    lines += _allowed_lines(rep)
    return "\n".join(lines)


def _allowed_lines(rep):
    return ["ALLOWED-DELETE %s:%d %s" % (rep.rel, ln, why) for ln, why in rep.allowed]


def run_check(args):
    repo = os.path.abspath(args.repo or ROOT)
    use_repo(repo)
    try:
        current_env()
    except ValueError as e:
        print("comment_guard: %s" % e, file=sys.stderr)
        return 2
    base_rev = args.base
    reports = []
    allow = None
    if args.allow:
        try:
            allow = Allowlist.load(args.allow)
        except (OSError, ValueError) as e:
            print("comment_guard: cannot use --allow %s: %s" % (args.allow, e), file=sys.stderr)
            return 2
    paths = list(args.paths)
    if args.base_file:
        if len(paths) != 1:
            print("comment_guard: --base-file needs exactly one path (the edited copy)", file=sys.stderr)
            return 2
        base_bytes = read_file(args.base_file)
        if base_bytes is None:
            print("comment_guard: cannot read --base-file %s" % args.base_file, file=sys.stderr)
            return 2
        rel = rel_of(repo, paths[0]) or os.path.abspath(paths[0]).replace("\\", "/")
        reports.append(check_texts(rel, base_bytes, read_file(paths[0]), allow))
    else:
        if not paths:
            names = changed_files(repo, base_rev, args.staged)
            if names is None:
                print("comment_guard: git diff failed against %r" % base_rev, file=sys.stderr)
                return 2
            paths = [os.path.join(repo, n) for n in names]
        if not paths:
            print("comment_guard: no changed files against %s" % base_rev)
            return 0
        for p in paths:
            rel = rel_of(repo, p)
            if rel is None:
                print("comment_guard: %s is outside the repo %s (use --base-file)" % (p, repo), file=sys.stderr)
                return 2
            if args.ignore_out_of_scope and not in_scope(rel):
                continue
            base = git_blob(repo, "%s:%s" % (base_rev, rel))
            new = git_blob(repo, ":" + rel) if args.staged else read_file(os.path.join(repo, rel))
            reports.append(check_texts(rel, base, new, allow))
    bad = 0
    for rep in reports:
        if not args.quiet or not rep.ok:
            print(fmt_report(rep))
        elif rep.allowed or rep.notes:
            print("\n".join(_note_lines(rep) + _allowed_lines(rep)))
        bad += 0 if rep.ok else 1
    print("comment_guard: %d file(s) checked, %d ok, %d FAILED" % (len(reports), len(reports) - bad, bad))
    stale = allow.stale(r.rel for r in reports) if allow else []
    for e in stale:
        print("STALE-ALLOW %s:%d %s (matched no missing protected comment)" % (e["file"], e["line"], e["reason"]),
              file=sys.stderr)
    if stale:
        return 2
    return 1 if bad else 0


# ------------------------------------------------------------------ rewrite candidates
def rewrite_candidates(rel, data):
    """-> ["rel:line: first 80 chars of the comment after the tag"] for every TODO(rewrite) comment."""
    out = []
    for c in lex(prep(data)[0]).comments:
        m = _TAGGED.match(c.text)
        if m:
            body = collapse(m.group(2).replace("*/", " ").replace("\n", " "))
            out.append("%s:%d: %s" % (rel, c.start, body[:80]))
    return out


def run_rewrite_candidates(args):
    repo = os.path.abspath(args.repo or ROOT)
    rels = []
    if args.paths:
        for p in args.paths:
            r = rel_of(repo, p)
            if r is None:
                print("comment_guard: %s is outside the repo" % p, file=sys.stderr)
                return 2
            rels.append(r)
    else:
        rc, out = _git(repo, ["grep", "-l", "-F", REWRITE_TAG, "--"])
        if rc not in (0, 1):
            print("comment_guard: git grep failed", file=sys.stderr)
            return 2
        rels = [p for p in out.decode("utf-8", "replace").splitlines() if in_scope(p)]
    n = 0
    for rel in sorted(rels):
        data = read_file(os.path.join(repo, rel))
        if data is None or not in_scope(rel):
            continue
        for line in rewrite_candidates(rel, data):
            print(line)
            n += 1
    print("comment_guard: %d rewrite candidate(s)" % n, file=sys.stderr)
    return 0


# ------------------------------------------------------------------ object proof
# The COFF digest is shared with tools/tidy/run_tidy_safe.py, so it lives in tools/objhash.py.
sys.path.append(HERE)
import objhash  # noqa: E402

parse_coff = objhash.parse_coff
obj_digest = objhash.obj_digest
find_objs = objhash.find_objs
compare_files = objhash.compare_files


def _line_warn(repo, rel):
    data = read_file(os.path.join(repo, rel))
    if data is None:
        return None
    lx = lex(prep(data)[0])
    used = sorted(set(m.group(0) for _l, t in lx.logical for m in LINE_SENSITIVE.finditer(t)))
    return ("uses %s: line shifts change the object" % "/".join(used)) if used else None


def hash_files(repo, build_dir, rels):
    """-> (document, problems[str])"""
    return objhash.hash_files(repo, build_dir, rels, tool="comment_guard", warn_fn=_line_warn)


def run_hash_objs(args):
    repo = os.path.abspath(args.repo or ROOT)
    if not os.path.isdir(args.hash_objs):
        print("comment_guard: build dir %s not found" % args.hash_objs, file=sys.stderr)
        return 2
    rels = []
    for p in args.paths:
        r = rel_of(repo, p)
        if r is None:
            print("comment_guard: %s is outside the repo" % p, file=sys.stderr)
            return 2
        rels.append(r)
    if not rels:
        print("comment_guard: --hash-objs needs at least one source path", file=sys.stderr)
        return 2
    doc, problems = hash_files(repo, args.hash_objs, rels)
    json.dump(doc, sys.stdout, indent=1, sort_keys=True)
    sys.stdout.write("\n")
    for pr in problems:
        print("comment_guard: " + pr, file=sys.stderr)
    return 2 if problems else 0


def run_compare_objs(args):
    try:
        with open(args.compare_objs, "r", encoding="utf-8") as fh:
            before = json.load(fh)
    except (OSError, ValueError) as e:
        print("comment_guard: cannot read %s: %s" % (args.compare_objs, e), file=sys.stderr)
        return 2
    lines, bad = compare_files(before)
    print("\n".join(lines))
    print("comment_guard: object compare, %d problem(s)" % bad)
    return 1 if bad else 0


# ------------------------------------------------------------------ selftest
SELFTEST_CASES = []


def case(fn):
    SELFTEST_CASES.append(fn)
    return fn


_NO_ENV = Env({}, [])      # fixtures are hermetic: no repo needles or split stems unless a case pins its own


def R(base, new, rel="code/x.cpp", allow=None, env=_NO_ENV):
    enc = lambda s: None if s is None else s.encode("utf-8") if isinstance(s, str) else s  # noqa: E731
    return check_texts(rel, enc(base), enc(new), allow, env)


def _kinds(rep):
    return sorted(set(k for k, _m in rep.findings))


def _msgs(rep):
    return " || ".join(m for _k, m in rep.findings)


@case
def st_comment_only_deletion_passes():
    base = "int g;\n// loop over the items\nint f() {\n\tint x = 1; // one\n\t/* scratch */\n\treturn x;\n}\n"
    new = "int g;\nint f() {\n\tint x = 1;\n\treturn x;\n}\n"
    rep = R(base, new)
    assert rep.ok, _msgs(rep)
    assert rep.new_cb < rep.base_cb


@case
def st_string_literal_with_slashes_is_code():
    rep = R('const char* s = "// not a comment"; // note\n', 'const char* s = "";\n')
    assert _kinds(rep) == ["code"], _msgs(rep)
    assert R('const char* s = "// not a comment"; // note\n',
             'const char* s = "// not a comment";\n').ok


@case
def st_char_literal_and_digit_separator():
    base = "int a = 1'000'000; // big\nchar c = '\"'; // quote\nint b = 2;\n"
    assert R(base, "int a = 1'000'000;\nchar c = '\"';\nint b = 2;\n").ok
    assert _kinds(R(base, "int a = 1'000'000;\nchar c = 'x';\nint b = 2;\n")) == ["code"]


@case
def st_protected_deletion_fails_with_text():
    base = "// the lock must be held across both calls\nlock();\n"
    rep = R(base, "lock();\n")
    assert _kinds(rep) == ["protected"], _msgs(rep)
    assert "the lock must be held across both calls" in _msgs(rep)


@case
def st_protected_pattern_families():
    samples = ["[TERRAIN v1] fields: a b", "MC2_GPU_CULL gates it", "see 3df9e4c2d for context",
               "fixed 2026-10-09", "TODO later", "NOLINT(readability)", "@param x the thing",
               "do not reorder", "this avoids a race", "Co-Authored by", "SPDX-License-Identifier: MIT",
               "because the driver lies", "WHY: stride", "workaround for AMD"]
    for s in samples:
        rep = R("int a;\n// %s\nint b;\n" % s, "int a;\nint b;\n")
        assert _kinds(rep) == ["protected"], s
    for s in ["loop over items", "increment i", "debug output", "trace the path"]:
        assert R("int a;\n// %s\nint b;\n" % s, "int a;\nint b;\n").ok, s


@case
def st_adding_comment_bytes_fails():
    rep = R("int a;\n", "int a; // brand new\n")
    assert _kinds(rep) == ["grew"], _msgs(rep)


@case
def st_multiline_block_comment():
    base = "int f() {\n  /* line one\n     line two\n     line three */\n  return 1;\n}\n"
    assert R(base, "int f() {\n  return 1;\n}\n").ok
    mid = "foo(a, /* explain\n  b */ b);\n"
    assert R(mid, "foo(a, b);\n").ok
    assert _kinds(R("a/**/b;\n", "ab;\n")) == ["code"]


@case
def st_raw_string_with_comment_markers():
    base = 'const char* s = R"(// inside\n/* also */)"; // trailing\nint q;\n'
    assert R(base, 'const char* s = R"(// inside\n/* also */)";\nint q;\n').ok
    assert _kinds(R(base, 'const char* s = R"(/* also */)";\nint q;\n')) == ["code"]
    d = 'x = R"xx(a)"b)xx"; // c\n'
    assert R(d, 'x = R"xx(a)"b)xx";\n').ok


@case
def st_gates_files_rejected():
    rep = R("// plain\nint a;\n", "int a;\n", rel="Gates/gate_table.cpp")
    assert _kinds(rep) == ["gates"], _msgs(rep)
    assert R("int a;\n", "int a;\n", rel="Gates/gate_table.cpp").ok
    assert _kinds(R("// plain\nint a;\n", "int a;\n", rel="code/Gates/x.h")) == ["gates"]


@case
def st_blank_line_only_diff_passes():
    assert R("int a;\n\n\n\nint b;\n", "int a;\nint b;\n").ok
    assert R("int a;\nint b;\n", "\nint a;\n\n\nint b;\n\n").ok


@case
def st_crlf_files():
    base = "int z;\r\n// narration\r\nint a;\r\nint b; // more\r\n"
    assert R(base, "int z;\r\nint a;\r\nint b;\r\n").ok
    rep = R(base, "int z;\nint a;\nint b;\n")        # uniform CRLF -> uniform LF is an autocrlf artifact, not a finding
    assert rep.ok and rep.notes == [EOL_NOTE], _msgs(rep)
    assert R("int z;\nint a; // x\n", "int z;\r\nint a;\r\n").ok
    assert "line endings normalised" in fmt_report(rep)
    assert _kinds(R(base, "int z;\nint a;\nint c;\n")) == ["code"]                  # a code change is still caught
    assert _kinds(R(base, "int z;\r\nint a;\nint b;\r\n")) == ["format"]            # mixed endings fail
    assert _kinds(R("int z;\r\nint a;\nint b;\n", "int z;\nint a;\nint b;\n")) == ["format"]
    prot = "int z;\r\n// must stay\r\nint a;\r\n"
    assert _kinds(R(prot, "int z;\r\nint a;\r\n")) == ["protected"]


@case
def st_utf8_bom():
    bom = b"\xef\xbb\xbf"
    base = bom + "int z;\n// plain é note\nint a;\n".encode("utf-8")
    assert R(base, bom + b"int z;\nint a;\n").ok
    assert _kinds(R(base, b"int z;\nint a;\n")) == ["format"]


@case
def st_moved_comment_passes():
    base = "// must keep this\nint a;\nint b;\n"
    new = "int a;\n// must keep this\nint b;\n"
    assert R(base, new).ok


@case
def st_protected_whitespace_normalised():
    assert R("// must   keep\nint a;\n", "//  must keep\nint a;\n").ok


@case
def st_license_header_protected():
    base = "/* (c) 1999 Someone. All rights reserved. */\nint a; // x\n"
    rep = R(base, "int a;\n")
    assert _kinds(rep) == ["protected"] and "file header" in _msgs(rep)
    assert R(base, "/* (c) 1999 Someone. All rights reserved. */\nint a;\n").ok


@case
def st_include_angle_brackets_are_code():
    assert _kinds(R("#include <a//b>\n", "#include <a>\n")) == ["code"]
    assert R("#include <a//b> // narration\n", "#include <a//b>\n").ok


@case
def st_line_continuation_in_macro():
    base = "#define M(a) \\\n    foo(a) /* note */ \\\n    bar(a)\nint z;\n"
    assert R(base, "#define M(a) \\\n    foo(a) \\\n    bar(a)\nint z;\n").ok
    cont = "int z;\n// narration \\\nint hidden;\nint y;\n"
    assert R(cont, "int z;\nint y;\n").ok


@case
def st_pragma_and_static_assert_neighbours():
    rep = R("int z;\n// keep layout\n#pragma pack(push, 1)\nint a;\n", "int z;\n#pragma pack(push, 1)\nint a;\n")
    assert _kinds(rep) == ["protected"] and "pragma" in _msgs(rep)
    rep = R("static_assert(1);\n// tidy\nint a;\n", "static_assert(1);\nint a;\n")
    assert _kinds(rep) == ["protected"]


@case
def st_code_edit_detected_with_line_pair():
    rep = R("int a;\n// n\nint b;\n", "int a;\nint c;\n")
    assert _kinds(rep) == ["code"] and "int b;" in _msgs(rep) and "int c;" in _msgs(rep)
    rep = R("int a;\n", "int a;\nint extra;\n")
    assert _kinds(rep) == ["code"] and "<end of file>" in _msgs(rep)


@case
def st_literal_whitespace_is_code():
    assert _kinds(R('s = "a  b"; // x\n', 's = "a b";\n')) == ["code"]


@case
def st_scope_and_missing_files():
    assert _kinds(R("# x\n", "x\n", rel="docs/a.md")) == ["scope"]
    assert _kinds(R(None, "int a;\n")) == ["no-base"]
    assert _kinds(R("int a;\n", None)) == ["no-new"]


@case
def st_unterminated_block_comment_is_lex_error():
    assert "lex" in _kinds(R("int a; /* open\n", "int a;\n"))


@case
def st_unterminated_quote_in_error_directive():
    base = "#error don't build\nint a; // note\n"
    assert R(base, "#error don't build\nint a;\n").ok


@case
def st_merged_paragraphs_keep_protected_text():
    base = "int z;\n// first narration\n//\n// the order must not change\nint a;\n"
    assert R(base, "int z;\n// the order must not change\nint a;\n").ok
    assert R(base, "int z;\n// first narration\n// the order must not change\nint a;\n").ok
    assert _kinds(R(base, "int z;\n// first narration\nint a;\n")) == ["protected"]


@case
def st_glsl_extension_in_scope():
    assert R("#version 450\n// narration\nvoid main() {}\n", "#version 450\nvoid main() {}\n", rel="shaders/a.frag").ok


class _gate_names:
    """Fixture scope: pin the registered gate set, then restore lazy loading from the table."""

    def __init__(self, names):
        self.names = names

    def __enter__(self):
        set_gate_names(self.names)

    def __exit__(self, *_exc):
        set_gate_names(None)


def _tagged(text):
    return "// %s %s" % (REWRITE_TAG, text)


@case
def st_unregistered_mc2_token_is_not_protective():
    base = "int a;\n// MC2_VERIFY (slice MISSION-DATA-DEREF-HARDEN-1) was added here\nint b;\n"
    with _gate_names({"MC2_REAL_GATE"}):
        assert R(base, "int a;\nint b;\n").ok
        rep = R("int a;\n// MC2_REAL_GATE=1 turns this on\nint b;\n", "int a;\nint b;\n")
        assert _kinds(rep) == ["protected"] and "gate-name" in _msgs(rep), _msgs(rep)
    with _gate_names(False):        # unreadable table -> every MC2_ token protects
        assert _kinds(R(base, "int a;\nint b;\n")) == ["protected"]


@case
def st_slice_tag_alone_is_not_protective():
    with _gate_names(set()):
        for tag in ("BUILDING-GLASS-1", "SHADOW-FRUSTUM-AUDIT-1", "P1-2"):
            assert R("int a;\n// added by %s\nint b;\n" % tag, "int a;\nint b;\n").ok, tag
        # a rationale word beside the tag still protects; a date inside a tag-shaped token still protects
        assert _kinds(R("int a;\n// SHADOW-AUDIT-1: must stay\nint b;\n", "int a;\nint b;\n")) == ["protected"]
        assert _kinds(R("int a;\n// REL-2026-10-09\nint b;\n", "int a;\nint b;\n")) == ["protected"]


@case
def st_tagged_protected_comment_survives():
    base = "int a;\n// the lock must be held\nint b;\n"
    assert R(base, "int a;\n" + _tagged("the lock must be held") + "\nint b;\n").ok
    assert R(base, "int a;\n//TODO(rewrite):the lock must be held\nint b;\n").ok
    blk = "int a;\n/* the lock must be held\n   across both calls */\nint b;\n"
    assert R(blk, "int a;\n/* TODO(rewrite): the lock must be held\n   across both calls */\nint b;\n").ok


@case
def st_tag_with_altered_body_fails():
    base = "int a;\n// the lock must be held\nint b;\n"
    assert _kinds(R(base, "int a;\n" + _tagged("the lock is held") + "\nint b;\n")) == ["protected"]
    # a tag that is not right after the opener is not a tag: the protected text is lost and the bytes grew
    assert _kinds(R(base, "int a;\n// the lock must be held TODO(rewrite):\nint b;\n")) == ["grew", "protected"]


@case
def st_tag_growth_budget():
    base = "int a;\n// the lock must be held\nint b;\n"
    assert R(base, "int a;\n" + _tagged("the lock must be held") + "\nint b;\n").new_cb == len(base.split("\n")[1]) + 15
    # one tag pays for itself only: padding elsewhere in the comment still counts as growth
    fat = "int a;\n" + _tagged("the lock must be held") + "\nint b; // extra words here\n"
    assert "grew" in _kinds(R(base, fat))


@case
def st_never_must_rules_unchanged():
    for s in ("Guard 2: packet_oob - globalPktIdx must be valid index", "never null here", "do not reorder"):
        assert _kinds(R("int a;\n// %s\nint b;\n" % s, "int a;\nint b;\n")) == ["protected"], s


@case
def st_rewrite_candidates_listing():
    src = "int a;\n" + _tagged("legacy " + "x" * 100) + "\n/* TODO(rewrite): odd\n   block */\nint b; // TODO later\n"
    got = rewrite_candidates("code/x.cpp", src.encode("utf-8"))
    assert len(got) == 2, got
    assert got[0].startswith("code/x.cpp:2: legacy xxxx") and len(got[0].split(": ", 1)[1]) == 80, got
    assert got[1] == "code/x.cpp:3: odd block", got


@case
def st_allowlist_deletes_protected_comment():
    base = "int a;\n// the lock must be held\nint b;\n"
    al = Allowlist([{"file": "code/x.cpp", "base_line": 3, "reason": "restates the guard below"}])   # +1 off
    rep = R(base, "int a;\nint b;\n", allow=al)
    assert rep.ok and rep.allowed == [(2, "restates the guard below")], _msgs(rep)
    assert "ALLOWED-DELETE code/x.cpp:2 restates the guard below" in fmt_report(rep)
    assert not al.stale(["code/x.cpp"])


@case
def st_allowlist_wrong_line_or_stale_fails():
    base = "int a;\n// the lock must be held\nint b;\n"
    al = Allowlist([{"file": "code/x.cpp", "base_line": 9, "reason": "r"}])
    rep = R(base, "int a;\nint b;\n", allow=al)
    assert _kinds(rep) == ["protected"]
    assert len(al.stale(["code/x.cpp"])) == 1 and not al.stale(["code/other.cpp"])


SENTINEL_SRC = "void f() {\n\tscan();\n\t// MOVER-COHERENCE-AUDIT-END\n}\n"


@case
def st_checker_needle_protects_slice_tag():
    env = Env({"MOVER-COHERENCE-AUDIT-END": "scripts/check-mover-registry-coherence.sh"}, [])
    rep = R(SENTINEL_SRC, "void f() {\n\tscan();\n}\n", env=env)
    assert _kinds(rep) == ["protected"], _msgs(rep)
    assert "MOVER-COHERENCE-AUDIT-END" in _msgs(rep) and "scripts/check-mover-registry-coherence.sh" in _msgs(rep), _msgs(rep)
    assert R(SENTINEL_SRC, SENTINEL_SRC, env=env).ok
    # a needle inside prose still counts, and a needle the checker does not hold does not
    assert _kinds(R("int a;\n// note: see NOFOCUS-HARDEN-1 for why\nint b;\n", "int a;\nint b;\n",
                    env=Env({"NOFOCUS-HARDEN-1": "tests/a.py"}, []))) == ["protected"]


@case
def st_tag_absent_from_checkers_is_not_protective():
    tagged = "void f() {\n\tscan();\n\t// NOFOCUS-HARDEN-1\n}\n"
    env = Env({"OTHER-SENTINEL-X": "scripts/check-x.sh"}, [])
    assert R(tagged, "void f() {\n\tscan();\n}\n", env=env).ok
    assert R(tagged, "void f() {\n\tscan();\n}\n").ok
    assert _kinds(R(tagged, "void f() {\n\tscan();\n}\n", env=Env({"NOFOCUS-HARDEN-1": "tests/t.py"}, []))) == ["protected"]


@case
def st_needle_match_is_case_sensitive_and_verbatim():
    env = Env({"MOVER-COHERENCE-AUDIT-END": "scripts/c.sh"}, [])
    assert R("int a;\n// mover-coherence-audit-end\nint b;\n", "int a;\nint b;\n", env=env).ok
    assert R("int a;\n// MOVER-COHERENCE-AUDIT\nint b;\n", "int a;\nint b;\n", env=env).ok


@case
def st_needle_comment_deletable_only_by_allowlist():
    env = Env({"MOVER-COHERENCE-AUDIT-END": "scripts/c.sh"}, [])
    al = Allowlist([{"file": "code/x.cpp", "base_line": 3, "reason": "checker retired"}])
    rep = R(SENTINEL_SRC, "void f() {\n\tscan();\n}\n", env=env, allow=al)
    assert rep.ok and rep.allowed, _msgs(rep)


@case
def st_begin_end_pair_markers_protected():
    for tag in ("S6-REGION-BEGIN", "S6-REGION-END", "AUDIT-2-END"):
        rep = R("int a;\n// %s\nint b;\n" % tag, "int a;\nint b;\n")
        assert _kinds(rep) == ["protected"] and "begin/end" in _msgs(rep), tag
    assert R("int a;\n// BEGIN of the loop\nint b;\n", "int a;\nint b;\n").ok     # prose is not a marker


@case
def st_fenced_markers_protected():
    for line in ("@@ region one @@", "=== terrain section ===", "======== UPDATE ========"):
        rep = R("int a;\n// %s\nint b;\n" % line, "int a;\nint b;\n")
        assert _kinds(rep) == ["protected"] and "fenced" in _msgs(rep), line
    assert R("int a;\n// ==========\nint b;\n", "int a;\nint b;\n").ok              # a bare ruler is not a fence


@case
def st_split_piece_header_protected():
    env = Env({}, [("code", "foo")])
    src = '#include "foo_internal.h"\n// plain description of this piece\nint a;\n'
    piece = R(src, '#include "foo_internal.h"\nint a;\n', rel="code/foo_bar.cpp", env=env)
    assert _kinds(piece) == ["protected"] and "split-piece" in _msgs(piece), _msgs(piece)
    assert R(src, '#include "foo_internal.h"\nint a;\n', rel="code/other.cpp", env=env).ok          # no allow entry for the stem
    assert R(src, '#include "foo_internal.h"\nint a;\n', rel="mclib/foo_bar.cpp", env=env).ok       # another directory
    deep = "int a;\n" * 30 + "// plain narration deep in the file\nint b;\n"
    assert R(deep, "int a;\n" * 30 + "int b;\n", rel="code/foo_bar.cpp", env=env).ok                 # only the first lines are read


@case
def st_needle_extraction_from_checker_sources():
    sh = ("# MOVER-NOT-A-NEEDLE only in a comment\n"
          "b=\"$(grep -n 'MOVER-COHERENCE-AUDIT-BEGIN' \"$f\" | head -1)\"\n"
          "rg -q SENTINEL_MARKER: \"$f\"\nawk '/FOO-REGION-END/' x\n")
    got = needles_from_source("scripts/c.sh", sh)
    assert {"MOVER-COHERENCE-AUDIT-BEGIN", "SENTINEL_MARKER:", "FOO-REGION-END"} <= set(got), got
    assert "MOVER-NOT-A-NEEDLE" not in got and "$f" not in got
    py = ('"""doc mentions DOC-ONLY-TAG-1"""\n'
          "# NOFOCUS-COMMENT-ONLY-1\n"
          "TAG = 'NOFOCUS-HARDEN-1'\n"
          "def f(text):\n"
          "    if 'sentinel-In-text' in text: pass\n"
          "    if TAG in text: pass\n"
          "    return re.search(r'\bregion-Marker-here\b', text) or re.compile('AB-CD-EF')\n")
    got = needles_from_source("scripts/c.py", py)
    assert {"NOFOCUS-HARDEN-1", "sentinel-In-text", "region-Marker-here", "AB-CD-EF"} <= set(got), got
    assert "DOC-ONLY-TAG-1" not in got and "NOFOCUS-COMMENT-ONLY-1" not in got, got
    assert "FOO-REGION-END" in needles_from_source("cmake/a.cmake", "if(x MATCHES \"FOO-REGION-END\")\n# NOPE-IN-COMMENT-1\n")
    assert "NOPE-IN-COMMENT-1" not in needles_from_source("cmake/a.cmake", "# NOPE-IN-COMMENT-1\n")
    assert "SLICE-LIST-7" in needles_from_source("scripts/a.json", '{"a": ["SLICE-LIST-7"]}')
    assert set(needles_from_source("scripts/c.py", "x = 'abc'\ny = 'WX-1'\n")) == set()     # shorter than the minimums


@case
def st_word_needles_need_a_marker_shape():
    for ok in ("TIER2-EXCLUDED", "TEX-CLASS:", "SENTINEL_MARKER:", "region-Marker-here", "VK-only-ITEM-X"):
        assert looks_like_marker(ok), ok
    for prose in ("byte-identical", "gos_terrain", "static_prop", "MATERIAL", "MC2_VK", "ring_frames", "some-hyphen-word",
                  "GPU_CULL", "MC2_TERRAIN_INDIRECT", "default-off", "Short-Id"):
        assert not looks_like_marker(prose), prose
    got = needles_from_source("scripts/c.sh", "grep -q 'byte-identical' f\ngrep -q static_prop f\ngrep -q 'TIER2-EXCLUDED' f\n")
    assert set(got) == {"TIER2-EXCLUDED"}, got


@case
def st_stoplisted_word_needle_does_not_protect():
    env = Env(needles_from_source("scripts/c.sh", "grep -q 'byte-identical' f; grep -c MATERIAL f; grep -q 'ZONE-OPEN:' f\n"), [])
    assert R("int a;\n// byte-identical when off\nint b;\n", "int a;\nint b;\n", env=env).ok
    assert R("int a;\n// MATERIAL table\nint b;\n", "int a;\nint b;\n", env=env).ok
    assert _kinds(R("int a;\n// ZONE-OPEN: here\nint b;\n", "int a;\nint b;\n", env=env)) == ["protected"]


@case
def st_external_needles_file_parsing():
    lits, res = parse_needle_file("# header\n\nre:\\[PROJECTZ:[A-Za-z]+ id=[^\\]]+\\]  # oracle ids\nSIDE-CHANNEL-ID  # other tool\n")
    assert list(lits) == ["SIDE-CHANNEL-ID"] and len(res) == 1 and res[0][2].endswith("(external)")
    for bad in ("SIDE-CHANNEL-ID\n", "re:(unclosed  # r\n", "re:x*  # matches empty\n", "abc  # too short\n", "re:  # empty\n"):
        try:
            parse_needle_file(bad)
        except ValueError as e:
            assert "comment_guard_needles.txt:1" in str(e), e
        else:
            assert False, bad


@case
def st_external_regex_needle_protects_comment():
    lits, res = parse_needle_file("re:\\[PROJECTZ:[A-Za-z]+ id=[^\\]]+\\]  # external oracle tool reads these ids\n")
    env = Env(lits, [], res)
    src = "void f() {\n\t// [PROJECTZ:ScreenXYOracle id=artlry_position_screen]\n\tg();\n}\n"
    rep = R(src, "void f() {\n\tg();\n}\n", env=env)
    assert _kinds(rep) == ["protected"] and "PROJECTZ:ScreenXYOracle id=artlry_position_screen" in _msgs(rep), _msgs(rep)
    assert "comment_guard_needles.txt (external)" in _msgs(rep)
    assert R(src, src, env=env).ok
    assert R(src, "void f() {\n\tg();\n}\n").ok                      # without the needle file the comment is plain narration
    assert R("int a;\n// [PROJECTZ:Other no id]\nint b;\n", "int a;\nint b;\n", env=env).ok


@case
def st_mfc_classwizard_markers_protected():
    for m in ("//{{AFX_MSG_MAP(CMainFrame)", "//}}AFX_MSG_MAP", "//{{AFX_DATA_INIT(CAbout)", "//}}AFX_DATA_INIT",
              "//{{AFX_DATA_MAP(CAbout)", "//{{AFX_VIRTUAL(CView)", "//}}AFX_VIRTUAL", "//{{AFX_MSG(CView)", "//}}AFX_MSG",
              "//{{AFX_DATA(CAbout)", "//}}AFX_DATA", "//{{AFX_INSERT_LOCATION}}"):
        rep = R("int a;\n%s\nint b;\n" % m, "int a;\nint b;\n")
        assert _kinds(rep) == ["protected"] and "ClassWizard" in _msgs(rep), m
    assert R("int a;\n// AFX_MSG_MAP is generated\nint b;\n", "int a;\nint b;\n").ok     # prose about the macro is not a marker


def run_selftest():
    failed = 0
    for fn in SELFTEST_CASES:
        try:
            fn()
        except AssertionError as e:
            failed += 1
            print("FAIL  %s: %s" % (fn.__name__, e))
    n = len(SELFTEST_CASES)
    print("comment_guard selftest: %d/%d ok" % (n - failed, n))
    return 1 if failed else 0


# ------------------------------------------------------------------ CLI
def build_parser():
    ap = argparse.ArgumentParser(description="Fail-closed guard for comment-only C/C++ edits.")
    ap.add_argument("paths", nargs="*", help="files to check (default: every file changed vs the base)")
    ap.add_argument("--check", action="store_true", help="comment-only check (default mode)")
    ap.add_argument("--base", "--rev", dest="base", default="HEAD", help="base git rev (default HEAD)")
    ap.add_argument("--base-file", help="take the base text from this file instead of git (one path)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--staged", action="store_true", help="new text = the git index")
    src.add_argument("--worktree", action="store_true", help="new text = the working tree (default)")
    ap.add_argument("--repo", help="repo root (default: the repo this tool lives in)")
    ap.add_argument("--ignore-out-of-scope", action="store_true",
                    help="skip non-C/C++ files instead of failing them")
    ap.add_argument("-q", "--quiet", action="store_true", help="print failing files only")
    ap.add_argument("--allow", metavar="JSON", help="reviewer-written list of protected comments that may be "
                    "deleted: [{\"file\", \"base_line\", \"reason\"}]")
    ap.add_argument("--rewrite-candidates", action="store_true",
                    help="list every TODO(rewrite) comment as file:line: text (paths, or all tracked files)")
    ap.add_argument("--needles", action="store_true",
                    help="list the checker needles (grep/regex/sentinel strings from scripts, tools, tests, cmake, hooks) "
                    "and the first file defining each; a comment containing one is protected")
    ap.add_argument("--selftest", action="store_true", help="run the in-memory fixtures")
    ap.add_argument("--hash-objs", metavar="BUILD_DIR", help="print a JSON object-hash record for the paths")
    ap.add_argument("--compare-objs", metavar="BEFORE_JSON", help="compare current objects with a --hash-objs record")
    return ap


def main(argv=None):
    ap = build_parser()
    args = ap.parse_intermixed_args(argv)
    if args.selftest:
        return run_selftest()
    if args.needles:
        return run_needles(args)
    if args.rewrite_candidates:
        return run_rewrite_candidates(args)
    if args.hash_objs:
        return run_hash_objs(args)
    if args.compare_objs:
        return run_compare_objs(args)
    return run_check(args)


if __name__ == "__main__":
    sys.exit(main())
