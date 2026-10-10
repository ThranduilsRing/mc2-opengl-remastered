#!/usr/bin/env python3
"""check-raw-gl-fbobind.py -- RAW-GL-FBO-BIND-GATE-1 (Phase 8 enforcement).

North-star Phase 8 FIFTH raw-GL axis enforcement gate -- and the highest-context
one. Bans NEW unsanctioned glBindFramebuffer() call sites on ADDED diff lines,
outside a sanctioned-file allowlist, so the FBO-bind backlog cannot grow new
bypasses of the sanctioned FBO-redirect chokepoint
(executor applyTopLevelGenericAxes / the FBO-owner TUs).

NOT a flat clone of the four scalar axis gates (depth-func/mask, color-mask,
blend-func). An FBO bind is HIGHER CONTEXT than a scalar state setter: FBO
identity is a logical resource, not a flag, and many binds are LOAD-BEARING
save/restore brackets that RETURN to a previously-saved FBO (cannot redirect to
a new target). So this gate uses a HYBRID taxonomy:

  1. file allowlist          -- sanctioned FBO-owner / executor / editor TUs
  2. // FBO-OWNER: tag        -- a deliberate new owner in a non-allowlisted file
                                 may escape by tagging the added line (or the
                                 immediately-previous added line) with the tag.
  3. restore-pattern exempt  -- a bind whose added line targets a prev/saved/
                                 csmSaved* variable is a save/restore RETURN, not
                                 a new redirect -> auto-exempt.

Two modes (same shape as the scalar axis gates):
  * Mode A (default, ENFORCEMENT): inspect only the lines ADDED in a diff vs a
    base ref. Any newly-added glBindFramebuffer in a NON-allowlisted file that is
    NEITHER a restore (rule 3) NOR FBO-OWNER-tagged (rule 2) fails (exit 1).
  * Mode B (--all, AUDIT): walk the render TUs and list EVERY existing
    glBindFramebuffer site with file:line + a class tag (ALLOWLISTED / RESTORE /
    OWNER-TAGGED / RAW) + per-file counts. Advisory, exit 0.

Comment-only matches (// ... or /* ... */) are ignored.

Pure stdlib + git subprocess.

Exit code: Mode A -> 1 if any new unsanctioned raw glBindFramebuffer, else 0.
           Mode B -> always 0.
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# bare glBindFramebuffer(  (allow whitespace after the name, before the paren)
BIND_RE = re.compile(r"\bglBindFramebuffer\s*\(")

# A bind whose added line targets a prev/saved/csmSaved* variable is a
# save/restore RETURN to a previously-captured FBO -- it cannot redirect to a
# new target, so it is auto-exempt.
RESTORE_ARG_RE = re.compile(r"\b(prev|saved|csmSaved)\w*", re.IGNORECASE)

# Owner-tag escape: a deliberate new owner may tag the added line (or the
# immediately-previous added line) with `// FBO-OWNER: <reason>`.
OWNER_TAG_RE = re.compile(r"//\s*FBO-OWNER:")

SRC_EXT = ("*.cpp", "*.h", "*.hpp", "*.cc", "*.cxx")

# ---------------------------------------------------------------------------
# ALLOWLIST -- sanctioned files that MAY contain glBindFramebuffer. Only NEW raw
# binds in NON-allowlisted files (that are not restores/tagged) trip Mode A.
#
# v1 = file-allowlist + // FBO-OWNER: tag + prev/saved-restore exempt. This
#      freezes the current 84-site backlog (all sanctioned) so it cannot grow in
#      new TUs, while leaving the three documented escape paths open.
# v2 (after RESOURCE-REGISTRY-COMPLETE-1) = require the FBO-OWNER tag to name a
#      ledger-registered logical resource + optional runtime resolve!=Unknown.
# ---------------------------------------------------------------------------

# Exact-path sanctioned sites.
ALLOWLIST_EXACT = {
    # FBO owner: create / init / resize / post-process apply + registerFbo.
    "GameOS/gameos/gos_postprocess.cpp",
    "GameOS/gameos/gos_postprocess_effects.cpp",
    "GameOS/gameos/gos_postprocess_executor.cpp",
    "GameOS/gameos/gos_postprocess_fbo.cpp",
    "GameOS/gameos/gos_postprocess_shadow.cpp",
    # executor applyTopLevelGenericAxes + shadow brackets.
    "GameOS/gameos/gameos_graphics.cpp",
    # screenshot / video READ-blit brackets.
    "GameOS/gameos/gameosmain.cpp",
    # water-reflection prev-restore brackets.
    "GameOS/gameos/gos_terrain_indirect_dispatch.cpp",
    # readback bracket.
    "RenderWorld/RenderWorld.cpp",
    # editor TUs, out of the frame loop.
    "editor/EditorGameOS.cpp",
    "editor/EditorInterface.cpp",
    "editor/EditorInterface_mouse.cpp",
}

# Prefix-matched sanctioned trees (out-of-engine tools, offline harnesses).
ALLOWLIST_PREFIX = (
    "tools/",
    "tests/",
)


def is_allowlisted(path):
    """path is a repo-relative posix path (forward slashes)."""
    if path in ALLOWLIST_EXACT:
        return True
    for pref in ALLOWLIST_PREFIX:
        if path.startswith(pref):
            return True
    return False


def strip_comments(line):
    """Remove // line comments and /* */ block comments (single-line heuristic)."""
    line = re.sub(r"//.*$", "", line)
    line = re.sub(r"/\*.*?\*/", "", line)
    return line


