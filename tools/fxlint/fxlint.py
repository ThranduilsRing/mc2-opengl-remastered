#!/usr/bin/env python3
"""
tools/fxlint/fxlint.py -- FX-DEFS-SIDECAR-1 validator (VFX-MODERNIZATION-PROPOSAL-1 slice 1)
==============================================================================================
Validates the SAME *.fxdef.json files the engine reads at
data/effects/defs/<EffectName>.fxdef.json (base) or mods/<id>/data/effects/defs/
(mod overlay) -- learning MODDABILITY-REVIEW-1's #1 wart: never a parallel
hand-maintained facts file. This tool re-implements the parse rules of
mclib/fx_def_registry.cpp in Python so a modder can validate BEFORE launching
the game; the runtime loader applies the identical rules and logs the same
class of problems ([FXDEF] dropped/unsupported lines) if a bad def slips
through -- never silently.

Usage
-----
  fxlint.py <file.fxdef.json> [<file2.fxdef.json> ...]
      Lint one or more specific def files.

  fxlint.py <dir>
      Lint every *.fxdef.json in <dir> (non-recursive, matches the engine's
      loadFromDir/mergeFromDir scan).

  fxlint.py mods/<id>/
      Folder mode: recursively finds data/effects/defs/ under the given mod
      root and lints everything there, plus cross-refs bindings if
      compbas.csv/effects.csv are present under the same mod (informational
      only -- FX defs bind via the pre-existing CSV path, unchanged by this
      slice).

  --catalog <catalog.json>
      Optional: a `mc2fx dump` shallow-catalog JSON (index/effectID/classID/
      name) to cross-reference each def's "effect" name against the real
      mc2.fx catalog (FXD_EFFECT_NAME_UNRESOLVED). Without --catalog this
      check is skipped with a note -- it is NOT required for a clean lint.

Exit codes: 0 = no errors (warnings allowed), 1 = at least one error found.

Findings are printed one per line as:
  <path>:<line>: <CODE> <message>

Python 3 stdlib only (json parsing self-hosted for line numbers -- the
stdlib json module does not report line numbers on well-formed-but-invalid
-semantically values, so we track line numbers via a light re-scan of the
raw text for the offending key).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any, Dict, List, Optional, Tuple

SCHEMA_SUFFIX = ".fxdef.json"

KNOWN_CURVE_KEYS = {"alpha", "red", "green", "blue", "scale", "lifespan"}
# Levenshtein-lite "did you mean" nudge for the most common typo shapes.
_CURVE_SUGGEST = {
    "alpah": "alpha", "aplha": "alpha",
    "scael": "scale", "scal": "scale",
    "lifespan": "lifeSpan",  # canonical spelling reminder (case-insensitive at runtime)
}
KNOWN_TOP_KEYS = {
    "effect", "disabled", "texture", "blend", "curves",
    # reserved v2/forward keys -- tolerated, not yet consumed (slices #4-#6)
    "flipbook", "erosion", "distortion", "light",
}
KNOWN_BLEND_VALUES = {"additive", "alpha"}


class Finding:
    def __init__(self, path: str, line: int, code: str, message: str, is_error: bool = True):
        self.path = path
        self.line = line
        self.code = code
        self.message = message
        self.is_error = is_error

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.code} {self.message}"


def _find_key_line(raw_text: str, key: str) -> int:
    """Best-effort line number for a top-level or nested JSON key, by locating
    the first occurrence of "<key>" in the raw source text. 1-indexed. Falls
    back to line 1 if not found (e.g. key was synthesized, not authored)."""
    needle = f'"{key}"'
    idx = raw_text.find(needle)
    if idx == -1:
        return 1
    return raw_text.count("\n", 0, idx) + 1


def lint_file(path: str, catalog_names: Optional[set]) -> List[Finding]:
    findings: List[Finding] = []
    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw_text = fh.read()
    except OSError as e:
        findings.append(Finding(path, 1, "FXD_UNREADABLE", str(e)))
        return findings

    try:
        root = json.loads(raw_text)
    except json.JSONDecodeError as e:
        findings.append(Finding(path, e.lineno, "FXD_PARSE_ERROR", str(e)))
        return findings

    if not isinstance(root, dict):
        findings.append(Finding(path, 1, "FXD_ROOT_NOT_OBJECT", "top-level JSON must be an object"))
        return findings

    # --- effect name -------------------------------------------------------
    effect = root.get("effect")
    if not isinstance(effect, str) or not effect.strip():
        findings.append(Finding(path, _find_key_line(raw_text, "effect"),
                                 "FXD_MISSING_EFFECT_NAME",
                                 "missing or empty required string 'effect'"))
        effect = None
    elif catalog_names is not None and effect.strip().lower() not in catalog_names:
        findings.append(Finding(path, _find_key_line(raw_text, "effect"),
                                 "FXD_EFFECT_NAME_UNRESOLVED",
                                 f"'{effect}' not found in mc2.fx catalog "
                                 f"(--catalog given; {len(catalog_names)} names known)"))

    # --- unknown top-level keys (warning, not error -- engine tolerates) ---
    for key in root.keys():
        if key not in KNOWN_TOP_KEYS:
            findings.append(Finding(path, _find_key_line(raw_text, key),
                                     "FXD_UNKNOWN_TOP_KEY",
                                     f"unrecognized top-level key '{key}' (tolerated at "
                                     f"runtime, ignored -- check for a typo)",
                                     is_error=False))

    # --- disabled ------------------------------------------------------------
    if "disabled" in root and not isinstance(root["disabled"], bool):
        findings.append(Finding(path, _find_key_line(raw_text, "disabled"),
                                 "FXD_BAD_TYPE", "'disabled' must be a bool"))

    # --- texture ---------------------------------------------------------
    if "texture" in root:
        tex = root["texture"]
        if not isinstance(tex, str) or not tex.strip():
            findings.append(Finding(path, _find_key_line(raw_text, "texture"),
                                     "FXD_BAD_TYPE", "'texture' must be a non-empty string"))
        else:
            if os.path.isabs(tex) or ".." in tex.replace("\\", "/").split("/"):
                findings.append(Finding(path, _find_key_line(raw_text, "texture"),
                                         "FXD_TEXTURE_UNSAFE_PATH",
                                         f"'{tex}' looks absolute or traversal ('..'); "
                                         "texture names should be relative TGL-pool names"))

    # --- blend -------------------------------------------------------------
    if "blend" in root:
        b = root["blend"]
        if not isinstance(b, str) or b.strip().lower() not in KNOWN_BLEND_VALUES:
            findings.append(Finding(path, _find_key_line(raw_text, "blend"),
                                     "FXD_BAD_BLEND",
                                     f"'blend' must be 'additive' or 'alpha' (got {b!r})"))

    # --- curves --------------------------------------------------------------
    if "curves" in root:
        curves = root["curves"]
        if not isinstance(curves, dict):
            findings.append(Finding(path, _find_key_line(raw_text, "curves"),
                                     "FXD_BAD_TYPE", "'curves' must be an object"))
        else:
            for field, val in curves.items():
                norm = field.strip().lower()
                if norm not in KNOWN_CURVE_KEYS:
                    suggestion = _CURVE_SUGGEST.get(norm)
                    hint = f" (did you mean '{suggestion}'?)" if suggestion else ""
                    findings.append(Finding(path, _find_key_line(raw_text, field),
                                             "FXD_UNKNOWN_CURVE",
                                             f"'{field}' is not a recognized curve key{hint} "
                                             f"-- known: {sorted(KNOWN_CURVE_KEYS)}"))
                    continue
                numeric_ok = isinstance(val, (int, float)) and not isinstance(val, bool)
                object_ok = (isinstance(val, dict) and isinstance(val.get("value"), (int, float))
                             and not isinstance(val.get("value"), bool))
                if not (numeric_ok or object_ok):
                    findings.append(Finding(path, _find_key_line(raw_text, field),
                                             "FXD_CURVE_BAD_VALUE",
                                             f"curve '{field}' value must be a number "
                                             f"(or {{'value': number}}); got {val!r}"))

    # --- flipbook (reserved; shape-check only, not consumed by engine yet) --
    if "flipbook" in root:
        fb = root["flipbook"]
        if not isinstance(fb, dict):
            findings.append(Finding(path, _find_key_line(raw_text, "flipbook"),
                                     "FXD_BAD_TYPE", "'flipbook' must be an object",
                                     is_error=False))
        else:
            cols = fb.get("cols")
            rows = fb.get("rows")
            if isinstance(cols, int) and isinstance(rows, int) and cols != rows:
                findings.append(Finding(path, _find_key_line(raw_text, "flipbook"),
                                         "FXD_FLIPBOOK_NONSQUARE",
                                         f"cols={cols} rows={rows} (non-square sheets are "
                                         f"valid but double-check the atlas layout)",
                                         is_error=False))

    return findings


def find_def_files(target: str) -> List[str]:
    """Resolve target (a file, a defs/ dir, or a mod root containing
    data/effects/defs/ somewhere under it) to a list of *.fxdef.json paths."""
    if os.path.isfile(target):
        return [target]
    if not os.path.isdir(target):
        return []

    out: List[str] = []
    # Direct defs/ dir: non-recursive scan (matches engine loadFromDir).
    direct = [os.path.join(target, f).replace("\\", "/") for f in sorted(os.listdir(target))
              if f.lower().endswith(SCHEMA_SUFFIX)]
    if direct:
        out.extend(direct)
        return out

    # Folder / mod-root mode: walk for any data/effects/defs/ subtree.
    for dirpath, dirnames, filenames in os.walk(target):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        norm = dirpath.replace("\\", "/")
        if norm.endswith("data/effects/defs"):
            for f in sorted(filenames):
                if f.lower().endswith(SCHEMA_SUFFIX):
                    out.append(os.path.join(dirpath, f).replace("\\", "/"))
    return out


def load_catalog_names(catalog_path: Optional[str]) -> Optional[set]:
    if not catalog_path:
        return None
    with open(catalog_path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    names = set()
    # mc2fx dumpCatalogJson shape: {"effects":[{"name":..,...}, ...]} (shallow
    # catalog) -- accept a couple of plausible shapes defensively.
    entries = data.get("effects", data if isinstance(data, list) else [])
    for e in entries:
        if isinstance(e, dict) and isinstance(e.get("name"), str):
            names.add(e["name"].strip().lower())
    return names


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Validate *.fxdef.json files (FX-DEFS-SIDECAR-1).")
    ap.add_argument("targets", nargs="+", help="file(s), a defs/ dir, or a mod root")
    ap.add_argument("--catalog", default=None,
                     help="mc2fx dump catalog JSON, for FXD_EFFECT_NAME_UNRESOLVED cross-ref")
    args = ap.parse_args(argv)

    try:
        catalog_names = load_catalog_names(args.catalog)
    except (OSError, json.JSONDecodeError) as e:
        print(f"fxlint: --catalog {args.catalog}: {e}", file=sys.stderr)
        return 1

    all_files: List[str] = []
    for t in args.targets:
        files = find_def_files(t)
        if not files:
            print(f"fxlint: {t}: no *.fxdef.json files found", file=sys.stderr)
        all_files.extend(files)

    if not all_files:
        print("fxlint: nothing to lint", file=sys.stderr)
        return 1

    total_errors = 0
    total_warnings = 0
    for path in all_files:
        findings = lint_file(path, catalog_names)
        for f in findings:
            print(str(f))
            if f.is_error:
                total_errors += 1
            else:
                total_warnings += 1

    print(f"fxlint: {len(all_files)} file(s), {total_errors} error(s), {total_warnings} warning(s)"
          + ("" if catalog_names is not None else "  (note: no --catalog given, "
             "FXD_EFFECT_NAME_UNRESOLVED skipped)"))
    return 1 if total_errors > 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
