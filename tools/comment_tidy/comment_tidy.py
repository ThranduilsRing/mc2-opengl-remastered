#!/usr/bin/env python3
"""comment-tidy: "clang-tidy for comments" for the MC2 repo.

Finds stale / wrong / bloated comments in C++, GLSL, Python, shell and CMake
sources.  Report-only by default.  --fix (CT003 only for now) prints a
unified diff; --apply writes the change.

  py -3 tools/comment_tidy/comment_tidy.py                       # text summary
  py -3 tools/comment_tidy/comment_tidy.py --json out.json       # + JSON file
  py -3 tools/comment_tidy/comment_tidy.py --checks CT001,CT003 --paths GameOS code
  py -3 tools/comment_tidy/comment_tidy.py --fix                 # dry-run diff
  py -3 tools/comment_tidy/comment_tidy.py --fix --apply         # write (CT003)

Checks (see README.md): CT001 dead-env, CT002 dead-path, CT003 stale-line-ref,
CT004 dead-commit, CT005 dead-symbol, CT006 commented-out-code,
CT007 provenance-essay, CT008 stale-todo, CT009 lane-era, CT010 tombstone.
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures as cf
import datetime as dt
import difflib
import json
import os
import re
import subprocess
import sys
import time

SCOPE_DIRS = ["GameOS", "mclib", "code", "RenderCore", "GuiRuntime", "shaders", "scripts", "tools"]
EXCLUDE_TOP = {"3rdparty", "data", "graphify-out"}
EXCLUDE_PREFIX = ("build",)
EXCLUDE_REL = ("tools/comment_tidy/",)

C_EXT = {".cpp", ".cc", ".c", ".h", ".hpp", ".inl", ".glsl", ".vert", ".frag", ".comp",
         ".geom", ".tesc", ".tese", ".hglsl", ".mm", ".cs"}
HASH_EXT = {".py", ".sh", ".cmake", ".ps1"}
# Extra files used only to build the "does X exist in code" corpus (no comments reported).
CORPUS_EXTRA_EXT = {".json", ".ini", ".cfg", ".yml", ".yaml", ".bat", ".txt", ".toml"}

CHECKS = {
    "CT001": ("dead-env", "warning", False),
    "CT002": ("dead-path", "warning", False),
    "CT003": ("stale-line-ref", "info", True),
    "CT004": ("dead-commit", "warning", False),
    "CT005": ("dead-symbol", "info", False),
    "CT006": ("commented-out-code", "warning", False),
    "CT007": ("provenance-essay", "info", False),
    "CT008": ("stale-todo", "info", False),
    "CT009": ("lane-era-ref", "info", False),
    "CT010": ("tombstone", "info", False),
}


def kind_of(path: str):
    base = os.path.basename(path)
    ext = os.path.splitext(base)[1].lower()
    if ext in C_EXT:
        return "c"
    if ext in HASH_EXT or base == "CMakeLists.txt":
        return "py" if ext == ".py" else "hash"
    return None


# --------------------------------------------------------------------------
# Comment extraction
# --------------------------------------------------------------------------
C_TOKEN = re.compile(
    r'//[^\n]*'
    r'|/\*.*?\*/'
    r'|R"([A-Za-z0-9_]{0,16})\(.*?\)\1"'
    r'|"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*'",
    re.S)
PY_TOKEN = re.compile(
    r'"""(?:\\.|[^\\])*?"""'
    r"|'''(?:\\.|[^\\])*?'''"
    r'|"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*'"
    r'|#[^\n]*', re.S)
HASH_TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"'
    r"|'[^'\n]*'"
    r'|(?:(?<=\s)|^)#[^\n]*', re.S | re.M)


class Block:
    """A comment block: consecutive // or # lines, or one /* */ comment."""
    __slots__ = ("file", "lines", "standalone", "style", "_col")

    def __init__(self, file, style, standalone):
        self.file = file
        self.style = style          # "line" | "block"
        self.standalone = standalone
        self._col = 0
        self.lines = []             # (lineno, text, col_start, col_end)

    @property
    def start(self):
        return self.lines[0][0]

    @property
    def end(self):
        return self.lines[-1][0]

    @property
    def text(self):
        return "\n".join(t for _, t, _, _ in self.lines)


