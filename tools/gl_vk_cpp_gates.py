#!/usr/bin/env python3
"""gl_vk_cpp_gates.py -- which MC2_* gate drives a given uniform / define on the C++ side.

WHY: the GL->VK coverage check needs, for a shader feature (a uniform name or a compile-time
define), the MC2_* gate that decides whether the GL path feeds it. The engine has no table for
that; the answer lives in the C++ around the setter. This module recovers it heuristically but
deterministically, and reports HOW FAR the gate is from the setter so the caller can grade
confidence:

  hops  what links the gate to the setter
  0..2  the setter's own value/condition reaches a getenv through at most two local aliases or
        accessor functions                                   -> HIGH
  3..4  longer alias chains                                   -> MEDIUM
  prox  the gate is read somewhere in the same function but nothing links it to the setter -> LOW

Limits (so nobody over-trusts it): a value passed in through a struct member or a parameter
filled by another function is invisible, and a setter reached only through a gated call site is
not attributed. Missing a gate yields no row, never a wrong status.
"""
from __future__ import annotations

import re

from gl_vk_source import blank_comments

# An env-read call (getenv("MC2_X"), envFlag("MC2_X")) or a typed gate reader (gate::get<"MC2_X">() and its
# state/isSet/isNonEmpty/str/is siblings); group 2 is the gate name in both forms.
_ENV_CALL_RE = re.compile(r'(?:\b(\w*[Ee][Nn][Vv]\w*)\s*\(\s*|\b(?:get|state|isSet|isNonEmpty|armed|regionTier1Armed|str|is)\s*<\s*)"(MC2_[A-Z0-9_]+)"')
_KEYWORDS = {"if", "else", "for", "while", "switch", "catch", "do", "try", "return", "sizeof",
             "namespace", "class", "struct", "enum", "union", "const", "static", "auto", "bool",
             "int", "float", "char", "void", "true", "false", "nullptr", "this", "new", "delete",
             "unsigned", "double", "long", "short", "inline", "constexpr", "typedef", "using",
             "operator", "template", "typename", "default", "case", "break", "continue", "goto"}
NL = chr(10)
ACCESSOR_MAX_LINES = 40   # larger functions read many gates for unrelated reasons
WRAPPER_MAX_LINES = 12    # only tiny functions count as wrappers of another accessor
_IDENT_RE = re.compile(r"(?<!\w)[A-Za-z_]\w*")      # not the f of 2.0f
_ASSIGN_RE = re.compile(r"(?<![\w.>])([A-Za-z_]\w*)\s*=(?!=)")
_FUNC_TAIL_RE = re.compile(r"(?:\b(?:const|noexcept|override|final)\b\s*|->\s*[\w:<>,&*\s]+?)*$")
_LAMBDA_RE = re.compile(r"\[[^\]\[]*\]\s*(?:\([^)]*\))?\s*(?:mutable\s*)?(?:->\s*[\w:<>&*\s]+)?\s*$")
_CONTROL_HEAD_RE = re.compile(r"\s*(?:else\s+)?(?:if|for|while|switch|catch)\b")
_CONTROL_RE = re.compile(r"^(?:else\s+)?(if|while|for|switch|catch)\b|^(else|do|try)\b")
_TYPE_RE = re.compile(r"^(namespace|class|struct|enum|union)\b|\b(?:class|struct|enum|union|namespace)\s+\w*\s*(?::[^{]*)?$")


class Block:
    __slots__ = ("open", "close", "header", "parent", "kind", "name")

    def __init__(self, open_, header, parent):
        self.open, self.close, self.header, self.parent = open_, None, header, parent
        self.kind, self.name = "other", ""


def _header_start(skel, pos):
    depth, i = 0, pos - 1
    while i >= 0:
        c = skel[i]
        if c in ")]":
            depth += 1
        elif c in "([":
            depth -= 1
        elif depth <= 0 and c in ";{}":
            return i + 1
        i -= 1
    return 0


