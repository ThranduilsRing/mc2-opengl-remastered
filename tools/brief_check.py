#!/usr/bin/env python3
"""brief_check - verify the paths and shas a brief cites, before a lane wastes time on what does not exist.

Lanes lose time when a brief cites a file, path or sha that does not exist. This tool extracts the citations
from a brief's markdown and checks each one:

  * Windows drive paths (A:/, E:/, G:/, ...) and relative repo paths (a / and an extension)
      OK when the path exists, MISSING when it does not.
  * 7-12 hex shas (maximal runs; a full 40-hex sha is not one)
      OK when `git cat-file -e <sha>^{commit}` accepts it in --repo, UNKNOWN when it does not.
  * Paths with placeholders (<...>, NN, *)
      SKIP (a template, not a citation).

  py -3 tools/brief_check.py <brief.md> [--repo DIR]

--repo is the repo relative paths and shas are checked against (default: the git top level of the current
directory, else the current directory). One line per finding, first-seen order, deduped:
`OK|MISSING|SKIP|UNKNOWN <citation>`. Exit 0 = nothing MISSING/UNKNOWN, 1 = at least one, 2 = usage error.
Read-only. Stdlib only.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

#: A drive path (a lone letter, / or \; the lookbehind keeps "https:" from reading as a drive),
#: a relative repo path (segments with a / and a file extension; the lookbehind keeps URL hosts out),
#: or a maximal 7-12 lowercase-hex run (a git short sha; longer runs are not extracted).
CITE_RE = re.compile(
    r"(?P<drive>(?<![A-Za-z])[A-Za-z]:[\\/][^\s`'\"(){}\[\],;|]*)"
    r"|(?P<rel>(?<![\w./\\-])(?:[A-Za-z0-9_.\-*]+/)+[A-Za-z0-9_.\-*]+\.[A-Za-z0-9]{1,8})"
    r"|(?P<sha>(?<![0-9a-f])[0-9a-f]{7,12}(?![0-9a-f]))"
)
#: A template citation, not a real one: angle-bracket placeholder, NN slot, or a glob star.
PLACEHOLDER_RE = re.compile(r"[<>]|\*|NN")
#: Sentence punctuation a drive path may swallow at the end of a sentence.
TRAILING = ".,;:!?)"


def extract(text: str) -> list[tuple[str, str]]:
    """(kind, citation) pairs in first-seen order, deduped. kind is 'path', 'sha' or 'skip'."""
    seen: set[tuple[str, str]] = set()
    out: list[tuple[str, str]] = []
    for m in CITE_RE.finditer(text):
        if m.lastgroup == "sha":
            kind, cite = "sha", m.group("sha")
        else:
            kind = "path"
            cite = (m.group("drive") or m.group("rel")).rstrip(TRAILING)
            if not cite or cite.endswith(("/", "\\")):
                continue
            if PLACEHOLDER_RE.search(cite):
                kind = "skip"
        if (kind, cite) not in seen:
            seen.add((kind, cite))
            out.append((kind, cite))
    return out


def resolve(cite: str, repo: str) -> str:
    if re.match(r"^[A-Za-z]:[\\/]", cite) or os.path.isabs(cite):
        return cite
    return os.path.join(repo, cite)


def check_path(cite: str, repo: str) -> str:
    return "OK" if os.path.exists(resolve(cite, repo)) else "MISSING"


def check_sha(cite: str, repo: str) -> str:
    r = subprocess.run(["git", "-C", repo, "cat-file", "-e", cite + "^{commit}"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return "OK" if r.returncode == 0 else "UNKNOWN"


def default_repo() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=os.getcwd(),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    except OSError:
        pass
    return os.getcwd()


def check_brief(text: str, repo: str) -> list[tuple[str, str]]:
    """(status, citation) per finding: OK/MISSING for paths, OK/UNKNOWN for shas, SKIP for placeholders."""
    findings = []
    for kind, cite in extract(text):
        if kind == "skip":
            status = "SKIP"
        elif kind == "sha":
            status = check_sha(cite, repo)
        else:
            status = check_path(cite, repo)
        findings.append((status, cite))
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="check the paths and shas a brief cites")
    ap.add_argument("brief", help="the brief markdown to check")
    ap.add_argument("--repo", default=None,
                    help="repo relative paths and shas are checked against (default: git top level of the CWD)")
    args = ap.parse_args(argv)
    try:
        with open(args.brief, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print(f"brief_check: cannot read {args.brief}: {e}", file=sys.stderr)
        return 2
    repo = args.repo or default_repo()
    findings = check_brief(text, repo)
    for status, cite in findings:
        print(f"{status} {cite}")
    return 1 if any(s in ("MISSING", "UNKNOWN") for s, _ in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
