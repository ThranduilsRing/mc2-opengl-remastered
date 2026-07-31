#!/usr/bin/env python3
"""checklib.py — META-FIX INFRA-2: shared skeleton for the check-*.py family.

Shared helpers for the static contract checkers (design C's
check-split-path-contracts.py; designs D/F in later batches). The .sh
checkers (A, B, E) follow the check-asset-scale-callers.sh precedent and do
not use this module.

Provides:
  * comment stripping (// and /* */) — MANDATORY before any return-scan
    (design F refutation (b): commented-out `return(4)`s in code/mech.cpp);
  * whitespace normalization for regex rules (design C);
  * brace-matched function-body extraction (design F; same technique as the
    check-*-ownership.py family);
  * anchor-pin assertion that matches SYMBOL + SHAPE and reports
    file + found-line (never frozen line numbers);
  * baseline-JSON ratchet: load / compare / --update-baseline (design D
    rule B; design E's count ratchet stays .sh per its design);
  * --self-test scaffold helper: run a callable that injects a synthetic
    violation and assert the checker fails (house pattern:
    check-contracts.sh vulkan_bindings_selftest).

Exit convention (check-combat-soak.py:20-22):
  0 = pass, 1 = a contract violated, 2 = harness error.
"""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass
from typing import Callable, Iterable, Optional

EXIT_OK = 0
EXIT_VIOLATION = 1
EXIT_HARNESS_ERROR = 2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------------------
# Source loading
# ---------------------------------------------------------------------------

def read_text(rel_or_abs: str) -> str:
    """Read a file as UTF-8 (errors replaced). Relative paths resolve
    against the worktree root."""
    p = rel_or_abs
    if not os.path.isabs(p):
        p = os.path.join(ROOT, p)
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def iter_source_files(rel_dirs: Iterable[str],
                      exts: Iterable[str]) -> Iterable[str]:
    """Yield worktree-relative paths of files under rel_dirs with the given
    extensions (e.g. ('.cpp', '.h')). Skips build64* and 3rdparty."""
    ext_set = tuple(exts)
    for d in rel_dirs:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [n for n in dirnames
                           if not n.startswith("build64") and n != "3rdparty"]
            for fn in filenames:
                if fn.endswith(ext_set):
                    full = os.path.join(dirpath, fn)
                    yield os.path.relpath(full, ROOT).replace("\\", "/")


# ---------------------------------------------------------------------------
# Comment stripping + whitespace normalization
# ---------------------------------------------------------------------------

_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def strip_comments(text: str) -> str:
    """Remove /* */ and // comments, preserving line structure (newlines
    inside block comments are kept so line numbers stay stable).

    Lexically naive by design (string literals containing comment markers
    are rare in this codebase's checker scopes); good enough for the
    grep-tier contract checks, same bar as the .sh checkers.

    THAT CLAIM IS FALSE ON AT LEAST ONE REAL FILE, MEASURED (CONTRACT-GREEN-1,
    2026-07-31). It is not string literals that break it -- it is `/*` inside a
    `//` comment, which this function has no state to notice. Prose comments in
    this repo quote shell and git one-liners, and one of them contains `/*`:

        GameOS/gameos/gameos_graphics.cpp:2929  // ... `git log -S'/*'` ...

    The unmatched `/*` opens a block comment that runs to the next `*/`
    ANYWHERE in the file -- here 2304 lines later, at an inline
    `/* invariant: shadow path always on */` on line 5233. The regex deletes
    everything between: 4167 of the file's 10504 non-blank lines, 40% of the
    largest TU in the engine, including real code such as

        gameos_graphics.cpp:5184  const char* tessEnv = getenv("MC2_TERRAIN_TESS");

    Any rule that greps `strip_comments(<that file>)` is silently blind over
    that span. 17 checkers call this function; auditing and repointing each is
    out of this lane's scope, so the fix here is additive: use
    `strip_comments_lexed` below for any NEW rule, and for any existing rule
    whose blind spot is shown to matter. This docstring is the ledger entry --
    do not delete it while the naive body is still live.
    """
    def _block(m: re.Match) -> str:
        return "\n" * m.group(0).count("\n")
    text = _BLOCK_COMMENT_RE.sub(_block, text)
    text = _LINE_COMMENT_RE.sub("", text)
    return text


