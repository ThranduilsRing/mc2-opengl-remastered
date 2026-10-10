#!/usr/bin/env python3
"""tools/asset_cook/tests/run_tests.py — manifest-schema TDD harness.

Asserts:
  - Cook golden (mc2-asset-manifest-v1): VALIDATES
  - Cook broken fixtures: REJECTED for the intended reason
  - Authoring golden (authoring format): VALIDATES
  - Authoring broken (material_fail_*): REJECTED for the intended reason

Pure data/validation — no cook, no engine. (C1: single canonical validator)

  py -3 tools/asset_cook/tests/run_tests.py

Exit 0 = all expectations met. Exit 1 = any golden-fail or broken-pass.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

HERE = Path(__file__).resolve().parent
VALIDATOR = HERE.parent / "validate_asset_manifest.py"

# (path, expected_exit, must_mention) — must_mention asserts the failure is for
# the INTENDED reason, not an incidental schema slip.
CASES = [
    # FORMAT A: Cook manifests (mc2-asset-manifest-v1)
    (HERE / "golden" / "bigbox" / "manifest.json", 0, None),
    (HERE / "broken" / "broken_class_camelcase.json", 1, "class"),
    (HERE / "broken" / "broken_slot_index.json", 1, "slot"),
    (HERE / "broken" / "broken_replaces_mismatch.json", 1, "replaces"),
    (HERE / "broken" / "broken_scale.json", 1, "scale"),
    (HERE / "broken" / "broken_alphaclass.json", 1, "alphaClass"),
    # FORMAT B: Authoring manifests (scaffold format, ported per ruling C1)
    (HERE / "golden" / "authoring" / "minimal_asset_manifest.json", 0, None),
    (HERE / "golden" / "authoring" / "material_validation_pass.json", 0, None),
    (HERE / "golden" / "authoring" / "extended_asset_manifest.json", 0, None),
    (HERE / "broken" / "material_fail_alphamode.json", 1, "alphaMode"),
    (HERE / "broken" / "material_fail_colorspace.json", 1, "colorSpace"),
    (HERE / "broken" / "material_fail_normal_no_tangents.json", 1, "hasTangents"),
]


def main() -> int:
    failures = 0
    print(f"{'CASE':<34} {'WANT':>4} {'GOT':>4}  RESULT")
    print("-" * 64)
    for path, want_exit, must_mention in CASES:
        proc = subprocess.run(
            [sys.executable, str(VALIDATOR), str(path)],
            capture_output=True, text=True,
            **console_child.child_kwargs(redirected=True),
        )
        got = proc.returncode
        out = proc.stdout + proc.stderr
        ok = (got == want_exit)
        if ok and must_mention and must_mention.lower() not in out.lower():
            ok = False
            reason = f"exit ok but did not mention {must_mention!r}"
        else:
            reason = "" if ok else f"exit {got} != {want_exit}"
        if not ok:
            failures += 1
        print(f"{path.name:<34} {want_exit:>4} {got:>4}  {'PASS' if ok else 'FAIL ' + reason}")
        if not ok:
            for line in out.splitlines():
                print(f"      | {line}")
    print("-" * 64)
    cook_cases = sum(1 for p, _, _ in CASES if "authoring" not in str(p) and "material_fail" not in p.name)
    auth_cases = len(CASES) - cook_cases
    print(f"schema: {len(CASES) - failures}/{len(CASES)} cases as expected "
          f"({cook_cases} cook + {auth_cases} authoring)")

    # G1 stage geometry gate
    for sub_test in ("test_g1_stage.py", "test_g2_textures.py", "test_g3b_assemble.py",
                     "test_g_pathb_2civliving.py"):
        r = subprocess.run([sys.executable, str(HERE / sub_test)], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        print(r.stdout.strip() + (("\n" + r.stderr.strip()) if r.stderr.strip() else ""))
        if r.returncode != 0:
            failures += 1

    print("-" * 64)
    if failures:
        print(f"{failures} check(s) FAILED")
        return 1
    print(f"ALL PASS -- schema {len(CASES)}/{len(CASES)} "
          f"(cook: 1 golden + 5 broken; authoring: 3 golden + 3 material_fail) "
          f"+ G1 stage + G2 cook + G3b assemble/projection/no-central-write")
    return 0


if __name__ == "__main__":
    sys.exit(main())
