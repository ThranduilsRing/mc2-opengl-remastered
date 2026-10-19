#!/usr/bin/env python3
"""log_tag_census - the set of structured log lines the engine emits, frozen so a conversion cannot change one.

WHAT: scans tracked C++ sources and .cpp.in templates for calls that write a `[TAG vN] key=...` (or `[TAG] ...`) line -- printf, fprintf, puts, SPEW, the log_*
macros, sprintf-style buffers (snprintf, mc2::fmt::to_buf*), and the facade's MC2_EVENT* / mc2::log::event<> -- reads the format literal across
continuation lines and adjacent literals, and records (tag, version, stream, key order) with a count. tests/unit/log_tag_baseline.json holds
the frozen census plus the tags the Python parsers consume.
WHY: the smoke harness greps these lines (logparse.py, run_smoke.py, ...). Converting printf to std::format must not change a
tag, a version, the key order or the stream; a line-by-line grep misses calls whose format string sits on a continuation line, so this reads
calls. A converted site must produce the same record as the printf it replaced.

  py -3 tools/log_tag_census.py --check     # exit 1 on any difference from the baseline (the board row)
  py -3 tools/log_tag_census.py --write     # re-freeze (after an intended change)
  py -3 tools/log_tag_census.py --report    # print the census
"""
from __future__ import annotations

import argparse
import ast
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "tidy"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402
import cxx_calls  # noqa: E402

BASELINE = os.path.join(ROOT, "tests", "unit", "log_tag_baseline.json")
CPP_EXT = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".inl", ".inc", ".cpp.in")
SKIP_PREFIX = ("3rdparty/", "build", "tests/")

# callee -> (index of the stream/FILE argument or None, index of the format argument, stream name when fixed)
PRINTF_LIKE = {
    "printf": (None, 0, "stdout"), "puts": (None, 0, "stdout"),
    "fprintf": (0, 1, None), "fputs": (None, 0, "file"), "vfprintf": (0, 1, None),
    "OutputDebugStringA": (None, 0, "debugger"), "OutputDebugString": (None, 0, "debugger"),
    "snprintf": (None, 2, "buffer"), "_snprintf": (None, 2, "buffer"), "sprintf": (None, 1, "buffer"),
    "_snprintf_s": (None, 3, "buffer"), "to_buf": (None, 1, "buffer"), "to_buf_n": (None, 2, "buffer"),
    "print_err": (None, 0, "stderr"), "print_out": (None, 0, "stdout"), "print": (0, 1, None),
}
EVENT_MACROS = {"MC2_EVENT": "stderr", "MC2_EVENT_OUT": "stdout", "MC2_EVENT_NOVER": "stderr", "MC2_EVENT_NOVER_OUT": "stdout"}
LOG_MACROS = {"MC2_LOG", "MC2_LOG_OUT", "MC2_LOG_DBG", "SHADER_LOG_ERROR"}

#: Lines a harness reads that are not `[UPPER_CASE vN]` shaped: a bare leading word, or a bracket with a space or lower case in it.
PLAIN_TAGS = ("VEG_SUMMARY", "ORDER_LIVENESS_SUMMARY", "TerrainLOD parity", "SHADER ERROR")
_PLAIN = "|".join(re.escape(p) for p in PLAIN_TAGS)
TAG = re.compile(r"^\s*(?:\[([A-Z][A-Z0-9_]*)(?: v(\d+))?\]|\[(%s)\]|(%s)\b)" % (_PLAIN, _PLAIN))
KEY = re.compile(r"(?<![\w\x01])([A-Za-z_][\w.]*)=")
# A printf conversion and a std::format replacement field are the same thing for the census: a placeholder. Without this a key whose NAME is a
# placeholder (`%s=%u` becoming `{}={:d}`) would look like a different key list after a conversion.
_PRINTF_FIELD = re.compile(r"%%|%[-+ #0-9.*]*(?:hh|h|ll|l|z|j|t|L|I64|I32)?[A-Za-z]")
_STD_FIELD = re.compile(r"\{\{|\}\}|\{[^{}]*\}")
_LITERAL = {"%%": "%", "{{": "{", "}}": "}"}
#: callees whose format is a std::format string; every other format in the census is a printf one (a `{` in it is just a brace)
STD_FORMAT_CALLEES = {"to_buf", "to_buf_n", "print_err", "print_out", "print"} | LOG_MACROS
_NAME_RE = "|".join(sorted(list(PRINTF_LIKE) + list(EVENT_MACROS) + sorted(LOG_MACROS) + ["SPEW"], key=len, reverse=True))
_EVENT_TMPL = re.compile(r'event\s*<\s*"([A-Z][A-Z0-9_]*)"\s*,\s*(-?\d+)\s*>\s*\(\s*(?:::)?(?:mc2::log::)?Stream::(Err|Out|Dbg)\s*,\s*((?:"(?:\\.|[^"\\\n])*"\s*)+)')


