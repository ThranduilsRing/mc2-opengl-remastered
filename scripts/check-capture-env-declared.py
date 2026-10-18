#!/usr/bin/env python3
"""CAPTURE-ENV-PIN-1 ratchet -- captures must declare the MC2_* env they ran in.

Two independent enforcement layers, because either alone is bypassable:

  [CODE]     A harness that launches mc2.exe for a capture must build its child
             environment through scripts/mc2_env_pin.py, not os.environ.copy().
             This is what stops a NEW harness reintroducing silent inheritance.

  [ARTIFACT] Every capture manifest must carry an `mc2_env` record, and that
             record must show a clean shell -- no ambient MC2_* was scrubbed --
             unless the deviation was declared. This is what catches a capture
             taken TODAY from a contaminated shell, which is the concrete
             incident this ratchet exists for (three stale vars persisted in
             every already-open shell after being removed from the registry, and
             rode into dozens of captures unrecorded; one of them changed a
             capture hash and hid 35 never-baked light slots).

A non-empty `ambient_scrubbed` is NOT a claim the capture is wrong -- the pin
scrubbed the contaminant, so the frame is correct. It is a claim the OPERATOR'S
SHELL is dirty and will keep producing suspect evidence in any harness that has
not been pinned yet. Fix the shell.

Exit 0 = pass, 1 = violation.

  --self-test   inject a synthetic contaminated manifest and assert this ratchet
                rejects it (proves the ratchet actually fires).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

#: Harnesses that launch mc2.exe to produce visual capture evidence. These must
#: pin. Add a harness here when it starts producing captures.
PINNED_HARNESSES = [
    "scripts/run_visual_capture.py",
]

#: Where capture manifests land.
MANIFEST_GLOBS = [
    "tests/visual/captures/**/*_capture_manifest.json",
    "tests/visual/baselines/**/set.json",
]

_COPY_RE = re.compile(r"^\s*env\s*=\s*os\.environ\.copy\(\)", re.M)


def check_code() -> list[str]:
    """Layer 1: pinned harnesses must not inherit the ambient environment."""
    problems: list[str] = []
    for rel in PINNED_HARNESSES:
        path = ROOT / rel
        if not path.exists():
            problems.append(f"{rel}: listed as a pinned harness but missing")
            continue
        src = path.read_text(encoding="utf-8", errors="replace")
        if _COPY_RE.search(src):
            problems.append(
                f"{rel}: builds a launch env with `os.environ.copy()`. A capture "
                f"harness must not inherit the ambient MC2_* environment -- use "
                f"mc2_env_pin.build_pinned_env() so the run defaults to the "
                f"shipped configuration and records what it measured.")
        if "mc2_env_pin" not in src:
            problems.append(
                f"{rel}: does not import mc2_env_pin. Every capture harness must "
                f"pin and record its MC2_* environment.")
    return problems


def _iter_manifests(scan_root: Path):
    for pattern in MANIFEST_GLOBS:
        yield from scan_root.glob(pattern)


def check_artifacts(scan_root: Path, strict_missing: bool) -> list[str]:
    """Layer 2: capture manifests must record a declared, clean MC2_* env."""
    problems: list[str] = []
    for man in _iter_manifests(scan_root):
        try:
            doc = json.loads(man.read_text(encoding="utf-8"))
        except Exception as e:
            problems.append(f"{man}: unreadable manifest ({e})")
            continue

        rec = doc.get("mc2_env") or (doc.get("extra") or {}).get("mc2_env")
        if rec is None and isinstance(doc.get("identity"), dict):
            rec = doc["identity"].get("mc2_env")
        if rec is None:
            # Artifacts predating environment pinning have no record at all. Those are
            # exactly the captures whose environment is unknowable after the
            # fact -- report them under --strict-missing, but do not fail a
            # normal contract run on historical artifacts nobody can retrofit.
            if strict_missing:
                problems.append(
                    f"{man}: no mc2_env record -- environment unknowable, "
                    f"predates CAPTURE-ENV-PIN-1 (recapture to grade it)")
            continue

        scrubbed = rec.get("ambient_scrubbed") or {}
        if scrubbed:
            names = ", ".join(f"{k}={v}" for k, v in sorted(scrubbed.items()))
            problems.append(
                f"{man}: captured from a CONTAMINATED SHELL -- undeclared "
                f"ambient MC2_* present at capture time: {names}. The pin "
                f"scrubbed them so this frame is still shipped-config, but the "
                f"shell will corrupt any un-pinned harness. Open a clean shell, "
                f"or pass --env-declare if the deviation is deliberate.")
        if rec.get("policy") != "pinned-shipped":
            problems.append(
                f"{man}: mc2_env.policy={rec.get('policy')!r}, expected "
                f"'pinned-shipped'")
    return problems


def _self_test() -> int:
    """Prove the ratchet fires: inject a contaminated manifest and require a
    violation. A ratchet nobody has seen fail is not evidence of anything."""
    print("[self-test] injecting a contaminated capture manifest ...")
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        d = root / "tests" / "visual" / "captures" / "inject"
        d.mkdir(parents=True)
        man = d / "mc2_10_capture_manifest.json"
        man.write_text(json.dumps({
            "kind": "visual-capture",
            "extra": {"mc2_env": {
                "policy": "pinned-shipped",
                "effective": {"MC2_SMOKE_MODE": "1"},
                "ambient_scrubbed": {
                    "MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE": "1"},
                "clean_shell": False,
            }},
        }, indent=2), encoding="utf-8")

        problems = check_artifacts(root, strict_missing=False)
        if not problems:
            print("[self-test] FAIL: ratchet did NOT fire on an injected "
                  "contaminant. The ratchet is inert.", file=sys.stderr)
            return 1
        print(f"[self-test] ratchet fired as required ({len(problems)}):")
        for p in problems:
            print(f"[self-test]   {p}")

        # Negative control: a clean manifest must NOT trip it, or the ratchet is
        # just a constant failure and equally worthless.
        man.write_text(json.dumps({
            "kind": "visual-capture",
            "extra": {"mc2_env": {
                "policy": "pinned-shipped",
                "effective": {"MC2_SMOKE_MODE": "1"},
                "ambient_scrubbed": {},
                "clean_shell": True,
            }},
        }, indent=2), encoding="utf-8")
        residual = check_artifacts(root, strict_missing=False)
        if residual:
            print(f"[self-test] FAIL: ratchet fires on a CLEAN manifest: "
                  f"{residual}", file=sys.stderr)
            return 1
        print("[self-test] clean manifest passes (no false positive)")
    print("[self-test] PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan-root", default=str(ROOT))
    ap.add_argument("--strict-missing", action="store_true",
                    help="also fail on capture manifests with NO mc2_env "
                         "record (pre-CAPTURE-ENV-PIN-1 artifacts)")
    ap.add_argument("--self-test", action="store_true",
                    help="prove the ratchet fires on an injected contaminant")
    args = ap.parse_args()

    if args.self_test:
        return _self_test()

    problems = check_code()
    problems += check_artifacts(Path(args.scan_root), args.strict_missing)

    if problems:
        print("[check-capture-env-declared] FAIL", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    print("[check-capture-env-declared] OK: capture harnesses pin their MC2_* "
          "environment and no manifest records an undeclared contaminant")
    return 0


if __name__ == "__main__":
    sys.exit(main())
