#!/usr/bin/env python3
"""MULTIPLEX-ANCHOR-1 self-test (VERIFY-DEBT-1).

check-sampler-bindings.py's MULTIPLEX_ANCHORS table replaced DEMOTED_ANCHORS, a
bare tuple that nothing read. Demoting `ssaoTex` out of HARD_ANCHORS had dropped
enforced anchors 3 -> 2 with no compensating check; the replacement re-arms the
name PER PASS. This proves the replacement actually fires, by building a synthetic
repo root that reproduces the real ssaoTex multiplex and then breaking it one way
at a time.

  M0  clean synthetic root                      -> no multiplex-anchor FAIL
  M1  ssao_blur.frag unit comments INVERTED     -> FAIL (the real CONTRACT-REPAIR-2
                                                   bug; the whole-name unit SET is
                                                   {0,1} either way, so ONLY the
                                                   per-pass comment pin sees it)
  M2  ssaoBlurProg_ binds ssaoTex to unit 2     -> FAIL (per-pass unit + unit set)
  M3  the blur pass is deleted (de-multiplexed) -> FAIL (promote back to HARD_ANCHORS)
  M4  a THIRD pass declares ssaoTex             -> FAIL (unexpected decl file)

Run:  py -3 scripts/check-sampler-bindings-selftest.py
Exit 0 = every clause fired where it should and nowhere else.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

HERE = Path(__file__).resolve().parent
CHECKER = HERE / "check-sampler-bindings.py"

APPLY_FRAG = """#version 430
// synthetic mirror of shaders/ssao_apply.frag
uniform sampler2D ssaoTex;       // unit 0: half-res AO
uniform vec2 ssaoTexel;
void main() {}
"""

BLUR_FRAG = """#version 430
// synthetic mirror of shaders/ssao_blur.frag
uniform sampler2D ssaoTex;        // unit 1: half-res AO (the buffer being blurred)
uniform sampler2D sceneDepthTex;  // unit 0: full-res scene depth (edge-stopping)
void main() {}
"""

CPP = """// synthetic mirror of the gos_postprocess.cpp bind sites
void gosPostProcess::runSSAO() {
    ssaoBlurProg_->setInt("ssaoTex", 1);
    ssaoBlurProg_->setInt("sceneDepthTex", 0);
    ssaoApplyProg_->setInt("ssaoTex", 0);
}
"""

FAILURES = []


def build_root(tmp: Path) -> Path:
    root = tmp / "root"
    (root / "shaders").mkdir(parents=True)
    (root / "GameOS").mkdir(parents=True)
    (root / "shaders" / "ssao_apply.frag").write_text(APPLY_FRAG, encoding="utf-8")
    (root / "shaders" / "ssao_blur.frag").write_text(BLUR_FRAG, encoding="utf-8")
    (root / "GameOS" / "gos_postprocess.cpp").write_text(CPP, encoding="utf-8")
    return root


def run(root: Path, tmp: Path, tag: str) -> dict:
    out = tmp / f"report_{tag}.json"
    subprocess.run([sys.executable, str(CHECKER), "--root", str(root),
                    "--json", str(out), "--quiet"],
                   capture_output=True, text=True, timeout=180, **console_child.child_kwargs(redirected=True))
    return json.loads(out.read_text(encoding="utf-8"))


def mx_fails(rep: dict) -> "list[str]":
    return [f for f in rep["fails"] if f.startswith("multiplex-anchor")]


def check(label: str, ok: bool, detail: str = "") -> None:
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f" -- {detail}" if detail else ""))
    if not ok:
        FAILURES.append(label)


def expect_fail(tmp: Path, tag: str, label: str, mutate, needle: str) -> None:
    root = build_root(tmp / tag)
    mutate(root)
    rep = run(root, tmp, tag)
    f = mx_fails(rep)
    hit = any(needle.lower() in x.lower() for x in f)
    check(label, bool(f) and hit,
          (f[0][:120] if f else "NO multiplex-anchor FAIL was raised"))


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="mxanchor_selftest_"))
    try:
        print("check-sampler-bindings-selftest: MULTIPLEX-ANCHOR-1")

        # M0 -- the clean synthetic root must NOT trip the anchor.
        (tmp / "m0").mkdir(parents=True)
        rep0 = run(build_root(tmp / "m0"), tmp, "m0")
        check("M0 clean synthetic root raises no multiplex-anchor FAIL",
              not mx_fails(rep0), str(mx_fails(rep0))[:120])
        check("M0 the anchor is actually evaluated (1 name, 2 passes)",
              rep0["summary"]["multiplex_anchors"] == 1
              and rep0["summary"]["multiplex_anchor_passes"] == 2,
              f"anchors={rep0['summary']['multiplex_anchors']} "
              f"passes={rep0['summary']['multiplex_anchor_passes']}")

        # M1 -- the real historical bug: inverted unit comments in the blur pass.
        def m1(root: Path) -> None:
            p = root / "shaders" / "ssao_blur.frag"
            p.write_text(BLUR_FRAG
                         .replace("ssaoTex;        // unit 1",
                                  "ssaoTex;        // unit 0")
                         .replace("sceneDepthTex;  // unit 0",
                                  "sceneDepthTex;  // unit 1"),
                         encoding="utf-8")
        expect_fail(tmp, "m1",
                    "M1 inverted unit comments in ssao_blur.frag are caught",
                    m1, "per-pass comment drift")

        # M2 -- per-pass unit drift.
        def m2(root: Path) -> None:
            p = root / "GameOS" / "gos_postprocess.cpp"
            p.write_text(CPP.replace('setInt("ssaoTex", 1)',
                                     'setInt("ssaoTex", 2)'), encoding="utf-8")
        expect_fail(tmp, "m2",
                    "M2 ssaoBlurProg_ rebound to unit 2 is caught",
                    m2, "expected unit 1")

        # M3 -- de-multiplexed: the blur pass disappears entirely.
        def m3(root: Path) -> None:
            (root / "shaders" / "ssao_blur.frag").unlink()
            p = root / "GameOS" / "gos_postprocess.cpp"
            p.write_text("\n".join(l for l in CPP.splitlines()
                                   if "ssaoBlurProg_" not in l), encoding="utf-8")
        expect_fail(tmp, "m3",
                    "M3 de-multiplexed name demands promotion back to HARD_ANCHORS",
                    m3, "NO LONGER MULTIPLEXED")

        # M4 -- an unrecorded third pass appears.
        def m4(root: Path) -> None:
            (root / "shaders" / "ssao_third.frag").write_text(
                "#version 430\nuniform sampler2D ssaoTex; // unit 4\nvoid main(){}\n",
                encoding="utf-8")
            p = root / "GameOS" / "gos_postprocess.cpp"
            p.write_text(CPP.replace('}', '    ssaoThirdProg_->setInt("ssaoTex", 4);\n}'),
                         encoding="utf-8")
        expect_fail(tmp, "m4",
                    "M4 an unrecorded THIRD pass declaring ssaoTex is caught",
                    m4, "unexpected GLSL decl file")

        print()
        if FAILURES:
            print(f"check-sampler-bindings-selftest: FAIL "
                  f"({len(FAILURES)}): " + ", ".join(FAILURES))
            return 1
        print("check-sampler-bindings-selftest: OK -- every multiplex-anchor clause "
              "fired on its synthetic mutation")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
