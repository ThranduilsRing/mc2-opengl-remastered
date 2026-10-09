#!/usr/bin/env python3
"""axis_swizzle_ratchet - monotonic counter for hand-written MC2<->render-space axis swaps.

MC2/Stuff space and render (GL/VK) space differ by the permutation (x, y, z) -> (-x, z, y).
It is its own inverse, so one spelling serves both directions. Today every shader and C++
caller re-types it; a wrong sign or a missed twin is a recurring bug class (the SUN_AXIS_FIX
saga). dup_lines_ratchet cannot see these because each site is a single line. This tool
counts them at token level and lets the count only go DOWN, so the D11 migration to one
typed helper (GLSL include + C++ inline) can proceed site by site without regressions.

  py -3 tools/axis_swizzle_ratchet.py                      # report (shapes, per-dir, per-file)
  py -3 tools/axis_swizzle_ratchet.py --write-doc          # regenerate docs/axis-conversion-sites.md
  py -3 tools/axis_swizzle_ratchet.py --write-baseline     # (re)write axis_swizzle_ratchet.baseline.json
  py -3 tools/axis_swizzle_ratchet.py --check              # exit 1 if the hand-written count rose
  py -3 tools/axis_swizzle_ratchet.py --check --tighten    # on success, lower the baseline to current

Shapes recognised (comments and string literals are blanked first, so prose never counts):
  triplet-ctor    vec3(-a.x, a.z, a.y)  Stuff::Point3D(-p.x, p.z, p.y)  v3(-v.x, v.z, v.y)
  triplet-brace   { -a.x, a.z, a.y }
  triplet-arglist f(..., -a.x, a.z, a.y, ...)
  decl-chain      float lx = -d.x, ly = d.z, lz = d.y;
  swizzle-zy      vec3(-a.x, a.zy)
  swizzle-xzy     a.xzy * vec3(-1, 1, 1)
  assign-triple   o.x = -i.x;  o.y = i.z;  o.z = i.y;   (three adjacent statements, any order,
                                                          member or [index] access)
  assign-scalars  X = -v.x;  Y = v.z;  Z = v.y;          (three adjacent statements into plain locals)
Only a swap whose three source components share ONE base expression counts; vec3(a.x, a.y, a.z)
and vec3(-a.x, a.y, a.z) are not the conversion.

Not counted as hand-written: definitions of an already-centralized helper (HELPER_NAMES), files
in HELPER_HOMES, and lines carrying the waiver `axis-swizzle: allow`. Helper CALLS are counted
separately so the report distinguishes copies from centralized use.

Source of truth: the COMMITTED tree (--rev, default HEAD), so other agents' uncommitted work
never flaps the gate. --staged scans the index, --worktree the dirty tree.

Exit: 0 ok, 1 the hand-written count rose, 2 usage/baseline error.  Stdlib only.
"""
from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.abspath(os.path.join(HERE, ".."))
BASELINE = os.path.join(HERE, "axis_swizzle_ratchet.baseline.json")
DOC_REL = "docs/axis-conversion-sites.md"
PORT_DIFF_REL = "tools/glsl_port_diff.py"

EXTS = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".inl",
        ".frag", ".vert", ".comp", ".tesc", ".tese", ".geom", ".mesh", ".task",
        ".glsl", ".hglsl")
EXCLUDE_PARTS = ("3rdparty/", "graphify-out/", "/__pycache__/", "/Tracy/", "shaders/spv/")
SHADER_EXTS = (".frag", ".vert", ".comp", ".tesc", ".tese", ".geom", ".mesh", ".task", ".glsl", ".hglsl")

# Names of already-centralized swap helpers. Calls to these are the migration target; a
# definition is the one allowed spelling of the permutation. Includes the names the D11
# builder slice is expected to introduce so the first helper lands without a baseline bump.
HELPER_NAMES = ("mc2ToRender", "renderToMc2", "toRenderSpace", "fromRenderSpace", "axisFix",
                "swapStuffToMC2", "stuffToMc2", "stuffToGL", "mc2ToGL", "glToMc2",
                "stuff_to_mc2", "mc2_to_render", "render_to_mc2")
