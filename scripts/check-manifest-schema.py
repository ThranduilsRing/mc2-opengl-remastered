#!/usr/bin/env python3
"""S12 -- manifest schema conformance / drift checker (ADVISORY).

Scans a directory tree for artifact-manifest files and reports which ones
carry a conformant unified `identity` block (schema "mc2-manifest/1") and
which still use a legacy-only shape. This is the drift gate for the
unified-manifest rollout: as the five species adopt `manifest_schema.attach`,
this checker tracks coverage.

ADVISORY by default (exit 0 even with non-conformant legacy files) so it can
run in CI as a coverage report without blocking the pre-adoption species.
Pass --strict to exit nonzero when any KNOWN-adopted file is non-conformant
(use once a species has been migrated, to prevent regressions).

Recognized manifest filenames (the five species + new tools):
    manifest.json            run manifest (cockpit) / release-zip report
    *.install-receipt.json   install receipt (mc2mod)
    package.json             mod package (mc2mod)        [legacy-allowed]
    cook.json                cook sidecar (asset_cook)   [legacy-allowed]
    *_capture_manifest.json  visual-capture sidecar (S9 runner)

Usage:
    py -3 scripts/check-manifest-schema.py [ROOT ...]
    py -3 scripts/check-manifest-schema.py --strict tests/visual
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import manifest_schema  # noqa: E402

# Filenames that SHOULD carry a unified identity block once adopted. Species
# marked legacy_ok=True are not yet migrated and never fail --strict.
KNOWN = {
    "manifest.json": dict(legacy_ok=False, species="run/release manifest"),
    "package.json": dict(legacy_ok=True, species="mod package"),
    "cook.json": dict(legacy_ok=True, species="cook sidecar"),
    "set.json": dict(legacy_ok=False, species="golden set record"),
    "golden-sets.json": dict(legacy_ok=True, species="golden-sets registry"),
    "release_install_report.json": dict(legacy_ok=False, species="release install report"),
}
SUFFIX_KNOWN = {
    ".install-receipt.json": dict(legacy_ok=True, species="install receipt"),
    "_capture_manifest.json": dict(legacy_ok=False, species="visual-capture sidecar"),
}


# Keys that positively identify a document as belonging to SOME OTHER species
# that merely happens to be spelled manifest.json. UNWIRED-CHECKS-CLOSE-1:
# classify() keyed on the BASENAME alone, so every manifest.json anywhere in the
# tree was graded as a run/release manifest and told to grow an identity block.
# The two --strict failures that made this check unwirable were both this bug,
# not stale fixtures:
#   tests/fixtures/assets/orm_runtime/manifest.json -- an ASSET manifest
#       (assetId/materials); its schema is what check-asset-manifests.py owns.
#   tests/fixtures/brain_runtime/manifest.json -- declares its own
#       "$schema": "brain-fixture-manifest-v1" and is consumed by
#       check_brain_fixture_manifest.py and the brain dispatch harness.
# Neither should ever carry a run-manifest identity block. Demanding one would
# have corrupted two fixtures to turn a light green -- so the check stayed
# red, and therefore stayed unwired, and therefore never guarded the 218
# manifests it is actually right about.
FOREIGN_KEYS = ("$schema", "assetId")


def classify(path: Path) -> dict | None:
    name = path.name
    meta = None
    if name in KNOWN:
        meta = KNOWN[name]
    else:
        for suf, m in SUFFIX_KNOWN.items():
            if name.endswith(suf):
                meta = m
                break
    if meta is None:
        return None
    # A document that names its own foreign schema is not ours to grade.
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return meta          # unreadable is a real finding -- let it through
    if isinstance(doc, dict) and any(k in doc for k in FOREIGN_KEYS):
        return None
    return meta


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="*", default=None,
                    help="dirs to scan (default: tests/, run/, releases under repo)")
    ap.add_argument("--strict", action="store_true",
                    help="exit nonzero if any non-legacy species is non-conformant")
    args = ap.parse_args()

    roots = [Path(r) for r in (args.roots or [ROOT / "tests", ROOT / "run"])]

    scanned = conformant = legacy = broken = 0
    failures: list[str] = []
    for root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*.json")):
            meta = classify(path)
            if meta is None:
                continue
            scanned += 1
            try:
                doc = json.loads(path.read_text(encoding="utf-8"))
            except Exception as e:
                broken += 1
                failures.append(f"UNREADABLE {path}: {e}")
                continue
            ident = doc.get("identity") if isinstance(doc, dict) else None
            if ident is None:
                legacy += 1
                tag = "legacy-ok" if meta["legacy_ok"] else "LEGACY (no identity)"
                print(f"[manifest-check] {tag:22} {meta['species']:24} {path}")
                if not meta["legacy_ok"] and args.strict:
                    failures.append(f"NON-CONFORMANT (no identity block): {path}")
                continue
            problems = manifest_schema.validate_identity(ident)
            if problems:
                broken += 1
                print(f"[manifest-check] {'BROKEN':22} {meta['species']:24} {path}")
                for p in problems:
                    print(f"                   - {p}")
                failures.append(f"BROKEN identity in {path}: {problems}")
            else:
                conformant += 1
                print(f"[manifest-check] {'OK (mc2-manifest/1)':22} "
                      f"{meta['species']:24} {path}")

    print(f"\n[manifest-check] scanned={scanned} conformant={conformant} "
          f"legacy={legacy} broken={broken}")
    # Blindness floor (UNWIRED-CHECKS-CLOSE-1). Every manifest this walk grades
    # is found by rglob under tests/ and run/. Rename or move either and the
    # walk finds nothing and prints a confident OK over an empty corpus -- a
    # full-credit pass from a check that opened zero files. The repo tracks 13
    # qualifying fixtures under tests/, so a floor of 1 cannot be met by
    # accident and cannot be tripped by the absence of machine-local run/.
    if args.strict and scanned == 0:
        print("[manifest-check] FAIL: scanned 0 manifests -- the corpus is "
              "missing or the scan roots moved. An OK over an empty corpus is "
              "not a pass.", file=sys.stderr)
        return 1
    if args.strict and failures:
        print(f"[manifest-check] STRICT FAIL ({len(failures)} problem(s)):",
              file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
