"""printf_buffers - rewrite sprintf/snprintf-style buffer formatting to mc2::fmt::to_buf (LG12b), as a text transform.

WHAT: `snprintf(buf, N, "fmt", args)`, `sprintf(buf, "fmt", args)` and `_snprintf_s(buf, N, _TRUNCATE, "fmt", args)` become
`mc2::fmt::to_buf(buf, "{...}", wrapped args)` (array destination, bound taken from the array type) or `mc2::fmt::to_buf_n(buf, N, ...)`.
Each printf conversion maps to a std::format spec; integer arguments are wrapped in mc2::fmt::as_i32/as_u32/... (the varargs meaning of the
conversion, rejecting non-integers at compile time) and `%s` arguments in mc2::fmt::safe() unless the argument cannot be null.
WHY: the unbounded sprintf and the unchecked specifier/argument pairing are the memory-safety and correctness gap. Anything this module cannot
prove byte-equivalent is left as it is and reported (see `convert`'s second result).
A conversion that the printf grammar allows but the mapping does not cover (precision on an integer or string, `#`, `*`, h/hh/j/t/L/I64
lengths, `%n`, `%a`, wide strings) leaves the whole call alone.
"""
from __future__ import annotations

import re

import cxx_calls
import typedef_aggregates as _ta

FUNCS = {"snprintf", "_snprintf", "sprintf", "_snprintf_s"}
INCLUDE = '#include "mc2fmt.h"'
#: String-literal macros that may appear in a format ("data" PATH_SEPARATOR "x.var"): each is carried through the translation as one
#: private-use character and written back as the macro. mclib/paths.h defines it as "/" or "\\"; neither has a conversion or a brace.
MACROS = {"PATH_SEPARATOR": "\ue000"}

_SPEC = re.compile(r"%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d*))?(hh|h|ll|l|z|j|t|L|I64|I32|I)?([diouxXeEfFgGaAcspnS%])")


class Bail(Exception):
    """This call cannot be converted byte-exactly; the message is the reason."""


def _prev_token_allows_statement(text: str, pos: int) -> bool:
    """True when the expression starting at pos is a full statement (its value is discarded)."""
    i = pos - 1
    while i >= 0 and text[i].isspace():
        i -= 1
    if i < 0:
        return True
    if text[i] in ";{})" or text[i] == ":":
        return True
    return bool(re.search(r"\b(?:else|do)$", text[max(0, i - 4):i + 1]))


def _escape_braces(lit: str) -> str:
    return lit.replace("{", "{{").replace("}", "}}")


def arg_class_of_s(expr: str) -> str:
    """Syntactic class of a `%s` argument: how likely it is to be null."""
    e = expr.strip()
    if cxx_calls.string_literal_parts(e) is not None:
        return "literal"
    if re.search(r"\.c_str\(\)$", e) or re.search(r"(?:\.|->)Data\(\)$", e) or re.fullmatch(r"std::string\(.*\)\.c_str\(\)", e):
        return "nonnull-accessor"
    return "maybe-null"