# Matrix form of the same permutation: informational reference count only.
MATRIX_NAMES = ("kAxisSwapMC2toGL", "makeAxisSwapMC2toGL")
# Files that host the canonical helper definitions (sites inside them are not copies).
HELPER_HOMES = ("shaders/include/axis.hglsl", "RenderCore/axis_convert.h")
WAIVER = "axis-swizzle: allow"

SHAPES = ("triplet-ctor", "triplet-brace", "triplet-arglist", "decl-chain",
          "swizzle-zy", "swizzle-xzy", "assign-triple", "assign-scalars")

# ---------------------------------------------------------------- lexing helpers
_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def _blank(m):
    return re.sub(r"[^\n]", " ", m.group(0))


def strip_comments_strings(text):
    """Blank comments/strings with spaces, keeping every newline so line numbers survive."""
    return _TOKEN.sub(_blank, text)


# A base expression: identifier followed by .member / ->member / [index] steps.
_BASE = r"[A-Za-z_][\w:]*(?:\s*(?:\.\s*[A-Za-z_]\w*|->\s*[A-Za-z_]\w*|\[[^\[\]\n]*\]))*"


def _comp(k):
    """Component k (0=x,1=y,2=z) access suffix: .x  ->x  or [k]."""
    m = "xyz"[k]
    return r"(?:\.\s*%s\b|->\s*%s\b|\[\s*%d\s*\])" % (m, m, k)


_ONE = r"1(?:\.0*)?f?"
_TRIPLET = re.compile(r"-\s*(?P<b>%s)\s*%s\s*,\s*(?P=b)\s*%s\s*,\s*(?P=b)\s*%s"
                      % (_BASE, _comp(0), _comp(2), _comp(1)))
_DECL = re.compile(r"-\s*(?P<b>%s)\s*%s\s*,\s*[A-Za-z_]\w*\s*=\s*(?P=b)\s*%s\s*,\s*[A-Za-z_]\w*\s*=\s*(?P=b)\s*%s"
                   % (_BASE, _comp(0), _comp(2), _comp(1)))
_SWIZ_ZY = re.compile(r"-\s*(?P<b>%s)\s*%s\s*,\s*(?P=b)\s*\.\s*zy\b" % (_BASE, _comp(0)))
_SWIZ_XZY = re.compile(
    (r"\.\s*xzy\b[^;]*?\*\s*vec3\s*\(\s*-\s*{1}\s*,\s*{1}\s*,\s*{1}\s*\)"
     r"|vec3\s*\(\s*-\s*{1}\s*,\s*{1}\s*,\s*{1}\s*\)\s*\*[^;]*?\.\s*xzy\b").replace("{1}", _ONE))

# One simple assignment `lhs.c = [-]rhs.c;` -- the unit of the three-statement shape.
_ACC = r"(?:\.\s*(?P<{t}n>[xyz])\b|->\s*(?P<{t}m>[xyz])\b|\[\s*(?P<{t}i>[012])\s*\])"
_ASSIGN = re.compile(
    r"(?P<lb>%s)\s*%s\s*=\s*(?P<neg>-?)\s*(?P<rb>%s)\s*%s\s*;"
    % (_BASE, _ACC.format(t="l"), _BASE, _ACC.format(t="r")))


# Scalar form: `[type] name = [-]rhs.c;` -- locals receiving the swapped components.
_ASSIGN_S = re.compile(
    r"(?:[A-Za-z_][\w:]*\s+)?(?P<ln>[A-Za-z_]\w*)\s*=\s*(?P<neg>-?)\s*(?P<rb>%s)\s*%s\s*;"
    % (_BASE, _ACC.format(t="r")))


def _adjacent(clean, trio):
    """True when only whitespace/braces separate the statements."""
    return not any(clean[trio[k].end():trio[k + 1].start()].strip(" \t\r\n{}") for k in (0, 1))


def _cidx(m, t):
    c = m.group(t + "n") or m.group(t + "m")
    return "xyz".index(c) if c else int(m.group(t + "i"))


