#!/usr/bin/env python3
"""Lane precheck: a diff must not ADD a hardcoded drive-letter path (A:/ E:/ G:/) in scripts/ or tools/ Python code.

The farm laptop gate has no E: drive (W138d: a hardcoded E:\\ path passed `lane_merge --lint` and failed on merged HEAD).
Hard fail (not a warning): only paths ADDED by the diff count, so existing ones never fire, and the escapes are cheap:
  * read the path from an env var with the drive path as the default (`os.environ.get("X", "E:/...")`) -- exempt;
  * a docstring, comment or test file -- not checked (message strings that merely mention a path need the marker);
  * `# drive-path: ok <why>` on the line;
  * PC/box-only lane tooling in ALLOW_PREFIXES below.

  py -3 tools/new_drive_paths.py --base <ref> --head <ref>     exit 0 ok, 1 hardcoded drive paths added
"""
from __future__ import annotations

import argparse
import ast
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import console_child  # noqa: E402

DRIVE = re.compile(r"(?<![A-Za-z0-9])[AEG]:[/\\]")
MARKER = "drive-path: ok"
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))?")
# PC/box-only lane tooling (never runs on the laptop gate) and config files that exist to name the machine's drives.
ALLOW_PREFIXES = ("tools/lane_", "tools/box_", "tools/farm/host.py", "scripts/tests/", "tools/tests/", "tools/test_")


def git(*args: str) -> str:
    r = subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8", errors="replace",
                       **console_child.child_kwargs(redirected=True))
    if r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args[:2])} failed rc={r.returncode}: {r.stderr.strip()[:200]}")
    return r.stdout


def checked(path: str) -> bool:
    name = path.rsplit("/", 1)[-1]
    return (path.endswith(".py") and path.startswith(("scripts/", "tools/")) and not path.startswith(ALLOW_PREFIXES)
            and not name.startswith("test_"))


def added_lines(diff: str) -> dict[str, set[int]]:
    """{path: new-file line numbers added} from a `git diff -U0` text."""
    out: dict[str, set[int]] = {}
    path, line = None, 0
    for raw in diff.splitlines():
        if raw.startswith("+++ "):
            path = raw[6:] if raw.startswith("+++ b/") else None
        elif (m := HUNK.match(raw)):
            line = int(m.group(1))
        elif path and raw.startswith("+") and not raw.startswith("+++"):
            out.setdefault(path, set()).add(line)
            line += 1
    return out


def drive_strings(source: str, lines: set[int]) -> list[tuple[int, str]]:
    """(line, text) of non-docstring string constants on `lines` holding a drive path, minus env-var defaults and marked lines."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return []
    src = source.splitlines()
    skip: set[int] = set()   # id() of constants that are docstrings / expression statements, or an env lookup's default
    for node in ast.walk(tree):
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant):
            skip.add(id(node.value))
        if isinstance(node, ast.Call) and getattr(node.func, "attr", getattr(node.func, "id", "")) in ("get", "getenv"):
            skip.update(id(a) for a in node.args[1:] if isinstance(a, ast.Constant))
    bad = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in skip and DRIVE.search(node.value):
            if node.lineno in lines and MARKER not in src[node.lineno - 1]:
                bad.append((node.lineno, node.value.strip().splitlines()[0][:80]))
    return sorted(set(bad))


def check(base: str, head: str) -> list[str]:
    diff = git("diff", "-U0", "--no-color", f"{base}...{head}", "--", "scripts", "tools")
    problems = []
    for path, lines in sorted(added_lines(diff).items()):
        if not checked(path):
            continue
        for lineno, text in drive_strings(git("show", f"{head}:{path}"), lines):
            problems.append(f"{path}:{lineno}: hardcoded drive path {text!r}")
    return problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True)
    ap.add_argument("--head", required=True)
    a = ap.parse_args(argv)
    try:
        bad = check(a.base, a.head)
    except RuntimeError as exc:
        print(f"new_drive_paths: FAIL (cannot read the diff: {exc})")
        return 1
    for b in bad:
        print("NEW DRIVE PATH " + b + " (the laptop gate has no E: drive: read it from an env var default, or add `# " + MARKER + " <why>`)")
    print(f"new_drive_paths: {'FAIL ' + str(len(bad)) if bad else 'PASS'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
