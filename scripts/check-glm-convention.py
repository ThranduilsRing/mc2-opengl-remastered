#!/usr/bin/env python3
"""check-glm-convention -- engine matrices keep the Stuff ROW-vector convention; glm's column-convention helpers stay out.

WHY
---
Step 5 (C++23, 2026-10-18) made the engine matrices glm::mat4 holding the STUFF matrix: entry (row r, column c) is m[c][r],
points are row vectors (p' = p * M, mc2::math::transformPoint) and the translation is row 3. glm's helpers assume the column
convention: glm::translate writes column 3, glm::mat4_cast / glm::toMat4 return the transpose of the engine rotation,
glm::lookAt / perspective / ortho build column-convention clip matrices, and glm::quat(w, x, y, z) takes w first while every
engine quaternion was written x, y, z, w. Each compiles and is silently wrong on an engine matrix. The convention adapters live
in mclib/math/mc2_xform.h; engine code calls those.

RULE
----
In code/, mclib/ (except mclib/math/), GameOS/, RenderCore/, RenderWorld/, GameAdapters/, EditorBridge/, editor/ and tools/:
no glm::translate, rotate, scale, lookAt*, perspective*, ortho*, frustum*, mat4_cast, mat3_cast, toMat4, toMat3, quat_cast,
eulerAngle*, extractEulerAngle*, yawPitchRoll, and no glm::quat built from components (glm::quat(...) or
glm::quat q(a, b, ...); use mc2::math::quatXYZW). A file that
builds GPU-side column-convention matrices on purpose is listed in ALLOW with its reason.

USAGE
-----
  py -3 scripts/check-glm-convention.py              # exit 1 on a violation
  py -3 scripts/check-glm-convention.py --selftest   # negative control: every forbidden shape must be reported
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ("code", "mclib", "GameOS", "RenderCore", "RenderWorld", "GameAdapters", "EditorBridge", "editor", "tools")
EXEMPT_PREFIX = ("mclib/math/",)
ALLOW = {
    # path: reason (none today: no engine file uses a glm column-convention helper)
}
FORBIDDEN = re.compile(
    r"\bglm::(?:translate|rotate|scale|lookAt\w*|perspective\w*|ortho\w*|frustum\w*|mat4_cast|mat3_cast|toMat4|toMat3|"
    r"quat_cast|eulerAngle\w*|extractEulerAngle\w*|yawPitchRoll)\s*\(|\bglm::quat\s*[({]|\bglm::quat\s+\w+\s*[({][^;)}]*,")
EXTS = (".cpp", ".h", ".hpp", ".inl", ".cc")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def scan_text(text):
    out = []
    for i, line in enumerate(strip_comments(text).split("\n"), 1):
        for m in FORBIDDEN.finditer(line):
            out.append((i, m.group(0)))
    return out


def scan_tree(root=ROOT):
    bad = []
    for d in DIRS:
        base = os.path.join(root, d)
        for dp, dn, fn in os.walk(base):
            dn[:] = [x for x in dn if not x.startswith(("build", "3rdparty", "out", "."))]
            for f in fn:
                if not f.endswith(EXTS):
                    continue
                path = os.path.join(dp, f)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                if rel.startswith(EXEMPT_PREFIX) or rel in ALLOW:
                    continue
                try:
                    text = open(path, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                for line, what in scan_text(text):
                    bad.append(f"{rel}:{line}: {what}")
    return bad


PROBES_BAD = [
    "m = glm::translate(m, p);",
    "auto r = glm::mat4_cast(q);",
    "glm::quat q(w, x, y, z);",
    "auto q = glm::quat (1.0f, 0.0f, 0.0f, 0.0f);",
    "glm::mat4 v = glm::lookAt(eye, at, up);",
    "glm::mat4 p = glm::perspectiveFov(f, w, h, n, z);",
    "auto q2 = glm::quat_cast(m);",
    "auto e = glm::eulerAngleYXZ(y, x, z);",
]
PROBES_OK = [
    "glm::quat q = mc2::math::quatXYZW(x, y, z, w);",
    "// glm::translate is forbidden here",
    "auto p = mc2::math::transformPoint(v, m);",
    "glm::quat rotation;",
]


def selftest():
    fails = 0
    for p in PROBES_BAD:
        if not scan_text(p):
            print("SELFTEST FAIL: not reported:", p)
            fails += 1
    for p in PROBES_OK:
        if scan_text(p):
            print("SELFTEST FAIL: reported:", p)
            fails += 1
    print(f"[GLM_CONVENTION] selftest {len(PROBES_BAD)} bad probes fire, {len(PROBES_OK)} clean pass, {fails} failures")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    bad = scan_tree()
    for b in bad:
        print("GLM_CONVENTION:", b)
    print(f"[GLM_CONVENTION] {len(bad)} violation(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
