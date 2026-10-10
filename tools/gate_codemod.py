#!/usr/bin/env python3
"""gate_codemod -- DRY-RUN planner for the G1b pilot of the typed gate registry.

WHAT IT ANSWERS
---------------
For every raw `getenv("MC2_*")` read in a list of C++ files: "if this site were rewritten to
`mc2::gate::get<"MC2_*">()` against the gate row that is committed today, would anything a user can
observe change, and what exact text keeps it from changing?"  Design: .claude/cleanup/
typed-gate-registry-design.md section 5.7 (migration mechanics) and the review amendments 1, 2 and 9
(per-site rule override, numeric range=Reject, caching shape preserved).

It writes NO C++.  `--apply` exists only so the flag is reserved; it refuses unless `--i-mean-it`
is also given, and even then it does nothing in this slice.

HOW A SITE IS JUDGED
--------------------
 * Bool rows.  The site is run by the reader interpreter in scripts/gate_idioms.py against eight probe
   inputs (U unset, E "", Z "0", F "false", O "1", X "yes", X10 "10", X25 "2.5").  The result is matched
   against the truthiness table of every Rule in Gates/gate.h.  Same vector as the row's rule: NO-CHANGE.
   Differs only on E/F/X/X10/X25: EDGE.  Differs on U, Z or O: SEMANTICS, and the proposal is the
   per-site override `get<"N", Rule::R>()` that reproduces the old behaviour byte for byte.
 * Int and Float rows.  The old reader (whole lambda or initializer) is executed on a probe set that
   includes junk ("abc"), trailing junk ("12abc"), "2.0", negatives, and the values just inside and just
   outside every bound literal found in the code.  The result is compared with what the row's parser
   (Prefix, i.e. atoi/atof compatible, plus range Reject or Clamp) returns.  The tool searches for the
   range and default that make the two agree and lists them as ROW EDITS; whatever still differs is EDGE
   (SEMANTICS if the default itself differs).
 * The CACHING SHAPE is preserved: a `static const T s = <reader>` stays a static holding `get<>()`.

VERDICTS: NO-CHANGE | EDGE | SEMANTICS | UNHANDLED (the tool could not prove the site, so a human reads it).

USAGE
  py -3 tools/gate_codemod.py                       # TSV + summary of the G1b pilot files, from HEAD
  py -3 tools/gate_codemod.py --files A.cpp B.h     # other files
  py -3 tools/gate_codemod.py --write               # regenerate docs/gate-codemod-g1b.{md,tsv}
  py -3 tools/gate_codemod.py --worktree            # read the working tree instead of git HEAD
"""

import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
from collections import Counter

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import gate_idioms as gi  # noqa: E402
from console_child import child_kwargs  # noqa: E402

PILOT_FILES = (
    "GameOS/gameos/gos_terrain_patch_stream.cpp",
    "mclib/terrain_admission_mode.h",
    "mclib/terrain_surface_trace.h",
    "mclib/terrain_runtime.cpp",
    "GameOS/gameos/gos_terrain_surface.cpp",
    "GameOS/gameos/gos_terrain_lighting.cpp",
)
SEED_REL = "tools/gate_table.seed.json"
TABLE_REL = "Gates/gate_table.json"
LEDGER_REL = "tools/gate_g0_ledger.json"
OUT_MD = "docs/gate-codemod-g1b.md"
OUT_TSV = "docs/gate-codemod-g1b.tsv"

NS = "mc2::gate"

VERDICTS = ("NO-CHANGE", "EDGE", "SEMANTICS", "UNHANDLED")
SHAPES = ("namespace-static", "local-static", "lambda-static", "per-call")
PROBE_LABELS = tuple(gi.PROBE_LABELS)
PROBE_VALUES = tuple(v for _, v in gi.PROBES)
#: Probe labels whose result may differ in an EDGE (never U, Z or O: those are SEMANTICS).
EDGE_LABELS = ("E", "F", "X", "X10", "X25")

# ---------------------------------------------------------------------------
# The Bool rules of Gates/gate.h, ported so the tool can ask "what does rule R say about value V"
# for harness values and for the unit tests (cross-checked against gi.rule_vector in the tests).
# ---------------------------------------------------------------------------

BOOL_RULES = ("Presence", "NonEmpty", "Exact1", "Lead1", "Lead0", "Lead0Kill", "Lead0P", "DefOnLead0",
              "AtoiNZ", "DefOnAtoiNZ", "Is01", "Std", "Lead0Def")
_FALSE_TOK = ("0", "false", "off", "no")
_TRUE_TOK = ("1", "true", "on", "yes")


def apply_rule(rule, v, default):
    """The row-rule truthiness of environment text `v` (None = unset); mirrors applyRule() in gate.h."""
    st = v is not None
    ne = bool(v)
    f0 = ne and v[0] == "0"
    f1 = ne and v[0] == "1"
    x0 = v == "0"
    x1 = v == "1"
    ftok = ne and v.lower() in _FALSE_TOK
    nz = st and gi._cstr_prefix_int(v) != 0
    if rule == "Std":
        return (not ftok) if ne else default
    if rule == "Presence":
        return st
    if rule == "NonEmpty":
        return ne
    if rule == "Exact1":
        return x1
    if rule == "Lead1":
        return f1
    if rule == "Lead0":
        return ne and not f0
    if rule == "Lead0Kill":
        return not (st and f0)
    if rule == "Lead0P":
        return st and not f0
    if rule == "DefOnLead0":
        return (not st) or (ne and not f0)
    if rule == "AtoiNZ":
        return st and nz
    if rule == "DefOnAtoiNZ":
        return (not st) or nz
    if rule == "Lead0Def":
        return (not f0) if ne else default
    if rule == "Is01":
        return x0 or x1
    raise ValueError(rule)


def rule_vec(rule, default):
    """[bool] x 8 for the gate_idioms probe order."""
    return [apply_rule(rule, v, default) for v in PROBE_VALUES]


def vec_str(bits):
    return "".join("1" if b else "0" for b in bits)


def _probe_class(v):
    """Which of the eight probe classes a harness value falls in (for 'does a harness hit this diff')."""
    if v is None:
        return {"U"}
    if v == "":
        return {"E"}
    if v == "0":
        return {"Z"}
    if v.lower() in ("false", "off", "no"):
        return {"F"}
    if v == "1":
        return {"O"}
    out = {"X"}
    if v[0] == "1":
        out.add("X10")
    if re.match(r"^\s*[+-]?\d*\.\d+", v):
        out.add("X25")
    return out


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

class Row:
    """One gate row as the migration sees it (committed table first, seed for the extras)."""

    def __init__(self, name, kind, rule, default, rng=None, harness=(), readers=(), owner="", conflict=False):
        self.name = name
        self.kind = kind
        self.rule = rule
        self.default = default
        self.range = rng            # None | {"lo","hi","clamp"}
        self.harness = list(harness)
        self.readers = list(readers)
        self.owner = owner
        self.conflict = conflict


