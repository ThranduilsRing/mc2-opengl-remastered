#!/usr/bin/env python3
"""cxx17_ratchet - monotonic "legacy idiom" counter for the C++17 strangler-fig migration.

Counts C++98/03-era idioms per top-level owned directory with fast regex heuristics
(comments and string literals are blanked first). The counts may only go DOWN.

  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --report            # table per dir (+ --by-file N, --depth 2)
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --write-baseline    # (re)write baseline.json
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check             # exit 1 on any increase
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check --tighten   # on success, lower the baseline to current
  py -3 tools/cxx17_ratchet/cxx17_ratchet.py --selftest

Source of truth (all modes): the COMMITTED tree (HEAD) by default, so other agents' uncommitted
work never blocks you.  --staged scans the index (pre-commit hook); --worktree scans the dirty tree.

Gate rule (decision: "strict totals, per-dir warning"): per idiom, the REPO TOTAL may not exceed
baseline.json -- that fails.  A per-directory increase while the idiom's total did not rise (code
moved between dirs, e.g. code/ -> mclib/ by rename) is only a WARNING.  Trade-off: a new legacy site
can hide behind a decrease elsewhere in another dir; use --report --depth 2 / per-file counts in
baseline.json in review.  (Rename-aware per-dir exemption via `git diff -M` was rejected: fragile.)

LG19 (raw MC2_ environment reads) is STRICT per file, not per total: it is drive-to-zero, so a file may
never gain a site and a file the baseline does not know must have none; a drop elsewhere buys nothing.
editor/, gui/ and Viewer/ are not in the default dirs; their LG19 counts are tracked in baseline.json
("tracked") and a rise only warns until they migrate.

Exit: 0 ok, 1 an idiom total increased, 2 usage/baseline error.
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
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BASELINE = os.path.join(HERE, "baseline.json")

# Directory groups restrict idioms: editor/gui/Viewer track gates; tools/tests track gate literals.
DEFAULT_DIRS = ["GameOS", "mclib", "code", "RenderCore", "GuiRuntime",
                "RenderWorld", "GameAdapters", "HostServices", "EditorBridge", "tools", "tests",
                "editor", "gui", "Viewer"]
GATE_ONLY_DIRS = ("editor", "gui", "Viewer")
# Raw gate reads in engine/player UI code are counted.
LG19_TOOL_PREFIXES = ()
# Scanned for LG20 only (the other idioms are not tracked in the harnesses and tools).
LG20_ONLY_DIRS = ("tools", "tests")
# Unowned directories warn on LG19 increases rather than fail.
TRACKED_DIRS = []
# LG19 may only be read raw inside the gate module itself (Gates/gate.cpp parses the environment).
LG19_EXEMPT_PREFIXES = ("Gates/",)
# Idioms whose per-FILE count may never rise (drive-to-zero), whatever happens to the repo total.
STRICT_FILE_IDIOMS = ("LG19", "LG20")
# LG20: the gate module, its unit test and the generated registry header name gates as rows and fixtures, not as raw reads.
LG20_EXEMPT = ("Gates/", "tests/unit/test_gate.cpp", "RenderCore/RendererFeatureRegistry.h")
# Platform C API adapters preserve null-terminated parameters, sentinel results and OS entry points.
PLATFORM_API_EXEMPT = {
    "GameOS/include/platform_io.h": ("LG06", "LG11"),
    "GameOS/src/platform_io.cpp": ("LG05", "LG06", "LG11"),
    "GameOS/include/platform_winbase.h": ("LG11",),
    "GameOS/src/platform_winbase.cpp": ("LG05", "LG06", "LG11"),
    "GameOS/include/platform_stdlib.h": ("LG06",),
    # Win32 SDK stand-ins for non-Windows builds: they must spell NULL and the SDK typedefs as <windows.h> does.
    "GameOS/include/platform_windef.h": ("LG01",),
    "GameOS/include/platform_winnt.h": ("LG02",),
}
EXTS = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx", ".inl")
EXCLUDE_PARTS = {"3rdparty", "graphify-out", "__pycache__", ".git", "out", "Tracy"}

IDIOMS = {
    "LG01": ("NULL", "nullptr"),
    "LG02": ("typedef", "using alias"),
    "LG03": ("raw new/delete", "std::unique_ptr / make_unique / containers"),
    "LG04": ("manual tagged union / switch on type|kind member", "std::variant + std::visit"),
    "LG05": ("bool+out-param / -1,nullptr sentinel return", "std::optional<T> (not-found) / std::expected<T,E> (error)"),
    "LG06": ("const char* / const std::string& read-only param", "std::string_view"),
    "LG07": ("enable_if / tag-dispatch type switching", "concepts / requires (+ if constexpr)"),
    "LG08": (".first/.second pair access", "structured bindings"),
    "LG09": ("C-style or functional (int(x)) cast", "static_cast / reinterpret_cast / bit_cast"),
    "LG10": ("manual min/max clamp chain / one-sided if-clamp", "std::clamp"),
    "LG11": ("Win32/CRT path API or '\\' path concat", "std::filesystem"),
    "LG12a": ("printf/fprintf/puts/OutputDebugString/SPEW logging", "logging facade (std::print/format + source_location)"),
    "LG12b": ("sprintf/snprintf/vsprintf buffer formatting", "std::format_to / std::format"),
    "LG13": ("#define constant", "inline constexpr"),
    "LG14": ("unscoped enum", "enum class + using enum + std::to_underlying"),
    "LG15": ("auto_ptr/bind1st/ptr_fun/register/throw() spec", "removed-in-17 / deprecated facilities"),
    "LG16": ("manual multi-dim index math [y*w+x] / T** grids", "std::mdspan"),
    "LG17": ("pointer+count parameter pair (T* p, size_t n)", "std::span"),
    "LG18": ("type punning (reinterpret_cast<float*>, union, memcpy bits)", "std::bit_cast"),
    "LG19": ("raw MC2_ environment read (getenv/GetEnvironmentVariable/_dupenv_s/getenv_s/_environ/non-literal getenv)",
             "gate::get<\"MC2_X\">() (Gates/gate.h)"),
}
IDIOMS["LG20"] = ("MC2_ name literal outside a typed gate reader (helper-call reads, #define lines, putenv, messages)",
                  "gate::get<\"MC2_X\">() / state / isSet / str / is")
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
# LG09 also counts functional casts of arithmetic types: int(x), float(x), uint32_t(x) ...
# (clang-tidy google-readability-casting flags them; the fix is static_cast<T>(x)).  Not after
# . -> :: < or an identifier (member / template argument / declarator), not int() / int(*...).
R_FNCAST = re.compile(r"(?<![\w.>:<~])(?:unsigned\s+)?"
                      r"(?:u?int(?:8|16|32|64)_t|size_t|ptrdiff_t|float|double|bool|char|short|int|long|unsigned)"
                      r"\s*\((?!\s*[*&)])")
# LG10 one-sided clamp: if (v > b) v = b;  (also v < b, and b < v), body on the same line or the next.
R_ONESIDED = re.compile(r"\bif\s*\(\s*(?:([A-Za-z_][\w\.\->\[\]]*)\s*[<>]=?\s*([^;{}()<>]+?)"
                        r"|([^;{}()<>]+?)\s*[<>]=?\s*([A-Za-z_][\w\.\->\[\]]*))"
                        r"\s*\)\s*\{?\s*(?:\1|\4)\s*=\s*(?:\2|\3)\s*;")
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
R_LOG = re.compile(r"(?<![\w:.>])_?(?:[fv]|vf)?printf(?:_s|_l)?\s*\(|(?<![\w:.>])_?f?puts\s*\(|"
                   r"\bOutputDebugString\w*\s*\(|\bSPEW\w*\s*\(")
R_FMTBUF = re.compile(r"(?<![\w:.>])_?(?:s|sn|vs|vsn|sw|vsw|snw|vsnw|st)printf(?:_s|_l)?\s*\(|\bwsprintf\w*\s*\(")
_IDX = r"[A-Za-z_]\w*(?:\s*(?:\.|->)\s*[A-Za-z_]\w*)?"
R_MDIDX = re.compile(r"\[\s*(?:\(\s*" + _IDX + r"\s*\*\s*" + _IDX + r"\s*\+\s*" + _IDX + r"\s*\)|" + _IDX +
                     r")\s*\*\s*" + _IDX + r"\s*\+\s*" + _IDX + r"\s*\]")
R_PTRGRID = re.compile(r"(?<![\w:])new\s+[A-Za-z_][\w:]*\s*\*\s*\[")
_CNT = r"(?:n|count|len|length|num|size|n[A-Z]\w*|num[A-Z_]\w*|\w*(?:Count|Len|Length))"
R_PTRCNT = re.compile(r"(?<!\w)(?!void\b)[A-Za-z_][\w:]*\s*\*\s*(?:const\s+)?\w+\s*,\s*(?:const\s+)?"
                      r"(?:std::size_t|size_t|int|unsigned(?:\s+int)?|long|uint32_t|uint64_t|DWORD|ULONG)\s+" + _CNT + r"\s*[,)]")
_PUNT = r"(?:float|double|u?int(?:16|32|64)_t|DWORD|ULONG|unsigned(?:\s+(?:int|long))?)"
R_PUN = re.compile(r"reinterpret_cast\s*<\s*(?:const\s+)?" + _PUNT + r"\s*\*\s*>|"
                   r"\*\s*\(\s*(?:const\s+)?" + _PUNT + r"\s*\*\s*\)\s*&|"
                   r"\bmemcpy\s*\(\s*&\s*\w+\s*,\s*&\s*\w+\s*,\s*sizeof\s*\(\s*(?:float|double|u?int(?:32|64)_t|DWORD)\s*\)\s*\)")
R_FLOATTOK = re.compile(r"\b(?:float|double)\b")
R_INTTOK = re.compile(r"\b(?:u?int(?:32|64)_t|DWORD|ULONG|unsigned(?:\s+(?:int|long))?|int|long)\b")
R_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?!\()\s+(.+?)\s*$")
R_DEFVAL = re.compile(r"""^(?:[-+~(]|\d|0[xX]|\.\d|['"]|L["']|\w+\s*[-+*/|<]\s*[\w(])""")
R_ENUM = re.compile(r"\benum\b(?!\s+(?:class|struct)\b)\s*(?:\w+\s*)?(?::\s*[\w ]+)?\s*(?:\{|$)")
# LG19 counts unknown names because they may read MC2_ variables; known unrelated names are exempt.
R_ENVREAD = re.compile(
    r"""(?<![\w.>])(?:std::|::)?(?:getenv|_wgetenv)\s*\(\s*(?:L?"MC2_|[A-Za-z_])"""
    r"""|\bGetEnvironmentVariable[AW]?\s*\(\s*L?"MC2_"""
    r"""|\b_dupenv_s\b|\bgetenv_s\b|\b_wdupenv_s\b"""
    r"""|\b_environ\b|\bGetEnvironmentStrings[AW]?\b|\bSDL_getenv\b""")
# LG20 catches gate-name literals outside typed readers/overrides, including helpers and writes.
R_MC2LIT = re.compile(r'''L?"MC2_[A-Z0-9_]+"''')
R_TYPED_READER = re.compile(r'''(?<![A-Za-z_])(?:get|state|isSet|isNonEmpty|armed|regionTier1Armed|str|is|gate::name|gate::ScopedOverride)\s*<\s*L?"MC2_[A-Z0-9_]+"''')
R_LEGACY = re.compile(r"\b(?:auto_ptr|bind1st|bind2nd|ptr_fun|mem_fun|mem_fun_ref|unary_function|binary_function|"
                      r"random_shuffle|register)\b|\)\s*throw\s*\([^)]*\)\s*(?:[{;]|$)")


def scan_text(text: str, path: str = "") -> dict:
    """Return {idiom_id: [1-based line numbers]} for one file's text."""
    nc, bl = strip(text)
    hits = defaultdict(list)
    allocator_file = bool(ALLOC_FILE.search(os.path.basename(path)))
    last_fn = ""
    clamp_pending = []          # (line, var, op)
    onesided = []
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
            if not pp:
                for m in R_FNCAST.finditer(b):
                    if not re.search(r"\b(?:operator|sizeof|decltype|typename|new)\s*$", b[:m.start()]):
                        hits["LG09"].append(ln)
                        break
        # LG10
        if not pp and any(len(R_MINMAX.findall(statement)) >= 2 for statement in b.split(";")):
            hits["LG10"].append(ln)
        m = R_CLAMP_IF.search(b)
        if m:
            clamp_pending.append((ln, m.group(1), m.group(2)))
        if not pp and "if" in b:
            # one-sided clamp (clang-tidy readability-use-std-min-max); body may sit on the next line
            joined = b + (" " + bl[i + 1].strip() if i + 1 < n and b.rstrip().endswith((")", "{")) else "")
            if R_ONESIDED.search(joined):
                onesided.append(ln)
        # LG11 (strings kept)
        if R_WIN_PATH.search(b) or (R_PATH_CAT.search(c) and not pp and "#include" not in c):
            hits["LG11"].append(ln)
        # LG19 (strings kept; Gates/ is the one place allowed to read the environment)
        if not pp and R_ENVREAD.search(c) and not path.startswith(LG19_EXEMPT_PREFIXES):
            hits["LG19"].append(ln)
        # LG20 (strings kept; comments already stripped; preprocessor lines count)
        if not path.startswith(LG20_EXEMPT) and "MC2_" in c:
            for _ in R_MC2LIT.findall(R_TYPED_READER.sub("", c)):
                hits["LG20"].append(ln)
        # LG12a (logging) / LG12b (buffer formatting)
        if R_LOG.search(b):
            hits["LG12a"].append(ln)
        if R_FMTBUF.search(b):
            hits["LG12b"].append(ln)
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
        # LG16 (conservative: identifier*identifier+identifier inside [], new T*[n])
        if not pp and (R_MDIDX.search(b) or R_PTRGRID.search(b)):
            hits["LG16"].append(ln)
        # LG17 (only inside an open parameter list)
        if not pp and "(" in b:
            for m in R_PTRCNT.finditer(b):
                pre = b[:m.start()]
                if pre.count("(") > pre.count(")"):
                    hits["LG17"].append(ln)
                    break
        # LG18
        if not pp and R_PUN.search(b):
            hits["LG18"].append(ln)
        elif not pp and R_UNION.search(b):
            body = []
            for k in range(i, min(n, i + 8)):
                body.append(bl[k])
                if "}" in bl[k]:
                    break
            body = "\n".join(body)
            if R_FLOATTOK.search(body) and R_INTTOK.search(body):
                hits["LG18"].append(ln)
    # LG10 if-pair clamps: v<lo ... v>hi on the same var within 4 lines -> one hit
    used = set()
    paired = set()
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
                paired.update((l1, l2))
                break
    # one-sided clamps; the two halves of a counted pair are not counted again
    hits["LG10"].extend(x for x in onesided if x not in paired)
    return {k: sorted(set(v)) for k, v in hits.items() if k not in PLATFORM_API_EXEMPT.get(path, ())}


# ---------------------------------------------------------------- driver
def _wanted(rel, dirs):
    parts = rel.split("/")
    if parts[0] not in dirs or not rel.endswith(EXTS):
        return False
    return not any(x in EXCLUDE_PARTS or x.startswith("build") for x in parts[:-1])


def list_files(dirs):
    """Working-tree files (--worktree mode)."""
    out = []
    for d in dirs:
        base = os.path.join(ROOT, d)
        for dp, dn, fn in os.walk(base):
            dn[:] = [x for x in dn if x not in EXCLUDE_PARTS and not x.startswith("build")]
            for f in fn:
                if f.endswith(EXTS):
                    out.append(os.path.relpath(os.path.join(dp, f), ROOT).replace("\\", "/"))
    return sorted(out)


def _git(args, inp=None):
    return subprocess.run(["git"] + args, cwd=ROOT, input=inp, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True, **console_child.child_kwargs(redirected=True)).stdout


def git_blobs(dirs, mode):
    """{rel: bytes} of the committed tree ('head') or the index ('staged')."""
    if mode == "staged":
        names = _git(["ls-files", "-z", "--"] + list(dirs)).split(b"\0")
        spec = lambda r: ":" + r                      # noqa: E731
    else:
        names = _git(["ls-tree", "-r", "-z", "--name-only", "HEAD", "--"] + list(dirs)).split(b"\0")
        spec = lambda r: "HEAD:" + r                  # noqa: E731
    rels = sorted(n.decode("utf-8", "replace") for n in names if n)
    rels = [r for r in rels if _wanted(r, dirs)]
    raw = _git(["cat-file", "--batch"], "".join(spec(r) + "\n" for r in rels).encode("utf-8"))
    out, pos = {}, 0
    for r in rels:
        eol = raw.index(b"\n", pos)
        hdr = raw[pos:eol].split()
        pos = eol + 1
        if len(hdr) == 3 and hdr[1] == b"blob":
            size = int(hdr[2])
            out[r] = raw[pos:pos + size]
            pos += size + 1
    return out


def _scan_item(item):
    rel, data = item
    hits = scan_text(data.decode("latin-1"), rel)
    top = rel.split("/")[0]
    if top in LG20_ONLY_DIRS:
        keep = ("LG19", "LG20") if rel.startswith(LG19_TOOL_PREFIXES) else ("LG20",)
        hits = {k: v for k, v in hits.items() if k in keep}
    elif top in GATE_ONLY_DIRS:
        hits = {k: v for k, v in hits.items() if k in ("LG19", "LG20")}
    return rel, hits


def _scan_file(rel):
    try:
        with open(os.path.join(ROOT, rel), "rb") as fh:
            data = fh.read()
    except OSError:
        return rel, {}
    return _scan_item((rel, data))


def scan_repo(dirs, jobs=0, source="head"):
    """source: 'head' (committed tree, default), 'staged' (index) or 'worktree' (dirty tree)."""
    res = {}
    if source == "worktree":
        items, fn = list_files(dirs), _scan_file
    else:
        items, fn = list(git_blobs(dirs, source).items()), _scan_item
    if jobs == 1 or len(items) < 50:
        for it in items:
            k, v = fn(it)
            res[k] = v
    else:
        with ProcessPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 4)) as ex:
            for k, v in ex.map(fn, items, chunksize=16):
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
                                       stderr=subprocess.DEVNULL, text=True, **console_child.child_kwargs(redirected=True)).strip()
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
                                    stderr=subprocess.DEVNULL, text=True, errors="replace", **console_child.child_kwargs(redirected=True))
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