def normalise(literal: str, std: bool = False) -> str:
    """The format literal with every conversion/replacement field replaced by one placeholder character and escaped braces/percents unescaped.

    `std` selects the dialect: a std::format string has {} fields and {{ }} escapes (a % is a plain character); a printf string has %-conversions
    (and a { is a plain character, e.g. `mismatch{gameplay=%d}`)."""
    return (_STD_FIELD if std else _PRINTF_FIELD).sub(lambda m: _LITERAL.get(m.group(0), "\x01") if not std else
                                                      _LITERAL.get(m.group(0), "\x01"), literal)


def keys_of(literal: str, std: bool = False):
    """Key names in order of appearance in a format literal (`key=` tokens), placeholders ignored."""
    return KEY.findall(normalise(literal, std))


def record(tag, ver, stream, literal, std: bool = False):
    return (tag, -1 if ver is None else int(ver), stream, tuple(keys_of(literal, std)))


def stream_of(callee: str, call, text: str):
    idx_stream, _idx_fmt, fixed = PRINTF_LIKE[callee]
    if fixed:
        return fixed
    arg = call.arg_text(text, idx_stream).strip()
    return {"stderr": "stderr", "stdout": "stdout"}.get(arg, "file")


def census_of_text(text: str):
    """Counter of (tag, version, stream, keys) for the tagged lines the text emits."""
    out = collections.Counter()
    if not re.search(r'\[[A-Z][A-Z0-9_]*(?: v\d+)?\]|MC2_EVENT|event<|' + _PLAIN, text):
        return out
    for m in _EVENT_TMPL.finditer(text):
        lit = cxx_calls.string_literal_parts(m.group(4)) or ""
        out[record(m.group(1), m.group(2), {"Err": "stderr", "Out": "stdout", "Dbg": "debugger"}[m.group(3)], lit, True)] += 1
    names = set(PRINTF_LIKE) | set(EVENT_MACROS) | LOG_MACROS | {"SPEW"}
    for call in cxx_calls.find_calls(text, names, ("std::", "mc2::fmt::")):
        name = call.name
        if name in EVENT_MACROS:
            if len(call.args) < 4:
                continue
            tag = cxx_calls.string_literal_parts(call.arg_text(text, 1))
            ver = call.arg_text(text, 2).strip()
            lit = cxx_calls.string_literal_parts(call.arg_text(text, 3)) or ""
            if tag:
                out[(tag, -1 if ver.startswith("-") or not ver.lstrip("-").isdigit() else int(ver), EVENT_MACROS[name], tuple(keys_of(lit, True)))] += 1
            continue
        if name in PRINTF_LIKE:
            idx = PRINTF_LIKE[name][1]
            if len(call.args) <= idx:
                continue
            lit = cxx_calls.string_literal_parts(call.arg_text(text, idx))
            stream = stream_of(name, call, text)
        else:               # SPEW((group, "fmt", ...)) and the facade's free-form MC2_LOG(cat, level, "fmt", ...): the first literal argument
            lit, stream = None, "spew" if name == "SPEW" else "log"
            for i in range(len(call.args)):
                lit = cxx_calls.string_literal_parts(call.arg_text(text, i).strip("() "))
                if lit is not None:
                    break
        if lit is None:
            continue
        t = TAG.match(lit)
        if t:
            out[record(t.group(1) or t.group(3) or t.group(4), t.group(2), stream, lit[t.end():], name in STD_FORMAT_CALLEES)] += 1
    return out


def tracked(exts):
    r = subprocess.run(["git", "-C", ROOT, "ls-files"], stdout=subprocess.PIPE, text=True, errors="replace",
                       **console_child.child_kwargs(redirected=True))
    return [f for f in r.stdout.split() if f.endswith(exts) and not f.startswith(SKIP_PREFIX)]


def census():
    out = collections.Counter()
    for f in tracked(CPP_EXT):
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            out += census_of_text(fh.read())
    return out


#: A tag inside a Python string: `[TAG`, `\[TAG` (a regex), with or without the closing bracket and version (`"[VISUAL_CAPTURE" in log`).
PY_TAG = re.compile(r"\\?\[([A-Z][A-Z0-9_]{2,})(?=[\s\]\\]|$)")
#: Calls and names through which a harness file READS a log line. A tag that only appears in print()/emit()/write() is not consumed.
CONSUMER_CALLS = frozenset({"compile", "search", "match", "fullmatch", "findall", "finditer", "sub", "split", "startswith", "endswith", "find", "rfind",
                            "index", "count"})