def load_rows(root=ROOT, rev=None):
    """{name: Row}.  Rule, kind, default and range come from the committed compiled table
    (Gates/gate_table.json) because that is what get<>() will do; the seed supplies the rows the table
    lacks plus harnessValues and the reader list."""
    def _json(rel):
        if rev:
            try:
                raw = subprocess.run(["git", "-C", root, "show", "%s:%s" % (rev, rel)], check=True,
                                     **child_kwargs(redirected=True, capture_output=True)).stdout.decode("utf-8", "replace")
                return json.loads(raw)
            except (subprocess.CalledProcessError, ValueError, OSError):
                return {}
        try:
            with open(os.path.join(root, rel), "r", encoding="utf-8") as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return {}

    seed = {g["name"]: g for g in _json(SEED_REL).get("gates", [])}
    table = {g["name"]: g for g in _json(TABLE_REL).get("gates", [])}
    rows = {}
    for name in sorted(set(seed) | set(table)):
        s = seed.get(name, {})
        t = table.get(name)
        src = t if t is not None else s
        default = src.get("default")
        rows[name] = Row(name, src.get("kind", "Bool"), src.get("rule", "Std"), default,
                         rng=(t or {}).get("range"), harness=s.get("harnessValues", []),
                         readers=s.get("readers", []), owner=src.get("owner", ""),
                         conflict=bool(s.get("conflict")))
    return rows


def load_ledger(root=ROOT):
    try:
        with open(os.path.join(root, LEDGER_REL), "r", encoding="utf-8") as fh:
            return json.load(fh).get("gates", {})
    except (OSError, ValueError):
        return {}


def git_head(root=ROOT):
    try:
        return subprocess.run(["git", "-C", root, "rev-parse", "--short=9", "HEAD"], check=True,
                              **child_kwargs(redirected=True, capture_output=True)).stdout.decode().strip()
    except (subprocess.CalledProcessError, OSError):
        return "unknown"


def read_source(root, rel, rev):
    """File text with CRLF folded to LF; `rev` None reads the working tree."""
    if rev:
        raw = subprocess.run(["git", "-C", root, "show", "%s:%s" % (rev, rel)], check=True,
                             **child_kwargs(redirected=True, capture_output=True)).stdout
    else:
        with open(os.path.join(root, rel), "rb") as fh:
            raw = fh.read()
    return raw.decode("utf-8", "replace").replace("\r\n", "\n")


# ---------------------------------------------------------------------------
# Syntax helpers (all offsets are into comment-stripped text, which keeps byte offsets of the raw text)
# ---------------------------------------------------------------------------

_QUALS = {"static", "constexpr", "const", "inline", "thread_local", "volatile"}
_LAMBDA_HEAD_RE = re.compile(r"\[([^\[\]]*)\]\s*(?:\(([^()]*)\))?\s*(?:mutable\s+)?(?:noexcept\s+)?"
                             r"(?:->\s*[\w:<>\s\*&,]+?)?\s*$")
_CTRL_RE = re.compile(r"\s*(?:else\s+)?(?:if|while|for|switch|return|case|do)\b")
_PARSERS = ("atoi", "atol", "atoll", "_atoi64", "atof", "strtol", "strtoll", "strtoul", "strtoull", "strtod",
            "strtof", "stoi", "stol", "stoll", "stof", "stod")
_PARSER_RE = re.compile(r"\b(?:std::)?(%s)\b" % "|".join(_PARSERS))
_SAFE_WORDS = set("""
const char bool int long short float double unsigned signed auto void static constexpr inline return if else
nullptr NULL true false std static_cast reinterpret_cast const_cast uint8_t uint16_t uint32_t uint64_t int8_t
int16_t int32_t int64_t size_t getenv _getenv atoi atol atoll _atoi64 atof strtol strtoll strtoul strtoull
strtod strtof stoi stol stoll stof stod strcmp _stricmp stricmp strcasecmp strncmp _strnicmp strlen max min
fmax fmin fmaxf fminf clamp abs fabs fabsf labs tolower toupper isdigit isalpha string_view
""".split())


def _match_paren(T, open_at):
    """Index just past the `)` that closes the `(` at open_at, or -1."""
    depth, j, n = 0, open_at, len(T)
    while j < n:
        c = T[j]
        if c in "\"'":
            q = c
            j += 1
            while j < n and T[j] != q:
                j += 2 if T[j] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    return -1


def find_call(T, p):
    """-> (callee, call_start, call_end) of the call whose first argument literal starts at p, or None."""
    ws = max(0, p - 96)
    cm = gi._CALLEE_RE.search(T[ws:p])
    if not cm:
        return None
    cs = ws + cm.start(1)
    if T[max(0, cs - 2):cs] == "::":
        cs -= 2
    open_at = T.index("(", ws + cm.end(1))
    ce = _match_paren(T, open_at)
    if ce < 0:
        return None
    return cm.group(1), cs, ce


def find_lambda(T, p):
    """The immediately-invoked lambda whose body holds p.
    -> None | ("iife", lam_start, body_open, body_close, call_end) | ("other", reason)."""
    blk = gi.enclosing_block(T, p)
    while blk:
        before = T[max(0, blk[0] - 240):blk[0]]
        lm = _LAMBDA_HEAD_RE.search(before)
        if lm:
            lam_start = max(0, blk[0] - 240) + lm.start()
            if lm.group(1).strip():
                return ("other", "lambda with captures [%s]" % lm.group(1).strip())
            am = re.match(r"\s*\(\s*\)", T[blk[1]:blk[1] + 40])
            if not am:
                return ("other", "lambda is not invoked in place")
            return ("iife", lam_start, blk[0], blk[1], blk[1] + am.end())
        if gi._block_is_scope(T, blk[0]):
            return None
        blk = gi.enclosing_block(T, blk[0])
    return None


def _split_assign(st, before):
    """Offset of the first depth-0 plain `=` in st that lies before `before`, else -1."""
    depth = 0
    for i, c in enumerate(st[:before]):
        if c in "([{<" and c != "<":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "=" and depth == 0:
            prev = st[i - 1] if i else ""
            nxt = st[i + 1] if i + 1 < len(st) else ""
            if nxt == "=" or prev in "=!<>+-*/%&|^":
                continue
            return i
    return -1


def parse_lhs(lhs):
    """'static const bool s_x' -> dict(quals, type, name, is_decl); '  s_x' -> assignment."""
    toks = lhs.replace("*", " * ").replace("&", " & ").split()
    toks = [t for t in toks if t]
    if not toks:
        return None
    if len(toks) == 1 or (len(toks) >= 1 and not re.match(r"^[A-Za-z_]", toks[-1])):
        return {"quals": [], "type": "", "name": lhs.strip(), "is_decl": False}
    # an assignment to a member/element looks like one token; a declaration has a type word first
    name = toks[-1]
    head = toks[:-1]
    quals = [t for t in head if t in _QUALS]
    typ = " ".join(t for t in head if t not in _QUALS).replace(" *", "*").replace(" &", "&")
    if not typ:
        return {"quals": quals, "type": "", "name": name, "is_decl": False}
    return {"quals": quals, "type": typ, "name": name, "is_decl": True}


def at_namespace_scope(T, pos):
    blk = gi.enclosing_block(T, pos)
    return blk is None or gi._block_is_scope(T, blk[0])


def shape_of(is_lambda, is_decl, is_static, ns_scope):
    if is_lambda:
        return "lambda-static" if (is_static or (is_decl and ns_scope)) else "per-call"
    if is_decl and is_static:
        return "namespace-static" if ns_scope else "local-static"
    if is_decl and ns_scope:
        return "namespace-static"
    return "per-call"