class Site:
    __slots__ = ("path", "line", "shape", "text")

    def __init__(self, path, line, shape, text):
        self.path, self.line, self.shape, self.text = path, line, shape, text

    def key(self):
        return (self.path, self.line, self.shape)


def _norm(s):
    return re.sub(r"\s+", " ", s).strip()


def _prefix_shape(clean, start):
    """Classify a triplet by what precedes the leading minus."""
    i = start - 1
    while i >= 0 and clean[i] in " \t\r\n":
        i -= 1
    c = clean[i] if i >= 0 else ""
    return "triplet-ctor" if c == "(" else "triplet-brace" if c == "{" else "triplet-arglist"


def _helper_def_re():
    names = "|".join(re.escape(n) for n in HELPER_NAMES)
    # <return type> NAME(params) [const] { | end-of-line     (a definition, not a call)
    return re.compile(r"(?<![\w.>])(?!return\b)[\w:<>]+[\s&*]+(?:%s)\s*\([^;()]*\)\s*(?:const\s*)?(?:\{|$)" % names)


_HELPER_DEF = _helper_def_re()
_HELPER_CALL = re.compile(r"(?<![\w.])(?:%s)\s*\(" % "|".join(re.escape(n) for n in HELPER_NAMES))
_MATRIX_REF = re.compile(r"\b(?:%s)\b" % "|".join(MATRIX_NAMES))
_PORT_MARK = re.compile(r"//\s*PORT-(BEGIN|END):\s*(\w+)")


def scan_text(path, text):
    """-> dict(sites, helper_defs, helper_calls, matrix_refs, waived) for one file."""
    raw_lines = text.split("\n")
    clean = strip_comments_strings(text)
    nl = [i for i, c in enumerate(clean) if c == "\n"]
    line_of = lambda off: bisect.bisect_left(nl, off) + 1
    found = {}

    def add(start, end, shape):
        ln = line_of(start)
        found.setdefault((ln, shape), _norm(clean[start:end]))

    for rx in (_DECL, _SWIZ_ZY, _SWIZ_XZY):
        for m in rx.finditer(clean):
            add(m.start(), m.end(), "decl-chain" if rx is _DECL else "swizzle-zy" if rx is _SWIZ_ZY else "swizzle-xzy")
    decl_lines = {ln for (ln, sh) in found if sh == "decl-chain"}
    for m in _TRIPLET.finditer(clean):
        if line_of(m.start()) in decl_lines:
            continue
        add(m.start(), m.end(), _prefix_shape(clean, m.start()))

    # three adjacent assignments forming the swap, any order
    asg = list(_ASSIGN.finditer(clean))
    for i in range(len(asg) - 2):
        trio = asg[i:i + 3]
        if not _adjacent(clean, trio):
            continue
        lb = {_norm(t.group("lb")) for t in trio}
        rb = {_norm(t.group("rb")) for t in trio}
        if len(lb) != 1 or len(rb) != 1 or lb == rb:
            continue
        got = {(_cidx(t, "l"), _cidx(t, "r"), bool(t.group("neg"))) for t in trio}
        if got == {(0, 0, True), (1, 2, False), (2, 1, False)}:
            add(trio[0].start(), trio[2].end(), "assign-triple")

    # same three statements into three distinct plain locals (X=-v.x; Y=v.z; Z=v.y;)
    asg_s = list(_ASSIGN_S.finditer(clean))
    for i in range(len(asg_s) - 2):
        trio = asg_s[i:i + 3]
        if not _adjacent(clean, trio):
            continue
        if len({t.group("ln") for t in trio}) != 3 or len({_norm(t.group("rb")) for t in trio}) != 1:
            continue
        if [(_cidx(t, "r"), bool(t.group("neg"))) for t in trio] == [(0, True), (2, False), (1, False)]:
            add(trio[0].start(), trio[2].end(), "assign-scalars")

    helper_defs = helper_calls = matrix_refs = 0
    for ln, l in enumerate(clean.split("\n"), 1):
        if _MATRIX_REF.search(l):
            matrix_refs += len(_MATRIX_REF.findall(l))
        if _HELPER_DEF.search(l):
            helper_defs += 1
        else:
            helper_calls += len(_HELPER_CALL.findall(l))

    # A site is exempt when it sits in a helper's own definition or carries the waiver.
    sites, waived, defs_sites = [], [], []
    home = path in HELPER_HOMES
    for (ln, shape), snip in sorted(found.items()):
        near = "\n".join(clean.split("\n")[max(0, ln - 3):ln])
        s = Site(path, ln, shape, snip)
        w = any(WAIVER in raw_lines[k] for k in (ln - 1, ln - 2) if 0 <= k < len(raw_lines))
        if home or any(_HELPER_DEF.search(x) for x in near.split("\n")):
            defs_sites.append(s)
        elif w:
            waived.append(s)
        else:
            sites.append(s)

    open_, spans = {}, []
    for ln, l in enumerate(raw_lines, 1):
        m = _PORT_MARK.search(l)
        if m:
            if m.group(1) == "BEGIN":
                open_[m.group(2)] = ln
            elif m.group(2) in open_:
                spans.append((open_.pop(m.group(2)), ln, m.group(2)))
    return {"sites": sites, "waived": waived, "helper_def_sites": defs_sites,
            "helper_defs": helper_defs, "helper_calls": helper_calls,
            "matrix_refs": matrix_refs, "port_spans": spans}