def extract(path: str, text: str, kind: str):
    """Return (blocks, code_text).  code_text = source with comments blanked."""
    tok = {"c": C_TOKEN, "py": PY_TOKEN, "hash": HASH_TOKEN}[kind]
    line_starts = [0]
    for m in re.finditer(r"\n", text):
        line_starts.append(m.end())

    def lineno(off):
        lo, hi = 0, len(line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= off:
                lo = mid
            else:
                hi = mid - 1
        return lo

    spans = []   # (start, end, style)
    for m in tok.finditer(text):
        s = m.group(0)
        if kind == "c":
            if s.startswith("//"):
                spans.append((m.start(), m.end(), "line"))
            elif s.startswith("/*"):
                spans.append((m.start(), m.end(), "block"))
        else:
            if s.startswith("#"):
                spans.append((m.start(), m.end(), "line"))

    blocks = []
    last_line_blk = None
    for s, e, style in spans:
        ln = lineno(s)
        ls = line_starts[ln]
        standalone = text[ls:s].strip() == ""
        if style == "line":
            raw = text[s:e].rstrip("\r")
            if kind == "c":
                m = re.match(r"//+[!/<]?\s?", raw)
            else:
                m = re.match(r"#+!?\s?", raw)
            cstart = s - ls + m.end()
            body = raw[m.end():]
            cend = cstart + len(body)
            col = s - ls
            if (standalone and last_line_blk is not None and last_line_blk.end == ln
                    and abs(last_line_blk._col - col) <= 4):
                last_line_blk.lines.append((ln + 1, body, cstart, cend))
            else:
                b = Block(path, "line", standalone)
                b._col = col
                b.lines.append((ln + 1, body, cstart, cend))
                blocks.append(b)
                last_line_blk = b if standalone else None
        else:
            b = Block(path, "block", standalone)
            seg_off = s
            for part in text[s:e].split("\n"):
                l2 = lineno(seg_off)
                c0 = seg_off - line_starts[l2]
                body = part.rstrip("\r")
                p0 = 0
                if seg_off == s:
                    mm = re.match(r"/\*+[!<]?\s?", body)
                    p0 = mm.end() if mm else 0
                else:
                    mm = re.match(r"\s*\*?(?!/)\s?", body)
                    p0 = mm.end() if mm else 0
                body2 = body[p0:]
                if body2.rstrip().endswith("*/"):
                    body2 = body2.rstrip()[:-2].rstrip()
                b.lines.append((l2 + 1, body2.rstrip("*").rstrip() if body2.strip("*- =") == "" else body2,
                                c0 + p0, c0 + p0 + len(body2)))
                seg_off += len(part) + 1
            blocks.append(b)
            last_line_blk = None
    # code text: blank out comment spans (keeps newlines)
    parts, pos = [], 0
    for s, e, _ in spans:
        parts.append(text[pos:s])
        parts.append(re.sub(r"[^\n]", " ", text[s:e]))
        pos = e
    parts.append(text[pos:])
    return blocks, "".join(parts)


# --------------------------------------------------------------------------
# Repo context
# --------------------------------------------------------------------------
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
ENV_TOK = re.compile(r"\bMC2_[A-Z0-9][A-Z0-9_]*")


def git(root, *args, check=False, input=None):
    try:
        r = subprocess.run(["git", "-C", root, *args], capture_output=True, input=input,
                           timeout=300)
    except Exception:
        return None
    if r.returncode != 0:
        return None
    return r.stdout


class Ctx:
    def __init__(self, root, scope_files, all_files, env_tokens, idents, todo_days=365,
                 max_block=12, have_git=True):
        self.root = root
        self.scope_files = scope_files
        self.all_files = all_files
        self.env_tokens = env_tokens
        self.idents = idents
        self.todo_days = todo_days
        self.max_block = max_block
        self.have_git = have_git
        self.fileset = set(all_files)
        self.suffix_index = None
        self._exists_cache = {}

    def path_exists(self, rel_from_file_dir, p):
        """Does mentioned path p exist (repo-relative, file-relative, or by suffix)?"""
        key = (rel_from_file_dir, p)
        if key in self._exists_cache:
            return self._exists_cache[key]
        r = self._path_exists(rel_from_file_dir, p)
        self._exists_cache[key] = r
        return r

    def _path_exists(self, d, p):
        p = p.replace("\\", "/").strip("/") if not p.startswith("./") and not p.startswith("../") else p.replace("\\", "/")
        cands = [os.path.normpath(os.path.join(self.root, p)),
                 os.path.normpath(os.path.join(self.root, d, p))]
        for c in cands:
            if os.path.exists(c):
                return True
        if self.suffix_index is None:
            idx = collections.defaultdict(list)
            for f in self.all_files:
                idx[os.path.basename(f).lower()].append(f)
            self.suffix_index = idx
        q = p.lstrip("./")
        q = re.sub(r"^(\.\./)+", "", q)
        ql = q.lower()
        for f in self.suffix_index.get(os.path.basename(q).lower(), ()):
            fl = f.lower()
            if fl == ql or fl.endswith("/" + ql):
                return True
        return False


def list_files(root):
    out = git(root, "ls-files", "-co", "--exclude-standard", "-z")
    files = []
    if out is not None:
        files = [f for f in out.decode("utf-8", "replace").split("\0") if f]
    else:
        for dp, dn, fn in os.walk(root):
            dn[:] = [d for d in dn if d not in (".git",) and not d.startswith("build")
                     and d not in EXCLUDE_TOP]
            for f in fn:
                files.append(os.path.relpath(os.path.join(dp, f), root).replace(os.sep, "/"))
    res = []
    for f in set(files):
        top = f.split("/", 1)[0]
        if top in EXCLUDE_TOP or top.startswith(EXCLUDE_PREFIX):
            continue
        if os.path.isfile(os.path.join(root, f)):
            res.append(f)
    return sorted(res)


def in_scope(f, paths):
    if any(f.startswith(x) for x in EXCLUDE_REL):
        return False
    if paths:
        return any(f == p or f.startswith(p.rstrip("/") + "/") for p in paths)
    return f.split("/", 1)[0] in SCOPE_DIRS


def read_text(root, rel):
    try:
        with open(os.path.join(root, rel), "rb") as fh:
            b = fh.read()
    except OSError:
        return None
    if b"\0" in b[:4096] or len(b) > 3_000_000:
        return None
    return b.decode("utf-8", "replace")


def parse_one(args):
    root, rel, want_blocks = args
    text = read_text(root, rel)
    if text is None:
        return rel, None, set(), set()
    k = kind_of(rel)
    if k:
        blocks, code = extract(rel, text, k)
    else:
        blocks, code = [], text
    envs = set(ENV_TOK.findall(code))
    ids = set(IDENT.findall(code)) if (k or os.path.splitext(rel)[1].lower() in CORPUS_EXTRA_EXT) else set()
    return rel, (blocks if want_blocks else None), envs, ids


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------
class Finding(dict):
    pass


def mk(cid, blk, line, msg, snippet="", **extra):
    f = Finding(id=cid, name=CHECKS[cid][0], severity=CHECKS[cid][1], file=blk.file, line=line,
                end_line=extra.pop("end_line", line), message=msg,
                snippet=snippet.strip()[:200], fixable=CHECKS[cid][2])
    f.update(extra)
    return f


EXT_RE = r"(?:cpp|cc|c|h|hpp|inl|py|md|glsl|vert|frag|comp|hglsl|json|txt|cmake|sh|ps1|bat|spv|ini|cfg|yaml|yml|csv)"
PATH_RE = re.compile(r"(?<![\w./\\:%$<{*-])((?:\.{1,2}/)*[A-Za-z_][\w.+-]*(?:[/\\][\w.+-]+)+\.@E@)(?![\w])".replace("@E@", EXT_RE))
LINEREF_RE = re.compile(r"(?<![\w.])([\w+-][\w./\\+-]*\.@E@):(\d{1,6})(?:[-\u2013/,]\d{1,6})*(?::\d{1,4})?(?![\w])".replace("@E@", EXT_RE))
HEX_RE = re.compile(r"(?<![\w/.-])([0-9a-f]{7,40})(?![\w-])")
HEX_CTX = re.compile(r"(?i)\b(commit|sha|hash|cherry-?pick|revert(?:ed)?|rev|landed|merged|fixed in|introduced (?:in|by)|git|HEAD~?)\b|@\s*$|`$|\($")
FUNC_RE = re.compile(r"(?<![\w:.>])([A-Za-z_]\w*)\(\)")
SCOPED_RE = re.compile(r"(?<![\w:])([A-Z][A-Za-z0-9_]*)::([A-Za-z_]\w*)")
SKIP_NS = {"std", "ImGui", "SDL", "glm", "Tracy", "D3D", "Vk", "GL", "VK", "Qt", "Eigen", "boost"}
RETIRED_CTX = re.compile(r"(?i)\b(retired|removed|no longer|inert|never (defined|read)|not read|dead|deleted|gone|obsolete|killswitch)\b")
TODO_RE = re.compile(r"\b(TODO|FIXME|HACK|XXX)\b")
LANE_RE = re.compile(r"(?i)\bworktrees?\b|\.claude/worktrees|nifty-mendeleev|\blanes?\b|\bleases?\b|\bleased\b")
LANE_FP = re.compile(r"(?i)\b(simd|subgroup|wave\w*|warp|per-lane|lane ?(id|index|mask)|vector|sse|avx|neon|"
                     r"gpu|lanes? (of|in) (the )?(vector|simd)|traffic|road|bind(ing)?|mutex|license|"
                     r"release|lease time|dhcp|network|socket)\b")
TOMB_RE = re.compile(
    r"(?i)^\W*(removed|deleted|dropped|retired|formerly|gone)\b"
    r"|\b(was|were|has been|have been|got) (removed|deleted|dropped|retired)\b"
    r"|\bwas here\b|\bformerly\b|\bused to (live|be|contain|hold|sit)\b"
    r"|\b(code|block|function|path|call|helper|gate|shim|stub)\b[^.\n]{0,30}\b(removed|deleted)\b"
    r"|\b(removed|deleted) (the |this |old |legacy |dead )")
PROV_RE = re.compile(
    r"\b[A-Z][A-Z0-9]{1,}(?:-[A-Z0-9]+){1,4}\b"           # VK-RUNG8, CEMENT-EDGE-1
    r"|\bP\d+[a-z]?\b|\b[A-Z]\d+[a-z]\b"                    # P8a, A3b
    r"|\b20\d\d-\d\d-\d\d\b|\b20\d\d/\d\d/\d\d\b"
    r"|(?i:\b(lane|worktree|slice|rung|phase|wave|round|iteration|landed|ported|agent|recon|batch|"
    r"ticket|gate[- ]?\d|follow-?up|supersed\w*|previously|originally|history|used to|formerly|"
    r"commit|PR ?#?\d+|issue ?#?\d+)\b)")
CODE_C = [
    re.compile(r"^\s*#\s*(include|define|if|ifdef|ifndef|endif|else|pragma|undef)\b"),
    re.compile(r"^\s*(if|for|while|switch)\s*\(.*[){;]\s*$"),
    re.compile(r"^\s*(else|do|try)\s*\{?\s*$|^\s*\}\s*else\b.*$|^\s*(case\s+.+|default)\s*:\s*$"),
    re.compile(r"^\s*return\b[^.]*;\s*$|^\s*(break|continue);\s*$"),
    re.compile(r"^\s*[\w:<>,\s\*&\[\]\.\->()]+(=|\+=|-=|\*=|\|=)[^=].*;\s*$"),
    re.compile(r"^\s*[A-Za-z_][\w:\.\->]*\s*\(.*\)\s*;\s*$"),
    re.compile(r"^\s*[\w:<>\*&]+\s+[\w:\*&]+\s*\(.*\)\s*(const)?\s*\{?\s*$"),
    re.compile(r"^\s*[\}\{]\s*;?\s*$"),
    re.compile(r"^\s*(static |const |uniform |in |out |layout\(.*\) )*(int|float|vec[234]|uint|bool|char|void|double|auto|std::\w+)\s+\w+(\[.*\])?\s*(=.*)?;\s*$"),
]
CODE_PY = [
    re.compile(r"^\s*(import \w|from [\w.]+ import )"),
    re.compile(r"^\s*(def |class |if |elif |else:|for |while |try:|except\b|with )[^.]*:\s*$"),
    re.compile(r"^\s*return\b.*\S$"),
    re.compile(r"^\s*[A-Za-z_][\w.\[\]'\"]*\s*(=|\+=|-=)\s*[^=\s].*$"),
    re.compile(r"^\s*[A-Za-z_][\w.]*\(.*\)\s*$"),
    re.compile(r"^\s*(set|add_\w+|target_\w+|include|if|endif|foreach|endforeach|message|find_package)\(.*\)?\s*$"),
]
PROSE_HINT = re.compile(r"^\s*[A-Z][a-z]+(\s+[a-z]+){2,}")


def looks_code(line, kind):
    s = line.strip()
    if not s or PROSE_HINT.match(s):
        return False
    pats = CODE_C if kind == "c" else CODE_PY
    return any(p.match(line) for p in pats)


def check_block(blk, ctx, kind, enabled):
    out = []
    d = os.path.dirname(blk.file)
    if kind != "c" and blk.lines and blk.lines[0][0] == 1 and blk.lines[0][1].startswith("/"):
        blk.lines = blk.lines[1:]       # shebang (extract() strips '#!')
        if not blk.lines:
            return out
    # ---- per-line checks
    for ln, text, cs, ce in blk.lines:
        if "CT001" in enabled:
            for t in set(ENV_TOK.findall(text)):
                if t.endswith("_") or t in ctx.env_tokens:
                    continue
                f = mk("CT001", blk, ln, f"env/gate {t} not found in code or registry", text, token=t)
                if RETIRED_CTX.search(" ".join(x[1] for x in blk.lines)):
                    f["explains_retirement"] = True
                    f["message"] += " (comment says it is retired: tombstone-style)"
                out.append(f)
        if "CT003" in enabled or "CT002" in enabled:
            for m in LINEREF_RE.finditer(text):
                if "CT003" in enabled:
                    out.append(mk("CT003", blk, ln, f"line-number ref {m.group(0)} will rot", text,
                                  token=m.group(0), file_ref=m.group(1)))
        if "CT002" in enabled:
            for m in PATH_RE.finditer(text):
                p = m.group(1)
                if re.match(r"^[A-Za-z]:", p) or "//" in p or p.split("/")[0] in ("http", "https"):
                    continue
                if re.match(r"(?i)^(\.\./|\./)?(build|out|artifacts|_harness_out|dev_shell_out|screenshots|"
                            r"scratch|staging|logs?|terrain_\w+|run|tmp|test_output|golden_floors)/", p):
                    continue
                if re.match(r"^(GL|GLES\d?|vulkan|SDL2?|sys|X11|EGL|KHR|glm|spirv|openxr)/", p):
                    continue            # system / third-party include style
                if not ctx.path_exists(d, p):
                    out.append(mk("CT002", blk, ln, f"path {p} does not exist", text, token=p,
                                  category="agent-memory" if p.startswith("memory/") else p.split("/")[0]))
        if "CT004" in enabled and ctx.have_git:
            for m in HEX_RE.finditer(text):
                h = m.group(1)
                if not (re.search(r"[a-f]", h) and re.search(r"\d", h)):
                    continue
                if len(re.findall(r"[a-f]", h)) < 2 or len(set(h)) < 4:
                    continue
                pre = text[max(0, m.start() - 24):m.start()]
                if len(h) >= 12 or HEX_CTX.search(pre) or text[max(0, m.start() - 1):m.start()] in ("`",):
                    out.append(mk("CT004", blk, ln, f"commit {h} (unverified)", text, token=h))
        if "CT005" in enabled:
            for m in FUNC_RE.finditer(text):
                n = m.group(1)
                if len(n) >= 6 and (("_" in n) or (n != n.lower() and n != n.upper())) \
                        and not re.match(r"^(vk|gl|SDL_|Vk|ImGui)", n) and n not in ctx.idents:
                    out.append(mk("CT005", blk, ln, f"function {n}() not found in code", text, token=n))
            for m in SCOPED_RE.finditer(text):
                c, n = m.group(1), m.group(2)
                if c in SKIP_NS or re.match(r"^(Vk|ImGui|SDL|GL|D3D|Tracy)", c) or len(n) < 3:
                    continue
                if c not in ctx.idents or n not in ctx.idents:
                    out.append(mk("CT005", blk, ln, f"symbol {c}::{n} not found in code", text,
                                  token=f"{c}::{n}"))
        if "CT009" in enabled:
            m = LANE_RE.search(text)
            if m and not LANE_FP.search(text) and not (kind == "c" and blk.file.startswith("shaders/")
                                                         and m.group(0).lower().startswith("lane")):
                out.append(mk("CT009", blk, ln, f"worktree/lane-era reference '{m.group(0)}'", text,
                              token=m.group(0)))
        if "CT008" in enabled:
            m = TODO_RE.search(text)
            if m:
                out.append(mk("CT008", blk, ln, f"{m.group(1)} (age pending blame)", text, token=m.group(1)))
    # ---- block-level
    n = len(blk.lines)
    if "CT006" in enabled and n >= 3:
        run, best, best_start = 0, 0, 0
        for i, (_, t, _, _) in enumerate(blk.lines):
            if looks_code(t, "c" if kind == "c" else "py"):
                run += 1
                if run > best:
                    best, best_start = run, i - run + 1
            else:
                run = 0
        if best >= 3:
            l0 = blk.lines[best_start][0]
            out.append(mk("CT006", blk, l0, f"{best} consecutive lines look like commented-out code",
                          blk.lines[best_start][1], end_line=blk.lines[best_start + best - 1][0]))
    if "CT007" in enabled and n > ctx.max_block:
        txt = blk.text
        if not re.search(r"(?i)copyright|licen[sc]e|SPDX|permission is hereby", txt):
            hits = sum(1 for _, t, _, _ in blk.lines if t.strip() and PROV_RE.search(t))
            nonblank = sum(1 for _, t, _, _ in blk.lines if t.strip())
            if nonblank and hits / nonblank >= 0.4:
                out.append(mk("CT007", blk, blk.start,
                              f"{n}-line comment, {hits}/{nonblank} lines history/ticket-heavy; "
                              f"condense to 1-2 lines + doc pointer", blk.lines[0][1],
                              end_line=blk.end, lines=n, prov_ratio=round(hits / nonblank, 2)))
    if "CT010" in enabled and n <= 4:
        txt = " ".join(t.strip() for _, t, _, _ in blk.lines)
        m = TOMB_RE.search(txt)
        if m and not re.search(r"(?i)\b(is|are|will be|to be|must be|should be|if|when|unless)\s+(removed|deleted)", txt):
            out.append(mk("CT010", blk, blk.start, f"possible tombstone: '{m.group(0).strip()}'", txt,
                          end_line=blk.end))
    return out


def blame_dates(root, rel, lines):
    out = git(root, "blame", "--porcelain", "-w", "--", rel)
    if out is None:
        return {}
    want = set(lines)
    res, cur_time, cur_final, cache = {}, {}, None, {}
    sha = None
    for ln in out.decode("utf-8", "replace").split("\n"):
        m = re.match(r"^([0-9a-f]{40}) \d+ (\d+)", ln)
        if m:
            sha, cur_final = m.group(1), int(m.group(2))
            continue
        if ln.startswith("committer-time "):
            cache[sha] = int(ln.split()[1])
        elif ln.startswith("\t"):
            if cur_final in want and sha and sha in cache and not sha.startswith("0000000"):
                res[cur_final] = cache[sha]
    return res


def apply_blame(findings, ctx, jobs):
    todos = [f for f in findings if f["id"] == "CT008"]
    if not todos:
        return findings
    byfile = collections.defaultdict(list)
    for f in todos:
        byfile[f["file"]].append(f)
    now = time.time()
    keep_ids = set()
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(blame_dates, ctx.root, fn, [f["line"] for f in fl]): fn for fn, fl in byfile.items()}
        for fut in cf.as_completed(futs):
            fn = futs[fut]
            try:
                dates = fut.result()
            except Exception:
                dates = {}
            for f in byfile[fn]:
                t = dates.get(f["line"])
                if t is None:
                    f["message"] = f"{f['token']} (age unknown: uncommitted/untracked)"
                    f["age_days"] = None
                    continue
                age = int((now - t) / 86400)
                f["age_days"] = age
                f["date"] = dt.datetime.fromtimestamp(t).strftime("%Y-%m-%d")
                if age >= ctx.todo_days:
                    f["message"] = f"{f['token']} untouched for {age}d (since {f['date']})"
                    keep_ids.add(id(f))
    return [f for f in findings if f["id"] != "CT008" or id(f) in keep_ids]


