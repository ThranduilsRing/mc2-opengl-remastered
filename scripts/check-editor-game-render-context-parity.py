#!/usr/bin/env python3
"""check-editor-game-render-context-parity.py — GAMEOS-RENDER-CONTEXT-PARITY-1

Static guard ensuring the game and the editor establish their shared
render-context conventions (clip-control origin / NDC depth mode) through the
ONE shared entry point `InitializeRenderContextConventions`, and nowhere else
in an init path.

Why: the game (GameOS/gameos/gameosmain.cpp InitGameOS) and the editor
(editor/EditorGameOS.cpp InitGameOS) share the same GLSL shaders, which assume
reverse-Z [0,1] NDC depth via glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE). If
either host re-inlines glClipControl with a different origin/depth-mode — or
forgets it — depth silently corrupts. Slice 0 fixed exactly that divergence;
this check makes the class of bug non-recurring.

FAILS when:
  1. A raw `glClipControl(` call appears anywhere OUTSIDE the shared
     implementation file (gos_render_context.cpp) — i.e. a host re-inlined the
     convention instead of routing through InitializeRenderContextConventions.
  2. The shared implementation sets clip-control to anything other than the
     canonical (GL_LOWER_LEFT, GL_ZERO_TO_ONE) — i.e. the single source of
     truth itself drifted.
  3. A host init TU (gameosmain.cpp / EditorGameOS.cpp) does NOT call
     InitializeRenderContextConventions (the convention got dropped entirely).

Exit code: 0 on PASS, 1 on any FAIL. Grep-lite, Windows py3 runnable.

Usage:
  py -3 scripts/check-editor-game-render-context-parity.py [--root <repo>] [--quiet]
"""
import argparse
import os
import re
import sys

SHARED_IMPL = os.path.join("GameOS", "gameos", "gos_render_context.cpp")
HOST_INIT_TUS = [
    os.path.join("GameOS", "gameos", "gameosmain.cpp"),
    os.path.join("editor", "EditorGameOS.cpp"),
]
SHARED_CALL = "InitializeRenderContextConventions"
CANONICAL_CLIP = "(GL_LOWER_LEFT, GL_ZERO_TO_ONE)"

# A glClipControl( call, capturing its argument list up to the closing paren.
CLIP_CALL_RE = re.compile(r"glClipControl\s*\(([^)]*)\)")
COMMENT_LINE_RE = re.compile(r"^\s*(//|\*)")


def read(root, rel):
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def find_clip_calls(text):
    """Return list of (line_no, normalized_args) for non-comment glClipControl calls."""
    hits = []
    for i, line in enumerate(text.splitlines(), 1):
        if COMMENT_LINE_RE.match(line):
            continue
        # strip a trailing line comment so a commented mention doesn't count
        code = line.split("//", 1)[0]
        if "glClipControl" in code:
            m = CLIP_CALL_RE.search(code)
            args = m.group(1) if m else ""
            hits.append((i, "(" + re.sub(r"\s+", " ", args).strip() + ")"))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    failures = []

    def say(msg):
        if not args.quiet:
            print(msg)

    # ---- Check 2: the shared impl exists and sets the canonical convention.
    shared = read(root, SHARED_IMPL)
    if shared is None:
        failures.append("shared impl %s is missing" % SHARED_IMPL)
    else:
        clip_hits = find_clip_calls(shared)
        if not clip_hits:
            failures.append("%s does not call glClipControl — shared convention lost" % SHARED_IMPL)
        for ln, sig in clip_hits:
            if sig != CANONICAL_CLIP:
                failures.append("%s:%d glClipControl%s != canonical %s"
                                % (SHARED_IMPL, ln, sig, CANONICAL_CLIP))

    # ---- Check 1: no raw glClipControl outside the shared impl (in our dirs).
    scan_dirs = [os.path.join("GameOS", "gameos"), "editor"]
    for d in scan_dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dirpath, _, files in os.walk(base):
            for fn in files:
                if not fn.endswith((".cpp", ".c", ".cc", ".cxx")):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, fn), root)
                if rel.replace("\\", "/") == SHARED_IMPL.replace("\\", "/"):
                    continue  # the one allowed home
                text = read(root, rel)
                if text is None:
                    continue
                for ln, sig in find_clip_calls(text):
                    # Only the convention-SETTING form (literal GL_* clip-origin /
                    # depth-mode tokens) is in scope. A glClipControl that takes
                    # variables — e.g. restoreShadowGLState re-asserting a captured
                    # value after an FBO switch — is
                    # a per-frame state restore, not an init convention, so skip it.
                    if "GL_LOWER_LEFT" not in sig and "GL_UPPER_LEFT" not in sig \
                       and "GL_ZERO_TO_ONE" not in sig and "GL_NEGATIVE_ONE_TO_ONE" not in sig:
                        continue
                    failures.append("%s:%d raw glClipControl%s outside shared impl "
                                    "— route through %s" % (rel, ln, sig, SHARED_CALL))

    # ---- Check 3: each host init TU routes through the shared call.
    for rel in HOST_INIT_TUS:
        text = read(root, rel)
        if text is None:
            failures.append("host init TU %s is missing" % rel)
            continue
        # require at least one non-comment call site
        called = any(
            (SHARED_CALL + "(") in line.split("//", 1)[0]
            for line in text.splitlines()
            if not COMMENT_LINE_RE.match(line)
        )
        if not called:
            failures.append("%s does not call %s — render-context convention not "
                            "established for this host" % (rel, SHARED_CALL))

    if failures:
        print("FAIL: editor/game render-context parity")
        for f in failures:
            print("  - " + f)
        return 1

    say("PASS: editor/game render-context parity "
        "(clip-control routed through %s, canonical %s)" % (SHARED_CALL, CANONICAL_CLIP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