def tracked_counts(res):
    """LG19 per tracked (not yet owned) dir: {dir: count}. Reported, never failed."""
    out = {}
    for d in TRACKED_DIRS:
        n = sum(len(h.get("LG19", [])) for rel, h in res.items() if rel.split("/")[0] == d)
        out[d] = n
    return out


def make_baseline(res, dirs, tracked=None, previous=None):
    result = {"version": 2, "head": git_head(), "dirs": dirs,
            "idioms": {k: v[0] for k, v in IDIOMS.items()},
            "counts": aggregate(res), "files": per_file_counts(res),
            "tracked": {"LG19": tracked or {}}}
    if previous is not None:
        def lower(current, old):
            return {k: lower(v, old.get(k, {})) if isinstance(v, dict)
                    else min(v, old.get(k, 0)) for k, v in current.items()}
        for key in ('counts', 'files', 'tracked'):
            result[key] = lower(result[key], previous.get(key, {}))
    return result


def strict_file_failures(res, base):
    """Per-file rises of the drive-to-zero idioms: (idiom, rel, baseline_count, current_count, lines)."""
    bf = base.get("files", {})
    bad = []
    for i in STRICT_FILE_IDIOMS:
        for rel in sorted(res):
            lines = res[rel].get(i, [])
            was = bf.get(rel, {}).get(i, 0)
            if len(lines) > was:
                bad.append((i, rel, was, len(lines), lines))
    return bad