# ---------------------------------------------------------------- aggregate
class Result:
    def __init__(self):
        self.sites = []
        self.waived = []
        self.helper_calls = 0
        self.helper_defs = 0
        self.matrix_refs = 0
        self.per_file = Counter()
        self.per_dir = Counter()
        self.per_shape = Counter()
        self.helper_calls_by_file = Counter()
        self.port_spans = defaultdict(list)
        self.files_scanned = 0

    @property
    def total(self):
        return len(self.sites)


def dir_of(path):
    parts = path.split("/")
    if parts[0] == "shaders":
        return "/".join(parts[:-1])
    return parts[0]


def scan(files):
    """files: {rel: text} -> Result."""
    res = Result()
    for rel in sorted(files):
        r = scan_text(rel, files[rel])
        res.files_scanned += 1
        res.sites += r["sites"]
        res.waived += r["waived"]
        res.helper_calls += r["helper_calls"]
        res.helper_defs += r["helper_defs"]
        res.matrix_refs += r["matrix_refs"]
        if r["helper_calls"]:
            res.helper_calls_by_file[rel] = r["helper_calls"]
        if r["port_spans"]:
            res.port_spans[rel] = r["port_spans"]
        for s in r["sites"]:
            res.per_file[rel] += 1
            res.per_dir[dir_of(rel)] += 1
            res.per_shape[s.shape] += 1
    return res


# ---------------------------------------------------------------- git / fs sources
def _git(root, args, inp=None):
    return subprocess.run(["git"] + args, cwd=root, input=inp, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True).stdout


def _git_try(root, args):
    try:
        return _git(root, args)
    except subprocess.CalledProcessError:
        return None


def wanted(rel):
    return rel.endswith(EXTS) and not any(p in "/" + rel for p in EXCLUDE_PARTS) \
        and not rel.endswith((".gen.h", ".gen.hglsl"))


def load_sources(root, source, rev):
    """-> {rel: text}. source: head | staged | worktree."""
    if source == "worktree":
        names = _git(root, ["ls-files", "-z", "--cached", "--others", "--exclude-standard"]).split(b"\0")
    elif source == "staged":
        names = _git(root, ["ls-files", "-z"]).split(b"\0")
    else:
        names = _git(root, ["ls-tree", "-r", "-z", "--name-only", rev]).split(b"\0")
    rels = sorted(x.decode("utf-8", "replace") for x in names if x)
    rels = [r for r in rels if wanted(r)]
    out = {}
    if source == "worktree":
        for r in rels:
            try:
                with open(os.path.join(root, r), "rb") as fh:
                    out[r] = fh.read().decode("utf-8", "replace")
            except OSError:
                pass
        return out
    spec = (lambda r: ":" + r) if source == "staged" else (lambda r: rev + ":" + r)
    raw = _git(root, ["cat-file", "--batch"], "".join(spec(r) + "\n" for r in rels).encode("utf-8"))
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