def _classify(b):
    h = " ".join(b.header.split())
    if _CONTROL_RE.match(h):
        b.kind = "control"
        return
    if _TYPE_RE.search(h) and "(" not in h:
        b.kind = "scope"
        return
    if _LAMBDA_RE.search(h):
        b.kind = "lambda"
        return
    core = _FUNC_TAIL_RE.sub("", h).rstrip()
    if core.endswith(")"):
        depth, i = 0, len(core) - 1
        while i >= 0:
            if core[i] == ")":
                depth += 1
            elif core[i] == "(":
                depth -= 1
                if depth == 0:
                    break
            i -= 1
        m = re.search(r"([A-Za-z_~][\w:~]*)\s*$", core[:i])
        if m and m.group(1).split("::")[-1] not in _KEYWORDS:
            b.kind, b.name = "function", m.group(1).split("::")[-1]
            return
    b.kind = "other"


def _is_block_header(head):
    """True when `head` (text up to a `{`) opens a function/class body rather than a lambda or
    a brace-initialiser that belongs to the same statement."""
    h = head.rstrip()
    if "[" in h and h.endswith(")"):
        return False                      # lambda with a parameter list
    return h.endswith((")", "const", "override", "noexcept", "final")) or bool(
        re.search(r"(?:class|struct|namespace|enum)\s+\w*\s*(?::[^{]*)?$", h))


class CppFile:
    def __init__(self, path, raw):
        self.path = path
        self.code = blank_comments(raw)                  # comments gone, strings kept
        self.skel = blank_comments(raw, keep_strings=False)
        self.blocks = []
        self._idents = None
        stack = []
        for m in re.finditer(r"[{}]", self.skel):
            if m.group() == "{":
                hs = _header_start(self.skel, m.start())
                b = Block(m.start(), self.skel[hs:m.start()], stack[-1] if stack else -1)
                _classify(b)
                self.blocks.append(b)
                stack.append(len(self.blocks) - 1)
            elif stack:
                self.blocks[stack.pop()].close = m.start()
        for b in self.blocks:
            if b.close is None:
                b.close = len(self.skel)

    def chain(self, pos):
        """Blocks containing pos, innermost first."""
        out = []
        for i in range(len(self.blocks) - 1, -1, -1):
            b = self.blocks[i]
            if b.open < pos < b.close:
                out.append(b)
        out.sort(key=lambda b: -b.open)
        return out

    def stmt_bounds(self, pos):
        """(start, end) of the statement containing pos, line-oriented: it starts after the
        previous line that ends a statement/block and runs to the first top-level `;` (or the
        `{` that closes a control header) at or after pos."""
        s = self.skel
        start = s.rfind(NL, 0, pos) + 1
        while start > 0:
            pl_start = s.rfind(NL, 0, start - 1) + 1
            prev = s[pl_start:start - 1].strip()
            if not prev or prev.endswith((";", "{", "}")) or prev.startswith("#"):
                break
            start = pl_start
        depth, i, n = 0, start, len(s)
        while i < n:
            c = s[i]
            if c in "([":
                depth += 1
            elif c in ")]":
                depth -= 1
            elif c == "{":
                if depth <= 0 and i >= pos and (_CONTROL_HEAD_RE.match(s[start:i])
                                                or _is_block_header(s[start:i])):
                    return start, i
                depth += 1
            elif c == "}":
                depth -= 1
            elif c == ";" and depth <= 0 and i >= pos:
                return start, i + 1
            i += 1
        return start, n

    def ident_offsets(self):
        """{identifier: [offset]} over the skeleton, built once per file."""
        if self._idents is None:
            self._idents = {}
            for m in _IDENT_RE.finditer(self.skel):
                self._idents.setdefault(m.group(), []).append(m.start())
        return self._idents

    def readers(self):
        """[(gate, offset)] for every env-read call in the file."""
        return [(m.group(2), m.start()) for m in _ENV_CALL_RE.finditer(self.code)]


