#!/usr/bin/env python3
"""SHADER-VARIANT-MATRIX-HARNESS-1 — pure-parse shader variant matrix.

Cross-checks the two SEPARATELY-AUTHORED sides of MC2's shader variant system —
the C++ `#define` strings injected into makeProgram() prefixes, and the GLSL
`#ifdef`/`#if defined` guards that consume them — WITHOUT compiling any shader.
No glslangValidator, no GL context, no engine, no game.

WHAT IT ADDS (and what it does NOT):
  * Adds VISIBILITY + regression coverage for variant-matrix DRIFT: a define the
    C++ injects that no shader guards (stale/typo injection), and a guard a
    shader uses that nothing ever provides (dead branch / lost injection).
  * It does NOT fix shader variants, does NOT compile, and does NOT validate
    binary layout (output locations, bindings, std430 offsets) — that is
    tools/shader_reflect/reflect.py's job (which DOES compile). This harness is
    the complementary pre-compile symbol-graph check; reflect.py's
    KNOWN_VARIANT_MACROS only spans MC2_COALESCE / MC2_OBJECT_ID_BUFFER.

POLICY — WARN-first. Broad drift discovery is reported to stderr and NEVER
fails the run. Only the NAMED audit anchors (verified covered at authoring time:
MRT_ENABLED, TERRAIN_NORMAL_ARRAY, MC2_SHADOW_CSM, MC2_SHADOW_CSM_MAX) are hard
regression tests — they fail if a future edit drops either the C++ injection or
the GLSL guard for those specific, currently-stable pairs.

Run:
  py -3 tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py
  py -3 tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py --list
  py -3 tools/shader_variant_matrix_harness/shader_variant_matrix_harness.py --json
"""

import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))   # tools/<harness>/ -> repo root

sys.path.insert(0, os.path.join(_REPO_ROOT, "tools", "contract_harness_common"))
from contract_harness import Harness, Ctx              # noqa: E402

# C++ trees that build shader-prefix define strings.
CPP_DIRS = ("GameOS", "mclib", "RenderCore")
CPP_EXTS = (".cpp", ".h", ".hpp")
SHADER_DIR = "shaders"
SHADER_EXTS = (".vert", ".frag", ".comp", ".tesc", ".tese", ".geom", ".glsl", ".hglsl")

# Named audit anchors — verified injected+guarded at authoring time. Value macros
# (referenced as a token / fallback-#define rather than an #ifdef guard) are
# flagged with value=True so the anchor accepts any GLSL reference.
# GL end (lane2 s68): the C++ shader-prefix injection these anchors audited is gone with the GL
# shader path, so there is nothing left to anchor. The classifier selftest and drift report remain.
ANCHORS = ()

_STRLIT = re.compile(r'"((?:\\.|[^"\\])*)"')
_DEFINE_IN = re.compile(r'#define\s+([A-Za-z_]\w*)')
_GUARD_IFDEF = re.compile(r'^\s*#\s*(ifdef|ifndef)\s+([A-Za-z_]\w*)')
_GUARD_IF = re.compile(r'^\s*#\s*(if|elif)\b')
_DEFINED = re.compile(r'defined\s*\(?\s*([A-Za-z_]\w*)\s*\)?')
_GLSL_DEFINE = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)')
_WORD = re.compile(r'[A-Za-z_]\w*')


def _is_builtin(name):
    return name.startswith("GL_") or name.startswith("__") or name == "VERSION"


# ---- pure parsers (unit-testable, no filesystem) ---------------------------

def extract_injected_defines(cpp_text):
    """Macros `#define`d inside C++ string literals (shader-prefix injection).

    Only `#define` tokens that appear INSIDE a double-quoted string count, so
    real C++ preprocessor directives are not mistaken for injected shader
    defines. Handles multiple defines in one literal (snprintf format strings)."""
    found = set()
    for line in cpp_text.splitlines():
        if "#define" not in line:
            continue
        for lit in _STRLIT.findall(line):
            for m in _DEFINE_IN.findall(lit):
                found.add(m)
    return found


def extract_glsl_guards(glsl_text):
    """Macros consumed by #ifdef/#ifndef/#if defined()/#elif defined()."""
    found = set()
    for line in glsl_text.splitlines():
        mo = _GUARD_IFDEF.match(line)
        if mo:
            found.add(mo.group(2))
            continue
        if _GUARD_IF.match(line):
            for name in _DEFINED.findall(line):
                found.add(name)
    return found


def extract_glsl_defines(glsl_text):
    """Macros defined inline in GLSL (self-provided / fallback defaults)."""
    return {mo.group(1) for line in glsl_text.splitlines()
            if (mo := _GLSL_DEFINE.match(line))}


def classify(injected, guards, gdefs, token_text):
    """Return (covered, warn_used_unprovided, warn_injected_unreferenced).

    token_text = concatenation of all GLSL source, used to tell a genuinely
    unreferenced injected macro from a value macro used as a bare token."""
    tokens = set(_WORD.findall(token_text))
    used_unprovided = sorted(
        m for m in guards
        if m not in injected and m not in gdefs and not _is_builtin(m))
    injected_unreferenced = sorted(
        m for m in injected
        if m not in guards and m not in gdefs and m not in tokens
        and not _is_builtin(m))
    covered = sorted((injected & (guards | gdefs)) - set(filter(_is_builtin, injected)))
    return covered, used_unprovided, injected_unreferenced