def head_sha(root, rev):
    raw = _git_try(root, ["rev-parse", "--short=9", rev])
    return raw.decode().strip() if raw else "unknown"


def blob_text(root, source, rev, rel):
    if source == "worktree":
        try:
            with open(os.path.join(root, rel), "r", encoding="utf-8", errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""
    raw = _git_try(root, ["show", (":" if source == "staged" else rev + ":") + rel])
    return raw.decode("utf-8", "replace") if raw else ""


# ---------------------------------------------------------------- GL/VK twin map
def registry_pairs(port_diff_text):
    """(gl, vk) shader pairs registered with glsl_port_diff.py (read-only parse)."""
    return sorted(set(re.findall(r'Port\(\s*"[^"]+"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"', port_diff_text)))


def twins_for(path, all_paths, pairs):
    """Counterpart shader files of `path` as [(file, why)], sorted."""
    if not path.endswith(SHADER_EXTS) or not path.startswith("shaders/"):
        return []
    out = {}
    stem, ext = os.path.splitext(os.path.basename(path))
    if path.startswith("shaders/vulkan/"):
        rel = path[len("shaders/vulkan/"):]
        cand = ["shaders/" + rel]
        if stem.endswith("_tier1"):
            cand.append("shaders/" + rel[:-len(stem + ext)] + stem[:-len("_tier1")] + ext)
    else:
        rel = path[len("shaders/"):]
        cand = ["shaders/vulkan/" + rel, "shaders/vulkan/" + rel[:-len(ext)] + "_tier1" + ext]
    for c in cand:
        if c in all_paths and c != path:
            out[c] = "same name" if os.path.basename(c) == os.path.basename(path) else "_tier1 name"
    for gl, vk in pairs:
        other = vk if path == gl else gl if path == vk else None
        if other and other in all_paths:
            out.setdefault(other, "glsl_port_diff registry")
    return sorted(out.items())


# ---------------------------------------------------------------- baseline / check
def make_baseline(res, root, rev):
    return {"version": 1, "head": head_sha(root, rev), "total": res.total,
            "dirs": {d: res.per_dir[d] for d in sorted(res.per_dir)},
            "files": {k: v for k, v in sorted(res.per_file.items())},
            "shapes": {k: res.per_shape.get(k, 0) for k in SHAPES},
            "helper_calls": res.helper_calls}


def write_json(path, obj):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(obj, fh, indent=1, sort_keys=True)
        fh.write("\n")


def check(res, base, out=None):
    """Fail when the hand-written total rose. Per-dir/per-file rises with a flat total only warn."""
    out = out or sys.stdout
    if res.total > base["total"]:
        print("axis_swizzle_ratchet: FAIL hand-written axis swaps rose %d -> %d" % (base["total"], res.total), file=out)
        for f in sorted(set(base.get("files", {})) | set(res.per_file)):
            b, c = base.get("files", {}).get(f, 0), res.per_file.get(f, 0)
            if c > b:
                print("  %s: %d -> %d" % (f, b, c), file=out)
                for s in res.sites:
                    if s.path == f:
                        print("    %s:%d [%s] %s" % (s.path, s.line, s.shape, s.text[:100]), file=out)
        print("  Use the central helper (D11) instead of re-typing (x,y,z)->(-x,z,y); "
              "see docs/axis-conversion-sites.md.", file=out)
        return 1
    for d in sorted(set(base.get("dirs", {})) | set(res.per_dir)):
        b, c = base.get("dirs", {}).get(d, 0), res.per_dir.get(d, 0)
        if c > b:
            print("axis_swizzle_ratchet: warn: %s rose %d -> %d (total did not; code moved?)" % (d, b, c), file=out)
    print("axis_swizzle_ratchet: ok: %d hand-written (baseline %d), %d helper calls"
          % (res.total, base["total"], res.helper_calls), file=out)
    return 0


# ---------------------------------------------------------------- reports
def render_report(res, top_files=40):
    lines = ["hand-written axis swaps: %d   waived: %d   helper calls: %d   helper defs: %d   matrix refs: %d   (%d files scanned)"
             % (res.total, len(res.waived), res.helper_calls, res.helper_defs, res.matrix_refs, res.files_scanned), ""]
    lines.append("by shape:")
    for sh in SHAPES:
        lines.append("  %-16s %4d" % (sh, res.per_shape.get(sh, 0)))
    lines += ["", "by dir:"]
    for d, n in sorted(res.per_dir.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append("  %-24s %4d" % (d, n))
    lines += ["", "by file:"]
    for f, n in sorted(res.per_file.items(), key=lambda kv: (-kv[1], kv[0]))[:top_files]:
        lines.append("  %4d  %s" % (n, f))
    return "\n".join(lines)


def _md(s):
    return s.replace("|", "\\|")


def render_doc(res, all_paths, pairs):
    """Deterministic D11 work queue (no timestamps, no shas)."""
    o = []
    o.append("# MC2 <-> render-space axis conversion sites (D11 work queue)")
    o.append("")
    o.append("Generated by `py -3 tools/axis_swizzle_ratchet.py --write-doc` from the committed tree. "
             "Do not hand-edit; regenerate after each migration slice.")
    o.append("")
    o.append("The conversion is `(x, y, z) -> (-x, z, y)`; it is its own inverse, so the same spelling "
             "covers Stuff/MC2 -> render and back. The count below may only fall "
             "(`tools/axis_swizzle_ratchet.py --check`, baseline `tools/axis_swizzle_ratchet.baseline.json`).")
    o.append("")
    o.append("## Totals")
    o.append("")
    o.append("| Metric | Count |")
    o.append("|---|---:|")
    o.append("| Hand-written sites (ratcheted) | %d |" % res.total)
    o.append("| Waived (`%s`) | %d |" % (WAIVER, len(res.waived)))
    o.append("| Helper-call sites (centralized) | %d |" % res.helper_calls)
    o.append("| Helper definitions | %d |" % res.helper_defs)
    o.append("| Matrix-form references (`%s`) | %d |" % ("`, `".join(MATRIX_NAMES), res.matrix_refs))
    o.append("")
    o.append("## Shapes")
    o.append("")
    o.append("| Shape | Example | Sites |")
    o.append("|---|---|---:|")
    ex = {"triplet-ctor": "vec3(-a.x, a.z, a.y)", "triplet-brace": "{ -a.x, a.z, a.y }",
          "triplet-arglist": "f(.., -a.x, a.z, a.y)", "decl-chain": "float lx = -d.x, ly = d.z, lz = d.y;",
          "swizzle-zy": "vec3(-a.x, a.zy)", "swizzle-xzy": "a.xzy * vec3(-1, 1, 1)",
          "assign-triple": "o.x = -i.x; o.y = i.z; o.z = i.y;",
          "assign-scalars": "X = -v.x; Y = v.z; Z = v.y;"}
    for sh in SHAPES:
        o.append("| %s | `%s` | %d |" % (sh, _md(ex[sh]), res.per_shape.get(sh, 0)))
    o.append("")
    o.append("## By directory")
    o.append("")
    o.append("| Directory | Sites |")
    o.append("|---|---:|")
    for d, n in sorted(res.per_dir.items(), key=lambda kv: (-kv[1], kv[0])):
        o.append("| `%s` | %d |" % (d, n))
    o.append("")

    sh_sites = defaultdict(list)
    for s in res.sites:
        if s.path.endswith(SHADER_EXTS):
            sh_sites[s.path].append(s)
    o.append("## GL/VK shader twins")
    o.append("")
    o.append("Shaders that carry sites, with their counterpart in the other backend. Twins must migrate in "
             "the same slice so `tools/glsl_port_diff.py` (verbatim-port prover) stays green. "
             "Pairing is by identical name, `_tier1` suffix, or the glsl_port_diff registry; a row with no counterpart "
             "may still have a VK/GL twin under a different name, so check by hand before migrating. "
             "A site inside a `//PORT-BEGIN`..`//PORT-END` block is marked `port` under Sites: edit both sides "
             "of that block together, then re-run `py -3 tools/glsl_port_diff.py --all`.")
    o.append("")
    o.append("| Shader | Sites | Counterpart | Why paired | Counterpart sites |")
    o.append("|---|---:|---|---|---:|")
    for f in sorted(sh_sites):
        tw = twins_for(f, all_paths, pairs)
        if not tw:
            o.append("| `%s` | %d | _no counterpart found_ | | |" % (f, len(sh_sites[f])))
        for t, why in tw:
            o.append("| `%s` | %d | `%s` | %s | %d |" % (f, len(sh_sites[f]), t, why, res.per_file.get(t, 0)))
    o.append("")

    o.append("## Sites")
    o.append("")
    by_file = defaultdict(list)
    for s in res.sites:
        by_file[s.path].append(s)
    for f in sorted(by_file, key=lambda p: (-len(by_file[p]), p)):
        o.append("### `%s` (%d)" % (f, len(by_file[f])))
        o.append("")
        spans = res.port_spans.get(f, [])
        for s in sorted(by_file[f], key=lambda x: x.line):
            port = next((n for a, b, n in spans if a <= s.line <= b), None)
            o.append("- `%s:%d` %s%s: `%s`" % (f, s.line, s.shape, " (port `%s`)" % port if port else "",
                                             _md(s.text[:110])))
        o.append("")
    if res.helper_calls_by_file:
        o.append("## Helper-call sites")
        o.append("")
        for f, n in sorted(res.helper_calls_by_file.items()):
            o.append("- `%s`: %d" % (f, n))
        o.append("")
    return "\n".join(o).rstrip("\n") + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--report", action="store_true")
    g.add_argument("--write-baseline", action="store_true")
    g.add_argument("--write-doc", action="store_true", help="regenerate " + DOC_REL)
    g.add_argument("--check", action="store_true")
    ap.add_argument("--tighten", action="store_true", help="check: on success rewrite baseline lower")
    ap.add_argument("--rev", default="HEAD", help="git rev to scan (default HEAD)")
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--worktree", action="store_true", help="scan the dirty working tree")
    src.add_argument("--staged", action="store_true", help="scan the git index")
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--repo", default=DEFAULT_ROOT, help="repo root (tests)")
    a = ap.parse_args(argv)
    source = "worktree" if a.worktree else "staged" if a.staged else "head"

    base = None
    if a.check:
        if not os.path.exists(a.baseline):
            print("axis_swizzle_ratchet: no baseline at %s (run --write-baseline)" % a.baseline, file=sys.stderr)
            return 2
        with open(a.baseline, "r", encoding="utf-8") as fh:
            base = json.load(fh)

    files = load_sources(a.repo, source, a.rev)
    res = scan(files)
    if a.check:
        rc = check(res, base)
        if rc == 0 and a.tighten and (res.total < base["total"] or res.helper_calls != base.get("helper_calls")):
            write_json(a.baseline, make_baseline(res, a.repo, a.rev))
            print("axis_swizzle_ratchet: baseline tightened")
        return rc
    if a.write_baseline:
        nb = make_baseline(res, a.repo, a.rev)
        write_json(a.baseline, nb)
        print("axis_swizzle_ratchet: baseline written: %s (head %s, total %d, %d files with sites)"
              % (a.baseline, nb["head"], res.total, len(res.per_file)))
        return 0
    if a.write_doc:
        pairs = registry_pairs(blob_text(a.repo, source, a.rev, PORT_DIFF_REL))
        # twin lookup needs every shader path, not only the ones in `files` that matched EXTS
        doc = render_doc(res, set(files), pairs)
        with open(os.path.join(a.repo, DOC_REL), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(doc)
        print("axis_swizzle_ratchet: wrote %s (%d sites)" % (DOC_REL, res.total))
        return 0
    print(render_report(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
