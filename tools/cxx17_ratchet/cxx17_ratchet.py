#!/usr/bin/env python3
"""cxx17_ratchet - monotonic "legacy idiom" counter for the C++17 strangler-fig migration.

Counts C++98/03-era idioms per top-level owned directory with fast regex heuristics
(comments and string literals are blanked first). The counts may only go DOWN:
`--check` fails if any idiom count in any directory is higher than baseline.json.

  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --report            # table per dir (+ --by-file N, --depth 2)
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --write-baseline    # (re)write baseline.json
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check             # exit 1 on any increase
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check --tighten   # on success, lower the baseline to current
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --selftest

Exit: 0 ok, 1 an idiom count increased, 2 usage/baseline error.
These are HEURISTICS (grep-grade, not a parser): the goal is a stable, cheap monotonic
metric, not exact truth. A false positive is fine as long as it is stable.
Stdlib only; ~10 s for the whole repo.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BASELINE = os.path.join(HERE, "baseline.json")

# .clang-tidy owned dirs + the small seam dirs. editor/, gui/, Viewer/ are legacy tools:
# opt in with --dirs (they are not in HeaderFilterRegex either).
DEFAULT_DIRS = ["GameOS", "mclib", "code", "RenderCore", "GuiRuntime",
                "RenderWorld", "GameAdapters", "HostServices", "EditorBridge"]
EXTS = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx", ".inl")
EXCLUDE_PARTS = {"3rdparty", "graphify-out", "__pycache__", ".git", "out", "Tracy"}

IDIOMS = {
    "LG01": ("NULL", "nullptr"),
    "LG02": ("typedef", "using alias"),
    "LG03": ("raw new/delete", "std::unique_ptr / make_unique / containers"),
    "LG04": ("manual tagged union / switch on type|kind member", "std::variant + std::visit"),
    "LG05": ("bool+out-param / -1,nullptr sentinel return", "std::optional<T>"),
    "LG06": ("const char* / const std::string& read-only param", "std::string_view"),
    "LG07": ("enable_if / tag-dispatch type switching", "if constexpr"),
    "LG08": (".first/.second pair access", "structured bindings"),
    "LG09": ("C-style cast", "static_cast / reinterpret_cast / bit_cast"),
    "LG10": ("manual min/max clamp chain", "std::clamp"),
    "LG11": ("Win32/CRT path API or '\\' path concat", "std::filesystem"),
    "LG12": ("printf-family logging", "std::format logging facade (C++20)"),
    "LG13": ("#define constant", "inline constexpr"),
    "LG14": ("unscoped enum", "enum class"),
    "LG15": ("auto_ptr/bind1st/ptr_fun/register/throw() spec", "removed-in-17 / deprecated facilities"),
}
IDS = list(IDIOMS)

# ---------------------------------------------------------------- lexing helpers
_TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def _blank(m: "re.Match[str]", keep_strings: bool) -> str:
    s = m.group(0)
    if s[0] == "/":                      # comment: spaces, keep newlines
        return re.sub(r"[^\n]", " ", s)
    if keep_strings:
        return s
    return s[0] + " " * (len(s) - 2) + s[-1] if len(s) >= 2 else s


def strip(text: str):
    """Return (no_comments, blanked) with identical line structure."""
    nc = _TOKEN.sub(lambda m: _blank(m, True), text)
    bl = _TOKEN.sub(lambda m: _blank(m, False), text)
    return nc.split("\n"), bl.split("\n")


# ---------------------------------------------------------------- detectors
R_NULL = re.compile(r"\bNULL\b")
R_TYPEDEF = re.compile(r"^\s*typedef\b")
R_NEW = re.compile(r"(?<![\w:])new\s+(?!\()[A-Za-z_:]")
R_DELETE = re.compile(r"(?<![\w:])delete\b(?!\s*;)(?!\s*,)(?!\s*\))")
R_EQ_DELETE = re.compile(r"=\s*delete\b")
R_OPERATOR = re.compile(r"\boperator\s+(new|delete)\b")
ALLOC_FILE = re.compile(r"(heap|alloc|pool|memory|gosmem|new_?delete|arena)", re.I)
R_UNION = re.compile(r"\bunion\b\s*\w*\s*(\{|$)")
R_TAGFIELD = re.compile(r"\b\w*(?:type|kind|tag|Type|Kind|Tag)\w*\s*;|\benum\b")
R_SWITCH_TK = re.compile(r"\bswitch\s*\(\s*[\w\s\[\]\.\*\(\)]*?(?:->|\.)\s*\w*(?:[Tt]ype|[Kk]ind)\w*\s*\)")
R_FUNC_HDR = re.compile(r"^(?!\s)[\w:<>\*&~ ,]*?\b(\w+)\s*\(")
R_BOOL_FN = re.compile(r"\bbool\s+(?:\w+::)*(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)")
OUT_NAME = re.compile(r"^(?:out|p?out|result|ret|r?val|dst|o[A-Z_]|p[A-Z]|pResult)", re.I)
FIND_NAME = re.compile(r"^(?:get|find|try|lookup|search|read|parse|load|fetch|query|resolve|pick)", re.I)
R_RET_SENT = re.compile(r"\breturn\s+(?:-1L?|NOT_FOUND|INVALID_\w+|0xFFFFFFFFu?)\s*;")
R_RET_NULL = re.compile(r"\breturn\s+(?:nullptr|NULL)\s*;")
R_STR_PARAM = re.compile(r"\bconst\s+char\s*\*\s*(?!const\b)\w+\s*[,)=]|\bchar\s+const\s*\*\s*\w+\s*[,)=]|"
                         r"\bconst\s+(?:std::)?string\s*&\s*\w+\s*[,)=]")
R_SFINAE = re.compile(r"\benable_if(?:_t)?\b|\bvoid_t\b|\bstd::(?:true_type|false_type|integral_constant)\b|"
                      r"->\s*decltype\s*\(|\btag_dispatch\b|\bis_(?:same|integral|floating_point)(?:_v)?\s*<")
R_PAIR = re.compile(r"(?:->|\.)\s*(?:first|second)\b(?!\s*\()")
R_CAST = re.compile(r"\(\s*(?:const\s+|unsigned\s+|signed\s+|volatile\s+)*"
                    r"(?:u?int(?:8|16|32|64)?(?:_t)?|char|short|long(?:\s+long)?|float|double|size_t|ptrdiff_t|bool|"
                    r"U?LONG|DWORD|BYTE|WORD|[A-Za-z_]\w*(?:::\w+)*\s*\*+|void\s*\*+|[A-Z]\w*)"
                    r"\s*\)\s*(?=[\w(&*~!\-+\"'])")
CAST_PRE_KW = {"return", "case", "else", "do", "throw", "and", "or", "not", "co_return", "new", "delete"}
CAST_POST_KW = re.compile(r"\s*(?:const|override|noexcept|final|throw|volatile|mutable|try|asm)\b")
R_CAST_TYPE_FALSE = re.compile(r"\(\s*(?:int|void|float|double|char|bool|long|short|unsigned)\s*\)\s*$")
R_MINMAX = re.compile(r"\b(?:std::)?(?:__?)?(?:min|max|MIN|MAX|Min|Max)\s*\(")
R_CLAMP_IF = re.compile(r"\bif\s*\(\s*([A-Za-z_][\w\.\->\[\]]*)\s*(<|>)=?\s*[^;{}]+\)\s*\{?\s*\1\s*=[^=]")
R_WIN_PATH = re.compile(r"\b(?:PathCombine\w*|PathAppend\w*|PathFileExists\w*|PathRemoveFileSpec\w*|PathFindFileName\w*|"
                        r"GetCurrentDirectory\w*|SetCurrentDirectory\w*|CreateDirectory[AW]?|FindFirstFile\w*|FindNextFile\w*|"
                        r"GetFileAttributes\w*|GetModuleFileName\w*|GetFullPathName\w*|_splitpath\w*|_makepath\w*|_fullpath|"
                        r"_mkdir|_chdir|_getcwd|_rmdir|DeleteFile[AW]?|CopyFile[AW]?|MoveFile\w*|RemoveDirectory[AW]?|_access|_stat\w*)\s*\(")
R_PATH_CAT = re.compile(r"""'\\\\'|"\\\\"|%s\\\\|\\\\%s|"[\\/]"\s*\+|\+\s*"[\\/]|\bstrcat\s*\([^;]*"[\\/]""")
R_PRINTF = re.compile(r"(?<![\w:.>])_?(?:[fsvw]|sn|vsn|st|snw|vsnw)?printf(?:_s|_l)?\s*\(|\bOutputDebugString\w*\s*\(|"
                      r"\bwsprintf\w*\s*\(")
R_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?!\()\s+(.+?)\s*$")
R_DEFVAL = re.compile(r"""^(?:[-+~(]|\d|0[xX]|\.\d|['"]|L["']|\w+\s*[-+*/|<]\s*[\w(])""")
R_ENUM = re.compile(r"\benum\b(?!\s+(?:class|struct)\b)\s*(?:\w+\s*)?(?::\s*[\w ]+)?\s*(?:\{|$)")
R_LEGACY = re.compile(r"\b(?:auto_ptr|bind1st|bind2nd|ptr_fun|mem_fun|mem_fun_ref|unary_function|binary_function|"
                      r"random_shuffle|register)\b|\)\s*throw\s*\([^)]*\)\s*(?:[{;]|$)")


def scan_text(text: str, path: str = "") -> dict:
    """Return {idiom_id: [1-based line numbers]} for one file's text."""
    nc, bl = strip(text)
    hits = defaultdict(list)
    allocator_file = bool(ALLOC_FILE.search(os.path.basename(path)))
    last_fn = ""
    clamp_pending = []          # (line, var, op)
    union_lines = []
    n = len(bl)
    for i, (c, b) in enumerate(zip(nc, bl)):
        ln = i + 1
        s = b.strip()
        if not s:
            continue
        pp = s.startswith("#")
        if not pp:
            m = R_FUNC_HDR.match(b)
            if m and m.group(1) not in ("if", "for", "while", "switch", "return", "sizeof"):
                last_fn = m.group(1)
        # LG01
        if R_NULL.search(b) and not re.match(r"\s*#\s*(?:define|undef)\s+NULL\b", b):
            hits["LG01"].append(ln)
        # LG02
        if R_TYPEDEF.match(b):
            hits["LG02"].append(ln)
        # LG03
        if not allocator_file and not pp and not R_OPERATOR.search(b):
            if R_NEW.search(b):
                hits["LG03"].append(ln)
            elif R_DELETE.search(b) and not R_EQ_DELETE.search(b):
                hits["LG03"].append(ln)
        # LG04
        if R_UNION.search(b) and not pp:
            lo, hi = max(0, i - 30), min(n, i + 40)
            ctx = "\n".join(bl[lo:hi])
            if R_TAGFIELD.search(ctx):
                hits["LG04"].append(ln)
        if R_SWITCH_TK.search(b):
            hits["LG04"].append(ln)
        # LG05
        if not pp:
            for m in R_BOOL_FN.finditer(b):
                name, params = m.group(1), m.group(2)
                plist = [p.strip() for p in params.split(",") if p.strip()]
                if not plist:
                    continue
                last = plist[-1]
                if re.search(r"\bconst\b", last) and "*" not in last.replace("const", ""):
                    continue
                pm = re.search(r"([&*])\s*(\w+)\s*(?:=.*)?$", last)
                if pm and "const" not in last.split(pm.group(1))[0]:
                    # pointer params are usually inputs (Read(FitIniFile*)); need an out-ish
                    # name for them, while a non-const reference in a get/find/try fn also counts.
                    if OUT_NAME.search(pm.group(2)) or (pm.group(1) == "&" and FIND_NAME.search(name)):
                        hits["LG05"].append(ln)
                        break
            if R_RET_SENT.search(b):
                hits["LG05"].append(ln)
            elif R_RET_NULL.search(b) and FIND_NAME.search(last_fn or "") or \
                    (R_RET_NULL.search(b) and re.search(r"find|lookup|search", last_fn or "", re.I)):
                hits["LG05"].append(ln)
        # LG06
        if not pp and "(" in b and not R_TYPEDEF.match(b):
            for m in R_STR_PARAM.finditer(b):
                pre = b[:m.start()]
                if pre.count("(") > pre.count(")"):      # inside a parameter list, not a local
                    hits["LG06"].append(ln)
                    break
        # LG07
        if R_SFINAE.search(b):
            hits["LG07"].append(ln)
        # LG08
        if R_PAIR.search(b):
            hits["LG08"].append(ln)
        # LG09
        if not pp or "define" in s:
            for m in R_CAST.finditer(b):
                pre = b[:m.start()].rstrip()
                if pre:
                    pm = re.search(r"([A-Za-z_]\w*)$", pre)
                    if pm and pm.group(1) not in CAST_PRE_KW:
                        continue               # looks like a call: foo(int) ...
                    if pre[-1] in ")]>":
                        continue
                if CAST_POST_KW.match(b[m.end():]):
                    continue
                if re.match(r"\(\s*void\s*\)", m.group(0)):
                    continue
                if re.search(r"sizeof\s*$", pre):
                    continue
                inner = m.group(0)[1:-1].strip()
                if re.fullmatch(r"[A-Z]\w*", inner) and not re.fullmatch(
                        r"(?:U?LONG|DWORD|BYTE|WORD|[A-Z][a-z]+\w*|[A-Z]\w*_[A-Z]?\w*)", inner):
                    continue                   # (FOO) x -- almost certainly a macro paren
                if re.fullmatch(r"[A-Z0-9_]+", inner) and inner not in ("DWORD", "BYTE", "WORD", "ULONG", "LONG"):
                    continue
                hits["LG09"].append(ln)
        # LG10
        if len(R_MINMAX.findall(b)) >= 2 and not pp:
            hits["LG10"].append(ln)
        m = R_CLAMP_IF.search(b)
        if m:
            clamp_pending.append((ln, m.group(1), m.group(2)))
        # LG11 (strings kept)
        if R_WIN_PATH.search(b) or (R_PATH_CAT.search(c) and not pp and "#include" not in c):
            hits["LG11"].append(ln)
        # LG12
        if R_PRINTF.search(b):
            hits["LG12"].append(ln)
        # LG13
        m = R_DEFINE.match(b)
        if m and not m.group(1).endswith(("_H", "_H_", "_HPP", "__")) and R_DEFVAL.match(m.group(2)):
            hits["LG13"].append(ln)
        # LG14
        if not pp and R_ENUM.search(b) and not re.search(r"\benum\s+\w+\s+\w+\s*[;,)=]", b):
            hits["LG14"].append(ln)
        # LG15
        if R_LEGACY.search(b):
            hits["LG15"].append(ln)
    # LG10 if-pair clamps: v<lo ... v>hi on the same var within 4 lines -> one hit
    used = set()
    for j, (l1, v1, o1) in enumerate(clamp_pending):
        if j in used:
            continue
        for k in range(j + 1, len(clamp_pending)):
            l2, v2, o2 = clamp_pending[k]
            if l2 - l1 > 4:
                break
            if v2 == v1 and o2 != o1:
                hits["LG10"].append(l1)
                used.update((j, k))
                break
    return {k: sorted(set(v)) for k, v in hits.items()}


# ---------------------------------------------------------------- driver
def list_files(dirs):
    out = []
    for d in dirs:
        base = os.path.join(ROOT, d)
        for dp, dn, fn in os.walk(base):
            dn[:] = [x for x in dn if x not in EXCLUDE_PARTS and not x.startswith("build")]
            for f in fn:
                if f.endswith(EXTS):
                    out.append(os.path.relpath(os.path.join(dp, f), ROOT).replace("\\", "/"))
    return sorted(out)


def _scan_file(rel):
    try:
        with open(os.path.join(ROOT, rel), "rb") as fh:
            text = fh.read().decode("latin-1")
    except OSError:
        return rel, {}
    return rel, scan_text(text, rel)


def scan_repo(dirs, jobs=0):
    files = list_files(dirs)
    res = {}
    if jobs == 1 or len(files) < 50:
        for f in files:
            k, v = _scan_file(f)
            res[k] = v
    else:
        with ProcessPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 4)) as ex:
            for k, v in ex.map(_scan_file, files, chunksize=16):
                res[k] = v
    return res