# ---- collectors (filesystem) -----------------------------------------------

def _read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def _walk(root, exts):
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            if fn.endswith(exts):
                yield os.path.join(dirpath, fn)


def collect():
    """Scan the repo. Returns (injected, guards, gdefs, glsl_blob)."""
    injected, guards, gdefs = set(), set(), set()
    blob = []
    for d in CPP_DIRS:
        root = os.path.join(_REPO_ROOT, d)
        if not os.path.isdir(root):
            continue
        for p in _walk(root, CPP_EXTS):
            injected |= extract_injected_defines(_read(p))
    sroot = os.path.join(_REPO_ROOT, SHADER_DIR)
    for p in _walk(sroot, SHADER_EXTS):
        txt = _read(p)
        blob.append(txt)
        guards |= extract_glsl_guards(txt)
        gdefs |= extract_glsl_defines(txt)
    return injected, guards, gdefs, "\n".join(blob)


_CACHE = {}


def _matrix():
    if "m" not in _CACHE:
        _CACHE["m"] = collect()
    return _CACHE["m"]


# ---- tests -----------------------------------------------------------------

def _anchor_test(macro, is_value):
    def fn(t: Ctx) -> bool:
        injected, guards, gdefs, blob = _matrix()
        t.check(macro in injected,
                f"{macro}: no C++ '#define {macro}' injected into any shader prefix")
        if is_value:
            referenced = (macro in guards or macro in gdefs
                          or re.search(rf'\b{re.escape(macro)}\b', blob) is not None)
            t.check(referenced, f"{macro}: value macro not referenced by any shader")
        else:
            t.check(macro in guards,
                    f"{macro}: no GLSL #ifdef/#if-defined guard consumes it")
        return t.failures == 0
    return fn


def test_matrix_drift_report(t: Ctx) -> bool:
    """WARN-first visibility. ALWAYS passes; prints drift to stderr so a
    registered run never fails on broad discovery."""
    injected, guards, gdefs, blob = _matrix()
    covered, used_unprovided, injected_unreferenced = classify(
        injected, guards, gdefs, blob)
    pr = lambda *a: print(*a, file=sys.stderr)  # noqa: E731
    pr(f"    [matrix] injected={len(injected)} guards={len(guards)} "
       f"glsl_defines={len(gdefs)} covered={len(covered)}")
    if used_unprovided:
        pr(f"    [WARN] guard used but never provided ({len(used_unprovided)}): "
           f"{', '.join(used_unprovided)}")
    if injected_unreferenced:
        pr(f"    [WARN] injected but unreferenced by any shader "
           f"({len(injected_unreferenced)}): {', '.join(injected_unreferenced)}")
    if not used_unprovided and not injected_unreferenced:
        pr("    [matrix] no drift detected")
    return True


def test_selftest_classifier(t: Ctx) -> bool:
    """Prove the parsers + classifier actually catch drift (fake-green guard)."""
    cpp = 'defines.append("#define MRT_ENABLED 1\\n#define STALE_INJECT 1\\n");'
    inj = extract_injected_defines(cpp)
    t.check(inj == {"MRT_ENABLED", "STALE_INJECT"}, f"injected parse wrong: {inj}")

    glsl = ("#ifdef MRT_ENABLED\n#endif\n"
            "#if defined(DEAD_GUARD)\n#endif\n"
            "#define LOCAL_DEFAULT 3\n")
    guards = extract_glsl_guards(glsl)
    t.check(guards == {"MRT_ENABLED", "DEAD_GUARD"}, f"guard parse wrong: {guards}")
    gdefs = extract_glsl_defines(glsl)
    t.check(gdefs == {"LOCAL_DEFAULT"}, f"gdef parse wrong: {gdefs}")

    covered, used_unprovided, injected_unreferenced = classify(
        inj, guards, gdefs, glsl)
    t.check("MRT_ENABLED" in covered, "MRT_ENABLED should be covered")
    t.check(used_unprovided == ["DEAD_GUARD"],
            f"expected DEAD_GUARD unprovided, got {used_unprovided}")
    t.check(injected_unreferenced == ["STALE_INJECT"],
            f"expected STALE_INJECT unreferenced, got {injected_unreferenced}")
    # Builtins must be ignored.
    cov2, uu2, _ = classify({"GL_ES"}, {"GL_ES", "__VERSION__"}, set(), "")
    t.check(uu2 == [] and cov2 == [], f"builtins not ignored: {uu2} {cov2}")
    return t.failures == 0


def test_demo_fail(t: Ctx) -> bool:
    """Intentional-failure demo (only via --test demo_fail)."""
    t.check(False, "intentional demo failure")
    return False


h = Harness("shader_variant_matrix_harness")
for a in ANCHORS:
    h.add(f"anchor_{a['macro'].lower()}", _anchor_test(a["macro"], a["value"]))
h.add("matrix_drift_report", test_matrix_drift_report)
h.add("selftest_classifier", test_selftest_classifier)
h.add("demo_fail", test_demo_fail, in_default=False)

if __name__ == "__main__":
    raise SystemExit(h.run())