CONSUMER_NAMES = re.compile(r"PATTERN|PREFIX|MARKER|TAG|NEEDLE|_RE\b|REGEX|SIGNATURE|KEYWORD|CANAR", re.I)
#: Files that quote tags as sample data or emit them: never consumers.
NOT_CONSUMERS = ("tools/tidy/", "tools/log_tag_census.py", "tools/log_identity.py", "tools/test_log_tag_census.py", "tools/test_log_identity.py")


def _strings(node):
    for n in ast.walk(node):
        if isinstance(n, ast.Constant) and isinstance(n.value, str):
            yield n.value


def consumed_in_source(text: str) -> set:
    """Tags one Python source reads, found on the syntax tree so a regex continued over lines or an adjacent-literal pattern is seen whole.

    Consumed means: a string argument of a search/match/startswith/... call; a string compared with `in`, `not in` or `==`; a string held by a module-level
    constant whose name says pattern/prefix/marker/needle/canary, or any module-level tuple/list/set of strings that start with `[`."""
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return set()
    found = set()

    def take(strings):
        for s in strings:
            found.update(PY_TAG.findall(s))
            found.update(p for p in PLAIN_TAGS if p in s)

    for n in ast.walk(tree):
        if isinstance(n, ast.Call):
            f = n.func
            if (f.attr if isinstance(f, ast.Attribute) else getattr(f, "id", "")) in CONSUMER_CALLS:
                for a in list(n.args) + [k.value for k in n.keywords]:
                    take(_strings(a))
        elif isinstance(n, ast.Compare) and any(isinstance(o, (ast.In, ast.NotIn, ast.Eq)) for o in n.ops):
            take(_strings(n))
    for n in tree.body:
        if isinstance(n, (ast.Assign, ast.AnnAssign)) and n.value is not None:
            targets = n.targets if isinstance(n, ast.Assign) else [n.target]
            strings = list(_strings(n.value))
            named = any(CONSUMER_NAMES.search(getattr(t, "id", "")) for t in targets)
            listed = isinstance(n.value, (ast.Tuple, ast.List, ast.Set)) and strings and all(s.startswith("[") for s in strings)
            if named or listed:
                take(strings)
    return found


def consumed_tags():
    """Tags the Python parsers and checkers read (see consumed_in_source), sorted."""
    tags = set()
    for f in tracked((".py",)):
        if not f.startswith(("scripts/", "tools/")) or f.startswith(NOT_CONSUMERS) or os.path.basename(f).startswith("test_"):
            continue
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            tags |= consumed_in_source(fh.read())
    return sorted(tags)


def to_json(counter, consumed):
    rows = [{"tag": t, "ver": v, "stream": s, "keys": list(k), "count": n} for (t, v, s, k), n in sorted(counter.items())]
    return {"about": "Frozen by tools/log_tag_census.py --write. Emitters are (tag, version, stream, key order) with a count; "
                     "consumed_tags are the tags Python parsers/checkers read; plain_tags are the non-[UPPER vN] lines among them.", "emitters": rows,
            "consumed_tags": consumed, "plain_tags": list(PLAIN_TAGS)}


def diff(counter, baseline):
    base = collections.Counter({(r["tag"], r["ver"], r["stream"], tuple(r["keys"])): r["count"] for r in baseline["emitters"]})
    lines = []
    for k in sorted(set(base) | set(counter)):
        if base[k] != counter[k]:
            lines.append(f"  {'[%s v%s]' % (k[0], k[1]) if k[1] >= 0 else '[%s]' % k[0]} stream={k[2]} keys={','.join(k[3])}: baseline {base[k]}, now {counter[k]}")
    return lines


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    g.add_argument("--report", action="store_true")
    args = ap.parse_args(argv)
    now = census()
    if args.report:
        for (t, v, s, k), n in sorted(now.items()):
            print(f"{n:4d}  [{t}{'' if v < 0 else ' v%d' % v}] {s}  {','.join(k)}")
        print(f"log_tag_census: {sum(now.values())} tagged emitter(s), {len(now)} distinct")
        return 0
    if args.write:
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(to_json(now, consumed_tags()), fh, indent=1, sort_keys=False)
            fh.write("\n")
        print(f"log_tag_census: froze {sum(now.values())} emitter(s), {len(now)} distinct, into {os.path.relpath(BASELINE, ROOT)}")
        return 0
    with open(BASELINE, encoding="utf-8") as fh:
        baseline = json.load(fh)
    d = diff(now, baseline)
    if d:
        print("log_tag_census: the structured log lines changed (tag, version, stream or key order):")
        print("\n".join(d))
        print("If this is intended, re-freeze with `py -3 tools/log_tag_census.py --write` in the same commit as the parser change.")
        return 1
    print(f"log_tag_census: OK ({sum(now.values())} tagged emitters, {len(now)} distinct)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
