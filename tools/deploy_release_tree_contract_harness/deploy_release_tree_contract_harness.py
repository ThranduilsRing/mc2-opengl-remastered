#!/usr/bin/env python3
"""SUBSYSTEM-HARNESS-ARC / DEPLOY-RELEASE-TREE-CONTRACT-HARNESS-1

Validates an ALREADY-DEPLOYED release tree's runnable shape — it does NOT run
deploy and does NOT launch the game. Complements deploy_asset_contract_harness
(which checks the SOURCE payload lists); this checks the DEPLOYED output.

Attacks the operational "deployed tree is broken / wrong target" class:
  * empty/partial shaders/  -> black terrain, and tier1 smoke still PASSES
    (a dropped .comp compiles-fail non-fatally) — smoke cannot see this.
  * missing mc2.exe / DLLs / data / .fst archives.
  * wrong/nested target (v0.4 vs v0.4c vs v0.4d confusion; a deploy nested
    inside another release tree).

Source of truth: imports scripts/deploy_payload.py constants (FFMPEG_DLLS,
SUPPORT_SCRIPTS, SHADER_EXTS) — no duplicated lists.

The release tree is EXPLICIT and NON-BLOCKING by default:
  --release-root <path>   (preferred)   or   env MC2_RELEASE_ROOT
If neither is given, every release-tree test PASSES with a clear "not configured"
diagnostic, so the aggregate runner stays green on a fresh checkout / CI.

Run:
  py -3 .../deploy_release_tree_contract_harness.py --list
  py -3 .../deploy_release_tree_contract_harness.py --release-root A:/Games/mc2-opengl/mc2-win64-v0.4
  MC2_RELEASE_TREE_STRICT=1 ... --release-root <path>   # strict full payload
"""

import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))
sys.path.insert(0, os.path.join(_REPO_ROOT, "tools", "contract_harness_common"))
sys.path.insert(0, os.path.join(_REPO_ROOT, "scripts"))

from contract_harness import Harness, Ctx           # noqa: E402
import deploy_payload as dp                          # noqa: E402

RELEASE_ROOT = None      # set in main() from --release-root / MC2_RELEASE_ROOT
EXE_NAME = "mc2.exe"
CORE_SHADERS = ["gos_terrain.frag", "gos_terrain.vert", "mech.frag", "mech.vert",
                "postprocess.frag"]


def _configured():
    return RELEASE_ROOT is not None


def _p(*parts):
    return os.path.join(RELEASE_ROOT, *parts)


def _shader_files():
    sdir = _p("shaders")
    if not os.path.isdir(sdir):
        return []
    return [f for f in os.listdir(sdir)
            if os.path.splitext(f)[1] in dp.SHADER_EXTS]


def _missing_dlls():
    return [d for d in dp.FFMPEG_DLLS if not os.path.isfile(_p(d))]


# ---- tests (each is a no-op PASS when release root is unconfigured) ---------

def test_release_root_exists(t: Ctx) -> bool:
    if not _configured():
        return True
    if not os.path.isdir(RELEASE_ROOT):
        t.fail(f"release root does not exist: {RELEASE_ROOT}")
    return t.failures == 0


def test_mc2_exe_exists(t: Ctx) -> bool:
    if not _configured():
        return True
    if not os.path.isfile(_p(EXE_NAME)):
        t.fail(f"{EXE_NAME} missing from release root")
    return t.failures == 0


def test_required_runtime_dirs_exist(t: Ctx) -> bool:
    if not _configured():
        return True
    for d in ("shaders", "data"):
        if not os.path.isdir(_p(d)):
            t.fail(f"required runtime dir missing: {d}/")
    return t.failures == 0


def test_support_scripts_present(t: Ctx) -> bool:
    if not _configured():
        return True
    for s in dp.SUPPORT_SCRIPTS.get("game", []):
        if not os.path.isfile(_p(s)):
            t.fail(f"support script missing: {s}")
    return t.failures == 0


def test_shaders_dir_nonempty(t: Ctx) -> bool:
    if not _configured():
        return True
    n = len(_shader_files())
    print(f"    deployed shader files: {n}", file=sys.stderr)
    # A deployed tree should carry the full shader set; >50 catches the
    # silent-empty-shaders -> black-terrain trap without pinning an exact count.
    if n <= 50:
        t.fail(f"shaders/ has only {n} shader files (expected the full set; "
               "empty/partial deploy = silent black terrain)")
    return t.failures == 0