def git(args):
    return subprocess.run(["git"] + args, cwd=REPO_ROOT,
                          capture_output=True, text=True, errors="replace", **console_child.child_kwargs(redirected=True))


def default_base():
    """git merge-base HEAD origin/main, else HEAD."""
    r = git(["merge-base", "HEAD", "origin/main"])
    if r.returncode == 0 and r.stdout.strip():
        return r.stdout.strip()
    return "HEAD"


def mode_a(base, quiet=False):
    """Scan ADDED diff lines for new unsanctioned raw glBindFramebuffer."""
    r = git(["diff", base, "--"] + list(SRC_EXT))
    if r.returncode != 0:
        print("[check-raw-gl-fbobind] ERROR: git diff failed: %s"
              % r.stderr.strip(), file=sys.stderr)
        return 1
    cur_file = "?"
    prev_added = ""   # the previous ADDED (+) line, for the prev-line tag check
    findings = []     # (file, added_text)
    for line in r.stdout.splitlines():
        if line.startswith("+++ b/"):
            cur_file = line[6:]
            prev_added = ""
            continue
        if line.startswith("+++") or line.startswith("---"):
            continue
        if not line.startswith("+"):
            # any non-added line breaks the prev-added adjacency
            prev_added = ""
            continue
        added = line[1:]
        # only consider it a bind if the NON-comment part has the call
        if not BIND_RE.search(strip_comments(added)):
            prev_added = added
            continue
        if is_allowlisted(cur_file):
            prev_added = added
            continue
        # rule 3: restore/return to a saved FBO -> exempt
        if RESTORE_ARG_RE.search(added):
            prev_added = added
            continue
        # rule 2: FBO-OWNER tag on this added line OR the previous added line
        if OWNER_TAG_RE.search(added) or OWNER_TAG_RE.search(prev_added):
            prev_added = added
            continue
        findings.append((cur_file, added.strip()))
        prev_added = added

    # de-dup while preserving order
    seen = set()
    uniq = []
    for f in findings:
        if f not in seen:
            seen.add(f)
            uniq.append(f)

    if not quiet:
        print("[check-raw-gl-fbobind] Mode A (enforcement) -- base %s" % base)
    if not uniq:
        if not quiet:
            print("[check-raw-gl-fbobind] OK: no newly-added raw glBindFramebuffer "
                  "outside sanctioned sites (allowlist / FBO-OWNER tag / restore)")
        return 0
    for fpath, text in uniq:
        print("RAW-GL-FBOBIND-NEW %s    %s" % (fpath, text))
    print("[check-raw-gl-fbobind] FAIL: %d new raw glBindFramebuffer call(s) outside "
          "the sanctioned allowlist -- route FBO binds through "
          "applyTopLevelGenericAxes / an FBO-owner TU; tag a deliberate new owner "
          "with // FBO-OWNER: <reason>" % len(uniq), file=sys.stderr)
    return 1