def verify_commits(findings, root):
    cands = [f for f in findings if f["id"] == "CT004"]
    if not cands:
        return findings
    toks = sorted({f["token"] for f in cands})
    inp = ("\n".join(toks) + "\n").encode()
    out = git(root, "cat-file", "--batch-check", input=inp)
    if out is None:
        return [f for f in findings if f["id"] != "CT004"]
    status = {}
    for tok, ln in zip(toks, out.decode().splitlines()):
        parts = ln.split()
        status[tok] = parts[1] if len(parts) > 1 else "missing"
    res = []
    for f in findings:
        if f["id"] != "CT004":
            res.append(f)
        elif status.get(f["token"]) == "missing":
            f["message"] = f"commit {f['token']} not found in git history"
            res.append(f)
        # resolves (commit/blob/tree) or ambiguous -> not reported
    return res


def collect(root, paths=None, enabled=None, todo_days=365, max_block=12, jobs=None):
    enabled = set(enabled or CHECKS)
    jobs = jobs or min(16, (os.cpu_count() or 4))
    files = list_files(root)
    scope = [f for f in files if kind_of(f) and in_scope(f, paths)]
    corpus = [f for f in files if kind_of(f) or os.path.splitext(f)[1].lower() in CORPUS_EXTRA_EXT]
    scope_set = set(scope)
    have_git = git(root, "rev-parse", "--git-dir") is not None
    envs, ids, blocks_by_file = set(), set(), {}
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for rel, blocks, e, i in ex.map(parse_one, [(root, f, f in scope_set) for f in corpus], chunksize=32):
            envs |= e
            ids |= i
            if blocks is not None:
                blocks_by_file[rel] = blocks
    # registry header tokens are plain code, already included; parse explicitly for safety
    reg = read_text(root, "RenderCore/RendererFeatureRegistry.h")
    if reg:
        envs |= set(ENV_TOK.findall(reg))
    ctx = Ctx(root, scope, files, envs, ids, todo_days, max_block, have_git)
    findings = []
    for rel in scope:
        kind = kind_of(rel)
        kind = "c" if kind == "c" else "py"
        for blk in blocks_by_file.get(rel, ()):
            findings.extend(check_block(blk, ctx, kind, enabled))
    if "CT004" in enabled:
        findings = verify_commits(findings, root)
    if "CT008" in enabled and have_git:
        findings = apply_blame(findings, ctx, jobs)
    elif "CT008" in enabled:
        findings = [f for f in findings if f["id"] != "CT008"]
    findings.sort(key=lambda f: (f["file"], f["line"], f["id"]))
    return findings, ctx