def strip_comments_lexed(text: str) -> str:
    """Correct C/C++ comment stripping: a single left-to-right scan that
    tracks string and character literals, so a comment marker inside a
    literal is not a comment and a `/*` inside a `//` comment is not an
    opener. Line structure is preserved exactly (every newline in the input
    appears in the output at the same index-in-line-order), so a caller can
    still map an offset back to a line number.

    This is what `strip_comments` was documented to be. Prefer it whenever the
    scanned text is real C/C++ rather than GLSL; the cost is one pass.

    Known and deliberate limits (none present in the scanned trees today):
      * raw string literals R"delim( ... )delim" are treated as ordinary
        strings, so a `//` inside one is stripped;
      * a backslash-continued `//` comment ends at the first newline.
    Both are noisier-than-correct in the safe direction only for comments;
    neither can hide code the way the naive version does.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue                      # the newline is emitted next loop
        if c == "/" and nxt == "*":
            i += 2
            while i < n:
                if text[i] == "*" and i + 1 < n and text[i + 1] == "/":
                    i += 2
                    break
                if text[i] == "\n":
                    out.append("\n")      # keep line numbering stable
                i += 1
            continue
        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def normalize_ws(s: str) -> str:
    """Collapse all whitespace runs to single spaces and trim — the
    pre-pass for regex rules so formatting differences don't split
    matches (design C)."""
    return re.sub(r"\s+", " ", s).strip()


# ---------------------------------------------------------------------------
# Brace-matched function-body extraction
# ---------------------------------------------------------------------------

def extract_brace_body(text: str, open_brace_idx: int) -> Optional[str]:
    """Given the index of a '{' in text, return the body text INCLUDING the
    braces, or None if unbalanced. Comment-strip first for reliability."""
    if open_brace_idx >= len(text) or text[open_brace_idx] != "{":
        return None
    depth = 0
    for i in range(open_brace_idx, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace_idx:i + 1]
    return None


def find_function_body(text: str, signature_re: str) -> Optional[str]:
    """Find a function by signature regex and return its brace-matched body
    (including braces). The signature regex should match up to (but not
    necessarily including) the opening brace. Returns None if not found or
    unbalanced. Callers should pass comment-stripped text."""
    m = re.search(signature_re, text)
    if not m:
        return None
    brace = text.find("{", m.end())
    if brace < 0:
        return None
    return extract_brace_body(text, brace)


# ---------------------------------------------------------------------------
# Anchor pins (symbol + shape, never frozen line numbers)
# ---------------------------------------------------------------------------

@dataclass
class PinResult:
    ok: bool
    rel_path: str
    label: str
    line: Optional[int] = None
    error: str = ""

    def report(self) -> str:
        if self.ok:
            return "  PIN OK   %-40s %s:%d" % (self.label, self.rel_path, self.line)
        return "  PIN FAIL %-40s %s (%s)" % (self.label, self.rel_path, self.error)


def pin_anchor(rel_path: str, symbol_re: str, label: str,
               shape_re: Optional[str] = None) -> PinResult:
    """Assert that rel_path contains symbol_re; if shape_re is given, the
    matching LINE (whitespace-normalized) must also match shape_re. Reports
    the found line number so drift is diagnosable."""
    try:
        text = read_text(rel_path)
    except OSError as e:
        return PinResult(False, rel_path, label, error="cannot read: %s" % e)
    for lineno, line in enumerate(text.splitlines(), start=1):
        if re.search(symbol_re, line):
            if shape_re and not re.search(shape_re, normalize_ws(line)):
                continue
            return PinResult(True, rel_path, label, line=lineno)
    return PinResult(False, rel_path, label,
                     error="symbol %r not found%s" % (
                         symbol_re, " with shape %r" % shape_re if shape_re else ""))


# ---------------------------------------------------------------------------
# Baseline-JSON ratchet
# ---------------------------------------------------------------------------

class BaselineRatchet:
    """Checked-in JSON baseline of {key: count}. compare() fails when any
    key's current count EXCEEDS the baseline; decreases are reported as
    improvements (caller may auto-shrink via save())."""

    def __init__(self, rel_path: str):
        self.rel_path = rel_path
        self.abs_path = (rel_path if os.path.isabs(rel_path)
                         else os.path.join(ROOT, rel_path))
        self.data: dict[str, int] = {}

    def load(self) -> bool:
        try:
            with open(self.abs_path, "r", encoding="utf-8") as f:
                self.data = {str(k): int(v) for k, v in json.load(f).items()}
            return True
        except OSError:
            return False

    def compare(self, current: dict[str, int]):
        """Return (regressions, improvements): lists of (key, base, cur).
        Keys absent from the baseline count as base=0 (new keys regress)."""
        regressions, improvements = [], []
        for k, cur in sorted(current.items()):
            base = self.data.get(k, 0)
            if cur > base:
                regressions.append((k, base, cur))
            elif cur < base:
                improvements.append((k, base, cur))
        for k, base in sorted(self.data.items()):
            if k not in current and base > 0:
                improvements.append((k, base, 0))
        return regressions, improvements

    def save(self, current: dict[str, int]) -> None:
        with open(self.abs_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump({k: v for k, v in sorted(current.items()) if v > 0},
                      f, indent=2, sort_keys=True)
            f.write("\n")


# ---------------------------------------------------------------------------
# --self-test scaffold
# ---------------------------------------------------------------------------

def run_self_test(label: str,
                  run_checker: Callable[[], int],
                  inject_violation: Callable[[Callable[[], int]], int]) -> int:
    """House --self-test pattern (check-contracts.sh vulkan_bindings_selftest):
    1. the checker must PASS on the real tree;
    2. with a synthetic violation injected (inject_violation wraps/monkey-
       patches and calls run_checker), it must FAIL (nonzero).
    Returns EXIT_OK when both hold; EXIT_VIOLATION otherwise."""
    clean = run_checker()
    if clean != EXIT_OK:
        print("%s --self-test: FAIL (checker not green on current tree, rc=%d)"
              % (label, clean), file=sys.stderr)
        return EXIT_VIOLATION
    injected = inject_violation(run_checker)
    if injected == EXIT_OK:
        print("%s --self-test: FAIL (synthetic violation NOT detected)" % label,
              file=sys.stderr)
        return EXIT_VIOLATION
    print("%s --self-test: PASS (green on tree, red on synthetic violation)"
          % label)
    return EXIT_OK