def walk_source():
    for root, dirs, files in os.walk(REPO_ROOT):
        dirs[:] = [d for d in dirs
                   if d not in (".git", "build64", "3rdparty", ".claude")]
        for fn in files:
            if not fn.endswith((".cpp", ".h", ".hpp", ".cc", ".cxx")):
                continue
            yield os.path.join(root, fn)


def classify(fpath, scan_line, raw_line):
    """Class tag for a Mode B site."""
    if is_allowlisted(fpath):
        return "ALLOWLISTED"
    if RESTORE_ARG_RE.search(raw_line):
        return "RESTORE"
    if OWNER_TAG_RE.search(raw_line):
        return "OWNER-TAGGED"
    return "RAW"


def mode_b():
    """List every existing glBindFramebuffer site with class tag (advisory)."""
    per_file = {}  # rel_path -> [(line_no, tag), ...]
    for path in walk_source():
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                raw_text = f.read()
        except OSError:
            continue
        rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
        hits = []
        # STRIPPER-AUDIT-1 (2026-07-31): comment-strip the WHOLE FILE with the
        # shared lexer instead of the hand-rolled per-line block tracker that
        # used to live here. That tracker computed its opener index on the RAW
        # line (`line.rfind("/*")`) AFTER stripping `//` into `scan`, so a stock
        # MechCommander banner --
        #     //****************************************************************
        # -- armed in_block (`//` followed by `*` IS a `/*` at offset 1) and the
        # scanner then skipped every following line until some unrelated `*/`.
        # MEASURED across GameOS/ code/ mclib/ editor/: 264 files affected,
        # 67,983 lines of real code skipped. This axis is the lucky one -- 0 of
        # its glBindFramebuffer sites currently sit in a phantom span, so its
        # backlog total is unchanged. Its four siblings hide 10/10/13/15 real
        # sites. Fixed here anyway: the difference between the axes is where the
        # banner comments happen to fall, which is not a property anyone is
        # maintaining. The lexer preserves line structure exactly, so line
        # numbers are unchanged. Pre-filter on the RAW text first -- EXACT, not
        # an approximation, since stripping only ever DELETES characters.
        if not BIND_RE.search(raw_text):
            continue
        raw_lines = raw_text.splitlines()
        for i, scan in enumerate(
                checklib.strip_comments(raw_text).splitlines(), 1):
            raw = raw_lines[i - 1] if i <= len(raw_lines) else ""
            if BIND_RE.search(scan):
                hits.append((i, classify(rel, scan, raw)))
        if hits:
            per_file[rel] = hits

    total = sum(len(v) for v in per_file.values())
    by_class = {}
    for hits in per_file.values():
        for _, tag in hits:
            by_class[tag] = by_class.get(tag, 0) + 1

    print("[check-raw-gl-fbobind] Mode B (audit) -- frozen glBindFramebuffer backlog")
    print("[check-raw-gl-fbobind] total glBindFramebuffer sites: %d across %d file(s)"
          % (total, len(per_file)))
    print("[check-raw-gl-fbobind] by class: %s"
          % ("  ".join("%s=%d" % (k, by_class[k]) for k in sorted(by_class))))
    print("")
    for fpath in sorted(per_file):
        hits = per_file[fpath]
        cls = {}
        for _, tag in hits:
            cls[tag] = cls.get(tag, 0) + 1
        cls_str = " ".join("%s=%d" % (k, cls[k]) for k in sorted(cls))
        print("  %-44s %2d  %s" % (fpath, len(hits), cls_str))
        for ln, tag in hits:
            print("       %s:%d  [%s]" % (fpath, ln, tag))
    print("")
    raw_count = by_class.get("RAW", 0)
    print("[check-raw-gl-fbobind] (advisory; exit 0) v1 freezes these files; "
          "RAW (unsanctioned) sites: %d. v2 (after RESOURCE-REGISTRY-COMPLETE-1) "
          "will require the FBO-OWNER tag to name a ledger-registered logical "
          "resource." % raw_count)
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=None,
                    help="base ref for diff (default: merge-base HEAD origin/main)")
    ap.add_argument("--all", action="store_true",
                    help="Mode B: list the full glBindFramebuffer backlog (advisory)")
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress OK chatter (Mode A); offenders still printed")
    args = ap.parse_args()

    if args.all:
        return mode_b()

    base = args.base or default_base()
    return mode_a(base, quiet=args.quiet)


if __name__ == "__main__":
    sys.exit(main())
