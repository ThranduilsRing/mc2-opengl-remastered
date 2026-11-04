"""No compile definition may carry an absolute checkout/build path.

A -DFOO="A:/Games/mc2-laneN/..." is baked into the object file but is not part of
the shared compile-cache key, so a cache hit replays a deleted worktree's path
(W114: test_mc2str.cpp.obj). Resolve paths at runtime instead (see
tests/unit/test_repo_root.h). An entry in ALLOW (file, define text) needs a reason.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKIP = ("3rdparty", "build", "unused", "graphify-out", ".git", "node_modules")
STMT = re.compile(r"\b(target_compile_definitions|add_compile_definitions|add_definitions)\s*\(", re.I)
PATHVAR = re.compile(r"\$\{[A-Za-z0-9_]*(SOURCE_DIR|BINARY_DIR|WORKTREE_ROOT|REPO_ROOT|ROOT_DIR)[A-Za-z0-9_]*\}")

# (relative cmake file, exact variable text) -> reason
ALLOW: dict[tuple[str, str], str] = {}


def _statement_bodies(text):
    for m in STMT.finditer(text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        yield text[m.end():i - 1]


def _cmake_files():
    for p in ROOT.rglob("*"):
        rel = p.relative_to(ROOT).parts
        if rel[0].startswith(SKIP) or any(s in rel for s in SKIP):
            continue
        if p.name == "CMakeLists.txt" or p.suffix == ".cmake":
            yield p


def test_no_absolute_root_in_compile_definitions():
    bad = []
    for p in _cmake_files():
        text = p.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"(?m)^\s*#.*$", "", text)
        for body in _statement_bodies(text):
            for m in PATHVAR.finditer(body):
                key = (p.relative_to(ROOT).as_posix(), m.group(0))
                if key not in ALLOW:
                    bad.append(key)
    assert not bad, (
        "absolute path in a compile definition (resolve at runtime, or add an ALLOW entry with a reason): "
        + repr(bad))