def translate_format(lit: str, args: list):
    """printf format literal contents + argument expressions -> (std::format string contents, wrapped args, spec records)."""
    out, wrapped, specs, pos, ai = [], [], [], 0, 0
    for m in _SPEC.finditer(lit):
        out.append(_escape_braces(lit[pos:m.start()]))
        pos = m.end()
        flags, width, prec, length, conv = m.group(1), m.group(2), m.group(3), m.group(4) or "", m.group(5)
        if conv == "%":
            if flags or width or prec is not None or length:
                raise Bail("%% with modifiers")
            out.append("%")
            continue
        if width == "*" or prec == "*":
            raise Bail("* width or precision")
        if "#" in flags:
            raise Bail("# flag")
        if conv in "nSaA":
            raise Bail("conversion %" + conv)
        if length in ("hh", "h", "j", "t", "L", "I64", "I32", "I"):
            raise Bail("length modifier " + length)
        if ai >= len(args):
            raise Bail("fewer arguments than conversions")
        expr = args[ai]
        ai += 1
        left = "-" in flags
        zero = "0" in flags and not left
        sign = "+" if "+" in flags else (" " if " " in flags else "")
        w = width or ""
        spec_type, wrap, cls = "", None, None
        if conv in "di":
            if prec is not None:
                raise Bail("precision on an integer")
            if length in ("", "l"):
                wrap = "as_i32"
            elif length == "ll":
                wrap = "as_i64"
            else:
                raise Bail("%z" + conv)
            spec_type = "d"
        elif conv in "uxXo":
            if prec is not None:
                raise Bail("precision on an integer")
            wrap = {"": "as_u32", "l": "as_u32", "ll": "as_u64", "z": "as_size"}.get(length)
            if wrap is None:
                raise Bail("length " + length + " on %" + conv)
            spec_type = "d" if conv == "u" else conv
            if conv == "u" and sign:
                raise Bail("sign flag on %u")
        elif conv == "c":
            if length or prec is not None or zero or sign:
                raise Bail("modifiers on %c")
            wrap, spec_type = "as_char", "c"
        elif conv in "eEfFgG":
            if length not in ("", "l"):
                raise Bail("length on a float conversion")
            spec_type = conv
        elif conv == "p":
            if flags or width or prec is not None or length:
                raise Bail("modifiers on %p")
            wrap, spec_type, w, zero = "as_ptr", "016X", "", False
        elif conv == "s":
            if length or prec is not None or zero or sign:
                raise Bail("modifiers on %s")
            cls = arg_class_of_s(expr)
            wrap = None if cls in ("literal", "nonnull-accessor") else "safe"
        else:
            raise Bail("conversion %" + conv)
        # std::format spec: [[fill]align][sign][0][width][.prec][type]
        s = ""
        if conv == "s":
            if w:
                s = ("<" if left else ">") + w
        elif conv == "p":
            s = "016X"
        else:
            if left:
                s += "<"
            s += sign
            if zero:
                s += "0"
            s += w
            if prec is not None and conv in "eEfFgG":
                s += "." + (prec or "0")
            s += spec_type
        out.append("{" + (":" + s if s else "") + "}")
        wrapped.append(f"mc2::fmt::{wrap}({expr})" if wrap else expr)
        specs.append({"conv": conv, "length": length, "flags": flags, "width": width or "", "prec": prec if prec is not None else "",
                      "wrap": wrap or "", "cls": cls or ""})
    out.append(_escape_braces(lit[pos:]))
    if ai != len(args):
        raise Bail("more arguments than conversions")
    return "".join(out), wrapped, specs


def array_declared(text: str, name: str, pos: int) -> bool:
    """True when the nearest declaration of `name` before `pos` is `char name[...]` of a local, member or file-scope array (not a parameter,
    whose array type decays to a pointer). Textual, so it only ever answers True when it can see the declaration."""
    if not re.fullmatch(r"[A-Za-z_]\w*", name):
        return False
    last = None
    for m in re.finditer(r"\b(?:unsigned\s+|signed\s+)?char\s+(?:\w+\s+)*" + re.escape(name) + r"\s*\[[^\]]*\]\s*([;={,)])", text[:pos]):
        last = m
    if last is None or last.group(1) in (",", ")"):
        return False
    ptr = None
    for m in re.finditer(r"\b(?:const\s+)?(?:unsigned\s+|signed\s+)?char\s*\*+\s*(?:const\s+)?" + re.escape(name) + r"\b", text[:pos]):
        ptr = m
    return ptr is None or ptr.start() < last.start()