# --------------------------------------------------------------------------
# CT003 fix
# --------------------------------------------------------------------------
def fix_ct003_text(text: str, kind: str) -> str:
    """Drop ':NNN' / ':NNN-MMM' after file names, inside comments only."""
    blocks, _ = extract("x", text, kind)
    lines = text.split("\n")
    edits = collections.defaultdict(list)
    for b in blocks:
        for ln, t, cs, ce in b.lines:
            for m in LINEREF_RE.finditer(t):
                a = cs + m.start() + len(m.group(1))
                z = cs + m.end()
                edits[ln - 1].append((a, z))
    for i, spans in edits.items():
        s = lines[i]
        for a, z in sorted(spans, reverse=True):
            s = s[:a] + s[z:]
        lines[i] = s
    return "\n".join(lines)


def run_fix(root, findings, apply):
    files = sorted({f["file"] for f in findings if f["id"] == "CT003"})
    n_changed = n_edits = 0
    for rel in files:
        p = os.path.join(root, rel)
        with open(p, "rb") as fh:
            raw = fh.read()
        text = raw.decode("utf-8", "surrogateescape")
        new = fix_ct003_text(text, "c" if kind_of(rel) == "c" else "py")
        if new == text:
            continue
        n_changed += 1
        n_edits += sum(1 for f in findings if f["id"] == "CT003" and f["file"] == rel)
        if apply:
            with open(p, "wb") as fh:
                fh.write(new.encode("utf-8", "surrogateescape"))
        else:
            sys.stdout.writelines(difflib.unified_diff(
                text.splitlines(True), new.splitlines(True), f"a/{rel}", f"b/{rel}", n=0))
    print(f"\n[fix] CT003: {n_edits} refs in {n_changed} files "
          f"{'WRITTEN' if apply else '(dry-run; pass --apply to write)'}", file=sys.stderr)


