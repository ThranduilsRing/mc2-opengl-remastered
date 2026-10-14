#!/bin/sh
# Game calls stay behind the renderer API; new backend state calls retain their sanctioned owners.
cd "$(dirname "$0")/.." || exit 2
exec py -3 - "$@" <<'PY'
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path.cwd() / "scripts"))
import checklib
import console_child

ROOT = Path.cwd()
SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}
RAW_GL = re.compile(r"\bgl[A-Z][a-zA-Z]+\s*\(")


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                          text=True, errors="replace",
                          **console_child.child_kwargs(redirected=True))


def game_problems(sources, allowed):
    problems = []
    for path, text in sources.items():
        if path in allowed:
            continue
        for number, line in enumerate(checklib.strip_comments(text).splitlines(), 1):
            if RAW_GL.search(line):
                problems.append(f"{path}:{number}: raw GL call: {line.strip()}")
    return problems


def state_problems(diff, axes):
    problems = []
    path, previous = "", ""
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            path, previous = line[6:], ""
            continue
        if not line.startswith("+") or line.startswith("+++"):
            previous = ""
            continue
        added = line[1:]
        clean = checklib.strip_comments(added)
        for axis, rule in axes.items():
            if not re.search(rule["pattern"], clean):
                continue
            if path in rule["paths"] or any(path.startswith(p) for p in rule["prefixes"]):
                continue
            if axis == "fbobind" and (
                re.search(r"\b(?:prev|saved|csmSaved)\w*", added, re.I)
                or re.search(r"//\s*FBO-OWNER:", added)
                or re.search(r"//\s*FBO-OWNER:", previous)
            ):
                continue
            problems.append(f"{path}: new raw GL {axis}: {added.strip()}")
        previous = added
    return problems


def main():
    parser = argparse.ArgumentParser(description="Game GL firewall and backend state ownership")
    parser.add_argument("--base", help="diff base; defaults to merge-base HEAD origin/main, then HEAD")
    args = parser.parse_args()
    try:
        listed = git("ls-files", "-z", "code", "mclib")
        listed.check_returncode()
        sources = {p: (ROOT / p).read_text(encoding="utf-8", errors="replace")
                   for p in listed.stdout.split("\0") if p and Path(p).suffix in SOURCE_EXTS}
        if not sources:
            raise ValueError("no game sources enumerated")
        allowed = {line.strip() for line in (ROOT / "scripts/check-no-raw-gl-from-game.allowlist")
                   .read_text(encoding="utf-8").splitlines() if line.strip() and not line.startswith("#")}
        axes = json.loads((ROOT / "scripts/raw-gl-state.allowlist.json").read_text(encoding="utf-8"))
        if set(axes) != {"depthfunc", "depthmask", "colormask", "blendfunc", "fbobind"}:
            raise ValueError("backend state allowlist must cover all five axes")
        base = args.base
        if base is None:
            merged = git("merge-base", "HEAD", "origin/main")
            base = merged.stdout.strip() if merged.returncode == 0 else "HEAD"
        diff = git("diff", base, "--", *["*" + ext for ext in sorted(SOURCE_EXTS)])
        diff.check_returncode()
        problems = game_problems(sources, allowed) + state_problems(diff.stdout, axes)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        print(f"no_raw_gl_from_game: ERROR: {exc}", file=sys.stderr)
        return 2
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"no_raw_gl_from_game: {len(problems)} violation(s)")
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main())
PY