def body_impure(body, extra_ok=()):
    """First identifier in `body` that is not a keyword, a libc parse helper or a name the body declares."""
    s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', body)
    s = re.sub(r"'(?:[^'\\]|\\.)*'", "' '", s)
    declared = set(re.findall(r"\b(?:char|bool|int|long|float|double|unsigned|auto|u?int(?:8|16|32|64)_t|size_t)"
                              r"\b[\s\*&]*(?:const\s+)?([A-Za-z_]\w*)\s*[=;(,)]", s))
    declared |= set(re.findall(r"\bconst\s+(?:char|bool|int|long|float|double|auto)\b[\s\*&]*([A-Za-z_]\w*)", s))
    ok = _SAFE_WORDS | declared | set(extra_ok)
    for w in re.findall(r"(?<![\w.])([A-Za-z_]\w*)", s):
        # a literal suffix or hex digits are not identifiers; field access `.x` is filtered by the lookbehind
        if w in ok or re.fullmatch(r"[fFuUlL]+", w):
            continue
        return w
    return None


def _norm_ws(s, limit=320):
    s = re.sub(r"\s+", " ", s).strip()
    return s if len(s) <= limit else s[:limit - 3] + "..."


# ---------------------------------------------------------------------------
# Result record
# ---------------------------------------------------------------------------

class Result:
    __slots__ = ("file", "line", "gate", "callee", "idiom", "shape", "replacement", "verdict", "why", "old",
                 "row_edit", "diff", "harness_hits", "override", "reason", "mode", "kind")

    def __init__(self, file, line, gate, callee):
        self.file, self.line, self.gate, self.callee = file, line, gate, callee
        self.idiom = ""
        self.shape = "per-call"
        self.replacement = "-"
        self.verdict = "UNHANDLED"
        self.why = ""
        self.old = ""
        self.row_edit = ""
        self.diff = []
        self.harness_hits = []
        self.override = ""
        self.reason = ""
        self.mode = ""
        self.kind = ""

    @property
    def where(self):
        return "%s:%d" % (self.file, self.line)

    def tsv(self):
        cells = [self.where, self.gate, self.idiom, self.shape, self.replacement, self.verdict, self.why]
        return "\t".join(c.replace("\t", " ").replace("\n", " ") for c in cells)


TSV_HEADER = "\t".join(["file:line", "gate", "old idiom class", "old caching shape",
                        "proposed replacement", "verdict", "why"])


# ---------------------------------------------------------------------------
# Expression builders
# ---------------------------------------------------------------------------

def get_expr(name, rule=None, neg=False):
    e = "%s::get<\"%s\"%s>()" % (NS, name, (", %s::Rule::%s" % (NS, rule)) if rule else "")
    return ("!" + e) if neg else e


def _fmt_num(x):
    if isinstance(x, float) and x == int(x) and abs(x) < 1e15:
        return str(int(x))
    return "%g" % x if isinstance(x, float) else str(x)


# ---------------------------------------------------------------------------
# Bool resolution
# ---------------------------------------------------------------------------

_CAND_ORDER = ("Presence", "NonEmpty", "Exact1", "Lead1", "Lead0", "Lead0Kill", "Lead0P", "DefOnLead0",
               "AtoiNZ", "DefOnAtoiNZ", "Is01", "Std", "Lead0Def")


def _flip(x):
    return {"on": "off", "off": "on"}.get(x, x)


def _vec_match(site, ref):
    """site: list of on/off/D/?/inh per probe; ref: [bool].  True when every decided probe agrees."""
    for s, r in zip(site, ref):
        if s in ("on", "off") and (s == "on") != r:
            return False
    return True


def match_rule(site_vec, row_rule, row_default):
    """-> (rule, neg, default_used) of the first rule (preferring the row's own) whose vector equals the
    site's, with either polarity; None when no rule reproduces it."""
    dflts = (bool(row_default), not bool(row_default))
    cands = []
    order = ([row_rule] if row_rule in BOOL_RULES else []) + [r for r in _CAND_ORDER if r != row_rule]
    for neg in (False, True):
        for r in order:
            for d in (dflts if r in ("Std", "Lead0Def") else (dflts[0],)):
                cands.append((r, neg, d))
    # the row's own rule, either polarity, comes first; then the other rules
    cands.sort(key=lambda c: (c[0] != row_rule, c[2] != bool(row_default), c[1]))
    for r, neg, d in cands:
        ref = rule_vec(r, d)
        if neg:
            ref = [not b for b in ref]
        if _vec_match(site_vec, ref):
            return r, neg, d
    return None


def judge_bool(row, site_vec, why_prefix=""):
    """Bool/Presence verdict for a site whose value (as written, not orientated) is `site_vec`.
    -> dict(verdict, rule_used, neg, override(rule|None), diff[labels], alt, why, reason)."""
    if any(x == "inh" for x in site_vec):
        return {"reason": "Tri/umbrella-inheriting reader (use state<> in G4)"}
    m = match_rule(site_vec, row.rule, row.default)
    if m is None:
        return {"reason": "truthiness vector [%s] matches no Rule" % ",".join(site_vec)}
    r, neg, d = m
    row_vec = rule_vec(row.rule, bool(row.default)) if row.rule in BOOL_RULES else None
    if row_vec is None:
        return {"reason": "row rule %s is not a Bool rule" % row.rule}
    site_rule_vec = rule_vec(r, d)
    diff = [PROBE_LABELS[i] for i in range(8) if site_rule_vec[i] != row_vec[i]]
    out = {"rule": r, "neg": neg, "diff": diff, "default_used": d}
    if r in ("Std", "Lead0Def") and d != bool(row.default):
        out["reason"] = ("old reader is %s with default %s but the row's default is %s: no per-site override "
                         "can change a default (fix the row or keep the reader)" %
                         (r, "on" if d else "off", "on" if row.default else "off"))
        out["rule_for_text"] = r
        return out
    if not diff:
        out["verdict"] = "NO-CHANGE"
        out["override"] = None
    elif all(l in EDGE_LABELS for l in diff):
        out["verdict"] = "EDGE"
        out["override"] = None
        out["alt"] = r
    else:
        out["verdict"] = "SEMANTICS"
        out["override"] = r
    return out


def harness_impact(row, site_rule, site_default, diff):
    """Harness values (seed harnessValues) the row's rule would answer differently from the old reader."""
    if not diff or site_rule is None or row.rule not in BOOL_RULES:
        return []
    hits = []
    for h in row.harness:
        if h in ("String",):
            continue
        if apply_rule(site_rule, h, site_default) != apply_rule(row.rule, h, bool(row.default)):
            hits.append(h)
    return hits


# ---------------------------------------------------------------------------
# Numeric resolution
# ---------------------------------------------------------------------------

_LIT = r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?[fFuUlL]*"
_CMP_RE = re.compile(r"(?:[<>]=?\s*(%s))|(?:(%s)\s*[<>]=?)" % (_LIT, _LIT))
_CALLARG_RE = re.compile(r"\b(?:clamp|min|max|fmin|fmax|fminf|fmaxf)\s*\(([^()]*)\)")


def _lit_value(s):
    return float(re.sub(r"[fFuUlL]+$", "", s))


def bound_literals(text):
    """Numeric literals compared against or passed to clamp/min/max in `text`, in first-seen order."""
    seen = []
    for m in _CMP_RE.finditer(text):
        v = _lit_value(m.group(1) or m.group(2))
        if v not in seen:
            seen.append(v)
    for m in _CALLARG_RE.finditer(text):
        for lm in re.finditer(_LIT, m.group(1)):
            v = _lit_value(lm.group(0))
            if v not in seen:
                seen.append(v)
    return seen[:6]