def _idents(text):
    return [t for t in _IDENT_RE.findall(text) if t not in _KEYWORDS]


class GateIndex:
    """Symbol table (accessor functions / alias variables -> gates) over the scanned C++."""

    def __init__(self, files, ndefs=None):
        """ndefs: {function name: {path: definition count}} over the WHOLE tree (see count_defs);
        without it only the given files are counted."""
        self.files = {p: f for p, f in files.items()}
        self.sym = {}        # name -> {gate: (hops, path)}
        self.ndefs = ndefs if ndefs is not None else {}
        self._assigns = {}   # (path, function open) -> {ident: [offset after `ident =`]}
        if ndefs is None:
            for path, f in files.items():
                for b in f.blocks:
                    if b.kind == "function" and b.name:
                        d = self.ndefs.setdefault(b.name, {})
                        d[path] = d.get(path, 0) + 1
        self._build()

    def lookup(self, ident, path):
        """Gates an identifier stands for at a site in `path`. A name defined more than once
        (apply, end, getName ...) is only trusted inside the file that defines it once."""
        ent = self.sym.get(ident)
        if not ent:
            return {}
        defs = self.ndefs.get(ident, {})
        if sum(defs.values()) > 1:
            if defs.get(path) != 1:
                return {}
            return {g: v for g, v in ent.items() if v[1] == path}
        return ent

    def _add(self, name, gate, hops, path):
        cur = self.sym.setdefault(name, {})
        old = cur.get(gate)
        if old is None or hops < old[0]:
            cur[gate] = (hops, path)

    def _build(self):
        calls = []   # (path, funcname, set(idents)) for transitive closure
        for path, f in sorted(self.files.items()):
            for gate, off in f.readers():
                chain = f.chain(off)
                lam = next((b for b in chain if b.kind == "lambda"), None)
                fn = next((b for b in chain if b.kind == "function"), None)
                if fn is not None:
                    if fn.name and f.skel.count(NL, fn.open, fn.close) <= ACCESSOR_MAX_LINES:
                        self._add(fn.name, gate, 1, path)
                    continue          # locals are resolved per setter, never globally
                if any(b.kind != "scope" for b in chain if b is not lam):
                    continue          # not file/namespace scope: a misparsed local, not a global
                if lam is not None:
                    m = re.search(r"(\w+)\s*(?:=|\()\s*\[", lam.header)
                else:
                    st, _ = f.stmt_bounds(off)
                    m = re.match(r"\s*(?:[\w:<>*&\s]+?[\s*&])?(\w+)\s*(?:=|\{|\()[^;]*$", f.skel[st:off])
                if m and m.group(1) not in _KEYWORDS:
                    self._add(m.group(1), gate, 1, path)
            for b in f.blocks:
                if b.kind == "function" and b.name and f.skel.count(NL, b.open, b.close) <= WRAPPER_MAX_LINES:
                    calls.append((path, b.name, set(_idents(f.code[b.open:b.close]))))
        for path, fname, ids in calls:       # one level: a tiny wrapper of an accessor
            for ident in ids:
                if ident == fname:
                    continue
                for gate, (hops, gpath) in list(self.lookup(ident, path).items()):
                    if self.sym.get(fname, {}).get(gate) is None:
                        self._add(fname, gate, hops + 1, gpath)

    # --- per-site attribution -------------------------------------------------

    def _resolve_expr(self, f, text, hops, fn_block, before, seen, out):
        for m in _ENV_CALL_RE.finditer(text):
            self._note(out, m.group(2), hops)
        for ident in dict.fromkeys(_idents(text)):
            for gate, (h, gpath) in self.lookup(ident, f.path).items():
                self._note(out, gate, hops + h)
            if hops < 4 and ident not in seen and fn_block is not None:
                seen.add(ident)
                d = self._local_def(f, ident, fn_block, before)
                if d:
                    self._resolve_expr(f, d, hops + 1, fn_block, before, seen, out)

    @staticmethod
    def _note(out, gate, hops):
        if gate not in out or hops < out[gate]:
            out[gate] = hops

    def _local_def(self, f, ident, fn_block, before):
        """Right-hand side of the last `ident = ...` in the function before `before`."""
        key = (f.path, fn_block.open)
        table = self._assigns.get(key)
        if table is None:
            table = {}
            sk = f.skel[fn_block.open:fn_block.close]
            for m in _ASSIGN_RE.finditer(sk):
                table.setdefault(m.group(1), []).append(fn_block.open + m.end())
            self._assigns[key] = table
        ends = [e for e in table.get(ident, ()) if e < before]
        if not ends:
            return None
        start = ends[-1]
        end = f.stmt_bounds(start)[1]
        return f.code[start:min(end, before + 4000)]

    def attribute(self, path, offset):
        """{gate: hops} for the setter whose feature name / location variable sits at `offset`.
        Only the value arguments after that token and the enclosing if/while conditions count;
        the callee and receiver (setInt, edgeFogProg_->) say nothing about the gate."""
        f = self.files[path]
        chain = f.chain(offset)
        fn = next((b for b in chain if b.kind == "function"), None)
        _, en = f.stmt_bounds(offset)
        texts = [f.code[offset:en]]
        out, seen = {}, set()
        for t in texts:
            self._resolve_expr(f, t, 0, fn, offset, seen, out)
        return out

    def attribute_cond(self, path, offset):
        """{gate: hops} for the if/while conditions enclosing the setter. Weaker evidence than
        attribute(): a block usually holds many unrelated setters, so the caller caps these at
        MEDIUM."""
        f = self.files[path]
        fn = next((b for b in f.chain(offset) if b.kind == "function"), None)
        out, seen = {}, set()
        for b in f.chain(offset):
            if b.kind == "control":
                h = " ".join(b.header.split())
                if re.match(r"(?:else\s+)?(?:if|while)\b", h):
                    self._resolve_expr(f, h, 0, fn, offset, seen, out)
        return out

    def attribute_expr(self, path, expr):
        """Gates reachable from a bare expression (a variants-manifest condition)."""
        out, seen = {}, set()
        self._resolve_expr(self.files[path], expr, 0, None, 0, seen, out)
        return out


