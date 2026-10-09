#!/usr/bin/env python3
"""extract_gl_shader_variants.py -- derive the REAL GL shader variant set from C++.

Offline half of the GL shader variant-matrix check (see
scripts/check-gl-shader-variants.py). Scans GameOS/gameos/**/*.cpp for every
place the engine builds a GL program and records, per program:

    stages (vert/frag/tesc/tese/geom/comp), the exact prefix text handed to the
    GL driver in front of the shader source (the "#version ..." line plus every
    "#define ..." the C++ injects), and how the source is assembled (engine
    include flattening vs raw text).

Sources understood (all derived from the C++, nothing hand-listed per shader):

  * glsl_program::makeProgram / makeProgram2 call sites.  The prefix argument is
    resolved by statically evaluating the std::string / const char* it names
    (literals, +=, .append, snprintf'd char arrays, ternaries, std::to_string of
    constexpr ints, helper functions returning a #define string).
  * gosRenderMaterial::load("<name>", ...) material programs: shader_list[] x
    combinations[] flag masks, with the #define lines gosMaterialVariationHelper
    composes in getMaterialVariation().
  * Direct glShaderSource compute loaders (cardcloud_sim, cluster_depth_pyramid,
    lightgrid_build, postprocess_blur) whose strs[] array is evaluated, with
    function parameters expanded from the literal arguments at their call sites.
  * gpu_driven::BuildComputeProgramFromFile (engine include flattening) and the
    gpu_cull_compute.cpp loaders (raw text; predicate-include splice) and
    gos_terrain_lighting.cpp's marker splice.

A define the C++ injects only under an `if (...)` is OPTIONAL.  Optional defines
are grouped by their condition text and expanded to variants: base (none on),
all-on, and each group alone.  The expansion is part of the manifest so a diff
shows exactly which variants a C++ change adds or removes.

Usage:
    py -3 scripts/extract_gl_shader_variants.py            # (re)write manifest
    py -3 scripts/extract_gl_shader_variants.py --check    # exit 1 if stale
    py -3 scripts/extract_gl_shader_variants.py --stdout   # print, write nothing

Exit 0 ok; 1 stale (--check) or unresolvable C++ construct (an extraction gap is
a hard failure, never a silent skip).
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAMEOS = ROOT / "GameOS" / "gameos"
MANIFEST = ROOT / "scripts" / "gl_shader_variants.json"

STAGE_BY_EXT = {".vert": "vert", ".frag": "frag", ".tesc": "tesc",
                ".tese": "tese", ".geom": "geom", ".comp": "comp"}


class ExtractError(Exception):
    pass


# --------------------------------------------------------------------------
# Lexical helpers
# --------------------------------------------------------------------------
def strip_comments(t: str) -> str:
    """Blank out // and /* */ comments (keeping offsets and newlines)."""
    out = list(t)
    i, n = 0, len(t)
    while i < n:
        c = t[i]
        nx = t[i + 1] if i + 1 < n else ""
        if c == "/" and nx == "/":
            while i < n and t[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and nx == "*":
            while i < n and not (t[i] == "*" and i + 1 < n and t[i + 1] == "/"):
                if t[i] != "\n":
                    out[i] = " "
                i += 1
            for k in (i, i + 1):
                if k < n:
                    out[k] = " "
            i += 2
        elif c == '"' or c == "'":
            q = c
            i += 1
            while i < n and t[i] != q:
                i += 2 if t[i] == "\\" else 1
            i += 1
        else:
            i += 1
    return "".join(out)


_ESC = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'", "r": "\r", "0": "\0"}


def parse_string_literals(s: str):
    """If `s` is only adjacent C string literals, return the decoded text."""
    s = s.strip()
    if not s.startswith('"'):
        return None
    out = []
    i, n = 0, len(s)
    while i < n:
        while i < n and s[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        if s[i] != '"':
            return None
        i += 1
        while i < n and s[i] != '"':
            if s[i] == "\\" and i + 1 < n:
                out.append(_ESC.get(s[i + 1], s[i + 1]))
                i += 2
            else:
                out.append(s[i])
                i += 1
        if i >= n:
            return None
        i += 1
    return "".join(out)


def skip_string(t: str, i: int) -> int:
    q = t[i]
    i += 1
    while i < len(t) and t[i] != q:
        i += 2 if t[i] == "\\" else 1
    return i + 1


def split_top(t: str, sep: str):
    """Split `t` on single-char `sep` at depth 0 (outside strings)."""
    parts, depth, cur, i = [], 0, [], 0
    while i < len(t):
        c = t[i]
        if c in "\"'":
            j = skip_string(t, i)
            cur.append(t[i:j])
            i = j
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    parts.append("".join(cur))
    return parts


def call_args(t: str, open_idx: int):
    """t[open_idx] == '('. Return (args:list[str], end_idx_after_close)."""
    depth, i, start = 0, open_idx, open_idx + 1
    args = []
    while i < len(t):
        c = t[i]
        if c in "\"'":
            i = skip_string(t, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(t[start:i])
                return [a.strip() for a in args], i + 1
        elif c == "," and depth == 1:
            args.append(t[start:i])
            start = i + 1
        i += 1
    raise ExtractError("unbalanced call")


def brace_pairs(t: str):
    """Map open-brace index -> close-brace index."""
    pairs, stack, i = {}, [], 0
    while i < len(t):
        c = t[i]
        if c in "\"'":
            i = skip_string(t, i)
            continue
        if c == "{":
            stack.append(i)
        elif c == "}" and stack:
            pairs[stack.pop()] = i
        i += 1
    return pairs


def line_of(t: str, idx: int) -> int:
    return t.count("\n", 0, idx) + 1


# --------------------------------------------------------------------------
# Per-file model
# --------------------------------------------------------------------------
class Src:
    def __init__(self, path: Path):
        self.path = path
        self.raw = path.read_text(encoding="utf-8", errors="replace")
        self.t = strip_comments(self.raw)
        self.pairs = brace_pairs(self.t)
        try:
            self.rel = path.relative_to(ROOT).as_posix()
        except ValueError:
            self.rel = path.as_posix()

    def enclosing_blocks(self, pos: int):
        return sorted(o for o, c in self.pairs.items() if o < pos < c)

    def header_of(self, open_idx: int) -> str:
        j = open_idx
        k = j - 1
        while k >= 0 and self.t[k] not in ";{}":
            k -= 1
        return " ".join(self.t[k + 1:j].split())

    def function_body(self, pos: int):
        """(open, close, header) of the outermost function-like block around pos."""
        best = None
        for o in self.enclosing_blocks(pos):
            h = self.header_of(o)
            if re.match(r"(namespace|extern|class|struct|enum|union)\b", h):
                continue
            if re.match(r"(if|for|while|switch|catch|else|do)\b", h):
                continue
            if re.search(r"(\)|const|noexcept|override)\s*$", h):
                best = (o, self.pairs[o], h)
                break
        return best

    def stmt_end(self, i: int) -> int:
        """Index of the terminating ';' of the statement whose expr starts at i."""
        depth = 0
        while i < len(self.t):
            c = self.t[i]
            if c in "\"'":
                i = skip_string(self.t, i)
                continue
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == ";" and depth <= 0:
                return i
            i += 1
        raise ExtractError("unterminated statement")


# --------------------------------------------------------------------------
# Global constant / helper-function lookup across the repo
# --------------------------------------------------------------------------
class World:
    def __init__(self):
        self.files = {}
        for p in sorted(GAMEOS.rglob("*")):
            if p.suffix in (".cpp", ".h", ".hpp", ".inl") and p.is_file():
                self.files[p] = None
        self.consts = {}
        self._consts_loaded = False
        self.constfiles = list((ROOT / "mclib").glob("*.h")) + list(GAMEOS.rglob("*.h"))

    def src(self, p: Path) -> Src:
        if self.files.get(p) is None:
            self.files[p] = Src(p)
        return self.files[p]

    def const_int(self, name: str):
        if not self._consts_loaded:
            rx = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*(\d+)u?\s*[,;}]")
            for p in self.constfiles:
                try:
                    t = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
                except OSError:
                    continue
                for m in rx.finditer(t):
                    self.consts.setdefault(m.group(1), int(m.group(2)))
            self._consts_loaded = True
        return self.consts.get(name)

    def find_function(self, name: str, ret_hint: str = "std::string"):
        rx = re.compile(r"\b" + re.escape(ret_hint) + r"\s+" + re.escape(name) + r"\s*\(\s*\)\s*\{")
        for p in self.files:
            if p.suffix != ".cpp":
                continue
            s = self.src(p)
            m = rx.search(s.t)
            if m:
                o = m.end() - 1
                return s, o, s.pairs[o]
        return None


WORLD: World | None = None

# Sample values used when a format spec's runtime value is unknowable offline.
_SAMPLE_BY_ARG = [(re.compile(r"CsmCount"), 3)]


def fmt_sample(fmt: str, args: list[str]) -> str:
    out, ai = [], 0
    i = 0
    while i < len(fmt):
        m = re.compile(r"%([-+ 0#]*\d*(?:\.(\d+))?)(l|ll|z)?([diufsxX])").match(fmt, i) if fmt[i] == "%" else None
        if fmt.startswith("%%", i):
            out.append("%")
            i += 2
            continue
        if m:
            conv, prec = m.group(4), m.group(2)
            a = args[ai] if ai < len(args) else ""
            ai += 1
            val = int(a) if re.fullmatch(r"\d+", a.strip()) else 1
            for rx, v in _SAMPLE_BY_ARG:
                if rx.search(a):
                    val = v
            if conv in "di" or conv in "uxX":
                out.append(str(int(val)))
            elif conv == "f":
                out.append(f"{float(val) + 0.5:.{int(prec) if prec else 6}f}")
            else:
                out.append("X")
            i = m.end()
            continue
        out.append(fmt[i])
        i += 1
    return "".join(out)


# --------------------------------------------------------------------------
# String-expression evaluator
# --------------------------------------------------------------------------
# A value is a list of ALTERNATIVES; an alternative is a list of (text, cond)
# fragments where cond is None (unconditional) or a normalised condition string.
Frag = tuple
Value = list


class Ctx:
    def __init__(self, src: Src, body: tuple, params: dict | None = None,
                 hooks: dict | None = None):
        self.src = src
        self.body = body            # (open, close)
        self.params = params or {}  # name -> str/int literal value
        self.hooks = hooks or {}    # ident -> Value (pre-resolved)
        self.depth = 0
        self.ref = None


def _norm(s: str) -> str:
    return " ".join(s.split())


def cond_for(src: Src, pos: int, body_open: int, ref: int | None = None) -> str | None:
    """Condition text governing a statement at `pos` (inside function body)."""
    conds = []
    for o in src.enclosing_blocks(pos):
        if o <= body_open:
            continue
        if ref is not None and o < ref < src.pairs[o]:
            continue   # block also encloses the use site: not a condition on the value
        h = src.header_of(o)
        if re.match(r"(if|else|for|while)\b", h):
            conds.append(_norm(h))
    # braceless  `if (x) stmt;`
    k = pos - 1
    while k >= 0 and src.t[k] not in ";{}":
        k -= 1
    head = _norm(src.t[k + 1:pos])
    if re.match(r"(if|else|for|while)\b", head):
        conds.append(head)
    return " && ".join(conds) if conds else None


def _merge_cond(a: str, b: str | None) -> str:
    parts = a.split(" && ")
    for x in (b.split(" && ") if b else []):
        if x not in parts:
            parts.append(x)
    return " && ".join(parts)


def cross(a: Value, b: Value) -> Value:
    return [x + y for x in a for y in b]


def eval_expr(expr: str, ctx: Ctx, pos: int) -> Value:
    """Evaluate a C++ string expression ending before offset `pos`."""
    ctx.depth += 1
    if ctx.depth == 1:
        ctx.ref = pos
    if ctx.depth > 12:
        raise ExtractError("expression recursion too deep: " + expr[:80])
    try:
        return _eval(expr.strip(), ctx, pos)
    finally:
        ctx.depth -= 1


def _eval(e: str, ctx: Ctx, pos: int) -> Value:
    e = e.strip()
    if not e:
        return [[]]
    # strip outer parens
    while e.startswith("(") and e.endswith(")") and _balanced_outer(e):
        e = e[1:-1].strip()
    # ternary (top-level ?:)
    q = _find_top(e, "?")
    if q >= 0:
        c = _find_top(e[q + 1:], ":")
        if c >= 0:
            a, b = e[q + 1:q + 1 + c], e[q + 2 + c:]
            return _eval(a, ctx, pos) + _eval(b, ctx, pos)
    parts = split_top(e, "+")
    if len(parts) > 1:
        val: Value = [[]]
        for p in parts:
            val = cross(val, _eval(p, ctx, pos))
        return val
    lit = parse_string_literals(e)
    if lit is not None:
        return [[(lit, None)]]
    m = re.fullmatch(r"std::string\s*\((.*)\)", e, re.S)
    if m:
        return _eval(m.group(1), ctx, pos)
    m = re.fullmatch(r"std::to_string\s*\((.*)\)", e, re.S)
    if m:
        return [[(str(_const(m.group(1).strip(), ctx)), None)]]
    m = re.fullmatch(r"(.+?)\.c_str\s*\(\s*\)", e, re.S)
    if m:
        return _eval(m.group(1), ctx, pos)
    m = re.fullmatch(r"\(\s*(?:const\s+)?char\s*\*\s*\)\s*(.*)", e, re.S)
    if m:
        return _eval(m.group(1), ctx, pos)
    if e in ("nullptr", "NULL", "0"):
        return [[]]
    m = re.fullmatch(r"([A-Za-z_]\w*)\s*\(\s*\)", e)
    if m:  # helper returning a #define string; empty when its gate is off
        fn = WORLD.find_function(m.group(1))
        if fn is None:
            raise ExtractError(f"unresolved helper call {e}")
        s, o, c = fn
        txt = s.t[o:c]
        mm = re.search(r"snprintf\s*\(([^;]*)\)\s*;", txt, re.S)
        if mm:
            args = split_top(mm.group(1), ",")
            fmt = parse_string_literals(args[2])
            if fmt is not None:
                return [[(fmt_sample(fmt, [a.strip() for a in args[3:]]), "call:" + m.group(1) + "()")]]
        lits = re.findall(r'"#define[^"]*"', txt)
        if lits:
            return [[(parse_string_literals(lits[0]), "call:" + m.group(1) + "()")]]
        raise ExtractError(f"helper {e} has no #define literal")
    if re.fullmatch(r"[A-Za-z_]\w*", e):
        return _eval_var(e, ctx, pos)
    raise ExtractError(f"cannot evaluate expression: {e[:100]}")


def _balanced_outer(e: str) -> bool:
    depth = 0
    for i, c in enumerate(e):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0 and i < len(e) - 1:
                return False
    return True


def _find_top(e: str, ch: str) -> int:
    depth, i = 0, 0
    while i < len(e):
        c = e[i]
        if c in "\"'":
            i = skip_string(e, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ch and depth == 0:
            if ch == "?" or ch == ":":
                if e[i:i + 2] == "::" or (i > 0 and e[i - 1] == ":"):
                    i += 2
                    continue
            return i
        i += 1
    return -1


def _const(name: str, ctx: Ctx):
    name = re.sub(r"\(\s*(?:int|uint32_t|unsigned)\s*\)", "", name).strip()
    if re.fullmatch(r"\d+", name):
        return int(name)
    if name in ctx.params:
        return ctx.params[name]
    short = name.split("::")[-1]
    # file-local constexpr first
    m = re.search(r"\b" + re.escape(short) + r"\s*=\s*(\d+)\b", ctx.src.t)
    if m:
        return int(m.group(1))
    v = WORLD.const_int(short)
    if v is not None:
        return v
    raise ExtractError(f"cannot resolve constant {name} in {ctx.src.rel}")


def _eval_var(name: str, ctx: Ctx, pos: int) -> Value:
    if name in ctx.hooks:
        return ctx.hooks[name]
    if name in ctx.params:
        return [[(str(ctx.params[name]), None)]]
    src = ctx.src
    bo, bc = ctx.body[0], ctx.body[1]
    # statements assigning NAME before pos, inside the function body
    stmts = []
    n = re.escape(name)
    decl = re.compile(r"(?:static\s+)?(?:const\s+)?(?:std::string|char\s*\*|auto|char)\s*\*?\s*" + n +
                      r"\s*(?:\[[^\]]*\])?\s*=\s*", re.S)
    asg = re.compile(r"(?<![\w.>])" + n + r"\s*(\+=|=)(?!=)\s*", re.S)
    app = re.compile(r"(?<![\w.>])" + n + r"\s*\.append\s*\(", re.S)
    snp = re.compile(r"\bsnprintf\s*\(\s*" + n + r"\s*,")
    seen = set()
    body = src.t
    for m in decl.finditer(body, bo, pos):
        e = src.stmt_end(m.end())
        stmts.append((m.start(), "=", body[m.end():e]))
        seen.add(m.start())
    for m in asg.finditer(body, bo, pos):
        if any(abs(m.start() - s) < 2 for s in seen):
            continue
        # skip matches that are really the tail of a decl
        pre = body[max(0, m.start() - 40):m.start()]
        if re.search(r"(std::string|char\s*\*|auto|char)\s*\*?\s*$", pre):
            continue
        e = src.stmt_end(m.end())
        stmts.append((m.start(), m.group(1), body[m.end():e]))
    for m in app.finditer(body, bo, pos):
        _, end = call_args(body, m.end() - 1)
        a, _ = call_args(body, m.end() - 1)
        stmts.append((m.start(), "+=", a[0]))
    for m in snp.finditer(body, bo, pos):
        a, _ = call_args(body, body.index("(", m.start()))
        fmt = parse_string_literals(a[2]) if len(a) > 2 else None
        if fmt is None:
            raise ExtractError(f"snprintf fmt not literal for {name}")
        stmts.append((m.start(), "=snprintf", fmt_sample(fmt, a[3:])))
    stmts.sort()
    if not stmts:
        raise ExtractError(f"no definition of '{name}' found in {src.rel} before offset")
    val: Value = [[]]
    for off, op, rhs in stmts:
        cond = cond_for(src, off, bo, ctx.ref)
        if op == "=snprintf":
            v: Value = [[(rhs, None)]]
        else:
            v = _eval(rhs, ctx, off)
        if cond:
            v = [[(t, _merge_cond(cond, c)) for (t, c) in alt] for alt in v]
        if op == "=" and not cond:
            val = v
        else:
            val = cross(val, v)
    return val


# --------------------------------------------------------------------------
# Variant expansion
# --------------------------------------------------------------------------
def expand_alt(frags: list, singles: bool = True):
    """Expand one alternative into prefix-text variants.

    Returns list of (label, text, mandatory_flag) with label describing which
    optional groups are on.
    """
    groups = []
    for _, c in frags:
        if c is not None and c not in groups:
            groups.append(c)
    combos = [()]
    if groups:
        combos.append(tuple(groups))
        if singles:
            for g in groups:
                combos.append((g,))
    seen, out = set(), []
    for on in combos:
        text = "".join(t for t, c in frags if c is None or c in on)
        if text in seen:
            continue
        seen.add(text)
        out.append((list(on), text))
    return out


def _def_label(text: str) -> list[str]:
    """`#define NAME VALUE` lines -> ['NAME', 'NAME=VALUE'] style labels."""
    labels = []
    for m in re.finditer(r"^#define\s+(\w+)[ ]*(.*)$", text, re.M):
        v = m.group(2).strip()
        if v.startswith("="):
            v = v[1:].strip()
        labels.append(m.group(1) if v in ("", "1") else f"{m.group(1)}={v}")
    return labels


def defines_of(text: str):
    return re.findall(r"^#define\s+(\w+)", text, re.M)


def version_of(text: str):
    m = re.search(r"^#version\s+(\d+)", text, re.M)
    return int(m.group(1)) if m else None


# --------------------------------------------------------------------------
# Collectors
# --------------------------------------------------------------------------
def add_variants(out: list, *, pid: str, src: str, line: int, stages: dict,
                 value: Value, assemble: str, extra: dict | None = None,
                 singles: bool = True):
    seen = set()
    for alt in value:
        for on, text in expand_alt(alt, singles):
            key = (pid, text)
            if key in seen:
                continue
            seen.add(key)
            ver = version_of(text)
            if ver is None:
                raise ExtractError(f"{src}:{line}: program '{pid}' prefix has no #version")
            d = {
                "id": pid,
                "source": f"{src}:{line}",
                "stages": stages,
                "assemble": assemble,
                "optional_groups_on": on,
                "defines": _def_label(text),
                "prefix": text,
            }
            if extra:
                d.update(extra)
            out.append(d)


def collect_make_program(world: World, out: list):
    call = re.compile(r"\bmakeProgram(2?)\s*\(")
    for p in sorted(world.files):
        if p.suffix != ".cpp":
            continue
        if p.name == "shader_builder.cpp":
            continue   # the implementation itself (makeProgram -> makeProgram2 forwarding)
        s = world.src(p)
        for m in call.finditer(s.t):
            # declaration/definition in shader_builder.*
            a, end = call_args(s.t, m.end() - 1)
            if a and re.match(r"(const\s+)?char\b", a[0]):
                continue
            two = m.group(1) == "2"
            if two:
                if len(a) != 9:
                    raise ExtractError(f"{s.rel}:{line_of(s.t, m.start())}: makeProgram2 arity {len(a)}")
                name, vp, hp, dp, gp, fp, _cnt, _xfb, pre = a
                stg = {"vert": vp, "tesc": hp, "tese": dp, "geom": gp, "frag": fp}
            else:
                if len(a) != 4:
                    raise ExtractError(f"{s.rel}:{line_of(s.t, m.start())}: makeProgram arity {len(a)}")
                name, vp, fp, pre = a
                stg = {"vert": vp, "frag": fp}
            ln = line_of(s.t, m.start())
            # material-loader generic call (printf-built paths): handled separately
            if any(parse_string_literals(v) is None and v not in ("nullptr", "0") for v in stg.values()):
                if "mvar" in pre or "getDefinesString" in pre:
                    continue
                raise ExtractError(f"{s.rel}:{ln}: non-literal shader path in makeProgram: {stg}")
            stages = {}
            for k, v in stg.items():
                if v in ("nullptr", "0"):
                    continue
                stages[k] = parse_string_literals(v)
            body = s.function_body(m.start())
            if body is None:
                raise ExtractError(f"{s.rel}:{ln}: call outside a function")
            ctx = Ctx(s, (body[0], body[1]))
            value = eval_expr(pre, ctx, m.start())
            nm = parse_string_literals(name)
            if nm is None:
                raise ExtractError(f"{s.rel}:{ln}: non-literal program name {name}")
            add_variants(out, pid=nm, src=s.rel, line=ln, stages=stages,
                         value=value, assemble="engine")


def collect_materials(world: World, out: list):
    p = GAMEOS / "gameos_graphics.cpp"
    s = world.src(p)
    # flag names
    m = re.search(r"g_shader_flags\s*\[\s*\]\s*=\s*\{(.*?)\};", s.t, re.S)
    flags = [parse_string_literals(x.strip()) for x in split_top(m.group(1), ",") if x.strip()]
    # shader_list
    m = re.search(r"shader_list\s*\[\s*\]\s*=\s*\{(.*?)\};", s.t, re.S)
    shader_list = [parse_string_literals(x.strip()) for x in split_top(m.group(1), ",") if x.strip()]
    # combinations
    m = re.search(r"combinations\s*\[\s*\]\s*=\s*\{(.*?)\};", s.t, re.S)
    combos = []
    for item in split_top(m.group(1), ","):
        item = item.strip()
        if not item:
            continue
        names = re.findall(r"gosGLOBAL_SHADER_FLAGS::(\w+)", item)
        fl = []
        for nm in names:
            idx = _flag_index(s, nm)
            fl.append(flags[idx])
        combos.append(fl)
    # extra material loads with an empty flag set
    extra = sorted(set(re.findall(r'gosRenderMaterial::load\(\s*"(\w+)"', s.t)))
    # tess shaders
    m = re.search(r'if\s*\(\s*strcmp\(shader,\s*"(\w+)"\)\s*==\s*0\s*\|\|\s*strcmp\(shader,\s*"(\w+)"\)\s*==\s*0', s.t)
    tess = {m.group(1), m.group(2)}
    # the variation composer
    fn = re.search(r"void\s+getMaterialVariation\s*\([^)]*\)\s*\{", s.t)
    fo = fn.end() - 1
    fc = s.pairs[fo]
    ctx = Ctx(s, (fo, fc), hooks={"d": [[("@FLAG@", None)]]})
    # base prefix = defines_str value with the per-flag loop removed
    loop = re.search(r"for\s*\(\s*auto\s+d\s*:\s*defines\s*\)\s*\{", s.t[fo:fc])
    lo = fo + loop.end() - 1
    lc = s.pairs[lo]
    loop_body = s.t[lo + 1:lc]
    app = re.search(r'defines_str\.append\("#define "\);\s*defines_str\.append\(d\);\s*defines_str\.append\(("[^"]*")\)', loop_body)
    if not app:
        raise ExtractError("getMaterialVariation loop shape changed; update extractor")
    sep = parse_string_literals(app.group(1))   # e.g. " = 1\n"
    hooks_val = eval_expr("defines_str", ctx, fc)
    # remove the loop-body fragments (they are conditional on 'for ...')
    base_value: Value = []
    for alt in hooks_val:
        base_value.append([(t, c) for (t, c) in alt if not (c and re.match(r"for\b", c))])
    version = None
    ver_txt = "".join(t for t, c in base_value[0] if c is None)
    if version_of(ver_txt) is None:
        raise ExtractError("material prefix has no #version")
    def with_flags(fl):
        flag_txt = "".join(f"#define {f}{sep}" for f in fl)
        v: Value = []
        for alt in base_value:
            first = alt[0]  # "#version 430\n" literal
            v.append([first, (flag_txt, None)] + alt[1:])
        return v
    for sh in shader_list:
        for fl in combos:
            _emit_material(out, s, sh, fl, with_flags(fl), tess)
    for sh in extra:
        _emit_material(out, s, sh, [], with_flags([]), tess)


def _flag_index(s: Src, flag: str) -> int:
    m = re.search(r"enum class gosGLOBAL_SHADER_FLAGS[^{]*\{(.*?)\}", s.t, re.S)
    items = [x.strip() for x in m.group(1).split(",") if x.strip()]
    for it in items:
        nm, _, val = it.partition("=")
        if nm.strip() == flag:
            return int(val.strip())
    raise ExtractError(f"flag {flag} not in enum")


def _emit_material(out, s, sh, fl, value, tess):
    path = f"shaders/{sh}"
    stages = {"vert": f"{path}.vert", "frag": f"{path}.frag"}
    if sh in tess:
        stages["tesc"] = f"{path}.tesc"
        stages["tese"] = f"{path}.tese"
    pid = sh
    ln = line_of(s.t, s.t.index("getMaterialVariation"))
    add_variants(out, pid=pid, src=s.rel, line=ln, stages=stages, value=value,
                 assemble="engine", singles=not fl)


def _param_calls(s: Src, fname: str):
    """Literal argument tuples at call sites of `fname(...)` in this file."""
    res = []
    for m in re.finditer(r"(?<![\w:])" + re.escape(fname) + r"\s*\(", s.t):
        # skip the definition
        head = s.t[max(0, m.start() - 12):m.start()]
        a, end = call_args(s.t, m.end() - 1)
        if re.match(r"\s*\{", s.t[end:end + 20]) or re.search(r"\b(GLuint|void|int)\s*$", head):
            continue
        res.append(a)
    return res


def collect_strs_compute(world: World, out: list):
    """Direct glShaderSource loaders: evaluate the strs[] array."""
    for p in sorted(world.files):
        if p.suffix != ".cpp":
            continue
        s = world.src(p)
        for m in re.finditer(r'(?:loadTextFile|load_text_file)\s*\(\s*"(shaders/[\w./]+\.comp)"\s*\)', s.t):
            body = s.function_body(m.start())
            if body is None:
                continue
            bo, bc, hdr = body
            fbody = s.t[bo:bc]
            arr = re.search(r"const\s+char\s*\*\s*(\w+)\s*\[\s*\d*\s*\]\s*=\s*\{([^}]*)\}\s*;", fbody)
            if not arr:
                continue   # not the strs[] shape (gpu_cull handled separately)
            fvar = re.search(r"char\s*\*\s*(\w+)\s*=\s*(?:loadTextFile|load_text_file)\s*\(", fbody).group(1)
            fm = re.search(r"(\w+)\s*\(([^)]*)\)\s*$", hdr)
            fname = fm.group(1)
            params = [x.strip().split()[-1].lstrip("*") for x in fm.group(2).split(",") if x.strip()]
            envs = [{}]
            if params:
                envs = []
                for args in _param_calls(s, fname):
                    env = {}
                    for pn, av in zip(params, args):
                        lit = parse_string_literals(av)
                        env[pn] = lit if lit is not None else int(av)
                    envs.append(env)
                if not envs:
                    raise ExtractError(f"{s.rel}: no literal call sites for {fname}")
            elems = [x.strip() for x in split_top(arr.group(2), ",") if x.strip()]
            for env in envs:
                ctx = Ctx(s, (bo, bc), params=env)
                value: Value = [[]]
                for el in elems:
                    if el == fvar:
                        continue
                    value = cross(value, eval_expr(el, ctx, bc))
                comp = m.group(1)
                suffix = ""
                if env:
                    suffix = "#" + "#".join(str(v) for v in env.values())
                pid = Path(comp).stem + suffix
                add_variants(out, pid=pid, src=s.rel, line=line_of(s.t, m.start()),
                             stages={"comp": comp}, value=value, assemble="raw",
                             extra={"raw_note": "loaded as plain text (no engine include flattening)"})


def collect_gpu_driven(world: World, out: list):
    s = world.src(GAMEOS / "gpu_driven_common.cpp")
    if 'kVersionPrefix = "#version 430\\n"' not in s.raw or "load_shader(fname" not in s.raw:
        raise ExtractError("gpu_driven::BuildComputeProgramFromFile shape changed; update extractor")
    for p in sorted(world.files):
        if p.suffix != ".cpp":
            continue
        t = world.src(p)
        for m in re.finditer(r"gpu_driven::BuildComputeProgramFromFile\s*\(", t.t):
            a, _ = call_args(t.t, m.end() - 1)
            f = parse_string_literals(a[0])
            if f is None:
                continue   # the definition (const char* fname, ...)
            if a[1] != "nullptr" or a[2] != "0":
                raise ExtractError(f"{t.rel}: gpu_driven preambles not nullptr; extend extractor")
            add_variants(out, pid=Path(f).stem, src=t.rel, line=line_of(t.t, m.start()),
                         stages={"comp": f}, value=[[("#version 430\n", None)]], assemble="engine")


def collect_gpu_cull(world: World, out: list):
    s = world.src(GAMEOS / "gpu_cull_compute.cpp")
    if 'kPrefix = "#version 430\\n"' not in s.raw or "kIncludeLine" not in s.raw:
        raise ExtractError("gpu_cull build_cull_program shape changed; update extractor")
    ln = line_of(s.t, s.t.index("build_cull_program"))
    binding = WORLD.const_int("READBACK_SSBO_BINDING")
    inc = re.search(r'kIncludeLine\s*=\s*"([^"]*)"', s.raw).group(1)
    base = f"#version 430\n#define READBACK_SSBO_BINDING {binding}\n"
    # preamble defines (each guarded by readback_isEnabled except the C1b marker)
    c1b = re.search(r'preambles\[nPreambles\+\+\]\s*=\s*"(#define \w+ 1\\n)"\s*;\s*if\s*\(\s*readback', s.raw)
    c1b_def = parse_string_literals('"' + c1b.group(1) + '"')
    rb = re.search(r'c1aPreamble\s*=\s*"(#define \w+ 1\\n)"', s.raw)
    rb_def = parse_string_literals('"' + rb.group(1) + '"')
    variants = {
        "gpu_cull#C1a": base,
        "gpu_cull#C1a#READBACK": base + rb_def,
        "gpu_cull#C1b": base + c1b_def,
        "gpu_cull#C1b#READBACK": base + c1b_def + rb_def,
    }
    for pid, pre in variants.items():
        out.append({
            "id": pid, "source": f"{s.rel}:{ln}",
            "stages": {"comp": "shaders/gpu_cull.comp"},
            "assemble": "raw", "optional_groups_on": [], "defines": _def_label(pre),
            "prefix": pre,
            "splice": {"include_line": inc, "file": "shaders/gpu_cull_predicate.glsl"},
        })
    for m in re.finditer(r'build_compute_program_from_file\s*\(\s*"(shaders/[\w./]+)"', s.t):
        f = m.group(1)
        out.append({
            "id": Path(f).stem, "source": f"{s.rel}:{line_of(s.t, m.start())}",
            "stages": {"comp": f}, "assemble": "raw", "optional_groups_on": [],
            "defines": [], "prefix": "#version 430\n",
        })


def collect_terrain_lighting(world: World, out: list):
    s = world.src(GAMEOS / "gos_terrain_lighting.cpp")
    if 'kBeginMarker' not in s.raw:
        raise ExtractError("terrain_lighting splice shape changed; update extractor")
    b = re.search(r'kBeginMarker\s*=\s*"([^"]*)"', s.raw).group(1)
    e = re.search(r'kEndMarker\s*=\s*"([^"]*)"', s.raw).group(1)
    m = re.search(r'tl_build_terrain_lighting_program\(\s*"([^"]+)",\s*"([^"]+)"', s.t)
    out.append({
        "id": "terrain_lighting", "source": f"{s.rel}:{line_of(s.t, m.start())}",
        "stages": {"comp": m.group(1)}, "assemble": "raw", "optional_groups_on": [],
        "defines": [], "prefix": "#version 430\n",
        "splice": {"begin_marker": b, "end_marker": e, "file": m.group(2)},
    })


# --------------------------------------------------------------------------
def extract() -> dict:
    global WORLD
    WORLD = World()
    variants: list = []
    collect_make_program(WORLD, variants)
    collect_materials(WORLD, variants)
    collect_strs_compute(WORLD, variants)
    collect_gpu_driven(WORLD, variants)
    collect_gpu_cull(WORLD, variants)
    collect_terrain_lighting(WORLD, variants)
    # Each variant gets a unique, human-readable key: program id + its define set.
    keyed, seen = [], {}
    for v in variants:
        d = [x for x in v["defines"]]
        base = v["id"] + ("{" + ",".join(x.replace("MC2_", "") for x in d) + "}" if d else "")
        n = seen.get(base, 0)
        seen[base] = n + 1
        v["key"] = base if n == 0 else f"{base}~{n}"
        keyed.append(v)
    keyed.sort(key=lambda v: (v["key"]))
    # de-dupe exact key collisions deterministically
    keys = [v["key"] for v in keyed]
    assert len(keys) == len(set(keys)), "variant key collision"
    return {
        "_comment": "GENERATED by scripts/extract_gl_shader_variants.py from GameOS/gameos/**. "
                    "Do not hand-edit; re-run the extractor (check-gl-shader-variants.py "
                    "fails when this file is stale).",
        "schema": 1,
        "variants": keyed,
    }


def dumps(d: dict) -> str:
    return json.dumps(d, indent=1, sort_keys=True) + "\n"


def main(argv) -> int:
    try:
        data = extract()
    except ExtractError as e:
        print(f"extract_gl_shader_variants: EXTRACTION GAP: {e}", file=sys.stderr)
        return 1
    text = dumps(data)
    if "--stdout" in argv:
        sys.stdout.write(text)
        return 0
    if "--check" in argv:
        cur = MANIFEST.read_text(encoding="utf-8") if MANIFEST.exists() else ""
        if cur != text:
            print("gl_shader_variants.json is STALE vs the C++ "
                  "(re-run: py -3 scripts/extract_gl_shader_variants.py)", file=sys.stderr)
            import difflib
            for i, ln in enumerate(difflib.unified_diff(
                    cur.splitlines(), text.splitlines(), "manifest", "re-extracted", lineterm="", n=1)):
                if i > 40:
                    print("  ...", file=sys.stderr)
                    break
                print("  " + ln, file=sys.stderr)
            return 1
        print(f"gl_shader_variants.json up to date ({len(data['variants'])} variants)")
        return 0
    MANIFEST.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {MANIFEST.relative_to(ROOT)}: {len(data['variants'])} variants")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