def _prefix_num(s, is_int):
    if is_int:
        m = re.match(r"\s*([+-]?\d+)", s)
        v = int(m.group(1)) if m else 0
        return max(-2 ** 31, min(2 ** 31 - 1, v))
    m = re.match(r"\s*([+-]?(?:inf(?:inity)?|nan))", s, re.I)
    if m:
        return float("nan") if "nan" in m.group(1).lower() else (float("-inf") if m.group(1)[0] == "-" else float("inf"))
    return gi._cstr_prefix_float(s)


def _f32(x):
    try:
        return struct.unpack("f", struct.pack("f", x))[0]
    except (OverflowError, struct.error):
        return float("inf") if x > 0 else float("-inf")


def row_eval(kind, default, rng, probe):
    """What get<> returns for an Int/Float row (Prefix parse, optional range) on environment text `probe`."""
    is_int = kind == "Int"
    if probe is None or probe == "":
        return default
    v = _prefix_num(probe, is_int)
    if not is_int and not math.isfinite(v):
        return default
    if rng is not None:
        lo, hi, clamp = rng
        if (lo is not None and v < lo) or (hi is not None and v > hi):
            if clamp:
                v = min(max(v, lo if lo is not None else v), hi if hi is not None else v)
            else:
                return default
    return v


def _same(kind, a, b):
    if isinstance(a, bool) or isinstance(b, bool) or a is None or b is None:
        return False
    if kind == "Float":
        a, b = _f32(float(a)), _f32(float(b))
        if math.isnan(a) and math.isnan(b):
            return True
    return a == b


def _probe_set(kind, lits, harness):
    base = [None, "", "abc", "12abc", "2.0", "0", "-1", "1", "2", "1.5", "300", "5000", "1e9", " 7", "+5"]
    if kind == "Float":
        base += ["0.3", "0.5", "1e-9", "1e999"]
    extra = []
    for L in lits:
        if kind == "Int" or L == int(L):
            for d in (-1, 0, 1):
                extra.append(str(int(L) + d))
        if kind == "Float":
            for d in (-0.01, 0.0, 0.01):
                extra.append("%g" % (L + d))
    seen, out = set(), []
    for x in base + extra + [h for h in harness if isinstance(h, str) and h != "String"]:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def eval_unit_values(interp, path, T, pos, gate, probes, var_name=None):
    """Run the reader statements around `pos` on each probe string; -> [value] or None if unsupported.
    The WHOLE enclosing block runs (not only the tainted chain), so a default assigned after the read and
    a clamp after the parse both take effect."""
    gi._WIN_CACHE.clear()
    w = gi.build_window(path, T, pos)
    interp.gate, interp.site_off, interp.path = gate, pos, path
    stmts = []
    for a, b in w.spans:
        n = interp.parse_stmt(w.tokens, a, b)
        if n is not None:
            stmts.append(n)
    if not stmts or any(s[0] == "skip" and s[1] <= pos <= s[2] for s in stmts):
        return None
    out = []
    for val in probes:
        ctx = gi.Ctx(interp, val, top=True)
        try:
            r = interp.exec_list(stmts, ctx)
        except (gi._Unsupported, RecursionError):
            return None
        if isinstance(r, gi._Ret):
            out.append(r.val)
        elif var_name and var_name in ctx.env:
            out.append(ctx.env[var_name][0])
        else:
            out.append(None)
    return out


def judge_numeric(row, interp, path, T, pos, unit_text, var_name):
    """-> dict(verdict, diff[probe strings], edit(str), cand, why, reason)."""
    kind = row.kind
    lits = bound_literals(unit_text)
    probes = _probe_set(kind, lits, row.harness)
    olds = eval_unit_values(interp, path, T, pos, row.name, probes, var_name)
    if olds is None:
        return {"reason": "interpreter cannot execute the reader on numeric probes"}
    if any(not isinstance(o, (int, float)) or isinstance(o, bool) for o in olds):
        bad = [probes[i] for i, o in enumerate(olds) if not isinstance(o, (int, float)) or isinstance(o, bool)]
        return {"reason": "reader result is not a number for input %r" % (bad[0],), "boolish": all(
            isinstance(o, bool) or o is None for o in olds)}
    old_u = olds[probes.index(None)]
    dflt_row = row.default if isinstance(row.default, (int, float)) and not isinstance(row.default, bool) else 0
    row_rng = None
    if row.range:
        row_rng = (row.range.get("lo"), row.range.get("hi"), bool(row.range.get("clamp")))

    def score(cand_default, cand_rng):
        diffs = [probes[i] for i in range(len(probes))
                 if not _same(kind, olds[i], row_eval(kind, cand_default, cand_rng, probes[i]))]
        return diffs

    # candidates: (default, range, label); the as-is row first so it wins ties
    cands = [(dflt_row, row_rng, "as-is")]
    if not _same(kind, old_u, dflt_row):
        cands.append((old_u, row_rng, "default"))
    if row_rng is None or True:
        los = [None] + lits + ([L + 1 for L in lits] if kind == "Int" else [])
        his = [None] + lits + ([L - 1 for L in lits] if kind == "Int" else [])
        for style in (False, True):
            for lo in los:
                for hi in his:
                    if lo is None and hi is None:
                        continue
                    if lo is not None and hi is not None and lo > hi:
                        continue
                    cands.append((old_u, (lo, hi, style), "range"))
    best = None
    for i, (d, rg, lab) in enumerate(cands):
        diffs = score(d, rg)
        # fewer diffs first; then the as-is row; then fewer edits (default kept, no range, reject before clamp)
        penal = (len(diffs), 0 if lab == "as-is" else 1, 0 if rg is None else 1, 1 if (rg and rg[2]) else 0,
                 0 if _same(kind, d, dflt_row) else 1, i)
        if best is None or penal < best[0]:
            best = (penal, d, rg, lab, diffs)
    _, d, rg, lab, diffs = best

    edits = []
    if not _same(kind, d, dflt_row):
        edits.append("default %s -> %s" % (_fmt_num(dflt_row), _fmt_num(d)))
    if rg != row_rng:
        if rg is None:
            edits.append("drop the range")
        else:
            lo, hi, cl = rg
            nm = "clampRange" if cl else "range"
            if lo is not None and hi is not None:
                edits.append("%s(%s, %s)" % (nm, _fmt_num(lo), _fmt_num(hi)))
            elif lo is not None:
                edits.append("%s(%s, FLT_MAX)" % (nm, _fmt_num(lo)))
            else:
                edits.append("%s(-FLT_MAX, %s)" % (nm, _fmt_num(hi)))
    res = {"diff": diffs, "edit": "; ".join(edits), "probes": probes, "olds": olds, "cand_rng": rg,
           "cand_default": d, "as_is_ok": lab == "as-is" and not diffs}
    if not diffs:
        res["verdict"] = "NO-CHANGE"
    elif any(x is None for x in diffs):
        res["verdict"] = "SEMANTICS"      # unset differs: the default itself
    else:
        res["verdict"] = "EDGE"
    return res


# ---------------------------------------------------------------------------
# The per-file analysis
# ---------------------------------------------------------------------------