_DEF_RE = re.compile(r"([A-Za-z_]\w*)\s*\([^;(){}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{")


def count_defs(path, code, into):
    """Cheap per-file function-definition census (name -> {path: count}) for files that are not
    parsed in full, so that a helper name used in many classes is recognised as ambiguous."""
    for m in _DEF_RE.finditer(code):
        name = m.group(1)
        if name not in _KEYWORDS:
            d = into.setdefault(name, {})
            d[path] = d.get(path, 0) + 1


def hops_to_conf(hops):
    if hops <= 2:
        return "HIGH"
    if hops <= 4:
        return "MEDIUM"
    return "LOW"


def feature_sites(files, name):
    """[(path, offset)] where the quoted feature name, or a location variable assigned from it,
    appears. Offsets index CppFile.code/skel."""
    quoted = '"' + name + '"'
    sites = []
    for path, f in sorted(files.items()):
        if quoted not in f.code:
            continue
        aliases = set()
        for m in re.finditer(re.escape(quoted), f.code):
            sites.append((path, m.start()))
            st, _ = f.stmt_bounds(m.start())
            pre = f.skel[st:m.start()]
            am = re.search(r"(\w+)\s*=\s*[\w:.>-]*\(?[^=;]*$", pre)
            if am and am.group(1) not in _KEYWORDS:
                aliases.add(am.group(1))
        have = {o for p, o in sites if p == path}
        for al in sorted(aliases):
            for off in f.ident_offsets().get(al, ()):
                if off not in have and (off == 0 or f.skel[off - 1] not in ".>"):
                    sites.append((path, off))
                    have.add(off)
    return sites
