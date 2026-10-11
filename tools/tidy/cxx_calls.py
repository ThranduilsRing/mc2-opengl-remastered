"""cxx_calls - find function-call expressions in C++ source text, with their top-level arguments.

WHAT: `find_calls(text, names)` yields every call `name(...)` (optionally `std::name`) that is not inside a comment, string literal or preprocessor
line, with the byte span of the whole call, of each argument, and of the callee. `string_literal_parts(arg)` returns the contents (source text
between the quotes, escapes untouched) of an argument that is nothing but adjacent narrow string literals, else None.
WHY: the printf-to-format transform and the log-tag census both need to read calls that continue over several lines and whose format string is
several adjacent literals; a per-line regex misses both (`[PERF v1]` in gos_smoke.cpp is such a call).
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

import typedef_aggregates as _ta


@dataclass
class Call:
    name: str
    start: int            # first character of the callee (including a `std::` prefix)
    open_paren: int
    end: int              # one past the closing parenthesis
    args: list = field(default_factory=list)       # [(start, end)] spans, whitespace trimmed

    def arg_text(self, text: str, i: int) -> str:
        a, b = self.args[i]
        return text[a:b]

    def gap(self, text: str, i: int) -> str:
        """The whitespace (and comments) between the separator before argument i and the argument itself: ' ' or a newline plus indentation."""
        start = self.open_paren + 1 if i == 0 else self.args[i - 1][1]
        chunk = text[start:self.args[i][0]]
        return chunk[chunk.index(",") + 1:] if i else chunk

    def tail(self, text: str) -> str:
        """The whitespace between the last argument and the closing parenthesis."""
        return text[self.args[-1][1]:self.end - 1]

    def rebuild(self, text: str, callee: str, pieces) -> str:
        """`callee(piece, piece, ...)` where each piece is (original argument index, new text) and keeps the whitespace that preceded that
        argument, so a call that spanned several lines still does (and `std::max` twice on a line does not appear from nowhere)."""
        out = [callee, "("]
        for n, (idx, piece) in enumerate(pieces):
            out.append(("," if n else "") + self.gap(text, idx if n else 0) + piece)    # the first piece takes the place of the first argument
        out.append(self.tail(text))
        out.append(")")
        return "".join(out)


_DEAD = re.compile(
    r"//[^\n]*"                              # line comment
    r"|/\*.*?\*/"                            # block comment
    r"|R\"([^()\\\s]{0,16})\(.*?\)\1\""      # raw string literal
    r"|\"(?:\\.|[^\"\\\n])*\""               # string literal
    r"|'(?:\\.|[^'\\\n])*'",                # character literal
    re.S)


def dead_ranges(text: str):
    """[(start, end)] of comments, string literals (raw ones included) and character literals. Regex-driven, so it is fast on big files."""
    return [(m.start(), m.end()) for m in _DEAD.finditer(text)]


def preprocessor_ranges(text: str):
    """[(start, end)] of preprocessor directives, backslash continuations included."""
    out = []
    for m in re.finditer(r"^[ \t]*#", text, re.M):
        end = m.end()
        while True:
            nl = text.find("\n", end)
            if nl < 0:
                end = len(text)
                break
            if text[nl - 1] == "\\" or (nl >= 2 and text[nl - 2:nl] == "\\\r"):
                end = nl + 1
                continue
            end = nl
            break
        out.append((m.start(), end))
    return out


def _in(ranges, pos):
    return any(a <= pos < b for a, b in ranges)


def _split_args(text: str, open_at: int, close: int, dead):
    """Top-level comma-separated argument spans between open_at+1 and close."""
    spans, depth, start, i = [], 0, open_at + 1, open_at + 1
    di = 0
    while i < close:
        while di < len(dead) and dead[di][1] <= i:
            di += 1
        if di < len(dead) and dead[di][0] <= i < dead[di][1]:
            i = dead[di][1]
            continue
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            spans.append((start, i))
            start = i + 1
        i += 1
    spans.append((start, close))
    out = []
    for a, b in spans:
        while a < b and text[a].isspace():
            a += 1
        while b > a and text[b - 1].isspace():
            b -= 1
        out.append((a, b))
    return out


def find_calls(text: str, names, qualifiers=("std::",)):
    """Yield Call for each `name(` / `std::name(` outside comments, strings and preprocessor lines."""
    dead = dead_ranges(text)
    pp = preprocessor_ranges(text)
    alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    quals = "|".join(re.escape(q) for q in qualifiers)
    pat = re.compile(r"(?<![\w.>:])((?:%s)?)(%s)\s*\(" % (quals, alt))
    for m in pat.finditer(text):
        # `->name(` and `.name(` and `Class::name(` are not the free function; the lookbehind covers `.`, `>` and `:` (but a `std::` prefix is kept)
        if _in(dead, m.start()) or _in(pp, m.start()):
            continue
        open_at = m.end() - 1
        close = _ta._matching_paren(text, open_at, dead)
        if close < 0:
            continue
        yield Call(m.group(2), m.start(), open_at, close + 1, _split_args(text, open_at, close, dead))


_LIT = re.compile(r'"((?:\\.|[^"\\\n])*)"')


_MACRO = re.compile(r"[A-Za-z_]\w*")


def string_literal_parts(arg: str, macros=None):
    """The concatenated contents of `"a" "b"` (escapes untouched), or None when the argument is anything but narrow string literals.

    macros: {NAME: placeholder} for object-like macros that expand to one string literal containing no printf conversion and no brace
    (PATH_SEPARATOR); each is returned as its placeholder character so the caller can put the macro back."""
    pos, parts = 0, []
    arg = arg.strip()
    while pos < len(arg):
        m = _LIT.match(arg, pos)
        if m:
            parts.append(m.group(1))
            pos = m.end()
        else:
            mm = _MACRO.match(arg, pos)
            if not (macros and mm and mm.group(0) in macros):
                return None
            parts.append(macros[mm.group(0)])
            pos = mm.end()
        while pos < len(arg) and arg[pos].isspace():
            pos += 1
    return "".join(parts) if parts else None
