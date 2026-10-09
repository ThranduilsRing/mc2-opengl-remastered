#!/usr/bin/env python3
"""gate_gen -- the Python half of the typed gate registry's generator (design S2, slice G1a).

WHY THIS EXISTS
---------------
The typed gate table (.claude/cleanup/typed-gate-registry-design.md, section 5)
replaces ~1400 raw getenv sites with one row per gate. Somebody has to write the
first ~1175 rows, and doing it by hand would guess at kinds, defaults and
truthiness rules and silently change behaviour. This tool derives every row from
what the tree already DOES: the reader-idiom interpreter (scripts/gate_idioms.py)
supplies each gate's truthiness rule and unset behaviour, the feature registry
supplies documented defaults and prose, and the harness/launcher configs supply
the values routine runs actually assign. The C++ half (G1a, a builder lane) then
only has to consume tools/gate_table.seed.json.

Nothing here edits C++ or runs a build. Everything is read from a git revision
(HEAD by default), not from the working tree, because other lanes edit the tree
continuously and a seed that follows their half-made edits would flap.

MODES
-----
  --import-legacy    write tools/gate_table.seed.json (deterministic, sorted by name, LF)
  --check-seed       re-derive in memory and diff against the committed seed; exit 1 on
                     drift or on an undocumented registry/reader default disagreement
  --stats            histograms, doc provenance, harness coverage, every unknown default
  --gate-history     write docs/gate-history.md (registry prose longer than one sentence,
                     verbatim, so the table's one-line doc loses nothing)
  --check-history    diff docs/gate-history.md against what --gate-history would write

SEED FIELDS (one record per gate name)
--------------------------------------
  name kind default rule owner retireBy doc readers conflict registered
  registryDefault seedDefaultSource harnessValues numericRange
plus the evidence the design's cross-check needs: readerDefault, defaultDisagreement,
registryKind, featureId, retireVerdict.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, HERE)
import gate_idioms as gi  # noqa: E402
import registry_parse as rp  # noqa: E402
import gate_conflicts as gc  # noqa: E402

SEED_REL = "tools/gate_table.seed.json"
HISTORY_REL = "docs/gate-history.md"
REGISTRY_REL = gc.REGISTRY.replace("\\", "/")
PLAN_A_REL = ".claude/cleanup/tierA-gates-plan.md"
PLAN_B_REL = ".claude/cleanup/tierB-trace-gates.md"
SEED_VERSION = 1

#: Every C++ directory a gate can be read in, including the standalone harnesses under
#: tools/ and tests/ whose `_putenv` writers are why a name must be in the table at all.
CPP_DIRS = ["code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld",
            "GameAdapters", "HostServices", "EditorBridge", "editor", "gui", "tools", "tests"]

#: Where a gate being ASSIGNED a value is visible to routine runs.
CONFIG_DIRS = ["scripts", "tests", "tools", "run", "data"]
CONFIG_EXTS = (".py", ".sh", ".bat", ".ps1", ".cmd", ".json", ".yaml", ".yml")
CONFIG_ROOT_EXTS = (".bat", ".ps1", ".cmd", ".json")
#: Files that name gates without assigning them (this tool's own output and tests,
#: the conflict census): counting them would make the seed depend on itself.
CONFIG_EXCLUDE = {SEED_REL, "tools/gate_gen.py", "tools/gate_conflicts.py",
                  "tools/gate_conflicts.baseline.json"}
CONFIG_EXCLUDE_PREFIX = ("scripts/tests/",)
MAX_CONFIG_BYTES = 2_000_000

# ---------------------------------------------------------------------------
# Owner
# ---------------------------------------------------------------------------

#: Domains specific enough to own a gate. A gate that also appears in generic engine
#: code or in the editor belongs to its specific domain; among the generic ones the
#: plurality decides.
SPECIFIC_OWNERS = ("terrain", "mech", "vk", "post", "props")
GENERIC_OWNERS = ("gameos", "code")
_NAME_KEYWORDS = (("vk", re.compile(r"(?:^|_)(?:VK|VULKAN)(?:_|$)")),
                  ("terrain", re.compile(r"TERRAIN|WATER|SHORE|CEMENT|HEIGHT")),
                  ("mech", re.compile(r"MECH")),
                  ("post", re.compile(r"POST|BLOOM|TONEMAP|SSAO|HZB|FOG")),
                  ("props", re.compile(r"PROP|BDACTOR|TREE|VEG")))


def file_owner(rel):
    """Directory-and-filename derived owner of one source file."""
    low = rel.lower()
    base = low.rsplit("/", 1)[-1]
    top = low.split("/", 1)[0]
    if top in ("tools", "tests"):
        return "unclassified"
    if top in ("editor", "gui", "editorbridge"):
        return "editor"
    if "vulkan" in base or "_vk" in base or base.startswith("vk_"):
        return "vk"
    if "terrain" in base or "water" in base or base.startswith(("quad", "terr", "tile")):
        return "terrain"
    if "mech" in base or base.startswith("gvactor"):
        return "mech"
    if "postprocess" in base or base.startswith("post"):
        return "post"
    if "prop" in base or base.startswith(("bdactor", "terrobj")):
        return "props"
    return "gameos" if top == "gameos" else "code"


def name_owner_hint(name):
    if name.startswith(("MC2_VK_", "MC2_VULKAN_")):
        return "vk"
    hits = [o for o, rx in _NAME_KEYWORDS if rx.search(name)]
    return hits[0] if len(hits) == 1 else None


def gate_owner(name, files):
    """Owner of one gate from the files that mention it: the strongest tier that
    appears wins, a plurality inside it decides, and a tie is broken by the gate's
    own name or left `unclassified` for a human."""
    owners = Counter(file_owner(f) for f in files)
    for tier in (SPECIFIC_OWNERS, GENERIC_OWNERS + ("editor",)):
        counts = {o: owners[o] for o in tier if owners.get(o)}
        if not counts:
            continue
        top = max(counts.values())
        leaders = sorted(o for o, c in counts.items() if c == top)
        if len(leaders) == 1:
            return leaders[0]
        hint = name_owner_hint(name)
        return hint if hint in leaders else "unclassified"
    return name_owner_hint(name) or "unclassified"


# ---------------------------------------------------------------------------
# Sentences and docs
# ---------------------------------------------------------------------------

_SENT_END_RE = re.compile(r"[.!?][\"')\]]*\s+")
_ABBREV = ("e.g", "i.e", "vs", "cf", "approx", "incl", "resp", "esp", "no", "fig")
_SENT_START_RE = re.compile(r"[A-Z0-9\[(`\"']")


def split_sentences(text):
    """Split prose at sentence boundaries; abbreviations (e.g., vs.) and a following
    lowercase word are not boundaries. Whitespace between sentences is dropped."""
    text = text.strip()
    out, start = [], 0
    for m in _SENT_END_RE.finditer(text):
        nxt = text[m.end():m.end() + 1]
        if not nxt or not _SENT_START_RE.match(nxt):
            continue
        if text[m.start()] == ".":
            tail = re.search(r"([A-Za-z.]+)$", text[start:m.start() + 1])
            if tail and tail.group(1).lower().rstrip(".") in _ABBREV:
                continue
        out.append(text[start:m.end()].strip())
        start = m.end()
    tail = text[start:].strip()
    if tail:
        out.append(tail)
    return out


def first_sentence(text):
    s = split_sentences(" ".join(text.split()))
    return s[0] if s else ""


_COMMENT_LINE_RE = re.compile(r"^\s*(?://+|/\*+|\*+(?!/))\s?(.*?)\s*(?:\*+/)?\s*$")
_PUNCT_ONLY_RE = re.compile(r"^[\W_]*$")


def comment_doc(raw_text, line):
    """First sentence of the comment block directly above the statement that holds
    the 1-based `line`; "" when there is none."""
    lines = raw_text.split("\n")
    i = line - 2
    steps = 0
    # A statement spread over several lines: step over its own earlier lines.
    while (i >= 0 and steps < 4 and lines[i].strip()
           and not _COMMENT_LINE_RE.match(lines[i])
           and not lines[i].rstrip().endswith((";", "{", "}", ":"))):
        i -= 1
        steps += 1
    block = []
    while i >= 0:
        m = _COMMENT_LINE_RE.match(lines[i]) if lines[i].strip() else None
        if not m:
            break
        block.append(m.group(1))
        i -= 1
    block.reverse()
    block = [b for b in block if b and not _PUNCT_ONLY_RE.match(b)]
    return first_sentence(" ".join(block)) if block else ""


# ---------------------------------------------------------------------------
# Site discovery (gate_idioms.scan_sources, plus the position each site needs)
# ---------------------------------------------------------------------------

def scan_sites(sources):
    """-> (sites, occurrences).

    Same site discovery and classification as gate_idioms.scan_sources, and each
    site additionally carries its offset so evidence can be pulled from the code
    around it. `occurrences` is every `"MC2_*"` literal in any context -- a
    `_putenv` writer or a table row has no reader but is still a gate name.
    """
    fns = gi.FnTable(sources)
    interp = gi.Interp(fns, "")
    gi._WIN_CACHE.clear()
    gi._CLEAN_CACHE.clear()
    sites = []
    occ = defaultdict(list)
    for path, text in sources:
        for m in gi._LITERAL_RE.finditer(text):
            gate = m.group(1)
            line = text.count("\n", 0, m.start()) + 1
            occ[gate].append((path, line))
            cm = gi._CALLEE_RE.search(text[max(0, m.start() - 96):m.start()])
            callee = cm.group(1) if cm else None
            base = callee.split("::")[-1] if callee else None
            if not (base in gi._RAW_ENV_CALLEES or (callee and fns.resolve(callee, path) is not None)):
                continue
            r = gi.analyze_site(interp, path, text, m.start(), gate)
            site = {"gate": gate, "file": path, "line": line, "pos": m.start(), "callee": base,
                    "status": r["status"], "family": None, "vector": None,
                    "cls": None, "inverted": False,
                    "parsers": r.get("parsers", []), "opaque": r.get("opaque", [])}
            d = gi.describe(r)
            if d:
                fam, vec, name, inv = d
                site.update(family=fam, vector=vec, cls=name, inverted=inv)
                if not any(x not in (gi.WILD, "D") for x in vec):
                    site["status"] = "unclassified:opaque"
                elif gi.is_uninformative(fam, vec, site["parsers"]):
                    site["status"] = "token"
            sites.append(site)
    return sites, occ


# ---------------------------------------------------------------------------
# Value evidence: tokens and numeric ranges a reader compares against
# ---------------------------------------------------------------------------

_PARSERS = {"atoi", "atol", "atoll", "_atoi64", "atof", "strtol", "strtoll", "strtoul",
            "strtoull", "strtod", "strtof", "stoi", "stol", "stoll", "stoul", "stof", "stod"}
_FLOAT_PARSERS = {"atof", "strtod", "strtof", "stof", "stod"}
_STRCMP = {"strcmp", "_stricmp", "stricmp", "strcasecmp", "strncmp", "_strnicmp",
           "strncasecmp", "wcscmp", "_wcsicmp"}
_CLAMPS = {"clamp", "clampf", "clampi", "saturate"}
_CMP_OPS = {"==", "!=", "<", "<=", ">", ">="}
_FLIP = {"<": ">", "<=": ">=", ">": "<", ">=": "<=", "==": "==", "!=": "!="}
_BOOLISH = {"0", "1", "off", "on", "false", "true", "no", "yes"}
_STD_BOOL_CLASSES = ("Presence", "Lead0", "Lead0Kill", "Lead0P", "DefOnLead0", "Lead1",
                     "Exact1", "NonEmpty", "AtoiNZ", "Is01", "Std(", "Tri(")


def _num(tok):
    """Numeric value of a number token, or None."""
    if tok[0] != "num":
        return None
    s = tok[1].replace("'", "").rstrip("fFuUlL") if not tok[1].lower().startswith("0x") \
        else tok[1].replace("'", "").rstrip("uUlL")
    try:
        return int(s, 0) if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", s) else float(s)
    except ValueError:
        return None


def _fmt(v):
    return str(int(v)) if isinstance(v, (int,)) or (isinstance(v, float) and v == int(v)
                                                      and abs(v) < 1e15) else repr(v)


def _collapse_parsers(toks):
    """Replace `atoi ( ... )` by one pseudo-identifier so a comparison against the
    parsed value reads like a comparison against a variable."""
    out, i, n = [], 0, len(toks)
    while i < n:
        t = toks[i]
        if t[0] == "id" and t[1] in _PARSERS and i + 1 < n and toks[i + 1][1] == "(":
            depth, j = 0, i + 1
            while j < n:
                if toks[j][1] == "(":
                    depth += 1
                elif toks[j][1] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            out.append(("id", "$N", t[2]))
            i = j + 1
            continue
        out.append(t)
        i += 1
    return _drop_c_casts(out)


_CAST_WORDS = set(gi._TYPE_WORDS) | {"float", "double", "int", "long", "size_t", "uint32_t", "int32_t",
                                      "uint64_t", "int64_t", "unsigned", "short", "const"}


def _drop_c_casts(toks):
    """Remove `( type )` casts so `(float)x` reads as `x`."""
    out, i, n = [], 0, len(toks)
    while i < n:
        if toks[i][1] == "(":
            j = i + 1
            while j < n and toks[j][0] == "id" and toks[j][1] in _CAST_WORDS:
                j += 1
            if j > i + 1 and j < n and toks[j][1] == ")":
                i = j + 1
                continue
        out.append(toks[i])
        i += 1
    return out


def _site_tokens(text, site, path):
    w = gi.build_window(path, text, site["pos"])
    keep, _si = gi._relevant_spans(w, site["pos"])
    toks = []
    for idx in keep:
        a, b = w.spans[idx]
        toks.extend(w.tokens[a:b])
    return toks


def numeric_evidence(toks):
    """-> dict(eq=[..], lo=[(v, exclusive)], hi=[(v, exclusive)], style=set): the
    constants the parsed value is compared against, clamped to, or switched on."""
    toks = _collapse_parsers(toks)
    n = len(toks)
    names = {"$N"}
    for i, t in enumerate(toks):
        if t[1] == "=" and i > 0 and toks[i - 1][0] == "id":
            for u in toks[i + 1:i + 14]:
                if u[1] == ";":
                    break
                if u[1] == "$N":
                    names.add(toks[i - 1][1])
                    break
    ev = {"eq": [], "lo": [], "hi": [], "style": set()}

    def atom(k):
        return 0 <= k < n and toks[k][0] == "id" and toks[k][1] in names

    def numat(k):
        """-> (value, index after it) for a possibly negated number at k, else None."""
        neg = 1
        if 0 <= k < n and toks[k][1] == "-":
            neg, k = -1, k + 1
        if 0 <= k < n:
            v = _num(toks[k])
            if v is not None:
                return neg * v, k + 1
        return None

    def note(op, v):
        if op in ("==", "!="):
            ev["eq"].append(v)
        elif op == ">=":
            ev["lo"].append((v, False))
        elif op == ">":
            ev["lo"].append((v, True))
        elif op == "<=":
            ev["hi"].append((v, False))
        elif op == "<":
            ev["hi"].append((v, True))

    switched = any(t[1] == "switch" for t in toks)
    for i, t in enumerate(toks):
        if atom(i) and i + 1 < n and toks[i + 1][1] in _CMP_OPS:
            r = numat(i + 2)
            if r:
                note(toks[i + 1][1], r[0])
        if t[1] in _CMP_OPS and atom(i + 1) and i > 0:
            v = _num(toks[i - 1])
            if v is not None:
                note(_FLIP[t[1]], v)
        if switched and t[1] == "case":
            r = numat(i + 1)
            if r:
                ev["eq"].append(r[0])
        if t[0] == "id" and t[1] in _CLAMPS and i + 3 < n and toks[i + 1][1] == "("                 and atom(i + 2) and toks[i + 3][1] == ",":
            lo = numat(i + 4)
            if lo and lo[1] < n and toks[lo[1]][1] == ",":
                hi = numat(lo[1] + 1)
                if hi:
                    ev["lo"].append((lo[0], False))
                    ev["hi"].append((hi[0], False))
                    ev["style"].add("clamp")
        if t[0] == "id" and t[1] in ("min", "max") and i + 3 < n and toks[i + 1][1] == "(":
            j = i + 2
            if atom(j) and toks[j + 1][1] == ",":
                a = numat(j + 2)
                if a:
                    (ev["lo"] if t[1] == "max" else ev["hi"]).append((a[0], False))
                    ev["style"].add("clamp")
    return ev


def orient_bounds(ev):
    """A range written as a REJECTION (`if (v < 0 || v > 2000) return dflt;`) records
    its bounds crossed: the "lower" bound sits above the "upper" one. Swap them and
    flip their exclusivity, so the result describes what is ACCEPTED. -> True if swapped."""
    if ev["lo"] and ev["hi"] and min(v for v, _ in ev["lo"]) > max(v for v, _ in ev["hi"]):
        ev["lo"], ev["hi"] = ([(v, not x) for v, x in ev["hi"]], [(v, not x) for v, x in ev["lo"]])
        ev["style"].add("reject")
        return True
    return False


def token_evidence(toks, gate, with_chars):
    """String (and, for level-style gates, first-character) tokens a reader compares
    the value against."""
    out = []
    n = len(toks)
    for i, t in enumerate(toks):
        if t[0] == "id" and t[1] in _STRCMP and i + 1 < n and toks[i + 1][1] == "(":
            depth, j = 0, i + 1
            while j < n:
                if toks[j][1] == "(":
                    depth += 1
                elif toks[j][1] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                elif toks[j][0] == "str" and depth == 1:
                    out.append(toks[j][1])
                j += 1
        elif t[1] in ("==", "!=") and 0 < i < n - 1:
            for a, b in ((toks[i - 1], toks[i + 1]), (toks[i + 1], toks[i - 1])):
                if a[0] == "str" and b[0] != "str":
                    out.append(a[1])
        if with_chars and t[0] == "chr" and i >= 5 and toks[i - 1][1] in ("==", "!=")                 and toks[i - 2][1] == "]" and toks[i - 3][1] == "0" and toks[i - 4][1] == "[":
            out.append(t[1])
    res = []
    for s in out:
        v = _unquote_tok(s)
        if not v or v == gate or v.startswith("MC2_") or v == "\0":
            continue
        res.append(v)
    return res


def _unquote_tok(s):
    if len(s) >= 2 and s[0] in "\"'" and s[-1] == s[0]:
        return gi._unescape(s[1:-1])
    m = re.match(r'^(?:u8|[LuU])"(.*)"$', s)
    return gi._unescape(m.group(1)) if m else None


# ---------------------------------------------------------------------------
# Harness values
# ---------------------------------------------------------------------------

_NAME_RE = re.compile(r"MC2_[A-Z0-9_]+")
_ASSIGN_RE = re.compile(
    r"""\A(?P<close>["']?\]?["']?)\s*(?P<sep>(?<![=!<>])=(?!=)|:(?!:)|,)\s*
        (?P<val>"(?:[^"\\\n]|\\.)*"|'[^'\n]*'|[^\s"',;)}\]]*)""", re.X)
_BARE_VALUE_RE = re.compile(r"^[A-Za-z0-9_+./\\-]+$")
#: Script languages whose unquoted right-hand side is a literal.
SHELL_EXTS = (".sh", ".bat", ".cmd", ".ps1")
_SCALAR_RE = re.compile(r"^(?:-?[0-9][0-9.]*|true|false|on|off|yes|no)$", re.I)
_READ_DEFAULT_RE =re.compile(r"(?:\.get|getenv|pop|setdefault|environ\.get)\(\s*[\"']?$", re.I)
_PUTENV_RE = re.compile(
    r"""(?:_putenv_s|setenv|SetEnvironmentVariable[AW]?|SDL_setenv)\s*\(\s*"(MC2_[A-Z0-9_]+)"\s*,\s*(?:"((?:[^"\\\n]|\\.)*)"|([A-Za-z0-9_.\-]+))""")
_PUTENV_EQ_RE = re.compile(r"""_putenv\s*\(\s*"(MC2_[A-Z0-9_]+)=((?:[^"\\\n]|\\.)*)\"""")


def config_values(text, bare_ok=True):
    """-> {gate: set of literal values assigned to it in a script or config}.

    `bare_ok` admits an unquoted word as a value (`set MC2_X=on`); pass False for
    languages where that word is a variable name.

    Only assignment-shaped mentions count (`MC2_X=1`, `"MC2_X": "1"`,
    `$env:MC2_X = "1"`, `env["MC2_X"] = "1"`, `("MC2_X", "1")`); a default passed to a
    reader (`environ.get("MC2_X", "1")`) is not an assignment. Values holding
    whitespace or a substitution (`$x`, `%x%`, `{x}`) are descriptions or templates,
    not a value a run reads, and are dropped.
    """
    out = defaultdict(set)
    for m in _NAME_RE.finditer(text):
        if m.start() > 0 and (text[m.start() - 1].isalnum() or text[m.start() - 1] == "_"):
            continue
        head = text[max(0, m.start() - 24):m.start()]
        if _READ_DEFAULT_RE.search(head):
            continue
        am = _ASSIGN_RE.match(text[m.end():m.end() + 160])
        if not am:
            continue
        val = am.group("val")
        quoted = bool(val) and val[0] in "\"'"
        if quoted:
            val = val[1:-1]
        sep = am.group("sep")
        # A comma or colon in prose is not an assignment: only a quoted value (or, for a
        # colon, an obvious scalar) after it is.
        if sep == "," and not quoted:
            continue
        if sep == ":" and not quoted and not _SCALAR_RE.match(val):
            continue
        if re.search(r"\s", val) or re.search(r"[${}%<>*]|MC2_[A-Z]", val) or len(val) > 64:
            continue
        if not quoted:
            # A bare word on the right of a script assignment is a variable, not a value;
            # only shell-family files spell a literal that way.
            if not (bare_ok or _SCALAR_RE.match(val)):
                continue
            if not _BARE_VALUE_RE.match(val) or val[-1] in ".-:":
                continue
        out[m.group(0)].add(val)
    return out


def cpp_putenv_values(text):
    out = defaultdict(set)
    for m in _PUTENV_RE.finditer(text):
        v = m.group(2) if m.group(2) is not None else m.group(3)
        # An unquoted argument is a C++ variable unless it is a number.
        if m.group(2) is None and not _SCALAR_RE.match(v or ""):
            continue
        if v is not None and not re.search(r"\s", v) and len(v) <= 64:
            out[m.group(1)].add(v)
    for m in _PUTENV_EQ_RE.finditer(text):
        v = m.group(2)
        if not re.search(r"\s|%", v) and len(v) <= 64:
            out[m.group(1)].add(v)
    return out


# ---------------------------------------------------------------------------
# Plans (retire verdicts)
# ---------------------------------------------------------------------------

def plan_verdicts(text_a, text_b):
    """-> {gate: "tierA:<verdict>" | "tierB:<verdict>"} for every non-KEEP verdict."""
    out = {}
    for line in (text_b or "").split("\n"):
        m = re.match(r"^\|\s*`(MC2_[A-Z0-9_]+)`\s*\|", line)
        v = re.search(r"\*\*(DELETE|MERGE|KEEP)\*\*", line)
        if m and v and v.group(1) != "KEEP":
            out[m.group(1)] = "tierB:" + v.group(1)
    for line in (text_a or "").split("\n"):
        m = re.match(r"^\|\s*`?(MC2_[A-Z0-9_]+)`?\s*\|", line)
        if not m:
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3:
            continue
        verdict = {"DELETE-SAFE": "DELETE-SAFE", "NRR": "NEEDS-READER-REMOVAL"}.get(cells[2].split()[0] if cells[2] else "")
        if verdict:
            out[m.group(1)] = "tierA:" + verdict
    return out


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

class Inputs:
    """Everything the derivation reads, in memory: C++ sources, the registry header,
    script/config files and the two retirement plans."""

    def __init__(self, cpp=None, registry_text=None, config=None, plan_a="", plan_b=""):
        self.cpp = cpp or {}
        self.registry_text = registry_text
        self.config = config or {}
        self.plan_a = plan_a
        self.plan_b = plan_b


def load_inputs(tree):
    rels = [r for r in tree.list(CPP_DIRS, gi.SCAN_EXTS) if r != REGISTRY_REL]
    cpp = tree.read_all(rels)
    crels = set(tree.list(CONFIG_DIRS, CONFIG_EXTS))
    crels |= set(tree.list(CONFIG_DIRS, CONFIG_ROOT_EXTS, root_level=True))
    crels = sorted(r for r in crels if r not in CONFIG_EXCLUDE
                   and not r.startswith(CONFIG_EXCLUDE_PREFIX)
                   and "/goldens/" not in r and "node_modules" not in r
                   and not r.startswith("scripts/tests/fixtures/"))
    config = {r: t for r, t in tree.read_all(crels).items() if len(t) <= MAX_CONFIG_BYTES}
    texts = tree.read_all([REGISTRY_REL, PLAN_A_REL, PLAN_B_REL])
    return Inputs(cpp, texts.get(REGISTRY_REL), config, texts.get(PLAN_A_REL, ""),
                  texts.get(PLAN_B_REL, ""))


# ---------------------------------------------------------------------------
# Derivation
# ---------------------------------------------------------------------------

_RULE_BASE = [("Std(", "Std"), ("Tri(", "Tri"), ("Custom[", "Custom"), ("Partial[", "Partial")]


def rule_name(cls):
    """A reader idiom class -> the rule name the table uses."""
    for pre, name in _RULE_BASE:
        if cls.startswith(pre):
            return name
    return cls


def _majority(values):
    """-> (winner or None on a tie / no values, Counter)."""
    c = Counter(values)
    if not c:
        return None, c
    top = max(c.values())
    leaders = sorted(k for k, v in c.items() if v == top)
    return (leaders[0] if len(leaders) == 1 else None), c


def _tokens_sorted(vals):
    seen = {}
    for v in vals:
        seen.setdefault(v.lower(), v)
    keys = list(seen)
    try:
        keys.sort(key=lambda k: float(k))
    except ValueError:
        keys.sort()
    return [seen[k] for k in keys]


def _num_default(vec0):
    try:
        f = float(vec0)
    except (TypeError, ValueError):
        return None
    return int(f) if f == int(f) and abs(f) < 1e15 else f


def derive(inp):
    """Inputs -> (records sorted by name, side-info dict). A pure function of `inp`."""
    sources = []
    raw = {}
    for rel in sorted(inp.cpp):
        raw[rel] = inp.cpp[rel]
        sources.append((rel, gi.strip_comments(inp.cpp[rel])))
    text_of = dict(sources)
    sites, occ = scan_sites(sources)
    by_gate = defaultdict(list)
    for s in sites:
        by_gate[s["gate"]].append(s)

    registry = {}
    nullenv_rows = []
    if inp.registry_text:
        for key, e in rp.parse_registry_text(inp.registry_text, REGISTRY_REL).items():
            if e["envVar"] is None:
                nullenv_rows.append(e["featureId"])
            else:
                registry[e["envVar"]] = e

    # Conflicts are gate_conflicts' definition, evaluated over its own scan dirs.
    cdirs = set(gc.SCAN_DIRS)
    conflicts = set()
    for g, ss in by_gate.items():
        scoped = [s for s in ss if s["file"].split("/", 1)[0] in cdirs]
        if gc.analyse_gate(scoped)["disagreements"]:
            conflicts.add(g)

    harness = defaultdict(set)
    for rel in sorted(inp.config):
        for g, vs in config_values(inp.config[rel], rel.endswith(SHELL_EXTS)).items():
            harness[g] |= vs
    for rel in sorted(raw):
        if raw[rel].count("MC2_") and ("putenv" in raw[rel] or "setenv" in raw[rel]
                                       or "SetEnvironmentVariable" in raw[rel]):
            for g, vs in cpp_putenv_values(raw[rel]).items():
                harness[g] |= vs

    retire = plan_verdicts(inp.plan_a, inp.plan_b)

    names = sorted(set(occ) | set(registry))
    records = []
    for name in names:
        records.append(_record(name, by_gate.get(name, []), occ.get(name, []), registry.get(name),
                               name in conflicts, sorted(harness.get(name, ())),
                               retire.get(name, ""), text_of, raw))
    side = {"scriptOnlyNames": sorted(set(harness) - set(names)),
            "nullEnvRegistryRows": sorted(nullenv_rows),
            "registryOnly": sorted(set(registry) - set(occ)),
            "sites": len(sites)}
    return records, side


def _record(name, sites, occurrences, reg, conflict, harness_values, retire_verdict, text_of, raw):
    readers_sorted = sorted(sites, key=lambda s: (s["file"], s["line"]))
    rsites = [s for s in readers_sorted if s["status"] == "reader" and s["vector"]]
    tsites = [s for s in readers_sorted if s["status"] == "token" and s["vector"]]
    bool_sites = [s for s in rsites if s["family"] == "bool"]
    level_sites = [s for s in bool_sites if s["cls"].startswith(("Custom", "Partial"))]
    num_sites = [s for s in rsites if s["family"] == "num"]
    str_sites = [s for s in rsites if s["family"] == "str"]
    tri = any(s["cls"].startswith("Tri(") for s in bool_sites)

    # --- evidence about the value space ---
    enum_vals, ev_nums = [], {"eq": [], "lo": [], "hi": [], "style": set()}
    for s in tsites + [x for x in rsites if x["family"] == "num" or not x["cls"].startswith(_STD_BOOL_CLASSES)]:
        toks = _site_tokens(text_of[s["file"]], s, s["file"])
        enum_vals += token_evidence(toks, name, with_chars=s["status"] == "token"
                                    or s["cls"].startswith(("Custom", "Partial")))
        if s["family"] == "num" or s["parsers"]:
            ev = numeric_evidence(toks)
            orient_bounds(ev)
            for k in ("eq", "lo", "hi"):
                ev_nums[k] += ev[k]
            ev_nums["style"] |= ev["style"]

    # A comparison against 0/1/on/off alone is a boolean's spelling, not a token set.
    real_tokens = [v for v in enum_vals if v.lower() not in _BOOLISH]

    # --- kind ---
    if tri:
        kind = "Tri"
    elif num_sites:
        parsers = {p for s in num_sites for p in s["parsers"]}
        kind = "Float" if parsers & _FLOAT_PARSERS else "Int"
    elif str_sites:
        kind = "String"
    elif bool_sites:
        kind = "Enum" if len(level_sites) == len(bool_sites) and real_tokens else "Bool"
    elif tsites:
        kind = "Enum" if real_tokens else "Bool"
    else:
        kind = "Bool"

    # --- rule ---
    if kind == "Tri":
        rule = "Tri"
    elif kind in ("Int", "Float"):
        rule = "Prefix"
    elif kind == "Bool" and bool_sites:
        counts = Counter(rule_name(s["cls"]) for s in bool_sites)
        top = max(counts.values())
        rule = sorted(r for r, c in counts.items() if c == top)[0]
    else:
        rule = "Std"

    # --- default ---
    reader_default, disagreement = None, ""
    if kind in ("Bool", "Tri") and (bool_sites or tsites):
        pool = [s for s in bool_sites if s not in level_sites] or ([] if bool_sites else tsites)
        known = [s["vector"][0] for s in pool if s["vector"][0] in ("on", "off")]
        winner, c = _majority(known)
        if len(c) > 1:
            disagreement = "readers disagree on unset: " + ", ".join("%s x%d" % (k, c[k]) for k in sorted(c))
        reader_default = None if winner is None else (winner == "on")
    elif kind in ("Int", "Float"):
        vals = [_num_default(s["vector"][0]) for s in num_sites]
        winner, c = _majority([v for v in vals if v is not None])
        if len(c) > 1:
            disagreement = "readers disagree on default: " + ", ".join("%s x%d" % (_fmt(k), c[k]) for k in sorted(c))
        reader_default = winner
    elif kind == "String":
        outs = [s["vector"][0] for s in str_sites]
        reader_default = "" if outs and all(o == "null" for o in outs) else None
    elif kind == "Enum":
        # Unset matching no token is the default; any other unset outcome is a value
        # the interpreter cannot name.
        evid = tsites + level_sites
        reader_default = "" if evid and all(s["vector"][0] in ("off", "null") for s in evid) else None

    reg_default = None if reg is None else reg["defaultOn"]
    if kind in ("Bool", "Tri") and reg_default is not None and reader_default is not None \
            and reg_default != reader_default:
        extra = "registry defaultOn=%s but readers say unset=%s" % (
            str(reg_default).lower(), "on" if reader_default else "off")
        disagreement = (disagreement + "; " + extra) if disagreement else extra

    # Readers that all agree on the unset case are what the engine does today, so they
    # outrank a registry row that says otherwise (the row is recorded, and flagged).
    unanimous = kind == "Bool" and reader_default is not None and not disagreement.startswith("readers disagree")
    if kind == "Tri":
        default, src = "inherit", "reader-idiom"
    elif kind == "Bool" and reg_default is not None and not (unanimous and reg_default != reader_default):
        default, src = reg_default, "registry"
    elif reader_default is not None:
        default, src = reader_default, "reader-idiom"
    else:
        default, src = None, "unknown"
    if kind == "Float" and isinstance(default, int) and not isinstance(default, bool):
        default = float(default)

    # --- range ---
    rng = None
    if kind in ("Int", "Float"):
        r = {}
        if ev_nums["lo"]:
            lo = min(ev_nums["lo"], key=lambda x: x[0])
            r["lo"] = lo[0]
            if lo[1]:
                r["loExclusive"] = True
        if ev_nums["hi"]:
            hi = max(ev_nums["hi"], key=lambda x: x[0])
            r["hi"] = hi[0]
            if hi[1]:
                r["hiExclusive"] = True
        eq = sorted(set(ev_nums["eq"]))
        if eq:
            r["enum"] = [_fmt(v) for v in eq]
        if r:
            r["style"] = ("clamp" if "clamp" in ev_nums["style"] else
                          "compare-reject" if "reject" in ev_nums["style"] else "compare")
            rng = r
    elif kind == "Enum" and real_tokens:
        rng = {"enum": _tokens_sorted(enum_vals)}

    # --- doc ---
    doc = ""
    if reg is not None and reg["doc"].strip():
        doc = first_sentence(reg["doc"])
    elif occurrences:
        f, ln = sorted(occurrences)[0]
        doc = comment_doc(raw.get(f, ""), ln)

    files = sorted({f for f, _ in occurrences})
    readers = []
    for s in readers_sorted:
        if s["status"] in ("reader", "token"):
            idiom = s["cls"] + ("(inv)" if s["inverted"] else "")
        elif s["status"] == "nonreader":
            idiom = "NonReader"
        else:
            idiom = "Unclassified"
        entry = {"at": "%s:%d" % (s["file"], s["line"]), "idiom": idiom}
        if entry not in readers:    # two reads on one line are one place to look
            readers.append(entry)

    rec = {
        "name": name,
        "kind": kind,
        "default": default,
        "rule": rule,
        "owner": gate_owner(name, files),
        "retireBy": "",
        "doc": doc,
        "readers": readers,
        "conflict": bool(conflict),
        "registered": reg is not None,
        "registryDefault": reg_default,
        "seedDefaultSource": src,
        "harnessValues": list(harness_values),
        "numericRange": rng,
        "readerDefault": reader_default,
        "defaultDisagreement": disagreement,
        "registryKind": "" if reg is None else reg["kind"],
        "featureId": "" if reg is None else reg["featureId"],
        "retireVerdict": retire_verdict,
    }
    return rec


# ---------------------------------------------------------------------------
# Serialisation, checks, history
# ---------------------------------------------------------------------------

def seed_document(records):
    return {"version": SEED_VERSION,
            "note": "Seed for the typed gate table (design S2, G1a). Derived by "
                    "`py -3 tools/gate_gen.py --import-legacy`; do not edit by hand.",
            "count": len(records),
            "gates": sorted(records, key=lambda r: r["name"])}


def dumps_seed(records):
    return json.dumps(seed_document(records), indent=1, ensure_ascii=True) + "\n"


def write_lf(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def diff_seed(committed, derived):
    """-> list of human-readable drift lines between two seed documents."""
    out = []
    a = {g["name"]: g for g in committed.get("gates", [])}
    b = {g["name"]: g for g in derived.get("gates", [])}
    for n in sorted(set(b) - set(a)):
        out.append("new gate not in the seed: " + n)
    for n in sorted(set(a) - set(b)):
        out.append("seed gate no longer found in the tree: " + n)
    for n in sorted(set(a) & set(b)):
        if a[n] != b[n]:
            fields = sorted(k for k in set(a[n]) | set(b[n]) if a[n].get(k) != b[n].get(k))
            out.append("%s drifted in: %s" % (n, ", ".join(fields)))
    if committed.get("version") != derived.get("version"):
        out.append("seed version %r != %r" % (committed.get("version"), derived.get("version")))
    return out


def default_disagreements(records):
    """Records whose registry default and reader-idiom default differ but that say
    so neither through `conflict` nor through `defaultDisagreement`."""
    bad = []
    for r in records:
        if r["kind"] in ("Bool", "Tri") and r["registryDefault"] is not None \
                and r["readerDefault"] is not None and r["registryDefault"] != r["readerDefault"]:
            if not r["conflict"] and not r["defaultDisagreement"]:
                bad.append(r["name"])
    return bad


def render_history(inp):
    """docs/gate-history.md: every registered gate whose registry text runs past one
    sentence, verbatim.

    The text is QUOTED HISTORY: it says "Default-off" for gates that have since flipped.
    The banner/marker (and the per-entry note) tell readers and the default-agreement
    checker (which skips marker-carrying docs) that those words are not current claims.
    """
    rows = []
    if inp.registry_text:
        for _key, e in rp.parse_registry_text(inp.registry_text, REGISTRY_REL).items():
            if e["envVar"] is None:
                continue
            if len(split_sentences(" ".join(e["doc"].split()))) > 1:
                rows.append((e["envVar"], e["doc"]))
    rows.sort()
    L = ["# Gate history\n",
         gi.QUOTED_HISTORY_MARKER + "\n",
         "> Historical registry text. Defaults and Default-on/off claims below are "
         "NOT current; the live default of each gate is in `tools/gate_table.seed.json` "
         "(and the code). The default-agreement checker skips this file.\n",
         "Registry prose for every registered gate whose description runs past one sentence, "
         "copied verbatim from `RenderCore/RendererFeatureRegistry.h`. The typed gate table keeps "
         "a one-line `doc` per row; the rest of the story lives here, one section per gate, so a "
         "row's `// history:` pointer lands on its full text. Generated by "
         "`py -3 tools/gate_gen.py --gate-history`; do not edit by hand.\n",
         "%d gates.\n" % len(rows)]
    for name, text in rows:
        L.append("## %s\n" % name)
        L.append("> historical registry text; defaults here are NOT current "
                 "(see `tools/gate_table.seed.json`)\n")
        L.append(text + "\n")
    return "\n".join(L)


def stats_text(records, side):
    L = []
    w = L.append
    w("gate records            : %d" % len(records))
    w("  literal-named         : %d" % (len(records) - len(side["registryOnly"])))
    w("  registry-only (no literal in scanned C++): %d" % len(side["registryOnly"]))
    w("kind histogram")
    for k, v in sorted(Counter(r["kind"] for r in records).items(), key=lambda kv: (-kv[1], kv[0])):
        w("  %-8s %5d" % (k, v))
    w("rule histogram")
    for k, v in sorted(Counter(r["rule"] for r in records).items(), key=lambda kv: (-kv[1], kv[0])):
        w("  %-12s %5d" % (k, v))
    w("owner histogram")
    for k, v in sorted(Counter(r["owner"] for r in records).items(), key=lambda kv: (-kv[1], kv[0])):
        w("  %-13s %5d" % (k, v))
    reg_doc = sum(1 for r in records if r["registered"] and r["doc"])
    com_doc = sum(1 for r in records if not r["registered"] and r["doc"])
    none_doc = sum(1 for r in records if not r["doc"])
    w("doc provenance          : registry %d, comment %d, none %d" % (reg_doc, com_doc, none_doc))
    w("registered              : %d" % sum(1 for r in records if r["registered"]))
    w("conflict                : %d" % sum(1 for r in records if r["conflict"]))
    w("defaultDisagreement     : %d" % sum(1 for r in records if r["defaultDisagreement"]))
    w("have harnessValues      : %d" % sum(1 for r in records if r["harnessValues"]))
    w("have numericRange       : %d" % sum(1 for r in records if r["numericRange"]))
    w("retire verdicts         : %d" % sum(1 for r in records if r["retireVerdict"]))
    w("seedDefaultSource       : " + ", ".join(
        "%s %d" % (k, v) for k, v in sorted(Counter(r["seedDefaultSource"] for r in records).items())))
    w("script-only names (assigned by a script/config, no C++ literal, not in seed): %d"
      % len(side["scriptOnlyNames"]))
    for n in side["scriptOnlyNames"]:
        w("  script-only: " + n)
    unk = [r["name"] for r in records if r["seedDefaultSource"] == "unknown"]
    w("seedDefaultSource=unknown (%d):" % len(unk))
    for n in unk:
        r = next(x for x in records if x["name"] == n)
        w("  %-48s %-6s %s" % (n, r["kind"], r["rule"]))
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def open_tree(root, worktree, rev):
    if worktree:
        return gc.Tree(root, None)
    t = gc.Tree(root, rev)
    try:
        t._git(["rev-parse", "--verify", rev])
    except (subprocess.CalledProcessError, OSError):
        return gc.Tree(root, None)
    return t


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--import-legacy", action="store_true", help="write " + SEED_REL)
    ap.add_argument("--check-seed", action="store_true", help="diff the committed seed against a fresh derivation")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--gate-history", action="store_true", help="write " + HISTORY_REL)
    ap.add_argument("--check-history", action="store_true")
    ap.add_argument("--worktree", action="store_true", help="read the working tree instead of a git revision")
    ap.add_argument("--rev", default="HEAD")
    args = ap.parse_args(argv)
    if not (args.import_legacy or args.check_seed or args.stats or args.gate_history or args.check_history):
        ap.print_help()
        return 2

    tree = open_tree(args.root, args.worktree, args.rev)
    inp = load_inputs(tree)
    rc = 0
    records = side = None
    if args.import_legacy or args.check_seed or args.stats:
        records, side = derive(inp)

    if args.import_legacy:
        write_lf(os.path.join(args.root, SEED_REL), dumps_seed(records))
        print("gate_gen: wrote %s (%d gates, %s)" % (SEED_REL, len(records), tree.describe()))

    if args.check_seed:
        path = os.path.join(args.root, SEED_REL)
        if not os.path.isfile(path):
            print("GATE-SEED check FAILED: %s is missing; run `py -3 tools/gate_gen.py --import-legacy`" % SEED_REL)
            return 1
        with open(path, "r", encoding="utf-8") as fh:
            committed = json.load(fh)
        drift = diff_seed(committed, json.loads(dumps_seed(records)))
        bad = default_disagreements(committed.get("gates", []))
        if drift or bad:
            print("GATE-SEED check FAILED (%s)" % tree.describe())
            for d in drift[:40]:
                print("  - " + d)
            if len(drift) > 40:
                print("  ... %d more" % (len(drift) - 40))
            for n in bad:
                print("  - %s: registry and reader defaults differ with no conflict/defaultDisagreement note" % n)
            print("  Re-run `py -3 tools/gate_gen.py --import-legacy` and commit the seed if the change is intended.")
            return 1
        print("gate_gen: seed matches the tree (%d gates, %s)" % (len(records), tree.describe()))

    if args.stats:
        sys.stdout.write(stats_text(records, side))

    if args.gate_history:
        text = render_history(inp)
        write_lf(os.path.join(args.root, HISTORY_REL), text)
        print("gate_gen: wrote %s (%d bytes)" % (HISTORY_REL, len(text.encode("utf-8"))))

    if args.check_history:
        path = os.path.join(args.root, HISTORY_REL)
        want = render_history(inp)
        have = open(path, "r", encoding="utf-8", newline="").read() if os.path.isfile(path) else None
        if have != want:
            print("GATE-HISTORY check FAILED: %s is %s; run `py -3 tools/gate_gen.py --gate-history`"
                  % (HISTORY_REL, "missing" if have is None else "stale"))
            return 1
        print("gate_gen: %s is current" % HISTORY_REL)
    return rc


if __name__ == "__main__":
    sys.exit(main())