def _cast_for(kind, typ):
    """Cast wrapper text for a numeric get<> assigned to C++ type `typ` ('' = none)."""
    t = (typ or "").replace("std::", "").strip()
    if not t or t == "auto":
        return None
    if kind == "Int" and t in ("int", "int32_t"):
        return None
    if kind == "Float" and t == "float":
        return None
    return t


def analyze_text(path, raw, rows, ledger=None, extra_sources=()):
    """All raw env reads in `raw` (the text of `path`) -> [Result], in source order."""
    ledger = ledger or {}
    T = gi.strip_comments(raw)
    fns = gi.FnTable([(path, T)] + [(p, gi.strip_comments(t)) for p, t in extra_sources])
    interp = gi.Interp(fns, "")
    gi._WIN_CACHE.clear()
    gi._CLEAN_CACHE.clear()
    results = []
    for m in gi._LITERAL_RE.finditer(T):
        gate = m.group(1)
        if gi.typed_gate_site(path, T, m, gate) is not None:
            continue                                # already migrated
        call = find_call(T, m.start())
        if call is None:
            continue
        callee, cs, ce = call
        base = callee.split("::")[-1]
        line = T.count("\n", 0, m.start()) + 1
        is_raw = base in ("getenv", "_getenv")
        if not is_raw and fns.resolve(callee, path) is None and base not in gi._RAW_ENV_CALLEES:
            continue                                # a table row, a _putenv, an indirect name
        res = Result(path, line, gate, base)
        try:
            _analyze_site(res, interp, path, raw, T, m.start(), cs, ce, is_raw, rows, ledger)
        except Exception as exc:                    # a site must never abort the whole report
            res.verdict, res.reason = "UNHANDLED", "tool error: %s: %s" % (type(exc).__name__, exc)
            res.why = res.reason
        results.append(res)
    return results


def _fail(res, reason):
    res.verdict = "UNHANDLED"
    res.reason = reason
    res.replacement = "-"
    res.why = reason
    return res


def _line_bounds(T, pos):
    a = T.rfind("\n", 0, pos) + 1
    b = T.find("\n", pos)
    return a, (len(T) if b < 0 else b)


def _analyze_site(res, interp, path, raw, T, p, cs, ce, is_raw, rows, ledger):
    gate = res.gate
    row = rows.get(gate)
    res.old = ""
    if row is None:
        return _fail(res, "gate is not in the gate table (Gates/gate_table.cpp)")
    res.kind = row.kind
    if not is_raw:
        return _fail(res, "helper-call reader %s(): helper mapping lands in G4/G6" % res.callee)

    # ----- context ---------------------------------------------------------------------------------
    lam = find_lambda(T, p)
    unit = None                       # dict(span=(a,b), lhs, init_kind, ...)
    mode = None
    if lam and lam[0] == "other":
        return _fail(res, lam[1])
    if lam:
        _, lam_start, bo, bc, call_end = lam
        ss = gi._stmt_start(T, lam_start)
        head = T[ss:lam_start]
        eq = _split_assign(head + "=", len(head) + 1)
        hm = re.match(r"^(.*?)=\s*$", head, re.S)
        sm = re.match(r"\s*;", T[call_end:call_end + 8]) or re.match(r"\s*(?=[,)])", T[call_end:call_end + 8])
        if not hm or not re.match(r"\s*;", T[call_end:call_end + 8]):
            if re.match(r"^\s*return\s*$", head) and re.match(r"\s*;", T[call_end:call_end + 8]):
                return _fail(res, "lambda returned directly (no variable to keep as a static)")
            return _fail(res, "lambda initializer is not a plain `T x = [..]{..}();` statement")
        lhs = parse_lhs(hm.group(1))
        if lhs is None:
            return _fail(res, "unrecognised lambda target")
        end = call_end + re.match(r"\s*;", T[call_end:call_end + 8]).end()
        body = T[bo + 1:bc - 1]
        bad = body_impure(body)
        if bad:
            return _fail(res, "lambda body names `%s`, which is state outside the read" % bad)
        is_static = "static" in lhs["quals"]
        ns_scope = at_namespace_scope(T, ss)
        res.shape = shape_of(True, lhs["is_decl"], is_static, ns_scope)
        unit = {"span": (ss, end), "lhs": lhs, "kind": "lambda", "text": T[lam_start:call_end], "var": lhs["name"]}
        mode = "lambda"
    else:
        ss = gi._stmt_start(T, cs)
        se = gi._stmt_end(T, ss)
        st = T[ss:se]
        if _CTRL_RE.match(st) and not st.lstrip().startswith("return"):
            ctrl = True
        else:
            ctrl = False
        eq = -1 if ctrl else _split_assign(st, cs - ss)
        ns_scope = at_namespace_scope(T, ss)
        if eq >= 0 and not ctrl:
            lhs = parse_lhs(st[:eq])
            init = st[eq + 1:]
            init_text = init.rstrip().rstrip(";")
            is_static = lhs is not None and "static" in lhs["quals"]
            res.shape = shape_of(False, bool(lhs and lhs["is_decl"]), is_static, ns_scope)
            ptr_decl = (lhs and lhs["is_decl"] and re.match(r"^(?:const\s+)?(?:char|auto)\s*\*?\s*(?:const)?$",
                                                              lhs["type"].replace("const", "").strip() or "x")
                        and re.fullmatch(r"\s*(?:\(\s*const\s+char\s*\*\s*\)\s*)?%s\s*" % re.escape(T[cs:ce]),
                                         init_text))
            if ptr_decl:
                unit = {"span": (ss, se), "lhs": lhs, "kind": "local", "ptr": lhs["name"].lstrip("*&")}
                mode = "local"
            elif row.kind in ("Int", "Float"):
                if lhs is None:
                    return _fail(res, "unrecognised assignment target")
                bad = body_impure(init_text)
                if bad:
                    return _fail(res, "initializer names `%s`, which is state outside the read" % bad)
                unit = {"span": (ss, se), "lhs": lhs, "kind": "decl", "text": init_text, "var": lhs["name"]}
                mode = "decl-num"
        if mode is None:
            res.shape = shape_of(False, bool(eq >= 0 and parse_lhs(st[:eq]) and parse_lhs(st[:eq])["is_decl"]),
                                 eq >= 0 and "static" in (parse_lhs(st[:eq]) or {"quals": []})["quals"], ns_scope)
            if ctrl or eq < 0:
                res.shape = "per-call"
            mode = "expr"

    # ----- classify the read (not for expr mode, which is syntactic) --------------------------------
    ledger_row = ledger.get(gate)
    notes = []
    if ledger_row:
        st_ = ledger_row.get("status", "")
        notes.append("G0 ledger %s: %s" % (st_, ledger_row.get("rule", "")))
        if st_ in ("divergent", "enum"):
            notes.append("OWNER DECISION PENDING: do not migrate until resolved")
    if row.conflict:
        notes.append("readers disagree (seed conflict flag)")

    if row.kind in ("Int", "Float"):
        return _numeric(res, row, interp, path, T, p, cs, ce, unit, mode, notes, raw)
    if row.kind in ("Tri", "Enum", "String"):
        if mode == "expr":
            sub = _expr_parts(T, cs, ce)
            if sub and sub["neg"] is not None and not sub["prefix_pointer"]:
                # a bare presence test of a non-Bool row: isSet<>
                return _expr_emit(res, row, T, raw, p, cs, ce, sub, notes, presence_only=True)
        return _fail(res, "row kind %s: read through %s (G3 Enum / G4 Tri / String via str<>)" %
                     (row.kind, {"Tri": "state<>/Tri", "Enum": "is<>/Enum token", "String": "str<>"}[row.kind]))
    # Bool row
    if mode == "expr":
        sub = _expr_parts(T, cs, ce)
        if sub is None:
            return _fail(res, "getenv result is used as a pointer here (not a bare presence test)")
        return _expr_emit(res, row, T, raw, p, cs, ce, sub, notes)
    r = gi.analyze_site(interp, path, T, p, gate)
    if r["status"] != "reader":
        why = r["status"]
        return _fail(res, "classifier: %s" % why)
    d = gi.describe(r)
    if d is None:
        return _fail(res, "classifier gave no outcome vector")
    fam, vec, cls, inv = d
    res.idiom = cls
    if fam != "bool":
        return _fail(res, "reader is %s-valued but the row is Bool" % fam)
    raw_vec = [_flip(x) for x in vec] if inv else list(vec)
    if mode == "local":
        pt = unit["ptr"]
        sub_unit = _local_unit(T, unit, pt)
        if isinstance(sub_unit, str):
            return _fail(res, sub_unit)
        unit.update(sub_unit)
    jd = judge_bool(row, raw_vec)
    if "reason" in jd and "verdict" not in jd:
        return _fail(res, jd["reason"])
    return _bool_emit(res, row, jd, unit, mode, notes, T, raw, inv)


