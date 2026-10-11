"""printf_log - rewrite untagged printf/fprintf logging calls to mc2::fmt::print_out / print_err / print (LG12a, step 3 B4), as a text transform.

WHAT: `printf("fmt", args)`, `fprintf(stderr|stdout|FILE*, "fmt", args)` become `mc2::fmt::print_out/print_err/print(...)` with the
std::format string and wrapped arguments of printf_buffers.translate_format. The text goes out as one fwrite to the same FILE*, nothing added.
Which calls are converted depends on `classes`: "untagged" (B4: no bracketed tag in the format), "unconsumed" (B5: a `[TAG vN]` line whose tag no
Python parser or checker mentions) and "consumed" (B6: a tag the harness parses; set MC2_PRINTF_LOG_TAG=NAME[,NAME...] to take only those tags). The text
is byte-identical in every class (the corpus proves it per format); the classes only order the risk. The value is kept: mc2::fmt::print returns what printf returns. Left alone: a format that is not a literal; vfprintf/puts/fputs/SPEW/OutputDebugString;
and files on the crash / allocator / assert paths (EXCLUDED_FILES), where building a std::string is not allowed.
WHY: same as printf_buffers: compile-time-checked formats, no vararg mismatches. Byte equality of the text is proved by the format corpus (shared with
printf_buffers) and by the game-log comparison.
"""
from __future__ import annotations

import fnmatch
import json
import os
import re

import cxx_calls
import printf_buffers as pb

FUNCS = {"printf", "fprintf"}
PATH_AWARE = True      # rewrite(text, path): EXCLUDED_FILES need the path
#: Crash handlers, assert/STOP paths and allocator internals must not allocate or recurse into themselves while printing.
EXCLUDED_FILES = ("*crashbundle*", "*gameos_debugging*", "*/err.cpp", "*/err.h", "*heap*", "*errorhandler*", "*/memorymanager*", "*seh*")
_BRACKET_TAG = re.compile(r"\[([A-Z][A-Z0-9_]*)(?: v\d+)?\]")      # the same shape the log_tag_census recognises, anywhere in the literal
BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "unit", "log_tag_baseline.json")
_consumed = None
_plain = None


def _baseline() -> dict:
    with open(BASELINE, encoding="utf-8") as fh:
        return json.load(fh)


def consumed_tags() -> frozenset:
    """Tags a Python parser or checker reads (tests/unit/log_tag_baseline.json, written by tools/log_tag_census.py --write)."""
    global _consumed
    if _consumed is None:
        _consumed = frozenset(_baseline()["consumed_tags"])
    return _consumed


def plain_tags() -> frozenset:
    """Lines a harness reads that are not `[UPPER vN]` shaped (VEG_SUMMARY, `[SHADER ERROR]`, ...); all of them are consumed."""
    global _plain
    if _plain is None:
        _plain = frozenset(_baseline().get("plain_tags", []))
    return _plain


def tags_of(literal: str) -> set:
    """Every tag name a format literal carries, bracketed or plain."""
    return set(_BRACKET_TAG.findall(literal)) | {p for p in plain_tags() if p in literal}


def tag_class(literal: str) -> str:
    """"untagged", "unconsumed" or "consumed" for a format literal."""
    tags = tags_of(literal)
    if not tags:
        return "untagged"
    return "consumed" if tags & (consumed_tags() | plain_tags()) else "unconsumed"


def is_excluded(path: str) -> bool:
    return any(fnmatch.fnmatch(path.replace("\\", "/"), pat) for pat in EXCLUDED_FILES)


def _sink_of(arg: str):
    a = arg.strip()
    if a == "stderr":
        return "print_err", None
    if a == "stdout":
        return "print_out", None
    if re.fullmatch(r"[A-Za-z_][\w.>\-\[\]]*", a):
        return "print", a
    raise pb.Bail("stream argument is not a plain FILE* expression")


def _convert_call(text: str, call, classes=("untagged",)):
    args = [call.arg_text(text, i) for i in range(len(call.args))]
    if call.name == "printf":
        if not args:
            raise pb.Bail("printf arity")
        fn, stream, fmt, rest = "print_out", None, args[0], args[1:]
    else:
        if len(args) < 2:
            raise pb.Bail("fprintf arity")
        fn, stream = _sink_of(args[0])
        fmt, rest = args[1], args[2:]
    lit = cxx_calls.string_literal_parts(fmt, pb.MACROS)
    if lit is None:
        raise pb.Bail("format is not a string literal")
    cls = tag_class(lit)
    if cls not in classes:
        raise pb.Bail("tagged line: " + cls)
    only = os.environ.get("MC2_PRINTF_LOG_TAG")
    if only and cls != "untagged" and not (set(only.split(",")) & tags_of(lit)):
        raise pb.Bail("tagged line: another tag")
    new_fmt, wrapped, specs = pb.translate_format(lit, rest)
    shown_old, shown_new = lit, new_fmt
    for macro, ch in pb.MACROS.items():
        new_fmt = new_fmt.replace(ch, '" ' + macro + ' "')
        shown_old, shown_new = shown_old.replace(ch, "/"), shown_new.replace(ch, "/")
    fmt_at = 0 if call.name == "printf" else 1
    pieces = ([(0, stream)] if stream else []) + [(fmt_at, f'"{new_fmt}"')] + [(fmt_at + 1 + k, w) for k, w in enumerate(wrapped)]
    return call.rebuild(text, f"mc2::fmt::{fn}", pieces), {"func": call.name, "old": shown_old, "new": shown_new, "specs": specs}


def convert(text: str, path: str = "", classes=("untagged",)):
    """-> (new text, records, [(line, reason)] for calls left alone)."""
    if path and is_excluded(path):
        return text, [], []
    edits, records, left = [], [], []
    for call in cxx_calls.find_calls(text, FUNCS):
        line = text.count("\n", 0, call.start) + 1
        try:
            repl, rec = _convert_call(text, call, classes)
        except pb.Bail as e:
            left.append((line, str(e)))
            continue
        rec["line"] = line
        records.append(rec)
        edits.append((call.start, call.end, repl))
    if not edits:
        return text, [], left
    out = text
    for a, b, r in sorted(edits, reverse=True):
        out = out[:a] + r + out[b:]
    if "mc2fmt.h" not in out:
        at = pb._include_insertion_point(out, min(a for a, _b, _r in edits))
        if at is None:
            return text, [], left + [(0, "no #include to anchor mc2fmt.h on")]
        nl = "\r\n" if "\r\n" in out[:2000] else "\n"
        if not out[:at].endswith("\n"):
            out = out[:at] + nl + out[at:]
            at += len(nl)
        out = out[:at] + pb.INCLUDE + nl + out[at:]
    return out, records, left


def rewrite(text: str, path: str = "") -> str:
    return convert(text, path)[0]