def check(res, base, samples=5, out=sys.stdout, use_git_diff=False, tracked=None):
    """Strict per-idiom REPO TOTALS (fail); per-dir increases with a flat/lower total only warn.

    STRICT_FILE_IDIOMS (LG19) fail on any per-FILE rise, and a file the baseline has no entry for must
    have none. `tracked` (LG19 counts of editor/gui/Viewer) only warns."""
    cur = aggregate(res)
    zero = dict.fromkeys(IDS, 0)
    tot_c = {i: sum(r.get(i, 0) for r in cur.values()) for i in IDS}
    tot_b = {i: sum(r.get(i, 0) for r in base["counts"].values()) for i in IDS}
    bad = [(i, tot_b[i], tot_c[i]) for i in IDS if tot_c[i] > tot_b[i]]
    warn = []
    for d in sorted(set(cur) | set(base["counts"])):
        c, b = cur.get(d, zero), base["counts"].get(d, zero)
        for i in IDS:
            if c.get(i, 0) > b.get(i, 0) and tot_c[i] <= tot_b[i]:
                warn.append((d, i, b.get(i, 0), c.get(i, 0)))
    for d, i, b, c in warn:
        print("  warn: %s %s rose %d -> %d but the repo total did not (moved code?)" % (d, i, b, c), file=out)
    for d, n in sorted((tracked or {}).items()):
        was = base.get("tracked", {}).get("LG19", {}).get(d)
        if was is not None and n > was:
            print("  warn: %s LG19 (tracked, not yet owned) rose %d -> %d" % (d, was, n), file=out)
    strict = strict_file_failures(res, base)
    improved = sum(max(0, tot_b[i] - tot_c[i]) for i in IDS)
    if strict:
        print("cxx17_ratchet: FAIL - drive-to-zero idiom rose in %d file(s) (%s):" % (len(strict), base.get("head", "?")), file=out)
        for i, rel, was, now, lines in strict[:max(samples, 10)]:
            print("  %s %s: %d -> %d  [%s -> %s]" % (i, rel, was, now, IDIOMS[i][0], IDIOMS[i][1]), file=out)
            for l in lines[:samples]:
                print("      %s:%d" % (rel, l), file=out)
        print("A new or growing file must read MC2_ gates through gate::get<\"MC2_X\">() (Gates/gate.h). "
              "If the file was only moved, rewrite the baseline with --write-baseline and say why.", file=out)
    if not bad and not strict:
        print("cxx17_ratchet: OK (no idiom total increased; %d fewer legacy sites than baseline)" % improved, file=out)
        return 0
    if not bad:
        return 1
    print("cxx17_ratchet: FAIL - %d idiom total(s) increased vs baseline (%s):" % (len(bad), base.get("head", "?")), file=out)
    bf = base.get("files", {})
    for i, b, c in bad:
        print("  %s total: %d -> %d (+%d)  [%s -> %s]" % (i, b, c, c - b, IDIOMS[i][0], IDIOMS[i][1]), file=out)
        shown = 0
        for d in sorted(cur):
            if cur[d].get(i, 0) <= base["counts"].get(d, zero).get(i, 0):
                continue
            print("    %s: %d -> %d" % (d, base["counts"].get(d, zero).get(i, 0), cur[d][i]), file=out)
            for rel in sorted(res):
                if not (rel == d or rel.startswith(d + "/")) or shown >= samples:
                    continue
                lines = res[rel].get(i, [])
                if len(lines) <= bf.get(rel, {}).get(i, 0):
                    continue
                adds = added_lines(rel) if use_git_diff else None
                cand = [l for l in lines if adds is None or l in adds] or lines
                for l in cand[:max(1, samples - shown)]:
                    print("      %s:%d" % (rel, l), file=out)
                    shown += 1
    print("Use the modern replacement, or (if the legacy form is unavoidable) pay it down elsewhere.", file=out)
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
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--worktree", action="store_true", help="scan the dirty working tree (default: committed HEAD)")
    src.add_argument("--staged", action="store_true", help="scan the git index (pre-commit use)")
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    dirs = a.dirs or DEFAULT_DIRS
    source = "worktree" if a.worktree else "staged" if a.staged else "head"
    if a.check:
        if not os.path.exists(a.baseline):
            print("cxx17_ratchet: no baseline at %s (run --write-baseline)" % a.baseline, file=sys.stderr)
            return 2
        base = load_baseline(a.baseline)
        if not a.dirs:
            dirs = base.get("dirs", dirs)
        res = scan_repo(dirs, a.jobs, source)
        tracked = tracked_counts(scan_repo(TRACKED_DIRS, a.jobs, source))
        rc = check(res, base, a.samples, use_git_diff=(source == "worktree"), tracked=tracked)
        if rc == 0 and a.tighten:
            nb = make_baseline(res, dirs, tracked, previous=base)
            with open(a.baseline, "w", encoding="utf-8", newline="\n") as fh:
                json.dump(nb, fh, indent=1, sort_keys=True)
                fh.write("\n")
            print("cxx17_ratchet: baseline tightened")
        return rc
    res = scan_repo(dirs, a.jobs, source)
    if a.write_baseline:
        nb = make_baseline(res, dirs, tracked_counts(scan_repo(TRACKED_DIRS, a.jobs, source)))
        with open(a.baseline, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(nb, fh, indent=1, sort_keys=True)
            fh.write("\n")
        print_table(nb["counts"], "baseline written: %s (head %s, %s, %d files)" % (a.baseline, nb["head"], source, len(res)))
        return 0
    print_table(aggregate(res, a.depth), "cxx17_ratchet report (%d files, %s)" % (len(res), source))
    if a.by_file:
        for i in IDS:
            rows = sorted(((len(h.get(i, [])), r) for r, h in res.items() if h.get(i)), reverse=True)[:a.by_file]
            print("\n%s top files:" % i)
            for n, r in rows:
                print("  %5d  %s" % (n, r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