def test_core_shader_files_present(t: Ctx) -> bool:
    if not _configured():
        return True
    for s in CORE_SHADERS:
        if not os.path.isfile(_p("shaders", s)):
            t.fail(f"core shader missing: shaders/{s}")
    return t.failures == 0


def test_required_dlls_present_or_reported(t: Ctx) -> bool:
    # Tolerant by default: report missing FFmpeg DLLs (movie playback dead) but
    # do NOT fail — the exe still runs. Strict mode enforces them.
    if not _configured():
        return True
    missing = _missing_dlls()
    if missing:
        print(f"    NOTE: {len(missing)} FFmpeg DLL(s) missing (movie playback "
              f"degraded): {', '.join(missing)}", file=sys.stderr)
    return True


def test_no_obvious_wrong_target_name_confusion(t: Ctx) -> bool:
    # A release tree must not contain a NESTED release tree (a deploy landed in
    # the wrong/layered target). Flag an immediate child dir that looks like
    # another release tree.
    if not _configured():
        return True
    try:
        children = [c for c in os.listdir(RELEASE_ROOT)
                    if os.path.isdir(_p(c))]
    except OSError as e:
        t.fail(f"cannot list release root: {e}")
        return False
    for c in children:
        low = c.lower()
        if low.startswith("mc2-win64") or low == "releases":
            if os.path.isfile(_p(c, EXE_NAME)):
                t.fail(f"nested release tree inside target (wrong-target deploy?): {c}/")
    return t.failures == 0


def test_release_tree_has_expected_data_shape(t: Ctx) -> bool:
    if not _configured():
        return True
    if not os.path.isdir(_p("data")):
        t.fail("data/ dir missing")
    # Packaged assets ship as .fst archives at the tree root.
    fst = [f for f in os.listdir(RELEASE_ROOT) if f.lower().endswith(".fst")] \
        if os.path.isdir(RELEASE_ROOT) else []
    print(f"    .fst archives at root: {len(fst)}", file=sys.stderr)
    if len(fst) < 3:
        t.fail(f"expected several .fst archives at root, found {len(fst)}")
    return t.failures == 0


# Strict (inDefault gated by MC2_RELEASE_TREE_STRICT; or --test): the FULL
# runtime payload must be present — every FFmpeg DLL.
def test_strict_full_runtime_payload(t: Ctx) -> bool:
    if not _configured():
        t.fail("strict requires --release-root / MC2_RELEASE_ROOT")
        return False
    for d in _missing_dlls():
        t.fail(f"strict: FFmpeg DLL missing: {d}")
    return t.failures == 0


def main():
    global RELEASE_ROOT, EXE_NAME
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--release-root", default=None)
    pre.add_argument("--exe-name", default="mc2.exe")
    ns, rest = pre.parse_known_args()
    RELEASE_ROOT = ns.release_root or os.environ.get("MC2_RELEASE_ROOT")
    EXE_NAME = ns.exe_name
    if RELEASE_ROOT is None and not any(a in rest for a in ("--list", "-h", "--help")):
        print("[deploy_release_tree] release root NOT configured — release-tree "
              "tests will PASS as no-ops. Pass --release-root <path> or set "
              "MC2_RELEASE_ROOT to actually validate a deployed tree.",
              file=sys.stderr)

    strict_default = os.environ.get("MC2_RELEASE_TREE_STRICT", "") == "1"
    h = Harness("deploy_release_tree_contract_harness")
    h.add("release_root_exists",                    test_release_root_exists)
    h.add("mc2_exe_exists",                         test_mc2_exe_exists)
    h.add("required_runtime_dirs_exist",            test_required_runtime_dirs_exist)
    h.add("support_scripts_present",                test_support_scripts_present)
    h.add("shaders_dir_nonempty",                   test_shaders_dir_nonempty)
    h.add("core_shader_files_present",              test_core_shader_files_present)
    h.add("required_dlls_present_or_reported",      test_required_dlls_present_or_reported)
    h.add("no_obvious_wrong_target_name_confusion", test_no_obvious_wrong_target_name_confusion)
    h.add("release_tree_has_expected_data_shape",   test_release_tree_has_expected_data_shape)
    h.add("strict_full_runtime_payload",            test_strict_full_runtime_payload,
          in_default=strict_default)
    return h.run(rest)


if __name__ == "__main__":
    raise SystemExit(main())
