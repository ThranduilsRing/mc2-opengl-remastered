#!/usr/bin/env python3
"""log_tag_census - the set of structured log lines the engine emits, frozen so a conversion cannot change one.

WHAT: scans the tracked C++ sources for calls that write a `[TAG vN] key=...` (or `[TAG] ...`) line -- printf, fprintf, puts, SPEW, the log_*
macros, sprintf-style buffers (snprintf, mc2::fmt::to_buf*), and the facade's MC2_EVENT* / mc2::log::event<> -- reads the format literal across
continuation lines and adjacent literals, and records (tag, version, stream, key order) with a count. tests/unit/log_tag_baseline.json holds
the frozen census plus the tags the Python parsers consume.
WHY: the smoke harness greps these lines (logparse.py, oracleparse.py, run_smoke.py, ...). Converting printf to std::format must not change a
tag, a version, the key order or the stream; a line-by-line grep misses calls whose format string sits on a continuation line, so this reads
calls. A converted site must produce the same record as the printf it replaced.

  py -3 tools/log_tag_census.py --check     # exit 1 on any difference from the baseline (the board row)
  py -3 tools/log_tag_census.py --write     # re-freeze (after an intended change)
  py -3 tools/log_tag_census.py --report    # print the census
"""
from __future__ import annotations

import argparse
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
CPP_EXT = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".inl")
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
LOG_MACROS = {"log_debug", "log_info", "log_warning", "log_error"}

TAG = re.compile(r"^\s*\[([A-Z][A-Z0-9_]*)(?: v(\d+))?\]")
KEY = re.compile(r"(?<![\w\x01])([A-Za-z_][\w.]*)=")
# A printf conversion and a std::format replacement field are the same thing for the census: a placeholder. Without this a key whose NAME is a
# placeholder (`%s=%u` becoming `{}={:d}`) would look like a different key list after a conversion.
_PLACEHOLDER = re.compile(r"%%|\{\{|\}\}|%[-+ #0-9.*]*(?:hh|h|ll|l|z|j|t|L|I64|I32)?[A-Za-z]|\{[^{}]*\}")
_LITERAL = {"%%": "%", "{{": "{", "}}": "}"}
_NAME_RE = "|".join(sorted(list(PRINTF_LIKE) + list(EVENT_MACROS) + sorted(LOG_MACROS) + ["SPEW"], key=len, reverse=True))
_EVENT_TMPL = re.compile(r'event\s*<\s*"([A-Z][A-Z0-9_]*)"\s*,\s*(-?\d+)\s*>\s*\(\s*(?:::)?(?:mc2::log::)?Stream::(Err|Out|Dbg)\s*,\s*((?:"(?:\\.|[^"\\\n])*"\s*)+)')


def normalise(literal: str) -> str:
    """The format literal with every conversion/replacement field replaced by one placeholder character and escaped braces/percents unescaped."""
    return _PLACEHOLDER.sub(lambda m: _LITERAL.get(m.group(0), "\x01"), literal)


def keys_of(literal: str):
    """Key names in order of appearance in a format literal (`key=` tokens), placeholders ignored."""
    return KEY.findall(normalise(literal))


def record(tag, ver, stream, literal):
    return (tag, -1 if ver is None else int(ver), stream, tuple(keys_of(literal)))


def stream_of(callee: str, call, text: str):
    idx_stream, _idx_fmt, fixed = PRINTF_LIKE[callee]
    if fixed:
        return fixed
    arg = call.arg_text(text, idx_stream).strip()
    return {"stderr": "stderr", "stdout": "stdout"}.get(arg, "file")


def census_of_text(text: str):
    """Counter of (tag, version, stream, keys) for the tagged lines the text emits."""
    out = collections.Counter()
    if not re.search(r'\[[A-Z][A-Z0-9_]*(?: v\d+)?\]|MC2_EVENT|event<', text):
        return out
    for m in _EVENT_TMPL.finditer(text):
        lit = cxx_calls.string_literal_parts(m.group(4)) or ""
        out[record(m.group(1), m.group(2), {"Err": "stderr", "Out": "stdout", "Dbg": "debugger"}[m.group(3)], lit)] += 1
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
                out[(tag, -1 if ver.startswith("-") or not ver.lstrip("-").isdigit() else int(ver), EVENT_MACROS[name], tuple(keys_of(lit)))] += 1
            continue
        if name in PRINTF_LIKE:
            idx = PRINTF_LIKE[name][1]
            if len(call.args) <= idx:
                continue
            lit = cxx_calls.string_literal_parts(call.arg_text(text, idx))
            stream = stream_of(name, call, text)
        else:               # SPEW((group, "fmt", ...)) and log_*("fmt", ...): the first literal argument
            lit, stream = None, "spew" if name == "SPEW" else "log"
            for i in range(len(call.args)):
                lit = cxx_calls.string_literal_parts(call.arg_text(text, i).strip("() "))
                if lit is not None:
                    break
        if lit is None:
            continue
        t = TAG.match(lit)
        if t:
            out[record(t.group(1), t.group(2), stream, lit[t.end():])] += 1
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


PY_TAG = re.compile(r"\\?\[([A-Z][A-Z0-9_]{2,})(?:\s+v(?:\\d|\d)[+*]?)?\\?\]")


def consumed_tags():
    """Tags the Python parsers and checkers mention inside a regex or comparison, sorted."""
    tags = set()
    for f in tracked((".py",)):
        if not f.startswith(("scripts/", "tools/")):
            continue
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                if re.search(r"re\.|grep|startswith|findall|\bin (?:line|text|out)|search|match", ln):
                    tags.update(m.group(1) for m in PY_TAG.finditer(ln))
    return sorted(tags)


def to_json(counter, consumed):
    rows = [{"tag": t, "ver": v, "stream": s, "keys": list(k), "count": n} for (t, v, s, k), n in sorted(counter.items())]
    return {"about": "Frozen by tools/log_tag_census.py --write. Emitters are (tag, version, stream, key order) with a count; "
                     "consumed_tags are the tags Python parsers/checkers mention.", "emitters": rows, "consumed_tags": consumed}


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
