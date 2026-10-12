#!/usr/bin/env python3
"""check-doc-links.py -- markdown reference drift reporter.

Pure reporter over COMMITTED files. Scans repo-root *.md plus docs/**/*.md
and verifies that file references actually resolve:

  1. BROKEN LINKS  -- relative markdown links [text](target) whose target
     exists neither relative to the containing file nor to the repo root.
  2. BROKEN PATHS  -- backticked repo paths (`docs/...`, `scripts/...`,
     `tools/...`, `shaders/...`, `code/...`, `mclib/...`, `GameOS/...`,
     `RenderCore/...`, `RenderWorld/...`, `GameAdapters/...`, `editor/...`)
     with a known source/doc extension that do not exist.

Files under any path segment named 'archive' (and *.original.md duplicates)
are scanned but reported separately as ADVISORY -- archived docs are allowed
to rot.

Advisory: exit 0 always, unless --strict AND category 1 or 2 non-empty for
non-archived docs.
"""

import argparse
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))
import console_child  # noqa: E402

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
TICK_RE = re.compile(r"`([A-Za-z0-9_][A-Za-z0-9_./\\-]*)`")
PATH_PREFIXES = (
    "docs/", "scripts/", "tools/", "shaders/", "code/", "mclib/",
    "GameOS/", "RenderCore/", "RenderWorld/", "GameAdapters/", "editor/",
    ".planning/", "tests/",
)
PATH_EXTS = (
    ".md", ".sh", ".py", ".cpp", ".h", ".hpp", ".c", ".frag", ".vert",
    ".comp", ".glsl", ".hglsl", ".json", ".cfg", ".bat", ".ini", ".yaml",
)
SKIP_SCHEMES = ("http://", "https://", "mailto:", "file://")


ADVISORY_PREFIXES = (
    # historical planning corpus: plans/specs/explorations reference
    # files that were planned, renamed, or never built -- allowed to rot
    "docs/superpowers/", "docs/plans/", "docs/observations/",
)


# point-in-time doc shapes: recon reports, slice plans/specs, probes,
# sweeps, handoffs -- they legitimately name files that were planned,
# renamed, or never built, so they get advisory (non-strict) treatment
ADVISORY_STEM_PATTERNS = ("-recon", "-plan", "_plan", "-spec", "_spec",
                          "-probe", "-sweep")
ADVISORY_STEM_PREFIXES = ("HANDOFF-", "SCOPE-", "handoff-")


# 2026-10-05 reset: these trees were archived to E:/mc2-archive-2026-10/docs
# (also recoverable from git history). A live doc pointing INTO them is a
# historical citation, not a broken link.
ARCHIVED_TARGET_MARKERS = ("superpowers/", "audit-lanes", "docs/archive/",
                           ".claude/",
                           # 2026-10-09 arc-docs-2026-10-09.tar.gz (E: archive):
                           "docs/observations/", "docs/plans/", "docs/recon/",
                           "observations/2026-05-",
                           "model-override-", "-rv-arc-", "terrain-8z-",
                           "trackv-", "track-r-closeout", "-recon-1.md",
                           # 2026-10-11 legacy-subsystem deletions (recovery: the cluster commits' parent shas);
                           # docs naming these are historical citations, not links to repair:
                           "mclib/stuff/auto_ptr.hpp", "scripts/quick_shot.py", "scripts/quick_sweep.py",
                           "scripts/water_visual_diff.py")


def archived_target(target):
    t = target.replace("\\", "/")
    return any(m in t for m in ARCHIVED_TARGET_MARKERS)


def is_archived(relpath):
    rel = relpath.replace("\\", "/")
    parts = rel.split("/")
    if any(p.lower().startswith("archive") for p in parts):
        return True
    if rel.startswith(ADVISORY_PREFIXES):
        return True
    stem = os.path.splitext(parts[-1])[0]
    if stem.startswith(ADVISORY_STEM_PREFIXES):
        return True
    if any(p in stem for p in ADVISORY_STEM_PATTERNS):
        return True
    return relpath.endswith(".original.md")