def _convert_call(text: str, call, assume_array: bool = False):
    """-> (replacement text, record) for one call, or raises Bail."""
    args = [call.arg_text(text, i) for i in range(len(call.args))]
    name = call.name
    statement = _prev_token_allows_statement(text, call.start) and text[call.end:call.end + 40].lstrip().startswith(";")
    if name == "sprintf":
        if len(args) < 2:
            raise Bail("sprintf arity")
        dest, size, fmt, rest, bounded = args[0], None, args[1], args[2:], False
    elif name in ("snprintf", "_snprintf"):
        if len(args) < 3:
            raise Bail(name + " arity")
        dest, size, fmt, rest = args[0], args[1], args[2], args[3:]
        if name == "_snprintf" and not statement:
            raise Bail("_snprintf value used (MSVC truncation semantics differ)")
    elif name == "_snprintf_s":
        if len(args) < 4 or args[2].strip() != "_TRUNCATE":
            raise Bail("_snprintf_s without _TRUNCATE")
        if not statement:
            raise Bail("_snprintf_s value used")
        dest, size, fmt, rest = args[0], args[1], args[3], args[4:]
    else:
        raise Bail("function " + name)
    lit = cxx_calls.string_literal_parts(fmt, MACROS)
    if lit is None:
        raise Bail("format is not a string literal")
    new_fmt, wrapped, specs = translate_format(lit, rest)
    shown_old, shown_new = lit, new_fmt
    for macro, ch in MACROS.items():
        new_fmt = new_fmt.replace(ch, '" ' + macro + ' "')
        shown_old, shown_new = shown_old.replace(ch, "/"), shown_new.replace(ch, "/")
    is_array = assume_array or array_declared(text, dest.strip(), call.start)
    if size is not None and re.fullmatch(r"sizeof\s*\(?\s*%s\s*\)?" % re.escape(dest.strip()), size.strip()) and is_array:
        size = None             # the bound is the array's own extent
    if size is None and not is_array:
        if name != "sprintf":
            raise Bail("internal: size lost")
        raise Bail("sprintf destination is not a visible char array")
    fmt_at = {"sprintf": 1, "snprintf": 2, "_snprintf": 2, "_snprintf_s": 3}[name]
    pieces = [(0, dest)] + ([(1, size)] if size is not None else []) + [(fmt_at, f'"{new_fmt}"')]
    pieces += [(fmt_at + 1 + k, w) for k, w in enumerate(wrapped)]
    call_text = call.rebuild(text, "mc2::fmt::to_buf" if size is None else "mc2::fmt::to_buf_n", pieces)
    return call_text, {"func": name, "old": shown_old, "new": shown_new, "specs": specs}


def _include_insertion_point(text: str, before: int):
    """Offset to insert `#include "mc2fmt.h"`: after the last top-level #include that precedes offset `before` (the first converted call),
    or None when there is none. A file may include more headers (even .cpp pieces) after code; those come too late."""
    depth, last = 0, None
    for m in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|endif|include)\b[^\n]*\n?", text, re.M):
        if m.start() >= before:
            break
        kind = m.group(1)
        if kind in ("if", "ifdef", "ifndef"):
            depth += 1
        elif kind == "endif":
            depth = max(0, depth - 1)
        elif depth == 0:
            last = m.end()
    return last


def convert(text: str, assume_array: bool = False, only_lines=None):
    """-> (new text, records of converted calls, [(approx line, reason)] for calls left alone).

    assume_array: treat every sprintf destination as an array and let the compiler judge (to_buf does not accept a pointer), for the
    per-site trial in printf_sites.py. only_lines: convert only the calls that start on these (1-based, original) lines."""
    edits, records, left = [], [], []
    for call in cxx_calls.find_calls(text, FUNCS):
        line = text.count("\n", 0, call.start) + 1
        if only_lines is not None and line not in only_lines:
            continue
        try:
            repl, rec = _convert_call(text, call, assume_array)
        except Bail as e:
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
        at = _include_insertion_point(out, min(a for a, _b, _r in edits))
        if at is None:
            return text, [], left + [(0, "no #include to anchor mc2fmt.h on")]
        nl = "\r\n" if "\r\n" in out[:at + 2] or "\r\n" in out[:2000] else "\n"
        if not out[:at].endswith("\n"):
            out = out[:at] + nl + out[at:]
            at += len(nl)
        out = out[:at] + INCLUDE + nl + out[at:]
    return out, records, left


def rewrite(text: str) -> str:
    return convert(text)[0]
