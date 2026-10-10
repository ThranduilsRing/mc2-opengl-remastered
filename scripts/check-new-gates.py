#!/usr/bin/env python3
"""check-new-gates.py -- undocumented MC2_* gate growth guard.

Background: the engine reads MC2_* env vars as its primary feature-gate system.
A census found 551 of 703 MC2_* gates are grep-only -- they are read in source
via getenv("MC2_...") but appear in NEITHER the canonical registry
(RenderCore/RendererFeatureRegistry.h) NOR the tier1 doc (docs/tier1_env_vars.md).
That backlog is too big to retro-fix in one pass, but it MUST stop growing: every
new gate that lands undocumented makes the gate system less discoverable and the
backlog worse.

This script has two modes:
  * Mode A (default, ENFORCEMENT): inspect only the lines ADDED in a diff vs a
    base ref. Any newly-added getenv("MC2_...") whose name is not in
    (registry union doc) fails the check (exit 1). Wire this into pre-commit/CI
    so the backlog cannot grow.
  * Mode B (--all, AUDIT): list EVERY MC2_* gate currently read in source but
    absent from (registry union doc) -- the full backlog count + names.
    Advisory, always exit 0.

Pure stdlib + git subprocess.

Exit code: Mode A -> 1 if any new unregistered gate, else 0. Mode B -> always 0.
2 if the registry header is missing (cannot establish the registered set).
"""
import argparse
import os
import re
import subprocess
import sys
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(REPO_ROOT, "RenderCore", "RendererFeatureRegistry.h")
DOC = os.path.join(REPO_ROOT, "docs", "tier1_env_vars.md")

GATE_RE = re.compile(r"MC2_[A-Z0-9_]+")
# getenv("MC2_..."  /  getenv( "MC2_..."  (allow whitespace after the paren)
GETENV_RE = re.compile(r'getenv\s*\(\s*"(MC2_[A-Z0-9_]+)"')

SRC_EXT = ("*.cpp", "*.h", "*.hpp", "*.cc", "*.cxx")


def names_in_file(path):
    if not os.path.isfile(path):
        return set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return set(GATE_RE.findall(f.read()))


def git(args):
    return subprocess.run(["git"] + args, cwd=REPO_ROOT,
                          capture_output=True, text=True, errors="replace", **console_child.child_kwargs(redirected=True))


def default_base():
    """git merge-base HEAD origin/main, else HEAD."""
    r = git(["merge-base", "HEAD", "origin/main"])
    if r.returncode == 0 and r.stdout.strip():
        return r.stdout.strip()
    return "HEAD"


def registered_set():
    reg = names_in_file(REGISTRY)
    doc = names_in_file(DOC)
    return reg | doc


def mode_a(base, known):
    """Scan ADDED diff lines for new undocumented gates."""
    r = git(["diff", base, "--"] + list(SRC_EXT))
    if r.returncode != 0:
        print("[check-new-gates] ERROR: git diff failed: %s"
              % r.stderr.strip(), file=sys.stderr)
        return 1
    cur_file = "?"
    findings = []
    for line in r.stdout.splitlines():
        if line.startswith("+++ b/"):
            cur_file = line[6:]
            continue
        if line.startswith("+++") or line.startswith("---"):
            continue
        if not line.startswith("+"):
            continue
        for m in GETENV_RE.finditer(line):
            name = m.group(1)
            if name not in known:
                findings.append((name, cur_file))

    # de-dup (name,file) while preserving order
    seen = set()
    uniq = []
    for f in findings:
        if f not in seen:
            seen.add(f)
            uniq.append(f)

    print("[check-new-gates] Mode A (enforcement) -- base %s" % base)
    if not uniq:
        print("[check-new-gates] OK: no newly-added undocumented MC2_* gates")
        return 0
    for name, fpath in uniq:
        print("UNREGISTERED-NEW-GATE %s %s" % (name, fpath))
    print("[check-new-gates] FAIL: %d new gate(s) not in "
          "RendererFeatureRegistry.h or docs/tier1_env_vars.md" % len(uniq),
          file=sys.stderr)
    return 1


def mode_b(known):
    """List the full backlog: gates read in source, absent from registry/doc."""
    read = {}  # name -> first file seen
    for root, dirs, files in os.walk(REPO_ROOT):
        # prune non-source / vendored trees
        dirs[:] = [d for d in dirs
                   if d not in (".git", "build64", "3rdparty", ".claude")]
        for fn in files:
            if not fn.endswith((".cpp", ".h", ".hpp", ".cc", ".cxx")):
                continue
            path = os.path.join(root, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for m in GETENV_RE.finditer(text):
                name = m.group(1)
                if name not in read:
                    read[name] = os.path.relpath(path, REPO_ROOT)

    backlog = sorted(n for n in read if n not in known)
    print("[check-new-gates] Mode B (audit) -- full undocumented backlog")
    print("[check-new-gates] gates read in source: %d   registered/doc'd: %d"
          % (len(read), len([n for n in read if n in known])))
    print("[check-new-gates] UNDOCUMENTED BACKLOG: %d" % len(backlog))
    for n in backlog:
        print("   %s  (%s)" % (n, read[n]))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=None,
                    help="base ref for diff (default: merge-base HEAD origin/main)")
    ap.add_argument("--all", action="store_true",
                    help="Mode B: list the full undocumented backlog (advisory)")
    args = ap.parse_args()

    if not os.path.isfile(REGISTRY):
        print("[check-new-gates] ERROR: registry header missing: %s"
              % REGISTRY, file=sys.stderr)
        return 2

    known = registered_set()

    if args.all:
        return mode_b(known)

    base = args.base or default_base()
    return mode_a(base, known)


if __name__ == "__main__":
    sys.exit(main())
