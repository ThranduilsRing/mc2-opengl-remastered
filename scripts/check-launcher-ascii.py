#!/usr/bin/env python3
"""LAUNCHER-ENCODING-1 -- ASCII-only contract for launcher-facing mod strings.

HARD RULE (user ruling 2026-07-18): every string the mc2-launcher displays from
a mod manifest must be plain 7-bit ASCII. The launcher is a Win32 ANSI app
(#define UNICODE 0); UTF-8 punctuation in mod.json used to render as mojibake
("Core Unlocks \xc3\xa2\xe2\x82\xac..." em-dash garbage). The launcher now
fail-softs by converting UTF-8->ACP at read time, but shipped manifests must
not rely on the ACP having a mapping -- ASCII-only is the contract.

Scans every mod.json under mods/ and examples/mods/ (the two launcher-visible
manifest sources in the repo) for ANY byte >= 0x80, anywhere in the file.
Whole-file scan on purpose: JsonGetString is a dumb byte extractor, so a
non-ASCII byte in any field can reach the UI.

FAILS (exit 1) listing file, byte offset, line:col, the offending character,
and the suggested ASCII replacement (em-dash -> "-", curly quotes -> straight,
ellipsis -> "...", NBSP -> " ").

Usage:
    py -3 scripts/check-launcher-ascii.py [--quiet] [ROOT ...]
    py -3 scripts/check-launcher-ascii.py --self-test
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Manifest globs the launcher (or its deploy) reads for display strings.
MANIFEST_GLOBS = [
    "mods/*/mod.json",
    "examples/mods/*/mod.json",
]

# Suggested ASCII replacements for the usual suspects (typography that word
# processors / LLMs love to emit).
SUGGEST = {
    "—": "-",      # em dash
    "–": "-",      # en dash
    "‒": "-",      # figure dash
    "‘": "'",      # left single curly quote
    "’": "'",      # right single curly quote
    "“": '"',      # left double curly quote
    "”": '"',      # right double curly quote
    "…": "...",    # ellipsis
    " ": " ",      # non-breaking space
    "·": "*",      # middle dot
    "•": "*",      # bullet
    "é": "e",      # e-acute (cafe...)
    "×": "x",      # multiplication sign
}


def scan_file(path: Path, data: bytes) -> list[str]:
    """Return human-readable violation lines for every non-ASCII char."""
    violations = []
    try:
        text = data.decode("utf-8")
        # Walk decoded chars so a 3-byte em-dash is ONE finding, with its
        # byte offset reconstructed as we go.
        byte_off = 0
        line, col = 1, 1
        for ch in text:
            blen = len(ch.encode("utf-8"))
            if ord(ch) >= 0x80:
                sugg = SUGGEST.get(ch)
                hint = f'replace with "{sugg}"' if sugg is not None else \
                    "replace with plain ASCII"
                violations.append(
                    f"{path}: byte {byte_off} (line {line}:{col}): "
                    f"U+{ord(ch):04X} {ascii(ch)} -- {hint}")
            if ch == "\n":
                line += 1
                col = 1
            else:
                col += 1
            byte_off += blen
    except UnicodeDecodeError:
        # Not even valid UTF-8 -- report raw high bytes.
        for off, b in enumerate(data):
            if b >= 0x80:
                violations.append(
                    f"{path}: byte {off}: 0x{b:02X} (not valid UTF-8) -- "
                    f"replace with plain ASCII")
    return violations


def run(roots: list[Path], quiet: bool) -> int:
    files = []
    for root in roots:
        for pattern in MANIFEST_GLOBS:
            files.extend(sorted(root.glob(pattern)))
    all_violations = []
    for f in files:
        all_violations.extend(scan_file(f, f.read_bytes()))
    if all_violations:
        print(f"FAIL launcher-ascii: {len(all_violations)} non-ASCII "
              f"character(s) in launcher-facing manifests:")
        for v in all_violations:
            print(f"  {v}")
        print("  Contract: launcher-displayed strings are ASCII-only "
              "(the launcher UI is Win32 ANSI).")
        return 1
    if not quiet:
        print(f"PASS launcher-ascii: {len(files)} manifest(s) scanned, "
              f"all ASCII.")
    return 0


def self_test() -> int:
    """Prove the checker still fails on a synthetic em-dash manifest."""
    import contextlib
    import io
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        mod = root / "mods" / "selftest_mod"
        mod.mkdir(parents=True)
        (mod / "mod.json").write_bytes(
            '{"name": "Self Test — em dash", "description": '
            '"curly ’quote’"}'.encode("utf-8"))
        with contextlib.redirect_stdout(io.StringIO()):
            rc_bad = run([root], quiet=True)
        (mod / "mod.json").write_bytes(
            b'{"name": "Self Test - em dash", "description": '
            b'"straight \'quote\'"}')
        rc_good = run([root], quiet=True)
    if rc_bad != 1:
        print("FAIL launcher-ascii self-test: synthetic em-dash manifest "
              "did NOT fail")
        return 1
    if rc_good != 0:
        print("FAIL launcher-ascii self-test: clean manifest did not pass")
        return 1
    print("PASS launcher-ascii self-test (synthetic em-dash fails, "
          "ASCII passes)")
    return 0


def main(argv: list[str]) -> int:
    quiet = "--quiet" in argv
    argv = [a for a in argv if a != "--quiet"]
    if "--self-test" in argv:
        return self_test()
    roots = [Path(a) for a in argv] or [ROOT]
    return run(roots, quiet)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