def aggregate(res, depth=1):
    tab = defaultdict(lambda: dict.fromkeys(IDS, 0))
    for rel, hits in res.items():
        key = "/".join(rel.split("/")[:depth]) if rel.count("/") >= depth else rel.split("/")[0]
        for i, lines in hits.items():
            tab[key][i] += len(lines)
    return {k: tab[k] for k in sorted(tab)}


def per_file_counts(res):
    return {rel: {i: len(v) for i, v in h.items() if v} for rel, h in sorted(res.items()) if h}


def git_head():
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""


def print_table(tab, title="", out=sys.stdout):
    if title:
        print(title, file=out)
    w = max([len(k) for k in tab] + [5])
    print("%-*s " % (w, "dir") + " ".join("%6s" % i for i in IDS), file=out)
    tot = dict.fromkeys(IDS, 0)
    for k, row in tab.items():
        print("%-*s " % (w, k) + " ".join("%6d" % row[i] for i in IDS), file=out)
        for i in IDS:
            tot[i] += row[i]
    print("%-*s " % (w, "TOTAL") + " ".join("%6d" % tot[i] for i in IDS), file=out)
    print("", file=out)
    for i in IDS:
        print("  %s  %-52s -> %s" % (i, IDIOMS[i][0], IDIOMS[i][1]), file=out)


