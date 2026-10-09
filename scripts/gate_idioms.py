#!/usr/bin/env python3
"""Shared reader-idiom machinery for the gate checkers.

WHY THIS EXISTS
---------------
scripts/check-gate-default-agreement.py answers one question per reader: "what
does it do when the variable is UNSET?" (ON / OFF). That is enough to prove a
registry default honest, and it is NOT enough to prove two readers of the same
gate AGREE: `v[0]=='1'` and `v && v[0]!='0'` both say OFF when unset yet
disagree on "", on "false" and on "yes". A typed gate table (design S2) has one
rule per gate, so every pair of readers that differ anywhere is a behaviour
change the migration must either unify or record.

This module holds the two things both tools need:

  1. the source-walking plumbing (comment stripping, block finding, file
     enumeration), lifted out of the checker unchanged so there is ONE copy of
     "what is code and what is a comment", and
  2. a reader INTERPRETER: it runs the C++ that surrounds an `"MC2_*"` read
     against eight probe values and reports the outcome for each. A reader is
     therefore described by what it DOES, not by which spelling it used, so
     `!(v && v[0]=='0')`, `(v && v[0]=='0') ? 0 : 1` and a helper that wraps
     either all land on the same outcome vector without a new regex each time.

Probe inputs (the six classes of the design plus two sub-probes of "other"):
  U unset, E "", Z "0", F "false", O "1", X "yes", X10 "10", X25 "2.5".
"""

import os
import re

# ---------------------------------------------------------------------------
# Source plumbing (lifted verbatim from check-gate-default-agreement.py)
# ---------------------------------------------------------------------------

SCAN_EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")
SKIP_DIRS = (".git", "build64", "third_party", "3rdparty")


_HOP_RE = re.compile(r"[\"']|//|/\*")
_STRING_END_RE = {
    '"': re.compile(r'[^"\\]*(?:\\[\s\S][^"\\]*)*"'),
    "'": re.compile(r"[^'\\]*(?:\\[\s\S][^'\\]*)*'"),
}
_NOT_NEWLINE_RE = re.compile(r"[^\n]")


def strip_comments(src):
    """Remove // and /* */ comments, preserving string literals and byte offsets.

    Offsets are preserved by overwriting comment bytes with spaces (newlines
    kept) so reported line numbers stay correct. String literals are preserved
    because env var names live inside them.
    """
    parts, last, i, n = [], 0, 0, len(src)
    search = _HOP_RE.search
    while True:
        m = search(src, i)
        if not m:
            break
        c, start = m.group(), m.start()
        if c == '"' or c == "'":
            em = _STRING_END_RE[c].match(src, start + 1)
            i = em.end() if em else n
        elif c == "//":
            end = src.find("\n", start)
            end = n if end == -1 else end
            parts.append(src[last:start])
            parts.append(" " * (end - start))
            last = i = end
        else:
            end = src.find("*/", start + 2)
            end = n if end == -1 else end + 2
            parts.append(src[last:start])
            parts.append(_NOT_NEWLINE_RE.sub(" ", src[start:end]))
            last = i = end
    parts.append(src[last:])
    return "".join(parts)


def enclosing_block(text, pos):
    """Span of the innermost { ... } containing `pos`, or None."""
    depth, open_at = 0, -1
    i = pos - 1
    while i >= 0:
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                open_at = i
                break
            depth -= 1
        i -= 1
    if open_at == -1:
        return None
    depth, j, n = 0, open_at, len(text)
    while j < n:
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, j + 1)
        j += 1
    return None


def load_sources(root, scan_dirs, exts=SCAN_EXTS, exclude=()):
    """-> [(relpath, comment-stripped text)] for every source file under scan_dirs."""
    excl = {os.path.normcase(os.path.abspath(p)) for p in exclude}
    out = []
    for d in scan_dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [x for x in dirnames if x not in SKIP_DIRS]
            for fn in filenames:
                if not fn.endswith(exts):
                    continue
                full = os.path.join(dirpath, fn)
                if os.path.normcase(os.path.abspath(full)) in excl:
                    continue
                try:
                    with open(full, "r", encoding="utf-8", errors="replace") as fh:
                        raw = fh.read()
                except OSError:
                    continue
                out.append((os.path.relpath(full, root).replace("\\", "/"),
                            strip_comments(raw)))
    return out


# ---------------------------------------------------------------------------
# Probes and outcomes
# ---------------------------------------------------------------------------

#: (label, value) -- value None means "variable not in the environment".
PROBES = (("U", None), ("E", ""), ("Z", "0"), ("F", "false"), ("O", "1"),
          ("X", "yes"), ("X10", "10"), ("X25", "2.5"))
PROBE_LABELS = tuple(p[0] for p in PROBES)

#: Outcome wildcard: the interpreter could not decide (opaque callee, unknown
#: global). A wildcard never counts as a disagreement.
WILD = "?"
#: Outcome for "inherits an umbrella gate" (mc2vkgate::subGate and friends).
INH = "inh"


class _Opaque:
    """A value the interpreter cannot compute. Absorbs through operators."""
    __slots__ = ("why",)

    def __init__(self, why=""):
        self.why = why

    def __repr__(self):
        return "OPAQ(%s)" % self.why


OPAQ = _Opaque()


class _Inherit(_Opaque):
    """The umbrella value of a sub-gate: distinct from any concrete answer."""


INHERIT = _Inherit("umbrella")


class _Ref:
    """`&local` -- lets a helper write its result through an out-parameter."""
    __slots__ = ("ctx", "name")

    def __init__(self, ctx, name):
        self.ctx, self.name = ctx, name


class _Unsupported(Exception):
    pass


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

_TOK_RE = re.compile(r'''
  (?P<ws>\s+)
 |(?P<str>(?:u8|[LuU])?"(?:\\.|[^"\\\n])*")
 |(?P<chr>'(?:\\.|[^'\\\n])+')
 |(?P<num>0[xX][0-9a-fA-F']+[uUlL]*
        |(?:\d[\d']*\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fFuUlL]*)
 |(?P<id>[A-Za-z_]\w*)
 |(?P<op>::|->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=
        |[-+*/%&|^~!<>=?:;,.(){}\[\]])
''', re.X)

# A directive runs to the end of its line, and past it for every line that ends
# in a backslash splice.
_DIRECTIVE_RE = re.compile(r"(?m)^[ \t]*#(?:[^\n\\]|\\\n|\\.)*")
_DEFINE_HEAD_RE = re.compile(r"#\s*define\s+\w+(?:\([^)]*\))?")


def blank_directives(text):
    """Blank preprocessor lines, keeping offsets. A macro BODY stays readable:
    several trace gates are read inside `#define` bodies and would otherwise
    vanish; only the `#define NAME(params)` head and the line splices go."""
    def repl(m):
        s = m.group(0)
        d = _DEFINE_HEAD_RE.search(s)
        keep_from = d.end() if d else len(s)
        head = re.sub(r"[^\n]", " ", s[:keep_from])
        tail = s[keep_from:].replace("\\\n", " \n")
        return head + tail
    return _DIRECTIVE_RE.sub(repl, text)


def tokenize(text, base=0):
    """-> list of (kind, text, offset). Unknown characters are dropped."""
    out = []
    pos, n = 0, len(text)
    while pos < n:
        m = _TOK_RE.match(text, pos)
        if not m:
            pos += 1
            continue
        pos = m.end()
        k = m.lastgroup
        if k == "ws":
            continue
        out.append((k, m.group(0), base + m.start()))
    # merge adjacent string literals
    merged = []
    for t in out:
        if t[0] == "str" and merged and merged[-1][0] == "str":
            a = merged[-1]
            merged[-1] = ("str", a[1][:-1] + t[1][t[1].index('"') + 1:], a[2])
        else:
            merged.append(t)
    return merged


_ESC = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", "'": "'",
        '"': '"', "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