# ----- bool emission ---------------------------------------------------------------------------------

def _why_bool(row, jd, notes, res):
    r = jd["rule"]
    parts = ["site %s%s vs row %s%s" % ("!" if jd["neg"] else "", r, row.rule,
                                       "(def=%s)" % ("on" if row.default else "off") if row.rule in ("Std", "Lead0Def") else "")]
    if jd["diff"]:
        parts.append("differs on %s" % ",".join(jd["diff"]))
    if jd["verdict"] == "EDGE":
        parts.append("byte-identical alternative: %s" % get_expr(row.name, jd.get("alt"), jd["neg"]))
    if res.harness_hits:
        parts.append("harness sets %s (changes behavior)" % ",".join(repr(h) for h in res.harness_hits))
    elif jd["diff"]:
        parts.append("no harness value in the seed hits a changed input" +
                     (" (seed harnessValues=%s)" % row.harness if row.harness else ""))
    return "; ".join(parts + list(notes))


def _bool_emit(res, row, jd, unit, mode, notes, T, raw, inv):
    res.verdict = jd["verdict"]
    res.diff = jd["diff"]
    res.override = jd.get("override") or ""
    res.harness_hits = harness_impact(row, jd["rule"], jd["default_used"], jd["diff"])
    e = get_expr(row.name, res.override or None, jd["neg"])
    lhs = unit["lhs"]
    if mode == "local":
        res.replacement = "%s%s;" % (unit["tgt"], e)
        res.old = _norm_ws(T[unit["span"][0]:unit["span2"][1]])
    else:
        res.replacement = "%s = %s;" % (_norm_ws(_lhs_text(lhs)), e)
        res.old = _norm_ws(T[unit["span"][0]:unit["span"][1]])
    res.mode = mode
    res.idiom = res.idiom or jd["rule"]
    if jd["neg"]:
        res.idiom += " (inverted use)"
    res.why = _why_bool(row, jd, notes, res)
    return res


def _lhs_text(lhs):
    return " ".join(lhs["quals"] + ([lhs["type"]] if lhs["type"] else []) + [lhs["name"]]).replace("* ", "*")


def _local_unit(T, unit, ptr):
    """For `const char* v = getenv(..);` followed by one simple statement that is the whole reader."""
    a, b = unit["span"]
    nxt_end = gi._stmt_end(T, b)
    st2 = T[b:nxt_end]
    if re.search(r"[{}]", st2) or _CTRL_RE.match(st2) and not st2.lstrip().startswith("return"):
        return "pointer %s is consumed by a block/branch statement: not a single expression" % ptr
    m = re.match(r"\s*return\s+(.*);\s*$", st2, re.S)
    tgt = None
    tgt_decl = ""
    if m:
        expr = m.group(1)
        tgt = "return "
    else:
        eq = _split_assign(st2, len(st2))
        if eq < 0:
            return "pointer %s is not consumed by a `return`/assignment" % ptr
        l2 = parse_lhs(st2[:eq])
        if l2 is None:
            return "pointer %s: unrecognised assignment target" % ptr
        expr = st2[eq + 1:].rstrip().rstrip(";")
        tgt = _norm_ws(_lhs_text(l2)) + " = "
        if l2["is_decl"]:
            tgt_decl = ""
    bad = body_impure(expr, extra_ok=(ptr,))
    if bad:
        return "expression using %s also names `%s`" % (ptr, bad)
    if not re.search(r"\b%s\b" % re.escape(ptr), expr):
        return "pointer %s is not used by the following statement" % ptr
    blk = gi.enclosing_block(T, a)
    tail = T[nxt_end:blk[1] if blk else len(T)]
    if re.search(r"\b%s\b" % re.escape(ptr), tail):
        return "pointer %s is used again later in the block" % ptr
    return {"span2": (b, nxt_end), "tgt": tgt, "tgt_decl": tgt_decl, "lhs": unit["lhs"]}


# ----- expression-mode (bare presence test or == / != nullptr) -----------------------------------------

_NULLCMP_RE = re.compile(r"\s*(!=|==)\s*(?:nullptr|NULL|0|\(\s*(?:const\s+)?char\s*\*\s*\)\s*0)(?![\w.])")


def _expr_parts(T, cs, ce):
    """-> dict(end, neg, prefix_pointer) for the sub-expression to replace, or None if the call is used as a
    pointer.  neg is True for `== nullptr`."""
    post = T[ce:ce + 60]
    nm = _NULLCMP_RE.match(post)
    if nm:
        return {"end": ce + nm.end(), "neg": nm.group(1) == "==", "prefix_pointer": False}
    pre = T[:cs].rstrip()
    after = post.lstrip()
    before_ok = pre.endswith(("(", "!", "&&", "||", "?", "=", ",", ":", "{")) or re.search(r"\breturn$", pre)
    after_ok = (after[:1] in (")", ";", "?", ",", ":", "}") or after.startswith("&&") or after.startswith("||"))
    whole_value = (pre.endswith("=") or re.search(r"return$", pre) or pre.endswith(",")) and after[:1] in (";", ",", ")")
    call_arg = False
    if pre.endswith("("):
        wm = re.search(r"([A-Za-z_]\w*|[>\]])\s*\($", pre)
        call_arg = bool(wm) and wm.group(1) not in ("if", "while", "for", "switch", "return", "case")
    if before_ok and after_ok and not whole_value and not call_arg:
        return {"end": ce, "neg": False, "prefix_pointer": False}
    return None