def added_lines(rel):
    """{line numbers added vs HEAD} from git diff -U0, or None when git cannot tell."""
    try:
        d = subprocess.check_output(["git", "diff", "-U0", "HEAD", "--", rel], cwd=ROOT,
                                    stderr=subprocess.DEVNULL, text=True, errors="replace")
    except Exception:
        return None
    added = set()
    for m in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", d, re.M):
        start, cnt = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
        added.update(range(start, start + cnt))
    return added or None


def load_baseline(path=BASELINE):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def make_baseline(res, dirs):
    return {"version": 1, "head": git_head(), "dirs": dirs,
            "idioms": {k: v[0] for k, v in IDIOMS.items()},
            "counts": aggregate(res), "files": per_file_counts(res)}


def check(res, base, samples=5, out=sys.stdout):
    cur = aggregate(res)
    bad = []
    for d in sorted(set(cur) | set(base["counts"])):
        c = cur.get(d, dict.fromkeys(IDS, 0))
        b = base["counts"].get(d, dict.fromkeys(IDS, 0))
        for i in IDS:
            if c.get(i, 0) > b.get(i, 0):
                bad.append((d, i, b.get(i, 0), c.get(i, 0)))
    improved = sum(max(0, base["counts"].get(d, {}).get(i, 0) - cur.get(d, {}).get(i, 0))
                   for d in base["counts"] for i in IDS)
    if not bad:
        print("cxx17_ratchet: OK (no idiom increased; %d fewer legacy sites than baseline)" % improved, file=out)
        return 0
    print("cxx17_ratchet: FAIL - %d idiom count(s) increased vs baseline (%s):" % (len(bad), base.get("head", "?")), file=out)
    bf = base.get("files", {})
    for d, i, b, c in bad:
        print("  %s %-14s %s: %d -> %d (+%d)  [%s -> %s]" % (i, "", d, b, c, c - b, IDIOMS[i][0], IDIOMS[i][1]), file=out)
        shown = 0
        for rel in sorted(res):
            if not (rel == d or rel.startswith(d + "/")):
                continue
            lines = res[rel].get(i, [])
            if len(lines) <= bf.get(rel, {}).get(i, 0):
                continue
            adds = added_lines(rel)
            cand = [l for l in lines if adds is None or l in adds] or lines
            for l in cand[:max(1, samples - shown)]:
                print("      %s:%d" % (rel, l), file=out)
                shown += 1
                if shown >= samples:
                    break
            if shown >= samples:
                break
    print("Use the modern replacement, or (if the legacy form is unavoidable) pay it down elsewhere in the same dir.", file=out)
    return 1