def ignored_docs():
    """Paths git ignores under docs/ (local scratch such as docs/beta): not part of the repo, so not part of its link contract."""
    try:
        out = subprocess.run(["git", "-C", REPO_ROOT, "ls-files", "-z", "--others", "--ignored", "--exclude-standard", "--", "docs"],
                             text=True, check=False, **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)).stdout
    except OSError:
        return set()
    return {os.path.normpath(p) for p in out.split("\0") if p}


def collect_md_files():
    out = []
    ignored = ignored_docs()
    for name in os.listdir(REPO_ROOT):
        if name.endswith(".md"):
            out.append(name)
    docs_root = os.path.join(REPO_ROOT, "docs")
    for dirpath, dirnames, filenames in os.walk(docs_root):
        for name in filenames:
            if name.endswith(".md"):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, REPO_ROOT)
                if os.path.normpath(rel) not in ignored:
                    out.append(rel)
    return sorted(out)


def resolve(target, containing_dir):
    """True if target exists relative to the file's dir or the repo root."""
    for base in (containing_dir, REPO_ROOT):
        if os.path.exists(os.path.normpath(os.path.join(base, target))):
            return True
    return False


def check_file(relpath):
    """Return (broken_links, broken_paths) lists of (lineno, target)."""
    full = os.path.join(REPO_ROOT, relpath)
    containing_dir = os.path.dirname(full)
    broken_links, broken_paths = [], []
    with open(full, "r", encoding="utf-8", errors="replace") as f:
        in_fence = False
        for lineno, line in enumerate(f, 1):
            if line.lstrip().startswith("```"):
                in_fence = not in_fence
                continue
            for m in LINK_RE.finditer(line):
                target = m.group(1)
                if target.startswith(SKIP_SCHEMES) or target.startswith("#"):
                    continue
                # drive-absolute, unix-absolute, or home-relative:
                # outside repo scope
                if re.match(r"^([A-Za-z]:[/\\]|/|~)", target):
                    continue
                target = target.split("#", 1)[0]
                if not target or "*" in target or "<" in target:
                    continue
                if not resolve(target, containing_dir) and not archived_target(target):
                    broken_links.append((lineno, m.group(1)))
            if in_fence:
                continue
            for m in TICK_RE.finditer(line):
                tok = m.group(1).replace("\\", "/")
                if not tok.startswith(PATH_PREFIXES):
                    continue
                if not tok.endswith(PATH_EXTS):
                    continue
                if "*" in tok or "<" in tok or "/." in tok:
                    continue  # compound shorthand like `x.vert/.frag`
                if not resolve(tok, containing_dir) and not archived_target(tok):
                    broken_paths.append((lineno, m.group(1)))
    return broken_links, broken_paths


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 if non-archived docs have broken refs")
    ap.add_argument("--include-archive", action="store_true",
                    help="also print findings for archived/.original docs")
    ap.add_argument("--quiet", action="store_true",
                    help="summary line only (implies --strict)")
    args = ap.parse_args()
    if args.quiet:
        args.strict = True

    enforced_hits = 0
    advisory_hits = 0
    files = collect_md_files()
    for relpath in files:
        links, paths = check_file(relpath)
        if not links and not paths:
            continue
        archived = is_archived(relpath)
        if archived:
            advisory_hits += len(links) + len(paths)
            if not args.include_archive:
                continue
        else:
            enforced_hits += len(links) + len(paths)
        if args.quiet:
            continue
        tag = " [ADVISORY: archived]" if archived else ""
        print(f"{relpath}{tag}")
        for lineno, target in links:
            print(f"  {lineno}: broken link -> {target}")
        for lineno, target in paths:
            print(f"  {lineno}: broken path ref -> `{target}`")

    print(f"\nscanned {len(files)} md files; "
          f"{enforced_hits} broken refs in live docs, "
          f"{advisory_hits} in archived docs (advisory)")
    if args.strict and enforced_hits:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