def _expr_emit(res, row, T, raw, p, cs, ce, sub, notes, presence_only=False):
    res.mode = "expr"
    res.idiom = "Presence" + (" (== nullptr)" if sub["neg"] else "")
    la, lb = _line_bounds(T, cs)
    old_line = raw[la:lb]
    if presence_only:
        new_e = "%s::isSet<\"%s\">()" % (NS, row.name)
        if sub["neg"]:
            new_e = "!" + new_e
        res.verdict = "NO-CHANGE"
        res.why = "presence test of a %s row: isSet<> is the presence read" % row.kind
        res.why = "; ".join([res.why] + list(notes))
    else:
        jd = judge_bool(row, ["on" if b else "off" for b in rule_vec("Presence", False)])
        if "verdict" not in jd:
            return _fail(res, jd.get("reason", "no verdict"))
        jd["neg"] = sub["neg"] if jd.get("rule") else sub["neg"]
        res.diff = jd["diff"]
        res.verdict = jd["verdict"]
        res.override = jd.get("override") or ""
        res.harness_hits = harness_impact(row, "Presence", False, jd["diff"])
        new_e = get_expr(row.name, res.override or None, sub["neg"])
        # the whole expression's value is the old test: a `== nullptr` flips it, nothing else changes
        jd2 = dict(jd)
        jd2["neg"] = sub["neg"]
        jd2["rule"] = "Presence"
        res.why = _why_bool(row, jd2, notes, res)
    # a declaration/assignment is shown whole (it may span lines); a control statement shows its own line
    ss = gi._stmt_start(T, cs)
    se = gi._stmt_end(T, ss)
    if not _CTRL_RE.match(T[ss:se]) and "{" not in T[ss:cs] and ss < la:
        la, lb = ss, se
        old_line = raw[la:lb]
    new_line = raw[la:cs] + new_e + raw[sub["end"]:lb]
    # `x = (A);` loses the redundant parentheses a `!= nullptr` test needed
    new_line = re.sub(r"=\s*\(\s*(%s)\s*\)\s*;" % re.escape(new_e), r"= \1;", new_line)
    res.old = _norm_ws(old_line)
    res.replacement = _norm_ws(new_line)
    return res


# ----- numeric emission -------------------------------------------------------------------------------

def _numeric(res, row, interp, path, T, p, cs, ce, unit, mode, notes, raw):
    if unit is None or mode not in ("lambda", "decl-num"):
        return _fail(res, "numeric reader is not a lambda/declaration initializer (e.g. multi-statement local)")
    lhs = unit["lhs"]
    res.idiom = "Num(%s)" % ",".join(sorted(set(_PARSER_RE.findall(unit["text"])))) if _PARSER_RE.search(unit["text"]) \
        else "Num"
    jd = judge_numeric(row, interp, path, T, p, unit["text"], unit["var"])
    if "verdict" not in jd:
        if jd.get("boolish"):
            return _fail(res, "numeric row read as a bool test: %s" % jd["reason"])
        return _fail(res, jd["reason"])
    cast = _cast_for(row.kind, lhs["type"])
    e = get_expr(row.name)
    if cast:
        e = "static_cast<%s>(%s)" % (cast, e)
    res.verdict = jd["verdict"]
    res.diff = [("unset" if x is None else repr(x)) for x in jd["diff"]]
    res.replacement = "%s = %s;" % (_norm_ws(_lhs_text(lhs)), e)
    res.old = _norm_ws(T[unit["span"][0]:unit["span"][1]])
    res.mode = mode
    res.row_edit = jd["edit"]
    hits = [h for h in row.harness if h != "String" and h in [x for x in jd["diff"] if x is not None]]
    res.harness_hits = hits
    parts = []
    if jd["edit"]:
        parts.append("ROW EDIT needed first (%s) %s" % (row.name, jd["edit"]))
    else:
        parts.append("no row edit needed")
    if jd["diff"]:
        parts.append("inputs whose result changes after the row edit: %s" % ", ".join(res.diff))
        parts.append("harness sets %s" % (",".join(repr(h) for h in hits) if hits else "none of them") +
                     (" (seed harnessValues=%s)" % row.harness if row.harness else ""))
    res.why = "; ".join(parts + list(notes))
    return res


# ---------------------------------------------------------------------------
# Whole run, summary and rendering
# ---------------------------------------------------------------------------

def run(files, root=ROOT, rev="HEAD", helpers=()):
    rows = load_rows(root, rev)
    ledger = load_ledger(root)
    results, srcs = [], []
    for rel in files:
        srcs.append((rel, read_source(root, rel, rev)))
    extra = [(p, t) for p, t in helpers]
    for rel, raw in srcs:
        others = [(p, t) for p, t in srcs if p != rel] + extra
        results.extend(analyze_text(rel, raw, rows, ledger, others))
    return results, rows


def summarize(results):
    c = Counter(r.verdict for r in results)
    by_file = {}
    for r in results:
        by_file.setdefault(r.file, Counter())[r.verdict] += 1
    shapes = Counter(r.shape for r in results)
    overrides = [r for r in results if r.override]
    edits = {}
    for r in results:
        if r.row_edit:
            edits[r.gate] = r.row_edit
    return {"total": len(results), "verdicts": {v: c.get(v, 0) for v in VERDICTS}, "by_file": by_file,
            "shapes": {s: shapes.get(s, 0) for s in SHAPES}, "overrides": len(overrides),
            "row_edits": edits}


def tsv_text(results):
    return TSV_HEADER + "\n" + "\n".join(r.tsv() for r in results) + "\n"


def _cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def _md_table(rs):
    out = ["| file:line | gate | old idiom class | old caching shape | proposed replacement | verdict | why |",
           "|---|---|---|---|---|---|---|"]
    for r in rs:
        out.append("| %s |" % " | ".join(_cell("`%s`" % x if k in (0, 1, 4) and x not in ("", "-") else x)
                                           for k, x in enumerate([r.where, r.gate, r.idiom, r.shape, r.replacement,
                                                                  r.verdict, r.why])))
    return out