def _unescape(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            out.append(_ESC.get(body[i + 1], body[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _str_value(tok_text):
    q = tok_text.index('"')
    return _unescape(tok_text[q + 1:-1])


def _num_value(s):
    t = s.replace("'", "")
    if t[:2] in ("0x", "0X"):
        return int(re.sub(r"[uUlL]+$", "", t), 16)
    is_float = bool(re.search(r"[.eE]", t)) or (t[-1:] in "fF" and re.search(r"\d", t[:-1]) and "." in t)
    t2 = re.sub(r"[fFuUlL]+$", "", t)
    try:
        return float(t2) if is_float else int(t2, 8 if len(t2) > 1 and t2[0] == "0" and t2.isdigit() else 10)
    except ValueError:
        return OPAQ


# ---------------------------------------------------------------------------
# Expression parser (C subset) -> tuple AST
# ---------------------------------------------------------------------------

class ParseError(Exception):
    pass


_BIN_PREC = {"*": 13, "/": 13, "%": 13, "+": 12, "-": 12, "<<": 11, ">>": 11,
             "<": 10, "<=": 10, ">": 10, ">=": 10, "==": 9, "!=": 9,
             "&": 8, "^": 7, "|": 6, "&&": 5, "||": 4}
_ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
_TYPE_WORDS = {"const", "unsigned", "signed", "char", "short", "int", "long",
               "float", "double", "bool", "size_t", "ptrdiff_t", "auto",
               "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
               "int16_t", "int32_t", "int64_t", "intptr_t", "uintptr_t",
               "volatile", "static", "constexpr", "inline", "thread_local",
               "register", "void", "wchar_t", "DWORD", "BOOL", "GLint",
               "GLuint", "GLfloat", "GLenum", "GLsizei", "ULONG", "LONG",
               "std", "u32", "u8", "i32", "f32", "mc2f", "long long"}
_CAST_NAMES = {"static_cast", "reinterpret_cast", "const_cast", "dynamic_cast"}
_KEYWORD_NOT_CALL = {"if", "for", "while", "switch", "catch", "return", "sizeof",
                     "alignof", "decltype", "else", "do", "throw", "new", "delete"}


def _match(ts, i, open_, close):
    """Index of the token matching ts[i] (an opener), or raise."""
    depth = 0
    for j in range(i, len(ts)):
        t = ts[j]
        if t[0] == "op":
            if t[1] == open_:
                depth += 1
            elif t[1] == close:
                depth -= 1
                if depth == 0:
                    return j
    raise ParseError("unbalanced " + open_)


def _is_op(t, s):
    return t is not None and t[0] == "op" and t[1] == s


class _P:
    """Recursive-descent parser over a token list."""

    def __init__(self, ts):
        self.ts = ts

    def peek(self, i):
        return self.ts[i] if i < len(self.ts) else None

    # --- expressions -------------------------------------------------------
    def expr(self, i, stop_comma=True):
        return self.assign(i)

    def assign(self, i):
        lhs, j = self.ternary(i)
        t = self.peek(j)
        if t and t[0] == "op" and t[1] in _ASSIGN_OPS:
            rhs, k = self.assign(j + 1)
            return ("assign", t[1], lhs, rhs), k
        return lhs, j

    def ternary(self, i):
        c, j = self.binary(i, 0)
        if _is_op(self.peek(j), "?"):
            a, k = self.assign(j + 1)
            if not _is_op(self.peek(k), ":"):
                raise ParseError("ternary ':'")
            b, m = self.assign(k + 1)
            return ("tern", c, a, b), m
        return c, j

    def binary(self, i, minp):
        lhs, j = self.unary(i)
        while True:
            t = self.peek(j)
            if not (t and t[0] == "op" and t[1] in _BIN_PREC):
                return lhs, j
            p = _BIN_PREC[t[1]]
            if p < minp:
                return lhs, j
            rhs, k = self.binary(j + 1, p + 1)
            lhs, j = ("bin", t[1], lhs, rhs), k

    def _castable(self, i):
        """`(type)` at ts[i]? -> index after ')' else None."""
        if not _is_op(self.peek(i), "("):
            return None
        try:
            c = _match(self.ts, i, "(", ")")
        except ParseError:
            return None
        inner = self.ts[i + 1:c]
        if not inner:
            return None
        ok = False
        for t in inner:
            if t[0] == "id" and (t[1] in _TYPE_WORDS or t[1].endswith("_t")):
                ok = True
            elif t[0] == "op" and t[1] in ("*", "&", "::"):
                continue
            elif t[0] == "id" and ok:
                continue          # std::string etc after a std
            else:
                return None
        if not ok:
            return None
        nxt = self.peek(c + 1)
        if nxt and (nxt[0] in ("id", "num", "chr", "str") or _is_op(nxt, "(")
                    or _is_op(nxt, "!") or _is_op(nxt, "-")):
            return c + 1
        return None

    def unary(self, i):
        t = self.peek(i)
        if t is None:
            raise ParseError("eof in expression")
        if t[0] == "op" and t[1] in ("!", "-", "+", "~", "*", "&"):
            e, j = self.unary(i + 1)
            return ("un", t[1], e), j
        if t[0] == "op" and t[1] in ("++", "--"):
            e, j = self.unary(i + 1)
            return ("opaque", "incdec"), j
        ci = self._castable(i)
        if ci is not None:
            e, j = self.unary(ci)
            tname = " ".join(x[1] for x in self.ts[i + 1:ci - 1])
            return ("cast", tname, e), j
        return self.postfix(i)

    def postfix(self, i):
        e, j = self.primary(i)
        while True:
            t = self.peek(j)
            if t is None or t[0] != "op":
                return e, j
            if t[1] == "(":
                args, j = self.args(j)
                e = ("call", e, args)
            elif t[1] == "[":
                idx, k = self.expr(j + 1)
                if not _is_op(self.peek(k), "]"):
                    raise ParseError("index ']'")
                e, j = ("idx", e, idx), k + 1
            elif t[1] in (".", "->"):
                nm = self.peek(j + 1)
                if nm is None or nm[0] != "id":
                    raise ParseError("member")
                e, j = ("mem", e, nm[1]), j + 2
            elif t[1] in ("++", "--"):
                e, j = ("opaque", "incdec"), j + 1
            else:
                return e, j

    def args(self, i):
        """ts[i] == '(' -> (list of exprs, index after ')')."""
        out = []
        j = i + 1
        if _is_op(self.peek(j), ")"):
            return out, j + 1
        while True:
            a, j = self.assign(j)
            out.append(a)
            t = self.peek(j)
            if _is_op(t, ","):
                j += 1
                continue
            if _is_op(t, ")"):
                return out, j + 1
            raise ParseError("args")

    def primary(self, i):
        t = self.peek(i)
        if t is None:
            raise ParseError("eof")
        k, s, _ = t
        if k == "num":
            return ("lit", _num_value(s)), i + 1
        if k == "chr":
            body = _unescape(s[1:-1])
            return ("lit", ord(body[0]) if body else 0), i + 1
        if k == "str":
            return ("lit", _str_value(s)), i + 1
        if k == "op":
            if s == "(":
                e, j = self.expr(i + 1)
                if not _is_op(self.peek(j), ")"):
                    raise ParseError("paren")
                return e, j + 1
            if s == "[":
                return self.lambda_(i)
            if s == "::":
                return self.primary(i + 1)
            raise ParseError("unexpected " + s)
        if k == "id":
            if s in ("true", "false"):
                return ("lit", s == "true"), i + 1
            if s in ("nullptr", "NULL"):
                return ("lit", None), i + 1
            name = s
            j = i + 1
            while _is_op(self.peek(j), "::") and self.peek(j + 1) and self.peek(j + 1)[0] == "id":
                name += "::" + self.peek(j + 1)[1]
                j += 2
            base = name.split("::")[-1]
            if base in _CAST_NAMES and _is_op(self.peek(j), "<"):
                gt = None
                depth = 0
                for m in range(j, len(self.ts)):
                    if _is_op(self.ts[m], "<"):
                        depth += 1
                    elif _is_op(self.ts[m], ">"):
                        depth -= 1
                        if depth == 0:
                            gt = m
                            break
                if gt is None or not _is_op(self.peek(gt + 1), "("):
                    raise ParseError("cast<>")
                e, m = self.expr(gt + 2)
                if not _is_op(self.peek(m), ")"):
                    raise ParseError("cast ')'")
                tname = " ".join(x[1] for x in self.ts[j + 1:gt])
                return ("cast", tname, e), m + 1
            if base in ("sizeof", "alignof", "decltype"):
                if _is_op(self.peek(j), "("):
                    c = _match(self.ts, j, "(", ")")
                    return ("opaque", base), c + 1
            return ("id", name), j

    def lambda_(self, i):
        """`[caps](params) [mutable] [-> T] { body }` and an optional `(args)` call."""
        cap_end = _match(self.ts, i, "[", "]")
        j = cap_end + 1
        params = []
        if _is_op(self.peek(j), "("):
            pe = _match(self.ts, j, "(", ")")
            params = _param_names(self.ts[j + 1:pe])
            j = pe + 1
        while self.peek(j) and not _is_op(self.peek(j), "{"):
            j += 1
            if j - cap_end > 40:
                raise ParseError("lambda head")
        if not _is_op(self.peek(j), "{"):
            raise ParseError("lambda body")
        be = _match(self.ts, j, "{", "}")
        body = ("lambda", params, self.ts[j + 1:be])
        k = be + 1
        if _is_op(self.peek(k), "("):
            args, k = self.args(k)
            return ("call", body, args), k
        return body, k


def _param_names(ts):
    """Parameter names of a parameter list, in order ('' when unnamed)."""
    groups, cur, depth = [], [], 0
    for t in ts:
        if t[0] == "op" and t[1] in "(<[{":
            depth += 1
        elif t[0] == "op" and t[1] in ")>]}":
            depth -= 1
        if depth == 0 and _is_op(t, ","):
            groups.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        groups.append(cur)
    names = []
    for g in groups:
        eq = next((k for k, t in enumerate(g) if _is_op(t, "=")), len(g))
        head = g[:eq]
        ids = [t for t in head if t[0] == "id"]
        names.append(ids[-1][1] if len(ids) >= 2 or (ids and any(_is_op(t, "*") or _is_op(t, "&") for t in head)) else "")
    return names


def _param_defaults(ts):
    """Default-argument expression token slices, aligned with _param_names."""
    groups, cur, depth = [], [], 0
    for t in ts:
        if t[0] == "op" and t[1] in "(<[{":
            depth += 1
        elif t[0] == "op" and t[1] in ")>]}":
            depth -= 1
        if depth == 0 and _is_op(t, ","):
            groups.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        groups.append(cur)
    out = []
    for g in groups:
        eq = next((k for k, t in enumerate(g) if _is_op(t, "=")), None)
        out.append(g[eq + 1:] if eq is not None else None)
    return out


# ---------------------------------------------------------------------------
# Statement splitting / parsing
# ---------------------------------------------------------------------------

_CTRL_START = {"if", "for", "while", "switch", "else", "try", "catch", "do",
               "namespace", "struct", "class", "enum", "union", "case",
               "default"}


def split_statements(ts):
    """Token list -> list of (start, end) token ranges, one per top-level statement.

    A statement ends at a depth-0 ';', or at the '}' that closes a control
    statement or bare block. A '}' that closes a lambda or an initializer inside
    an expression statement does not end it -- that statement finishes at its ';'.
    """
    out, n = [], len(ts)
    i = 0
    while i < n:
        start = i
        first = ts[i]
        ctrl = (first[0] == "id" and first[1] in _CTRL_START) or _is_op(first, "{")
        depth = 0
        j = i
        while j < n:
            t = ts[j]
            if t[0] == "op":
                c = t[1]
                if c in "([{":
                    depth += 1
                elif c in ")]}":
                    depth -= 1
                    if depth < 0:
                        break
                    if depth == 0 and c == "}" and ctrl:
                        nxt = ts[j + 1] if j + 1 < n else None
                        if nxt and nxt[0] == "id" and nxt[1] in ("else", "catch", "while"):
                            j += 1
                            continue
                        j += 1
                        break
                elif c == ";" and depth == 0:
                    nxt = ts[j + 1] if j + 1 < n else None
                    if ctrl and nxt and nxt[0] == "id" and nxt[1] == "else":
                        j += 1
                        continue
                    j += 1
                    break
            j += 1
        if j == start:
            j += 1
        out.append((start, j))
        i = j
    return out


def _has_return(node):
    """Does a parsed statement contain a `return` anywhere?"""
    if node is None:
        return False
    k = node[0]
    if k == "ret":
        return True
    if k == "block":
        return any(_has_return(x) for x in node[1])
    if k == "if":
        return _has_return(node[2]) or _has_return(node[3])
    return False


def _one_stmt_end(ts, i, limit):
    """Exclusive end index of the single statement starting at ts[i]."""
    if i >= limit:
        return limit
    t = ts[i]
    if _is_op(t, "{"):
        return _match(ts, i, "{", "}") + 1
    if t[0] == "id" and t[1] in ("if", "for", "while", "switch"):
        j = i + 1
        if j < limit and ts[j][0] == "id" and ts[j][1] == "constexpr":
            j += 1
        if j < limit and _is_op(ts[j], "("):
            j = _match(ts, j, "(", ")") + 1
        e = _one_stmt_end(ts, j, limit)
        if t[1] == "if" and e < limit and ts[e][0] == "id" and ts[e][1] == "else":
            e = _one_stmt_end(ts, e + 1, limit)
        return e
    depth = 0
    for j in range(i, limit):
        c = ts[j]
        if c[0] == "op":
            if c[1] in "([{":
                depth += 1
            elif c[1] in ")]}":
                depth -= 1
            elif c[1] == ";" and depth == 0:
                return j + 1
    return limit


# ---------------------------------------------------------------------------
# Function table
# ---------------------------------------------------------------------------

_FN_DEF_RE = re.compile(
    r"(?<![\w.>])(~?[A-Za-z_]\w*)\s*\(([^(){};]*)\)\s*(?:const\s*)?(?:noexcept\s*)?"
    r"(?:->\s*[\w:<>\s*&]+?)?\s*\{")
_LAMBDA_DEF_RE = re.compile(
    r"\b([A-Za-z_]\w*)\s*=\s*\[[^\]\n]*\]\s*\(([^(){};]*)\)\s*(?:mutable\s*)?"
    r"(?:->\s*[\w:<>\s*&]+?)?\s*\{")
_BODY_OPEN_RE = re.compile(r"\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->[^{;()]{1,60})?\{")
_PARAM_BAD_CHARS = re.compile(r"[(){};]")
_NAME_BEFORE_PAREN_RE = re.compile(r"(?<![\w.>])(~?[A-Za-z_]\w*)\s*$")
_LAMBDA_NAME_RE = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*\[[^\]\n]*\]\s*$")
_PARAM_OF_INTEREST = re.compile(r"char\s*\*|string|bool\s+\w|float\s+\w|int\s+\w|double\s+\w")
_NOT_FN = {"if", "for", "while", "switch", "catch", "return", "sizeof", "else",
           "do", "static_assert", "alignas", "decltype", "defined", "__declspec"}


_CONST_RE = re.compile(
    r"\b(?:constexpr|const)\s+(?:static\s+)?(?:bool|int|float|double|long|unsigned|u?int\d+_t|size_t)"
    r"\s+([A-Za-z_]\w*)\s*=\s*(true|false|-?\d+(?:\.\d*)?[fFuUlL]*)\s*;")


def _const_value(lit):
    if lit in ("true", "false"):
        return lit == "true"
    return _num_value(lit)


class FnDef:
    __slots__ = ("name", "path", "body_lo", "body_hi", "params_text",
                 "text", "_parsed")

    def __init__(self, name, path, text, body_lo, body_hi, params_text):
        self.name, self.path, self.text = name, path, text
        self.body_lo, self.body_hi, self.params_text = body_lo, body_hi, params_text
        self._parsed = None

    def parsed(self):
        if self._parsed is None:
            ptoks = tokenize(self.params_text)
            names = _param_names(ptoks)
            defs = _param_defaults(ptoks)
            body_text = blank_directives(self.text[self.body_lo:self.body_hi])
            bts = tokenize(body_text, self.body_lo)
            self._parsed = (names, defs, bts)
        return self._parsed


class FnTable:
    """name -> [FnDef]. Only functions taking a string/bool/number parameter are
    kept: that is every env helper in the tree and nothing else worth running."""

    def __init__(self, sources):
        self.defs = {}
        self.consts = {}
        for path, text in sources:
            for cm in _CONST_RE.finditer(text):
                self.consts.setdefault(cm.group(1), []).append((path, _const_value(cm.group(2))))
            # Every `){` is a candidate body; walk back to its parameter list. Cheaper
            # than one regex that tries to match a whole signature at every identifier.
            for m in _BODY_OPEN_RE.finditer(text):
                close = m.start()
                op = text.rfind("(", max(0, close - 2000), close)
                if op < 0:
                    continue
                params = text[op + 1:close]
                if _PARAM_BAD_CHARS.search(params) or not _PARAM_OF_INTEREST.search(params):
                    continue
                head = text[max(0, op - 96):op]
                nm = _NAME_BEFORE_PAREN_RE.search(head) or _LAMBDA_NAME_RE.search(head)
                if not nm:
                    continue
                name = nm.group(1)
                if name in _NOT_FN or name.startswith("~"):
                    continue
                lo = m.end() - 1
                hi = _brace_end(text, lo)
                if hi is None or hi - lo > 6000:
                    continue
                self.defs.setdefault(name, []).append(FnDef(name, path, text, lo, hi, params))

    def const(self, name, path):
        """Value of a `const`/`constexpr` scalar, or None when unknown/ambiguous."""
        cands = self.consts.get(name.split("::")[-1], [])
        same = [v for p, v in cands if p == path]
        if same:
            return same[0]
        vals = {repr(v) for _, v in cands}
        return cands[0][1] if cands and len(vals) == 1 else None

    def resolve(self, name, path):
        base = name.split("::")[-1]
        cands = self.defs.get(base, [])
        if not cands:
            return None
        same = [d for d in cands if d.path == path]
        if same:
            return same[0]
        if len(cands) == 1:
            return cands[0]
        # several definitions in OTHER files: usable only when they behave alike.
        return _AMBIG if len({_norm(d.text[d.body_lo:d.body_hi]) for d in cands}) > 1 else cands[0]


_AMBIG = object()
#: Callees that only REPORT a value; passing a gate through them is not a read.
_LOG_CALLEE_RE = re.compile(
    r"^(?:f?printf|snprintf|sprintf|vsnprintf|OutputDebugString\w*|puts|fputs|"
    r"MC2_\w+|mc2log\w*|log\w*|LOG\w*|DEBUGWINS_print|cerr|clog)$")


def _norm(s):
    return re.sub(r"\s+", "", s)


def _brace_end(text, lo):
    depth, n = 0, len(text)
    j = lo
    while j < n:
        c = text[j]
        if c == '"' or c == "'":
            q = c
            j += 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    return None


# ---------------------------------------------------------------------------
# Evaluator
# ---------------------------------------------------------------------------

class _Ret:
    __slots__ = ("val", "tainted", "void")

    def __init__(self, val, tainted, void=False):
        self.val, self.tainted, self.void = val, tainted, void


class Ctx:
    """One probe run. Only the top context records decision events: a helper's
    own branches are an implementation detail, only its return value matters."""

    def __init__(self, interp, probe, parent=None, top=False):
        self.interp = interp
        self.probe = probe
        self.env = {}
        self.parent = parent
        self.record = top
        self.path_t = False     # a tainted branch decided how we got here
        self.unc = False        # we followed a branch chosen by an unknown condition
        self.cdepth = 0         # nesting of tainted conditions (top context)
        self.depth = (parent.depth + 1) if parent else 0
        if top:
            self.top_ctx = self
            self.events = []
            self.parsers = set()
            self.opaque_calls = set()
        else:
            self.top_ctx = parent.top_ctx if parent else None

    def lookup(self, name):
        c = self
        while c is not None:
            if name in c.env:
                return c.env[name]
            c = c.parent
        return None


def _is_opq(v):
    return isinstance(v, _Opaque)


def _truth(v):
    """C truthiness of a value, three-valued."""
    if _is_opq(v):
        return v
    if v is None:
        return False
    if isinstance(v, str):
        return True
    return bool(v)


def _unrelated(truth, tainted):
    """An opaque value the probe did not feed: some other piece of program state."""
    return _is_opq(truth) and not tainted and not isinstance(truth, _Inherit)


def _neutral(truth, tainted, identity):
    """An UNRELATED unknown factor in `&&`/`||` is the operator's identity.

    `getenv("X") && frameCounter % 64 == 0` is still, as far as the gate X is
    concerned, a presence test: the other conjunct decides when the gate is
    consulted, not what the gate means. Probe-dependent unknowns and the
    umbrella inheritance stay unknown -- they ARE the gate's behaviour.
    """
    if _is_opq(truth) and not tainted and not isinstance(truth, _Inherit):
        return identity
    return truth


def _cstr_prefix_int(s):
    m = re.match(r"\s*([+-]?\d+)", s)
    return int(m.group(1)) if m else 0


def _cstr_prefix_float(s):
    m = re.match(r"\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)", s)
    return float(m.group(1)) if m else 0.0


def _ch(v, idx):
    """v[idx] for a C string."""
    if v is None or _is_opq(v):
        return OPAQ
    if not isinstance(v, str):
        return OPAQ
    return ord(v[idx]) if 0 <= idx < len(v) else 0


class Interp:
    def __init__(self, fns, path):
        self.fns = fns
        self.path = path
        self.gate = None
        self.site_off = -1
        self.max_depth = 6
        self._fn_cache = {}

    # ---- expression evaluation: returns (value, tainted) --------------------
    def ev(self, node, ctx):
        k = node[0]
        if k == "lit":
            return node[1], False
        if k == "id":
            return self.ev_id(node[1], ctx)
        if k == "un":
            return self.ev_un(node, ctx)
        if k == "bin":
            return self.ev_bin(node, ctx)
        if k == "tern":
            c, ct = self.ev(node[1], ctx)
            tc = _truth(c)
            if isinstance(tc, _Inherit):
                return INHERIT, True
            if _is_opq(tc):
                a, at = self.ev(node[2], ctx)
                b, bt = self.ev(node[3], ctx)
                if a == b and type(a) is type(b):
                    return a, ct or at or bt
                return OPAQ, ct or at or bt
            r, rt = self.ev(node[2] if tc else node[3], ctx)
            return r, ct or rt
        if k == "cast":
            v, t = self.ev(node[2], ctx)
            tn = node[1]
            if _is_opq(v):
                return v, t
            if re.search(r"\b(?:float|double)\b", tn) and "*" not in tn:
                try:
                    return float(v), t
                except (TypeError, ValueError):
                    return OPAQ, t
            if re.search(r"\b(?:int|long|short|unsigned|size_t|_t|DWORD)\b", tn) and "*" not in tn:
                try:
                    return int(v), t
                except (TypeError, ValueError):
                    return OPAQ, t
            if re.search(r"\bbool\b", tn):
                tv = _truth(v)
                return tv, t
            return v, t
        if k == "idx":
            b, bt = self.ev(node[1], ctx)
            i, it = self.ev(node[2], ctx)
            if _is_opq(i) or _is_opq(b):
                return OPAQ, bt or it
            if isinstance(b, str) and isinstance(i, int):
                return _ch(b, i), bt or it
            return OPAQ, bt or it
        if k == "mem":
            # `cfg.path = getenv(..); if (!cfg.path) return;` -- members are keyed
            # by name alone, which is exact within the few statements we run.
            mv = ctx.lookup("." + node[2])
            if mv is not None:
                return mv
            b, bt = self.ev(node[1], ctx)
            return OPAQ, bt
        if k == "call":
            return self.ev_call(node, ctx)
        if k == "assign":
            return self.ev_assign(node, ctx)
        if k == "lambda":
            return OPAQ, False
        if k == "opaque":
            return OPAQ, False
        raise _Unsupported(k)

    def ev_id(self, name, ctx):
        if name in ("INT_MAX", "UINT_MAX"):
            return OPAQ, False
        v = ctx.lookup(name)
        if v is not None:
            return v
        c = self.fns.const(name, self.path)
        if c is not None:
            return c, False
        return OPAQ, False

    def ev_un(self, node, ctx):
        op = node[1]
        if op == "&":
            tgt = node[2]
            if tgt[0] == "id":
                return _Ref(ctx, tgt[1]), False
            if tgt[0] == "mem":
                return _Ref(ctx, "." + tgt[2]), False
            return OPAQ, False
        v, t = self.ev(node[2], ctx)
        if op == "!":
            tv = _truth(v)
            return (tv if _is_opq(tv) else (not tv)), t
        if op == "*":
            if isinstance(v, str):
                return _ch(v, 0), t
            if isinstance(v, _Ref):
                return v.ctx.lookup(v.name) or (OPAQ, False)
            return OPAQ, t
        if _is_opq(v) or v is None or isinstance(v, str):
            return OPAQ, t
        if op == "-":
            return -v, t
        if op == "+":
            return v, t
        if op == "~":
            return ~int(v), t
        return OPAQ, t

    def ev_bin(self, node, ctx):
        op = node[1]
        if op in ("&&", "||"):
            ident = (op == "&&")        # the value that leaves the other operand alone
            a, at = self.ev(node[2], ctx)
            ta = _truth(a)
            if ta is (not ident):
                return (not ident), at      # short-circuit: False && x, True || x
            b, bt = self.ev(node[3], ctx)
            tb = _truth(b)
            if ta is ident:
                # `getenv(X) && counter % 64 == 0`: the unrelated factor decides WHEN
                # the gate is consulted, not what it means -- as far as X goes, it
                # is the operator's identity. Only when the probe fed this side.
                if at and _unrelated(tb, bt):
                    return ident, at
                return tb, at or bt
            if tb is (not ident):
                return (not ident), at or bt
            if _unrelated(ta, at):
                return (tb, True) if bt else (ta, False)
            if bt is False or tb is ident or _unrelated(tb, bt):
                return ta, at
            return ta, at or bt
        a, at = self.ev(node[2], ctx)
        b, bt = self.ev(node[3], ctx)
        t = at or bt
        if _is_opq(a) or _is_opq(b):
            return OPAQ, t
        if op in ("==", "!="):
            eq = self._eq(a, b)
            if eq is None:
                return OPAQ, t
            return (eq if op == "==" else not eq), t
        if a is None or b is None or isinstance(a, (str, _Ref)) or isinstance(b, (str, _Ref)):
            return OPAQ, t
        try:
            if op == "<":
                return a < b, t
            if op == "<=":
                return a <= b, t
            if op == ">":
                return a > b, t
            if op == ">=":
                return a >= b, t
            if op == "+":
                return a + b, t
            if op == "-":
                return a - b, t
            if op == "*":
                return a * b, t
            if op == "/":
                if b == 0:
                    return OPAQ, t
                return (a // b if isinstance(a, int) and isinstance(b, int) else a / b), t
            if op == "%":
                return (a % b if b else OPAQ), t
            ia, ib = int(a), int(b)
            if op == "&":
                return ia & ib, t
            if op == "|":
                return ia | ib, t
            if op == "^":
                return ia ^ ib, t
            if op == "<<":
                return ia << ib, t
            if op == ">>":
                return ia >> ib, t
        except (TypeError, ValueError, OverflowError):
            pass
        return OPAQ, t

    @staticmethod
    def _eq(a, b):
        if a is None or b is None:
            if a is None and b is None:
                return True
            other = b if a is None else a
            if isinstance(other, str):
                return False
            if isinstance(other, (int, float)) and not isinstance(other, bool):
                return other == 0
            if isinstance(other, bool):
                return other is False
            return None
        if isinstance(a, str) and isinstance(b, str):
            return a == b
        if isinstance(a, str) or isinstance(b, str):
            return None
        if isinstance(a, _Ref) or isinstance(b, _Ref):
            return None
        return a == b

    def ev_assign(self, node, ctx):
        op, lhs, rhs = node[1], node[2], node[3]
        v, t = self.ev(rhs, ctx)
        if op != "=":
            return OPAQ, t
        if lhs[0] == "id":
            self.set_var(ctx, lhs[1], v, t, None)
            return v, t
        if lhs[0] == "un" and lhs[1] == "*" and lhs[2][0] == "id":
            ref = ctx.lookup(lhs[2][1])
            if ref is not None and isinstance(ref[0], _Ref):
                r = ref[0]
                self.set_var(r.ctx, r.name, v, t, None)
            return v, t
        if lhs[0] == "mem":
            self.set_var(ctx, "." + lhs[2], v, t, None)
        elif (lhs[0] == "idx" and ctx.record and (t or ctx.path_t)
              and not isinstance(v, (str, _Ref)) and v is not None):
            ctx.top_ctx.events.append(("assign", v, t, "[]", ctx.unc))
        return v, t

    def set_var(self, ctx, name, v, t, decl_type):
        if decl_type and re.search(r"\bbool\b", decl_type) and "*" not in decl_type \
                and not _is_opq(v):
            v = _truth(v)
        # an `int`/`long` target truncates floats; keep as-is (only parsers matter)
        owner = ctx
        if decl_type is None:
            c = ctx
            while c is not None and name not in c.env:
                c = c.parent
            owner = c or ctx
        owner.env[name] = (v, t)
        if owner.record and (t or owner.path_t) and not isinstance(v, (str, _Ref)) \
                and v is not None:
            owner.top_ctx.events.append(("assign", v, t, name, owner.unc))
        # a pointer binding (str/None) is plumbing, not a decision

    # ---- calls ---------------------------------------------------------------
    def ev_call(self, node, ctx):
        fn, args = node[1], node[2]
        if fn[0] == "lambda":
            return self.call_lambda(fn, args, ctx)
        if fn[0] == "mem":
            for a in args:
                self.ev(a, ctx)
            v, t = self.ev(fn[1], ctx)
            return OPAQ, t
        if fn[0] != "id":
            return OPAQ, False
        name = fn[1]
        base = name.split("::")[-1]
        vals = None

        def evargs():
            nonlocal vals
            if vals is None:
                vals = [self.ev(a, ctx) for a in args]
            return vals

        if base in ("getenv", "_getenv"):
            (a, _), = evargs()[:1] or [(OPAQ, False)]
            if a == self.gate:
                return ctx.probe, True
            return OPAQ, False
        if base in ("GetEnvironmentVariableA", "GetEnvironmentVariable"):
            a = evargs()[0][0] if args else OPAQ
            if a == self.gate:
                p = ctx.probe
                return (len(p) if isinstance(p, str) else 0), True
            return OPAQ, False
        if base in ("max", "min", "fmax", "fminf", "fmaxf", "fmin"):
            vs = evargs()
            if len(vs) == 2 and all(isinstance(x[0], (int, float)) and not isinstance(x[0], bool)
                                    for x in vs):
                f = max if "max" in base else min
                return f(vs[0][0], vs[1][0]), vs[0][1] or vs[1][1]
            return OPAQ, any(t for _, t in vs)
        if base in ("abs", "fabs", "fabsf", "labs"):
            vs = evargs()
            if vs and isinstance(vs[0][0], (int, float)) and not isinstance(vs[0][0], bool):
                return abs(vs[0][0]), vs[0][1]
            return OPAQ, any(t for _, t in vs)
        if base == "umbrellaOn":
            return INHERIT, True
        if base in ("atoi", "atol", "atoll", "_atoi64"):
            return self._parse(ctx, base, evargs(), lambda s: _cstr_prefix_int(s))
        if base in ("atof", "strtod", "strtof", "strtold"):
            return self._parse(ctx, base, evargs()[:1], lambda s: _cstr_prefix_float(s))
        if base in ("strtol", "strtoll", "strtoul", "strtoull"):
            return self._parse(ctx, base, evargs()[:1], lambda s: _cstr_prefix_int(s))
        if base in ("stoi", "stol", "stoll", "stof", "stod"):
            vs = evargs()
            a, t = vs[0] if vs else (OPAQ, False)
            ctx.top_ctx.parsers.add(base)
            if isinstance(a, str):
                if base in ("stof", "stod"):
                    m = re.match(r"\s*[+-]?(?:\d+\.?\d*|\.\d+)", a)
                    return (_cstr_prefix_float(a) if m else OPAQ), t
                m = re.match(r"\s*[+-]?\d+", a)
                return (_cstr_prefix_int(a) if m else OPAQ), t
            return OPAQ, t
        if base in ("strcmp", "_stricmp", "stricmp", "strcasecmp", "_strcmpi"):
            vs = evargs()
            if len(vs) < 2:
                return OPAQ, False
            (a, at), (b, bt) = vs[0], vs[1]
            if isinstance(a, str) and isinstance(b, str):
                if base != "strcmp":
                    a, b = a.lower(), b.lower()
                return (0 if a == b else (-1 if a < b else 1)), at or bt
            return OPAQ, at or bt
        if base in ("strncmp", "_strnicmp", "strncasecmp"):
            vs = evargs()
            if len(vs) < 3:
                return OPAQ, False
            (a, at), (b, bt), (n, _) = vs[0], vs[1], vs[2]
            if isinstance(a, str) and isinstance(b, str) and isinstance(n, int):
                if base != "strncmp":
                    a, b = a.lower(), b.lower()
                a, b = a[:n], b[:n]
                return (0 if a == b else (-1 if a < b else 1)), at or bt
            return OPAQ, at or bt
        if base == "strlen":
            vs = evargs()
            a, t = vs[0] if vs else (OPAQ, False)
            return (len(a) if isinstance(a, str) else OPAQ), t
        if base in ("string", "string_view", "c_str") and len(args) == 1:
            a, t = evargs()[0]
            return a, t

        # user function?
        d = self.fns.resolve(name, self.path)
        if d is _AMBIG or d is None:
            vs = evargs()
            tainted = any(t for _, t in vs)
            if tainted:
                ctx.top_ctx.opaque_calls.add(base)
                if ctx.record and not _LOG_CALLEE_RE.match(base):
                    for av, at in vs:
                        if at and isinstance(av, (bool, int, float)):
                            ctx.top_ctx.events.append(("use", av, True, ctx.unc))
                            break
            return OPAQ, tainted
        return self.call_def(d, args, ctx, evargs())

    def _parse(self, ctx, base, vs, f):
        a, t = vs[0] if vs else (OPAQ, False)
        ctx.top_ctx.parsers.add(base)
        if isinstance(a, str):
            return f(a), t
        return OPAQ, t

    def call_def(self, d, args, ctx, vals):
        if ctx.depth >= self.max_depth:
            return OPAQ, any(t for _, t in vals)
        names, defaults, bts = d.parsed()
        sub = Ctx(self, ctx.probe, parent=None)
        sub.depth = ctx.depth + 1
        sub.top_ctx = ctx.top_ctx
        for idx, nm in enumerate(names):
            if not nm:
                continue
            if idx < len(vals):
                sub.env[nm] = vals[idx]
            elif idx < len(defaults) and defaults[idx]:
                try:
                    node, _ = _P(defaults[idx]).expr(0)
                    sub.env[nm] = self.ev(node, sub)
                except (ParseError, _Unsupported):
                    sub.env[nm] = (OPAQ, False)
            else:
                sub.env[nm] = (OPAQ, False)
        val, tainted = self.run_function(bts, sub)
        if not tainted:
            # The callee never returned anything probe-derived, so its answer is
            # some other piece of program state: unknown, and neutral to this gate.
            # A probe-derived ARGUMENT it swallowed is still a consumed read.
            if ctx.record and not _LOG_CALLEE_RE.match(d.name):
                for av, at in vals:
                    if at and isinstance(av, (bool, int, float)):
                        ctx.top_ctx.events.append(("use", av, True, ctx.unc))
                        break
            return OPAQ, False
        return val, tainted

    def call_lambda(self, lam, args, ctx):
        params, body_ts = lam[1], lam[2]
        vals = [self.ev(a, ctx) for a in args]
        sub = Ctx(self, ctx.probe, parent=ctx)
        for idx, nm in enumerate(params):
            if nm and idx < len(vals):
                sub.env[nm] = vals[idx]
        return self.run_function(body_ts, sub)

    def run_function(self, tokens, sub):
        """Execute a function/lambda body; -> (return value, tainted)."""
        key = (tokens[0][2], len(tokens)) if tokens else None
        stmts = self._fn_cache.get(key) if key else None
        if stmts is None:
            stmts = self.parse_block(tokens)
            if key:
                self._fn_cache[key] = stmts
        r = self.exec_list(stmts, sub)
        if isinstance(r, _Ret):
            if r.void:
                return OPAQ, False
            return r.val, r.tainted or sub.path_t
        return OPAQ, False

    # ---- statements --------------------------------------------------------
    def parse_block(self, tokens):
        out = []
        for (a, b) in split_statements(tokens):
            node = self.parse_stmt(tokens, a, b)
            if node is not None:
                out.append(node)
        return out

    def parse_stmt(self, ts, a, b):
        try:
            return self._parse_stmt(ts, a, b)
        except (ParseError, IndexError):
            return ("skip", ts[a][2], ts[b - 1][2] if b - 1 < len(ts) else ts[a][2])

    def _parse_stmt(self, ts, a, b):
        if a >= b:
            return None
        t = ts[a]
        lo_off, hi_off = ts[a][2], ts[b - 1][2]
        if _is_op(t, "{"):
            e = _match(ts, a, "{", "}")
            inner = ts[a + 1:e]
            return ("block", self.parse_block(inner), lo_off, hi_off)
        if _is_op(t, ";"):
            return None
        if t[0] == "id":
            kw = t[1]
            if kw == "if":
                j = a + 1
                if ts[j][0] == "id" and ts[j][1] == "constexpr":
                    j += 1
                if not _is_op(ts[j], "("):
                    raise ParseError("if (")
                ce = _match(ts, j, "(", ")")
                cond = self._parse_cond(ts[j + 1:ce])
                then_lo = ce + 1
                e0 = _one_stmt_end(ts, then_lo, b)
                then = self.parse_stmt(ts, then_lo, e0)
                els = None
                if e0 < b and ts[e0][0] == "id" and ts[e0][1] == "else":
                    e1 = _one_stmt_end(ts, e0 + 1, b)
                    els = self.parse_stmt(ts, e0 + 1, e1)
                return ("if", cond, then, els, lo_off, hi_off)
            if kw == "try":
                spans = split_statements(ts[a + 1:b])
                if spans:
                    s0, e0 = spans[0]
                    return self.parse_stmt(ts, a + 1 + s0, a + 1 + e0)
                return None
            if kw == "return":
                if a + 1 < b and _is_op(ts[a + 1], ";"):
                    return ("ret", None)
                if a + 1 >= b:
                    return ("ret", None)
                end = b - 1 if _is_op(ts[b - 1], ";") else b
                node, j = _P(ts[a + 1:end]).expr(0)
                if j != end - (a + 1):
                    raise ParseError("return tail")
                return ("ret", node)
            if kw in ("for", "while", "switch", "do", "else", "case", "default",
                      "break", "continue", "goto", "throw", "namespace",
                      "struct", "class", "enum", "union", "using", "typedef",
                      "catch", "static_assert", "delete", "template"):
                return ("skip", lo_off, hi_off)
        # simple statement
        end = b - 1 if _is_op(ts[b - 1], ";") else b
        return self._parse_simple(ts[a:end], lo_off, hi_off)

    def _parse_cond(self, ts):
        d = self._maybe_decl(ts)
        if d is not None:
            typ, name, rhs_ts = d
            node, _ = _P(rhs_ts).expr(0)
            return ("declcond", typ, name, node)
        # C++17 `if (init; cond)`
        depth = 0
        for k, t in enumerate(ts):
            if t[0] == "op":
                if t[1] in "([{":
                    depth += 1
                elif t[1] in ")]}":
                    depth -= 1
                elif t[1] == ";" and depth == 0:
                    init = self._parse_simple(ts[:k], 0, 0)
                    cond = self._parse_cond(ts[k + 1:])
                    return ("seq", init, cond)
        node, j = _P(ts).expr(0)
        if j != len(ts):
            raise ParseError("cond tail")
        return node

    @staticmethod
    def _maybe_decl(ts):
        """`[quals] type [*&] name = rhs` -> (type, name, rhs tokens) else None."""
        if len(ts) < 4:
            return None
        k = next((x for x, t in enumerate(ts) if _is_op(t, "=")), None)
        if k is None or k < 2:
            return None
        lhs = ts[:k]
        if lhs[-1][0] != "id":
            return None
        head = lhs[:-1]
        if not head or head[0][0] != "id":
            return None
        ids = [t[1] for t in head if t[0] == "id"]
        if not (ids[0] in _TYPE_WORDS or ids[0].endswith("_t") or (
                len(head) >= 2 and ids[0] not in _KEYWORD_NOT_CALL
                and all(t[0] == "id" or t[1] in ("*", "&", "::", "<", ">", ",") for t in head))):
            return None
        if any(t[0] == "op" and t[1] not in ("*", "&", "::", "<", ">", ",") for t in head):
            return None
        return " ".join(t[1] for t in head), lhs[-1][1], ts[k + 1:]

    def _parse_simple(self, ts, lo_off, hi_off):
        if not ts:
            return None
        d = self._maybe_decl(ts)
        if d is not None:
            typ, name, rhs_ts = d
            # split further declarators at depth-0 commas
            parts, cur, depth = [], [], 0
            for t in rhs_ts:
                if t[0] == "op" and t[1] in "([{":
                    depth += 1
                elif t[0] == "op" and t[1] in ")]}":
                    depth -= 1
                if depth == 0 and _is_op(t, ","):
                    parts.append(cur)
                    cur = []
                else:
                    cur.append(t)
            parts.append(cur)
            node, j = _P(parts[0]).expr(0)
            if j != len(parts[0]):
                raise ParseError("decl tail")
            decls = [("decl", typ, name, node, lo_off, hi_off)]
            for extra in parts[1:]:
                eq = next((x for x, t in enumerate(extra) if _is_op(t, "=")), None)
                if eq is not None and eq >= 1 and extra[eq - 1][0] == "id":
                    n2, _ = _P(extra[eq + 1:]).expr(0)
                    decls.append(("decl", typ, extra[eq - 1][1], n2, lo_off, hi_off))
            return decls[0] if len(decls) == 1 else ("block", decls, lo_off, hi_off)
        node, j = _P(ts).expr(0)
        if j != len(ts):
            # `T x(expr);` / `T x{...};` and friends: not interesting
            raise ParseError("expr tail")
        return ("expr", node, lo_off, hi_off)

    # ---- statement execution -----------------------------------------------
    def exec_list(self, stmts, ctx):
        for s in stmts:
            r = self.exec_stmt(s, ctx)
            if isinstance(r, _Ret):
                return r
        return None

    def exec_stmt(self, s, ctx):
        k = s[0]
        ev_list = ctx.top_ctx.events if ctx.record else None
        if k == "skip":
            return None
        if k == "block":
            return self.exec_list(s[1], ctx)
        if k == "expr":
            self.ev(s[1], ctx)
            return None
        if k == "decl":
            v, t = self.ev(s[3], ctx)
            self.set_var(ctx, s[2], v, t, s[1])
            return None
        if k == "ret":
            if s[1] is None:
                return _Ret(None, False, void=True)
            v, vt = self.ev(s[1], ctx)
            t = vt or ctx.path_t
            if ev_list is not None and t:
                ev_list.append(("ret", v, t and vt, ctx.unc))
            return _Ret(v, t)
        if k == "if":
            cond = s[1]
            c, t = self.ev_cond(cond, ctx)
            tc = _truth(c)
            saved_pt, saved_unc = ctx.path_t, ctx.unc
            if t:
                ctx.path_t = True
                if ev_list is not None:
                    ev_list.append(("cond", tc, ctx.cdepth, ctx.unc))
            if _is_opq(tc):
                # Unknown guard: follow the branch that holds the site, else `then`.
                then_has = self._holds_site(s[2])
                else_has = self._holds_site(s[3])
                take = s[3] if (else_has and not then_has) else s[2]
                if not (then_has or else_has):
                    ctx.unc = True      # we are guessing which way the code went
            else:
                take = s[2] if tc else s[3]
            other = s[3] if take is s[2] else s[2]
            r = None
            if take is not None:
                if t:
                    ctx.cdepth += 1
                r = self.exec_stmt(take, ctx)
                if t:
                    ctx.cdepth -= 1
            if isinstance(r, _Ret):
                if r.void and t and not _is_opq(tc) and ev_list is not None:
                    # `if (guard) return;` -- the feature does not apply on this path.
                    ev_list.append(("guard_exit",))
                return r
            # Join point. What follows depends on the condition only when the
            # branch NOT taken would have left the function (the early-out idiom).
            ctx.path_t = saved_pt or (t and _has_return(other))
            if not _is_opq(tc):
                ctx.unc = saved_unc
            return None
        raise _Unsupported(k)

    def ev_cond(self, cond, ctx):
        if isinstance(cond, tuple) and cond[0] == "declcond":
            v, t = self.ev(cond[3], ctx)
            self.set_var(ctx, cond[2], v, t, cond[1])
            return v, t
        if isinstance(cond, tuple) and cond[0] == "seq":
            self.exec_stmt(cond[1], ctx) if cond[1] else None
            return self.ev_cond(cond[2], ctx)
        return self.ev(cond, ctx)

    def _holds_site(self, node):
        if node is None:
            return False
        off = self.site_off
        if node[0] in ("block",):
            return any(self._holds_site(x) for x in node[1])
        if node[0] == "if":
            return node[4] <= off <= node[5]
        if node[0] in ("expr", "decl"):
            return node[-2] <= off <= node[-1]
        return False


# ---------------------------------------------------------------------------
# Site analysis
# ---------------------------------------------------------------------------

_NS_BLOCK_RE = re.compile(
    r"(?:namespace\s*[\w:]*|extern\s*\"[^\"]*\"|(?:class|struct|union|enum(?:\s+class)?)\s+"
    r"[\w:\s<>,]*?(?:final\s*)?(?::[^{;]*)?)\s*$")


def _block_is_scope(text, open_at):
    """True when `{` opens a namespace/class body (not an executable block)."""
    return bool(_NS_BLOCK_RE.search(text[max(0, open_at - 160):open_at]))


def _stmt_start(text, pos):
    best = -1
    for ch in ";{}":
        best = max(best, text.rfind(ch, 0, pos))
    return best + 1


def _stmt_end(text, pos):
    depth, n, j = 0, len(text), pos
    while j < n:
        c = text[j]
        if c == '"' or c == "'":
            q = c
            j += 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return j
            depth -= 1
        elif c == ";" and depth == 0:
            return j + 1
        j += 1
    return n


MAX_BLOCK_CHARS = 40000
TAIL_FORWARD = 6000


class Window:
    """Token window around a site, plus its top-level statements."""
    __slots__ = ("tokens", "spans", "site_tok", "scope_block")


_WIN_CACHE = {}


_CLEAN_CACHE = {}


def _clean_text(path, text):
    """`text` with every preprocessor line blanked. Statement and block discovery
    must not trip over a macro body's braces and semicolons; the macro body is
    still tokenized from the original text when a site lives inside one."""
    key = (path, hash(text))
    c = _CLEAN_CACHE.get(key)
    if c is None:
        c = _DIRECTIVE_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
        _CLEAN_CACHE[key] = c
    return c


def build_window(path, raw_text, pos):
    """Choose the executable scope around `pos` and tokenize it (cached per block)."""
    text = _clean_text(path, raw_text)
    if text[pos:pos + 1] == " ":      # the site is inside a directive: use the raw text
        text = raw_text
    blk = enclosing_block(text, pos)
    lo = hi = None
    use_block = False
    if blk and not _block_is_scope(text, blk[0]):
        sz = blk[1] - blk[0]
        if sz <= MAX_BLOCK_CHARS:
            lo, hi, use_block = blk[0] + 1, blk[1] - 1, True
    if not use_block:
        lo = _stmt_start(text, pos)
        hi = min(len(text), max(_stmt_end(text, lo), pos + 1))
        if blk and not _block_is_scope(text, blk[0]):
            hi = min(blk[1] - 1, max(hi, min(len(text), lo + TAIL_FORWARD)))
    key = (path, lo, hi)
    w = _WIN_CACHE.get(key)
    if w is None:
        w = Window()
        w.tokens = tokenize(blank_directives(text[lo:hi]), lo)
        w.spans = split_statements(w.tokens)
        _WIN_CACHE[key] = w
    return w


def _relevant_spans(w, site_off):
    """Statements that matter for a site: the one holding it, then every later
    statement that names a local derived from it."""
    ts, spans = w.tokens, w.spans
    si = None
    for idx, (a, b) in enumerate(spans):
        if ts[a][2] <= site_off <= ts[b - 1][2]:
            si = idx
            break
    if si is None:
        return [], None
    keep = [si]
    tainted = set()

    def lhs_names(a, b):
        # identifier directly before the first depth-0 '=' (decl or assignment)
        depth = 0
        for j in range(a, b):
            t = ts[j]
            if t[0] == "op":
                if t[1] in "([{":
                    depth += 1
                elif t[1] in ")]}":
                    depth -= 1
                elif t[1] == "=" and depth == 0 and j > a and ts[j - 1][0] == "id":
                    return ts[j - 1][1]
        return None

    a0, b0 = spans[si]
    nm = lhs_names(a0, b0)
    if nm:
        tainted.add(nm)
    # `if (const char* v = getenv(..))` binds inside the condition
    for j in range(a0, b0 - 1):
        if ts[j][0] == "id" and _is_op(ts[j + 1], "=") and ts[j][2] < site_off:
            tainted.add(ts[j][1])
    for idx in range(si + 1, len(spans)):
        a, b = spans[idx]
        names = {t[1] for t in ts[a:b] if t[0] == "id"}
        if names & tainted:
            keep.append(idx)
            nm = lhs_names(a, b)
            if nm:
                tainted.add(nm)
        elif ts[a][0] == "id" and ts[a][1] == "return":
            keep.append(idx)    # the fall-through result after the guards
    return keep, si


def analyze_site(interp, path, text, pos, gate, site_off=None):
    """Run the reader around text[pos] (the offset of the `"MC2_X"` literal).

    -> dict(outcomes=[8 strings], parsers=[...], opaque=[...], status=str).
    status is 'reader', 'nonreader' (no probe-dependent decision) or
    'unclassified:<why>'.
    """
    w = build_window(path, text, pos)
    keep, si = _relevant_spans(w, pos)
    if si is None:
        return {"status": "unclassified:no-statement", "outcomes": None,
                "parsers": [], "opaque": []}
    interp.gate = gate
    interp.site_off = pos
    interp.path = path
    stmts = []
    for idx in keep:
        a, b = w.spans[idx]
        n = interp.parse_stmt(w.tokens, a, b)
        if n is not None:
            stmts.append(n)
    if any(s[0] == "skip" and s[1] <= pos <= s[2] for s in stmts):
        return {"status": "unclassified:unparsed-statement", "outcomes": None,
                "parsers": [], "opaque": []}
    runs, finals, parsers, opaque = [], [], set(), set()
    site_stmt_start = w.tokens[w.spans[si][0]][2]
    prelude = _prelude(text, site_stmt_start, _assigned_names(stmts))
    for label, val in PROBES:
        ctx = Ctx(interp, val, top=True)
        for nm, lit in prelude.items():
            ctx.env[nm] = (lit, False)
        try:
            interp.exec_list(stmts, ctx)
        except _Unsupported as e:
            return {"status": "unclassified:unsupported-%s" % e, "outcomes": None,
                    "parsers": [], "opaque": []}
        except RecursionError:
            return {"status": "unclassified:recursion", "outcomes": None,
                    "parsers": [], "opaque": []}
        parsers |= ctx.parsers
        opaque |= ctx.opaque_calls
        runs.append(ctx.events)
        finals.append({k: v[0] for k, v in ctx.env.items()})
    outcomes = _decide_all(runs, finals)
    if all(o is None for o in outcomes):
        # The probe reached a call we cannot run and that is not a logger: that is
        # a reader we are blind to, not a harmless print.
        unknown = sorted(c for c in opaque if not _LOG_CALLEE_RE.match(c))
        if unknown:
            return {"status": "unclassified:opaque-callee(%s)" % ",".join(unknown[:3]),
                    "outcomes": None, "parsers": sorted(parsers), "opaque": sorted(opaque)}
        return {"status": "nonreader", "outcomes": None, "parsers": sorted(parsers),
                "opaque": sorted(opaque)}
    return {"status": "reader", "outcomes": outcomes, "parsers": sorted(parsers),
            "opaque": sorted(opaque)}


def _assigned_names(node, out=None):
    """Names assigned or declared anywhere inside parsed statements."""
    if out is None:
        out = set()
    if isinstance(node, (list, tuple)):
        if len(node) >= 4 and node[0] == "assign" and isinstance(node[2], tuple) and node[2][0] == "id":
            out.add(node[2][1])
        elif len(node) >= 4 and node[0] == "decl" and isinstance(node[2], str):
            out.add(node[2])
        for x in node:
            if isinstance(x, (list, tuple)):
                _assigned_names(x, out)
    return out


_DECL_INIT_ANY_RE = re.compile(
    r"\b(?:bool|int|float|double|long|unsigned|u?int\d+_t|size_t)\s+([A-Za-z_]\w*)\s*="
    r"\s*(true|false|-?\d+(?:\.\d*)?[fFuUlL]*)\s*;")


def _prelude(text, window_start, names):
    """Initial values of result variables declared just before the window
    (`int on = 0; if (const char* e = getenv(..)) on = ...;`)."""
    out = {}
    if not names:
        return out
    chunk = text[max(0, window_start - 3000):window_start]
    for m in _DECL_INIT_ANY_RE.finditer(chunk):     # later declarations win
        if m.group(1) in names:
            out[m.group(1)] = _const_value(m.group(2))
    return out


def _cond_chain(conds):
    """Truth of a nested chain of tainted conditions: the effect happens only if
    every enclosing condition held, and the deepest one evaluated decides."""
    first = conds[0]
    res, depth = first[1], first[2]
    for c in conds[1:]:
        if _is_opq(c[1]):
            break               # an unknown inner condition does not retell the gate
        if res is True and c[2] > depth:
            res, depth = c[1], c[2]
        else:
            break
    return res


def _decide_all(runs, finals=None):
    """Per-probe event lists -> per-probe decision (None / ('val'|'cond', value)).

    Decisions are made across all probes at once because what a run MEANS depends
    on its siblings: an assignment only counts when another probe shows the same
    variable receiving a probe-derived value, and a bare `return;` guard only
    means something when some other probe falls through it.
    """
    result_vars = set()
    for ev in runs:
        for e in ev:
            if e[0] == "assign" and e[2]:
                result_vars.add(e[3])
    has_guard = any(e[0] == "guard_exit" for ev in runs for e in ev)
    order = []
    for ev in runs:
        for e in ev:
            if e[0] == "assign" and e[2] and e[3] in result_vars and e[3] not in order:
                order.append(e[3])
    out = []
    for run_i, ev in enumerate(runs):
        known = lambda e: not (_is_opq(e[1]) and not isinstance(e[1], _Inherit))
        opq_seen = any(e[0] in ("ret", "assign") and not known(e) for e in ev)
        # A value the probe flowed into outranks one that merely sits on a path the
        # probe chose; both outrank a bare "was the condition true".
        rets = [e for e in ev if e[0] == "ret" and e[2] and known(e)]
        if rets:
            out.append(("val", OPAQ if rets[0][3] else rets[0][1]))
            continue
        # The FIRST such assignment is the read itself; later ones are clamps and
        # derivations (`enabled = false` when a sibling gate is off) of the result.
        assigns = [e for e in ev if e[0] == "assign" and e[3] in result_vars and known(e)]
        assigns = [e for e in assigns if e[2]] or assigns
        if assigns:
            out.append(("val", OPAQ if assigns[0][4] else assigns[0][1]))
            continue
        # No tainted assignment ran in this probe, but the variable that other
        # probes assign holds a known value (its initializer): that is the default.
        if finals is not None and order and not any(e[0] == "assign" and e[2] for e in ev):
            fv = finals[run_i].get(order[0])
            if fv is not None and not _is_opq(fv) and not isinstance(fv, str):
                out.append(("val", fv))
                continue
        uses = [e for e in ev if e[0] == "use"]
        if uses:
            out.append(("val", OPAQ if uses[0][3] else uses[0][1]))
            continue
        rets = [e for e in ev if e[0] == "ret" and known(e)]
        if rets:
            out.append(("val", OPAQ if rets[0][3] else rets[0][1]))
            continue
        if any(e[0] == "guard_exit" for e in ev):
            out.append(("val", False))
            continue
        conds = [e for e in ev if e[0] == "cond"]
        if conds:
            if has_guard:
                out.append(("val", True))     # fell through every guard
            else:
                v = _cond_chain(conds)
                out.append(("cond", OPAQ if any(c[3] for c in conds) else v))
            continue
        out.append(("val", OPAQ) if opq_seen else None)
    return out


# ---------------------------------------------------------------------------
# Outcome normalisation and naming
# ---------------------------------------------------------------------------

def _fmt_num(v):
    if isinstance(v, bool):
        return "on" if v else "off"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return ("%g" % v)
    return WILD


def normalise(raw, parsers=()):
    """Raw decisions -> (family, [8 outcome strings]).

    family: 'bool' | 'num' | 'str'. Numeric 0/1 results fold into bool, because
    `cached = (v != nullptr) ? 1 : 0` is a presence test spelled with integers.
    """
    kinds = set()
    for r in raw:
        if r is None:
            continue
        v = r[1]
        if isinstance(v, bool):
            kinds.add("bool")
        elif isinstance(v, (int, float)):
            kinds.add("num01" if (v in (0, 1) and not parsers) else "num")
        elif v is None or isinstance(v, str):
            kinds.add("str")
        elif isinstance(v, _Inherit):
            kinds.add("inh")
        else:
            kinds.add("opq")
    if "str" in kinds and not (kinds & {"bool", "num", "num01"}):
        fam = "str"
    elif "num" in kinds:
        fam = "num"
    else:
        fam = "bool"
    out = []
    has_val = any(r is not None and r[0] == "val" for r in raw)
    for r in raw:
        if r is None:
            out.append("D" if (has_val or fam != "bool") else "off")
            continue
        kind, v = r
        if kind == "cond" and has_val:
            out.append("D")     # no assignment happened: the default was kept
        elif isinstance(v, _Inherit):
            out.append(INH)
        elif isinstance(v, _Opaque):
            out.append(WILD)
        elif fam == "bool":
            if isinstance(v, (bool, int, float)):
                out.append("on" if v else "off")
            elif v is None:
                out.append("off")
            elif isinstance(v, str):
                out.append("on")
            else:
                out.append(WILD)
        elif fam == "num":
            if isinstance(v, bool):
                out.append("D" if kind == "cond" else _fmt_num(int(v)))
            elif isinstance(v, (int, float)):
                out.append(_fmt_num(v))
            else:
                out.append(WILD)
        else:   # str: does the probe's own text come out, or a substitute?
            if isinstance(v, (str, type(None))):
                probe = PROBES[len(out)][1]
                out.append("null" if v is None else
                           ("pass" if (probe is not None and v == probe) else "dflt"))
            else:
                out.append(WILD)
    inverted = False
    if fam == "bool" and out[4] == "off":
        # A reader whose answer for "1" is OFF is using the variable as a disable
        # flag (`!(v && v[0]=='1')`, `if (getenv(X)) return;`, `jsOff = v[0]=='0'`).
        # That is the same TEST as its complement, so orient it when the
        # complement is a recognised idiom. A level gate (`v[0]=='2'`) has no named
        # complement and is left alone: flipping it would misstate what it does.
        flip = {"on": "off", "off": "on"}
        flipped = [flip.get(x, x) for x in out]
        if _bits(out) not in _BOOL_NAMES and _bits(flipped) in _BOOL_NAMES:
            out, inverted = flipped, True
    return fam, out, inverted


def _bits(vec):
    return "".join({"on": "1", "off": "0"}.get(x, "?") for x in vec)


_BOOL_NAMES = {
    # (U E Z F O X X10 X25)
    "00001111": "Std(def=off)",
    "11001111": "Std(def=on)",
    "00011111": "Lead0",
    "11011111": "Lead0Kill",
    "01011111": "Lead0P",       # `v && v[0] != '0'` / envFlagDefaultOff
    "10011111": "DefOnLead0",   # `!v || (v[0] && v[0] != '0')`
    "00001010": "Lead1",
    "00001000": "Exact1",
    "00111111": "NonEmpty",
    "01111111": "Presence",
    "00001011": "AtoiNZ",
    "00101000": "Is01",         # envTriState(x) != -1 : set to exactly "0" or "1"
}


def is_uninformative(fam, vec, parsers=()):
    """A vector that never varies with the probe, or a number-valued test that only
    ever says one of two constants: the reader compares against some token (or a
    state we cannot see) that none of the probes hit."""
    vals = {v for v in vec if v not in (WILD, "D")}
    if len(vals) <= 1:
        return True
    return fam == "num" and not parsers and len(vals) <= 2


def class_name(fam, vec, parsers):
    if is_uninformative(fam, vec, parsers):
        return "Token/State"
    if fam == "bool":
        if any(x in (INH,) for x in vec):
            return "Tri(inherit)"
        key = _bits(vec)
        if "?" in key:
            return "Partial[%s]" % ",".join(vec)
        n = _BOOL_NAMES.get(key)
        if n:
            return n
        return "Custom[%s]" % key
    if fam == "str":
        return "Str[%s]" % ",".join(vec[:2])
    ps = "+".join(parsers) if parsers else "num"
    guard = "guarded" if vec[1] == "D" else "raw"
    return "Num(%s,%s)" % (ps, guard)


def vectors_conflict(fam, a, b):
    """-> list of probe labels where outcome vectors a and b genuinely differ."""
    out = []
    for i, label in enumerate(PROBE_LABELS):
        x, y = a[i], b[i]
        if x == y or x == WILD or y == WILD:
            continue
        if x == "D" or y == "D":
            continue
        out.append(label)
    return out


# ---------------------------------------------------------------------------
# Convenience entry points
# ---------------------------------------------------------------------------

def describe(result):
    """analyze_site() result -> (family, outcome vector, class name) or None."""
    if result.get("status") != "reader":
        return None
    fam, vec, inv = normalise(result["outcomes"], result.get("parsers", ()))
    return fam, vec, class_name(fam, vec, result.get("parsers", [])), inv


def classify_snippet(code, gate, helpers=""):
    """Classify the first read of `gate` in a C++ snippet (used by the tests).

    `helpers` is extra source holding helper definitions, kept in a separate
    file so it is resolved like a helper from another translation unit.
    """
    text = strip_comments(code)
    srcs = [("snippet.cpp", text)]
    if helpers:
        srcs.append(("helpers.h", strip_comments(helpers)))
    interp = Interp(FnTable(srcs), "snippet.cpp")
    _WIN_CACHE.clear()
    pos = text.index('"%s"' % gate)
    return analyze_site(interp, "snippet.cpp", text, pos, gate)


# ---------------------------------------------------------------------------
# Whole-tree scan
# ---------------------------------------------------------------------------

_LITERAL_RE = re.compile(r'"(MC2_[A-Z0-9_]+)"')
_CALLEE_RE = re.compile(r"([A-Za-z_][\w:]*)\s*\(\s*$")
_RAW_ENV_CALLEES = {"getenv", "_getenv", "GetEnvironmentVariableA",
                    "GetEnvironmentVariableW", "GetEnvironmentVariable"}


def scan_sources(sources):
    """Classify every `"MC2_*"` read in `sources` [(path, comment-stripped text)].

    -> (sites, stats). Each site is a dict:
         gate, file, line, callee, status, family, vector, cls, inverted,
         parsers, opaque
    status: 'reader' | 'token' (compares against a token no probe hits)
          | 'nonreader' (the probe never decides anything: a print, a path)
          | 'unclassified:<why>'
    A literal that is not the first argument of an env-reading call (a table
    row, a _putenv, an indirect name) is counted in stats['noncall'] only.
    """
    fns = FnTable(sources)
    interp = Interp(fns, "")
    _WIN_CACHE.clear()
    _CLEAN_CACHE.clear()
    sites = []
    stats = {"literals": 0, "noncall": 0, "gates_with_literal": set()}
    for path, text in sources:
        for m in _LITERAL_RE.finditer(text):
            gate = m.group(1)
            stats["literals"] += 1
            stats["gates_with_literal"].add(gate)
            cm = _CALLEE_RE.search(text[max(0, m.start() - 96):m.start()])
            callee = cm.group(1) if cm else None
            base = callee.split("::")[-1] if callee else None
            if not (base in _RAW_ENV_CALLEES or (callee and fns.resolve(callee, path) is not None)):
                stats["noncall"] += 1
                continue
            line = text.count("\n", 0, m.start()) + 1
            r = analyze_site(interp, path, text, m.start(), gate)
            site = {"gate": gate, "file": path, "line": line, "callee": base,
                    "status": r["status"], "family": None, "vector": None,
                    "cls": None, "inverted": False,
                    "parsers": r.get("parsers", []), "opaque": r.get("opaque", [])}
            d = describe(r)
            if d:
                fam, vec, name, inv = d
                site.update(family=fam, vector=vec, cls=name, inverted=inv)
                if not any(x not in (WILD, "D") for x in vec):
                    site["status"] = "unclassified:opaque"
                elif is_uninformative(fam, vec, site["parsers"]):
                    site["status"] = "token"
            sites.append(site)
    return sites, stats
