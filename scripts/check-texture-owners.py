#!/usr/bin/env python3
"""check-texture-owners.py -- TEXTURE-SAMPLER-LEDGER-1 static classification gate.

Mirrors check-gpu-buffer-owners.py's owned-OR-excluded partition, but for
image/sampler resources instead of SSBO/UBO buffers. Every glGenTextures /
glGenSamplers / glGenRenderbuffers call site in the RUNTIME renderer TUs
(GameOS/gameos + RenderCore + RenderWorld + GameAdapters) MUST be classified,
either:

  1. AUTO-RECOGNIZED as a render target -- the generated handle var is passed to
     a glFramebufferTexture* attach call in the SAME file (an FBO color/depth
     attachment already covered by the 8-FBO ledger). No manual tag required.
  OR
  2. Carry a `// TEX-CLASS: <category>` tag on the gen line or the 2 lines above
     it, with <category> one of:
        render-target    FBO attachment / GPU-produced image (or copy dst)
        asset-pool       content texture (mission/UI/mech/terrain/HDRI TGA-PNG-
                         BCn), file-static global or load-from-file factory
        per-pass-rebind  sampled per draw, GL-safe but Vulkan-needs-work
                         (sampler objects, compute-written sampled images) --
                         the DEFERRED class that does not survive Vk as-is
        debug-only       gated behind a debug/diagnostic path
        editor-only      EditRel-only (should not appear in engine TUs)
        dead-path        provably retired / gated-OFF

Any gen site that is neither auto-recognized NOR validly tagged FAILs (exit 1,
offender named). A tag with an unknown category FAILs.

Editor / asset_viewer / ui_editor tool textures are OUT OF SCOPE (their own
glGenTextures sites live outside SCAN_DIRS and are not counted), matching the
recon's runtime-engine-only scope.

Pure stdlib, grep/parse only. Exit 0 = all sites classified; nonzero + message
on any unclassified/bad-category site. Comments-only invariant -- no build.
"""
import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Runtime renderer TUs. Editor/asset_viewer/ui_editor deliberately excluded
# (out of engine scope).
SCAN_DIRS = [
    os.path.join(REPO_ROOT, "GameOS", "gameos"),
    os.path.join(REPO_ROOT, "RenderCore"),
    os.path.join(REPO_ROOT, "RenderWorld"),
    os.path.join(REPO_ROOT, "GameAdapters"),
]

VALID_CATEGORIES = {
    "render-target",
    "asset-pool",
    "per-pass-rebind",
    "debug-only",
    "editor-only",
    "dead-path",
}

# `glGenTextures(<n>, &<var>)` / `glGenSamplers(...)` / `glGenRenderbuffers(...)`
# -- capture the target handle var (peel one `[idx]` subscript, e.g.
# hzbLevelTex_[level] -> hzbLevelTex_). String-literal false positives (a printf
# mentioning "glGenTextures") never match because they lack the `(<n>, &` form.
GEN_CALL = re.compile(
    r"\bglGen(?:Textures|Samplers|Renderbuffers)\s*\(\s*[^,]+,\s*&\s*"
    r"([A-Za-z_]\w*)")

# `// TEX-CLASS: <category>` (rest of line is free-form rationale).
TEX_TAG = re.compile(r"//\s*TEX-CLASS:\s*([A-Za-z0-9_-]+)")

# The var appears as an argument to a glFramebufferTexture* attach call.
FBO_ATTACH = re.compile(r"glFramebufferTexture[A-Za-z0-9]*\s*\(")

TAG_WINDOW = 2  # tag may sit on the gen line or the 2 lines above it.


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def fbo_attached_vars(text):
    """Vars passed to any glFramebufferTexture* call in this file.

    Attach calls may span multiple physical lines, so scan the whole (comment-
    stripped) text: for each glFramebufferTexture*( ... ) call, collect every
    bare identifier in its argument list.
    """
    body = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    body = re.sub(r"//.*$", "", body, flags=re.MULTILINE)
    vars_ = set()
    for m in FBO_ATTACH.finditer(body):
        # grab up to the matching close paren (shallow -- these calls don't nest
        # parens in practice; a bounded 300-char window is plenty).
        seg = body[m.end():m.end() + 300]
        depth = 1
        end = 0
        for i, c in enumerate(seg):
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        args = seg[:end]
        for idm in re.finditer(r"\b([A-Za-z_]\w*)\b", args):
            vars_.add(idm.group(1))
    return vars_


def gen_sites():
    """Yield (relpath, lineno, var, lines, attached) per gen call site."""
    for d in SCAN_DIRS:
        if not os.path.isdir(d):
            continue
        for dirpath, _dirs, files in os.walk(d):
            for fn in sorted(files):
                if not fn.endswith(".cpp"):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
                text = read(path)
                lines = text.splitlines()
                attached = fbo_attached_vars(text)
                for i, line in enumerate(lines):
                    # ignore matches inside string literals: a real call has an
                    # `&` before the handle and is not preceded by a `"` on the
                    # same statement fragment. GEN_CALL already requires `&<var>`
                    # which printf strings lack, so this is sufficient.
                    m = GEN_CALL.search(line)
                    if not m:
                        continue
                    yield rel, i + 1, m.group(1), lines, attached


def classify(lines, i, var, attached):
    """Return (category, how) for the gen site at 0-based line i, or (None,None).

    how = 'tag' | 'auto-fbo'.
    """
    # explicit tag on this line or the TAG_WINDOW lines above.
    for j in range(max(0, i - TAG_WINDOW), i + 1):
        tm = TEX_TAG.search(lines[j])
        if tm:
            return tm.group(1), "tag"
    # auto-recognized render target: handle attached to an FBO in this file.
    if var in attached:
        return "render-target", "auto-fbo"
    return None, None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", "-q", action="store_true",
                    help="suppress OK chatter; offenders still printed")
    args = ap.parse_args()

    sites = list(gen_sites())
    if not sites:
        print("[check-texture-owners] ERROR: parsed 0 glGen{Textures,Samplers,"
              "Renderbuffers} sites (parser broke?)", file=sys.stderr)
        return 2

    failures = []
    by_cat = {}
    n_auto = 0
    for rel, lineno, var, lines, attached in sites:
        where = "%s:%d %s" % (rel, lineno, var)
        cat, how = classify(lines, lineno - 1, var, attached)
        if cat is None:
            failures.append(
                "%s -- un-classified image/sampler gen site (needs a "
                "// TEX-CLASS: <category> tag OR must be an FBO attachment)"
                % where)
            continue
        if cat not in VALID_CATEGORIES:
            failures.append(
                "%s -- // TEX-CLASS category %r not in %s"
                % (where, cat, sorted(VALID_CATEGORIES)))
            continue
        by_cat[cat] = by_cat.get(cat, 0) + 1
        if how == "auto-fbo":
            n_auto += 1

    if failures:
        print("[check-texture-owners] FAIL:", file=sys.stderr)
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        return 1

    if not args.quiet:
        print("[check-texture-owners] %d image/sampler gen site(s), 0 "
              "unclassified:" % len(sites))
        for cat in sorted(by_cat):
            print("  %-16s %d" % (cat, by_cat[cat]))
        print("  (of which %d render-target(s) auto-recognized via FBO attach)"
              % n_auto)
        print("[check-texture-owners] PASS -- every glGen{Textures,Samplers,"
              "Renderbuffers} site is FBO-attached OR carries a valid "
              "// TEX-CLASS: tag.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