def render_md(results, rows, files, rev_label):
    s = summarize(results)
    L = []
    L.append("# Gate codemod, G1b pilot plan (dry run)")
    L.append("")
    L.append("Generated by `py -3 tools/gate_codemod.py --write`. **No C++ was changed.** Each row says what happens if "
             "the raw `getenv` read at `file:line` is rewritten to `%s::get<\"MC2_X\">()` against the gate row "
             "committed in `Gates/gate_table.json` (design `typed-gate-registry-design.md` section 5.7, review "
             "amendments 1, 2, 9). Scanned: commit `%s` (files read from git, not the working tree)." % (NS, rev_label))
    L.append("")
    L.append("How to read the verdicts:")
    L.append("")
    L.append("* **NO-CHANGE** -- the old reader and the row answer identically for every probe input. Rewrite.")
    L.append("* **EDGE** -- differs only on `\"\"`, `false`, other text, or (numeric) junk / out-of-range input. Rewrite and list the file in the PR; "
             "the byte-identical alternative is named in the row.")
    L.append("* **SEMANTICS** -- differs on unset, `\"0\"` or `\"1\"`, or on the default. The proposed text is the per-site override "
             "(`get<\"N\", Rule::R>()`), never Std, so behavior is byte-identical; converting it to the row rule is a separate named commit.")
    L.append("* **UNHANDLED** -- the tool could not prove the site; a human reads the code line.")
    L.append("")
    L.append("Rows in the committed table still carry the legacy rule of their readers (`L(Presence, ...)` etc.), so NO-CHANGE here means "
             "\"identical to the row as committed\"; converting a row to Std is a separate named commit and would change these verdicts.")
    L.append("")
    L.append("Probe inputs: **U** unset, **E** `\"\"`, **Z** `\"0\"`, **F** `\"false\"`, **O** `\"1\"`, **X** `\"yes\"`, **X10** `\"10\"`, **X25** `\"2.5\"`. "
             "Caching shape is preserved: a `static const T s = <reader>` stays a static holding `get<>()`; a lambda static loses only the lambda.")
    L.append("")
    L.append("Every file that gets a replacement needs `#include \"Gates/gate.h\"`.")
    L.append("")
    L.append("## Summary")
    L.append("")
    L.append("| Measure | Count |")
    L.append("|---|---|")
    L.append("| sites analysed | %d |" % s["total"])
    for v in VERDICTS:
        L.append("| **%s** | %d |" % (v, s["verdicts"][v]))
    L.append("| per-site `Rule::` overrides proposed (each raises the `gate_overrides` baseline from 0) | %d |" % s["overrides"])
    L.append("| gates needing a row edit first | %d |" % len(s["row_edits"]))
    for sh in SHAPES:
        L.append("| shape `%s` | %d |" % (sh, s["shapes"][sh]))
    L.append("")
    L.append("| File | " + " | ".join(VERDICTS) + " | total |")
    L.append("|---|" + "---|" * (len(VERDICTS) + 1))
    for f in files:
        c = s["by_file"].get(f, Counter())
        L.append("| `%s` | %s | %d |" % (f, " | ".join(str(c.get(v, 0)) for v in VERDICTS), sum(c.values())))
    L.append("")
    for verdict, title, blurb in (
            ("SEMANTICS", "SEMANTICS rows", "Each must migrate with the override shown, never with the row rule."),
            ("EDGE", "EDGE rows", "Listed inputs change result if the row's own rule is used."),
            ("UNHANDLED", "Sites the tool could not handle", "The code line is the reader as written.")):
        rs = [r for r in results if r.verdict == verdict]
        L.append("## %s (%d)" % (title, len(rs)))
        L.append("")
        L.append(blurb)
        L.append("")
        if not rs:
            L.append("None.")
            L.append("")
            continue
        L.extend(_md_table(rs))
        L.append("")
        if verdict == "UNHANDLED":
            for r in rs:
                L.append("* `%s` (%s): `%s`" % (r.where, r.gate, r.old or _code_line_hint(r)))
            L.append("")
    L.append("## Row edits required before the pilot applies")
    L.append("")
    L.append("These change `Gates/gate_table.cpp` only; every other reader of the gate is still a raw `getenv` until it migrates, "
             "so nothing observable moves. After the edit the listed sites verify as shown above.")
    L.append("")
    if s["row_edits"]:
        L.append("| gate | row now | edit |")
        L.append("|---|---|---|")
        for g in sorted(s["row_edits"]):
            rw = rows[g]
            now = "%s %s default=%s range=%s" % (rw.kind, rw.rule, rw.default,
                                                 "none" if not rw.range else "%s..%s%s" % (
                                                     rw.range.get("lo"), rw.range.get("hi"),
                                                     " clamp" if rw.range.get("clamp") else " reject"))
            L.append("| `%s` | %s | %s |" % (g, now, _cell(s["row_edits"][g])))
    else:
        L.append("None.")
    L.append("")
    rowfix = sorted({r.gate for r in results if r.verdict == "SEMANTICS" and r.override and r.kind == "Bool"
                     and rows[r.gate].rule == "Std"})
    if rowfix:
        L.append("Bool rows whose committed rule is Std but whose reader is not: " +
                 ", ".join("`%s`" % g for g in rowfix) +
                 ". Fix the row (`L(Rule, ...)`) or keep the per-site override.")
        L.append("")
    L.append("## Full table")
    L.append("")
    L.extend(_md_table(results))
    L.append("")
    L.append("## Before and after")
    L.append("")
    for r in results:
        L.append("* `%s` %s (%s, %s)" % (r.where, r.gate, r.verdict, r.shape))
        L.append("  * before: `%s`" % (r.old or _code_line_hint(r)))
        L.append("  * after: `%s`" % r.replacement)
    L.append("")
    L.append("## Limits of this plan")
    L.append("")
    L.append("* The interpreter in `scripts/gate_idioms.py` models C string reads only (`getenv`, `atoi`/`atol`/`atof`, character tests, `strcmp`); "
             "a site that needs more is UNHANDLED, never guessed.")
    L.append("* `inf`/`nan` and float-to-double rounding at a bound are modelled only through the `1e999` probe; `NaN` input is not probed.")
    L.append("* Harness impact reads `harnessValues` in the gate seed (values found in scripts, tests, tools, data). Launcher `offVal`s and installed "
             "`launcher_env.json` files are not in the seed.")
    L.append("* A replacement that drops a `const char*` local also drops the local's name, so the tool refuses when the pointer is used again.")
    L.append("")
    return "\n".join(L).rstrip() + "\n"


def _code_line_hint(r):
    return r.old or "(see %s)" % r.where


def unhandled_code_lines(results, root, rev):
    """Fill r.old with the raw source line of UNHANDLED sites so the report shows the code."""
    cache = {}
    for r in results:
        if r.old:
            continue
        if r.file not in cache:
            cache[r.file] = read_source(root, r.file, rev).split("\n")
        lines = cache[r.file]
        if 0 < r.line <= len(lines):
            r.old = _norm_ws(lines[r.line - 1])


def main(argv=None):
    ap = argparse.ArgumentParser(description="DRY-RUN planner for the G1b gate migration pilot (writes no C++).")
    ap.add_argument("--files", nargs="*", help="repo-relative files (default: the G1b pilot files)")
    ap.add_argument("--rev", default="HEAD", help="git revision to read C++ and tables from (default HEAD)")
    ap.add_argument("--worktree", action="store_true", help="read the working tree instead of git")
    ap.add_argument("--write", action="store_true", help="write %s and %s" % (OUT_MD, OUT_TSV))
    ap.add_argument("--summary", action="store_true", help="print only the summary counts")
    ap.add_argument("--apply", action="store_true", help="reserved; refuses unless --i-mean-it, does nothing in this slice")
    ap.add_argument("--i-mean-it", action="store_true")
    a = ap.parse_args(argv)
    if a.apply:
        if not a.i_mean_it:
            print("gate_codemod: --apply refused: pass --i-mean-it as well (and note this slice is dry-run only)",
                  file=sys.stderr)
            return 2
        print("gate_codemod: --apply is not implemented: the G1b pilot commit rewrites C++ by hand from the reviewed "
              "table; this tool writes no C++", file=sys.stderr)
        return 2
    files = a.files or list(PILOT_FILES)
    rev = None if a.worktree else a.rev
    results, rows = run(files, ROOT, rev)
    unhandled_code_lines(results, ROOT, rev)
    s = summarize(results)
    if a.write:
        label = git_head(ROOT) if rev == "HEAD" else (a.rev if rev else "working tree")
        with open(os.path.join(ROOT, OUT_MD), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render_md(results, rows, files, label))
        with open(os.path.join(ROOT, OUT_TSV), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(tsv_text(results))
        print("gate_codemod: wrote %s and %s (%d sites)" % (OUT_MD, OUT_TSV, s["total"]))
    elif not a.summary:
        sys.stdout.write(tsv_text(results))
    print("gate_codemod: %d sites: %s; overrides proposed %d; gates needing a row edit %d" % (
        s["total"], ", ".join("%s %d" % (v, s["verdicts"][v]) for v in VERDICTS), s["overrides"],
        len(s["row_edits"])), file=sys.stderr if not a.summary else sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