def selftest():
    import unittest
    sys.path.insert(0, HERE)
    import test_cxx17_ratchet as t
    r = unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromModule(t))
    return 0 if r.wasSuccessful() else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--report", action="store_true")
    g.add_argument("--write-baseline", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--selftest", action="store_true")
    ap.add_argument("--dirs", nargs="*", default=None, help="top-level dirs (default: owned set)")
    ap.add_argument("--depth", type=int, default=1, help="report grouping depth (1 = top-level dir)")
    ap.add_argument("--by-file", type=int, default=0, metavar="N", help="report: top N files per idiom")
    ap.add_argument("--samples", type=int, default=5, help="check: file:line samples per increase")
    ap.add_argument("--tighten", action="store_true", help="check: on success rewrite baseline lower")
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    dirs = a.dirs or DEFAULT_DIRS
    if a.check:
        if not os.path.exists(a.baseline):
            print("cxx17_ratchet: no baseline at %s (run --write-baseline)" % a.baseline, file=sys.stderr)
            return 2
        base = load_baseline(a.baseline)
        if not a.dirs:
            dirs = base.get("dirs", dirs)
        res = scan_repo(dirs, a.jobs)
        rc = check(res, base, a.samples)
        if rc == 0 and a.tighten:
            nb = make_baseline(res, dirs)
            with open(a.baseline, "w", encoding="utf-8", newline="\n") as fh:
                json.dump(nb, fh, indent=1, sort_keys=True)
                fh.write("\n")
            print("cxx17_ratchet: baseline tightened")
        return rc
    res = scan_repo(dirs, a.jobs)
    if a.write_baseline:
        nb = make_baseline(res, dirs)
        with open(a.baseline, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(nb, fh, indent=1, sort_keys=True)
            fh.write("\n")
        print_table(nb["counts"], "baseline written: %s (head %s, %d files)" % (a.baseline, nb["head"], len(res)))
        return 0
    print_table(aggregate(res, a.depth), "cxx17_ratchet report (%d files)" % len(res))
    if a.by_file:
        for i in IDS:
            rows = sorted(((len(h.get(i, [])), r) for r, h in res.items() if h.get(i)), reverse=True)[:a.by_file]
            print("\n%s top files:" % i)
            for n, r in rows:
                print("  %5d  %s" % (n, r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
