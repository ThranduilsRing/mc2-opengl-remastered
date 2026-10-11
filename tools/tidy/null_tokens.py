"""null_tokens - replace the NULL token with nullptr, as a text transform.

WHAT: every `NULL` token outside comments, string and character literals and outside the preprocessor lines that mention NULL itself
(`#define NULL`, `#undef NULL`, `#ifndef NULL`, `#if defined(NULL)`) becomes `nullptr`.
WHY: clang-tidy's modernize-use-nullptr cannot rewrite a file it cannot parse (MSVC-only constructs) or a NULL inside a macro argument, and
those files hold most of the remaining sites. A blind token swap is only safe when the compiler agrees: NULL as an integer (`int x = NULL`),
in a variadic call or in an overload set changes or breaks the code. The swap is therefore a proposal behind the same objhash proof as every
other tidy edit: a file whose objects come out byte-identical keeps it, anything else is reverted.
"""
from __future__ import annotations

import re

import typedef_aggregates as _ta

_NULL = re.compile(r"\bNULL\b")
_PP_ABOUT_NULL = re.compile(r"^[ \t]*#[ \t]*(?:(?:define|undef)[ \t]+NULL\b|(?:ifdef|ifndef|if|elif)\b[^\n]*\bNULL\b)", re.M)


def rewrite(text: str) -> str:
    dead = _ta._dead_ranges(text)
    skip = [(m.start(), m.end()) for m in _PP_ABOUT_NULL.finditer(text)]

    def inside(ranges, pos):
        return any(a <= pos < b for a, b in ranges)

    out, last = [], 0
    for m in _NULL.finditer(text):
        if inside(dead, m.start()) or inside(skip, m.start()):
            continue
        out.append(text[last:m.start()])
        out.append("nullptr")
        last = m.end()
    out.append(text[last:])
    return "".join(out)