# --------------------------------------------------------------------------
def summarize(findings):
    per = collections.Counter(f["id"] for f in findings)
    pd = collections.defaultdict(collections.Counter)
    for f in findings:
        pd[f["id"]][f["file"].split("/", 1)[0]] += 1
    return per, pd


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
    ap.add_argument("--paths", nargs="*", help="repo-relative files/dirs (default: owned dirs)")
    ap.add_argument("--checks", help="comma list, e.g. CT001,CT003 (default all)")
    ap.add_argument("--json", nargs="?", const="-", metavar="FILE", help="write JSON (stdout if no FILE)")
    ap.add_argument("--fix", action="store_true", help="CT003 only: print dry-run diff")
    ap.add_argument("--apply", action="store_true", help="with --fix: write changes")
    ap.add_argument("--max-block", type=int, default=12, help="CT007 line threshold")
    ap.add_argument("--todo-days", type=int, default=365, help="CT008 age threshold")
    ap.add_argument("--max-print", type=int, default=15, help="examples printed per check")
    ap.add_argument("--jobs", type=int)
    a = ap.parse_args(argv)
    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(errors="replace")
        except Exception:
            pass
    root = os.path.abspath(a.root)
    enabled = [c.strip().upper() for c in a.checks.split(",")] if a.checks else list(CHECKS)
    bad = [c for c in enabled if c not in CHECKS]
    if bad:
        ap.error(f"unknown checks: {bad}")
    if a.apply and not a.fix:
        ap.error("--apply requires --fix")
    if a.fix:
        enabled = ["CT003"]
    t0 = time.time()
    findings, ctx = collect(root, a.paths, enabled, a.todo_days, a.max_block, a.jobs)
    dt_s = time.time() - t0
    if a.fix:
        run_fix(root, findings, a.apply)
        return 0
    per, pd = summarize(findings)
    if a.json:
        doc = {"root": root, "files_scanned": len(ctx.scope_files), "seconds": round(dt_s, 1),
               "counts": dict(per), "by_dir": {k: dict(v) for k, v in pd.items()},
               "findings": findings}
        s = json.dumps(doc, indent=1)
        if a.json == "-":
            print(s)
            return 0
        with open(a.json, "w", encoding="utf-8") as fh:
            fh.write(s)
    shown = collections.Counter()
    for f in findings:
        if shown[f["id"]] < a.max_print:
            shown[f["id"]] += 1
            print(f"{f['file']}:{f['line']}: {f['severity']}: {f['message']} [{f['id']} {f['name']}]")
    print(f"\ncomment-tidy: {len(ctx.scope_files)} files, {len(findings)} findings, {dt_s:.1f}s")
    for cid in sorted(CHECKS):
        if cid in enabled:
            print(f"  {cid} {CHECKS[cid][0]:<20} {per.get(cid, 0):>6}{'  [fixable]' if CHECKS[cid][2] else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
