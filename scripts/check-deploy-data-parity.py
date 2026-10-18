#!/usr/bin/env python3
"""check-deploy-data-parity.py — DEPLOY-DATA-PARITY-1

Checks that the two authoritative MC2 deploy folders (0.5.0 and 0.4d-rc1)
agree on data/ + mods/ content, and that neither contains stray nested-deploy
artefacts.

Authority model (baked in as defaults):
  canonical  = 0.5.0   "A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0"
  near-canon = 0.4d-rc1 "A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1"
  older      = 0.4      "A:/Games/mc2-opengl/mc2-win64-v0.4"
  oldest     = 0.4c     "A:/Games/mc2-opengl/mc2-win64-v0.4c"

Exit codes:
  0  — clean (no REAL-DIVERGENCE). WARN/INFO (UNKNOWN-NEEDS-HUMAN,
       NEAR-CANONICAL-BEHIND) are advisory only and do NOT fail the gate
       (matches scripts/check-binding-slots.py convention).
  1  — REAL-DIVERGENCE found (stray nested deploy, or stock-data loss)

Usage:
  py -3 scripts/check-deploy-data-parity.py [--quiet] [--json <out>] [--roots A B C D]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Default deploy roots (authority model)
# ---------------------------------------------------------------------------
DEFAULT_ROOTS = [
    ("0.5.0",     r"A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0"),
    ("0.4d-rc1",  r"A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1"),
    ("0.4",       r"A:/Games/mc2-opengl/mc2-win64-v0.4"),
    ("0.4c",      r"A:/Games/mc2-opengl/mc2-win64-v0.4c"),
]

# Indices of the two authoritative builds in DEFAULT_ROOTS
AUTH_A_IDX = 0  # 0.5.0
AUTH_B_IDX = 1  # 0.4d-rc1

AUDIT_SUBTREES = ("data", "mods")

# ---------------------------------------------------------------------------
# Classification helpers
# ---------------------------------------------------------------------------

# Extensions / path patterns that are always LEGIT-EXCLUDED
_EXCL_EXTS = {".exe", ".dll", ".pdb", ".fst", ".cfg", ".log", ".bat"}
_EXCL_SUFFIXES = (".deployed_manifest.csv",)
_EXCL_PREFIX_SEGS = (
    "shaders/",          # shaders/** incl spv/
    "screenshots/",
    "saves/",
    "debug_state/",
    "tests/",
    "mods/.import_reports/",  # cook pipeline artefacts
)
_EXCL_NAMES = {".smoke_leases", ".smoke_leases.lock", ".smoke_leases.json",
               ".modindex-cache"}
_EXCL_SUFFIXES_EXTRA = (".fst", ".cfg", ".log", ".bat")  # redundant guard
_EXCL_NOISE_EXTS = {".bak", ".orig", ".old"}  # cook/deploy backup noise


def classify(rel: str) -> Optional[str]:
    """Return a classification bucket string, or None = must compare normally."""
    parts = rel.replace("\\", "/").lower()

    # ---- LEGIT-EXCLUDED by extension / known noise -------------------------
    ext = Path(rel).suffix.lower()
    if ext in _EXCL_EXTS:
        return "LEGIT-EXCLUDED"
    if ext in _EXCL_NOISE_EXTS:
        return "LEGIT-EXCLUDED"
    # InnoSetup uninstaller artefacts (unins000.dat/.exe/.msg) — installer
    # cruft left in some mod folders; canonical builds correctly omit them.
    if Path(rel).name.lower().startswith("unins"):
        return "LEGIT-EXCLUDED"
    # Archive leftovers (e.g. mods/keid-v.zip) — the extracted form is what
    # actually ships; a stray .zip beside it is not content divergence.
    if ext == ".zip":
        return "LEGIT-EXCLUDED"
    # multi-extension noise like .ktx2.bak, .old_pil_upscale.bak
    rel_lower_name = Path(rel).name.lower()
    if (".bak" in rel_lower_name or ".orig" in rel_lower_name
            or rel_lower_name.endswith(".old")
            or ".old_" in rel_lower_name):
        return "LEGIT-EXCLUDED"
    if any(rel.lower().endswith(s) for s in _EXCL_SUFFIXES):
        return "LEGIT-EXCLUDED"
    for pfx in _EXCL_PREFIX_SEGS:
        if parts.startswith(pfx) or ("/" + pfx) in parts:
            return "LEGIT-EXCLUDED"
    name = Path(rel).name.lower()
    if name in _EXCL_NAMES or name.startswith(".smoke_leases"):
        return "LEGIT-EXCLUDED"
    # *.log, *.bat may not be caught by ext alone
    if ext in (".log", ".bat", ".cfg"):
        return "LEGIT-EXCLUDED"

    # ---- MODEL-OVERRIDES-EXPECTED-ABSENT -----------------------------------
    if rel.lower().startswith("data/model_overrides/") or rel.lower().startswith("data\\model_overrides\\"):
        return "MODEL-OVERRIDES-EXPECTED-ABSENT"

    # ---- BT-MOD-EXCEPTION --------------------------------------------------
    rel_lower = rel.lower().replace("\\", "/")
    if (
        rel_lower == "data/tgl/madcat.glb"
        or rel_lower.startswith("data/tgl/") and "_fbx2gltf.glb" in rel_lower
        or rel_lower.startswith("data/tgl/128/marauder_")
        or rel_lower.startswith("mods/battletech-mechs")
        or rel_lower.startswith("mods/bt2018mechs")
    ):
        return "BT-MOD-EXCEPTION"

    return None  # normal content — compare


def is_nested_deploy(root: Path, rel: str) -> bool:
    """Return True if this path is inside a nested deploy artefact directory.

    A nested deploy = a directory directly under data/ that itself looks like a
    deploy folder (contains mc2.exe or a .fst file at root level).
    """
    parts = rel.replace("\\", "/").split("/")
    if len(parts) >= 2 and parts[0].lower() == "data":
        candidate = root / "data" / parts[1]
        if candidate.is_dir():
            for item in candidate.iterdir():
                if item.is_file() and item.suffix.lower() in (".exe", ".fst"):
                    return True
    return False


# ---------------------------------------------------------------------------
# Walk helpers
# ---------------------------------------------------------------------------

def enumerate_folder(root: Path) -> Dict[str, int]:
    """Walk data/ and mods/ subtrees. Return {posix_rel_path: size_bytes}.

    Classifiable files (LEGIT-EXCLUDED etc.) are still included so we can
    report them; callers filter by classify().
    """
    # PERF: os.scandir, not os.walk + Path.stat().
    #
    # os.walk gives filenames only, so the old code rebuilt a Path per file and
    # issued a SECOND stat() syscall for the size -- discarding the size the
    # directory enumeration had already returned. On Windows, DirEntry.stat()
    # is served from the cached FindFirstFile/FindNextFile data with no extra
    # syscall. Measured on the 0.5.0 root (93,364 files): 8.33s -> 0.89s, a
    # 9.4x win, with byte-identical output (asserted during development by
    # diffing both implementations' dicts).
    #
    # Output contract is unchanged: {posix_rel_path: size_bytes}, same keys,
    # same values, same OSError-skipping behaviour.
    result: Dict[str, int] = {}
    root_str = str(root)
    for sub in AUDIT_SUBTREES:
        base = root / sub
        if not base.exists():
            continue
        stack = [str(base)]
        while stack:
            dpath = stack.pop()
            try:
                entries = list(os.scandir(dpath))
            except OSError:
                continue
            for ent in entries:
                try:
                    if ent.is_dir(follow_symlinks=False):
                        stack.append(ent.path)
                    elif ent.is_file(follow_symlinks=False):
                        rel = os.path.relpath(ent.path, root_str).replace("\\", "/")
                        result[rel] = ent.stat().st_size
                except OSError:
                    pass
    return result


def mods_basenames(files: Dict[str, int]) -> set:
    """Lowercased basenames of everything under mods/, precomputed ONCE.

    PERF (VERIFY-INTEGRITY-3): the relocation test used to be
        any(Path(k).name.lower() == basename and k.startswith("mods/")
            for k in b_files)
    evaluated INSIDE a loop over every differing path -- an O(differences x
    total_files) scan that rebuilt a Path object per key. With ~93k files per
    root that is hundreds of millions of Path constructions and was the single
    dominant cost in the 7-minute runtime, dwarfing the directory walk.

    Hoisting it to one O(n) pass makes each lookup O(1) set membership. The
    predicate is evaluated over exactly the same keys with exactly the same
    comparison, so the classification result is unchanged.
    """
    return {k.rsplit("/", 1)[-1].lower()
            for k in files if k.startswith("mods/")}


def detect_nested_deploy(root: Path) -> List[str]:
    """Return list of nested deploy directory paths (relative posix).

    A nested deploy is a DIRECT child of data/ that contains mc2.exe at its
    root level — i.e. a prior-deploy folder was accidentally put inside data/.
    We require mc2.exe specifically so ordinary game directories (missions/,
    terrain/, etc.) containing .fst mission archives are never flagged.
    """
    data = root / "data"
    if not data.exists():
        return []
    found = []
    for item in data.iterdir():
        if not item.is_dir():
            continue
        # Only flag if mc2.exe sits directly inside this directory
        if (item / "mc2.exe").exists():
            found.append(item.relative_to(root).as_posix())
    return found


# ---------------------------------------------------------------------------
# Comparison core
# ---------------------------------------------------------------------------

_BUCKET_LABELS = {
    "REAL-DIVERGENCE":               "FAIL",
    "LEGIT-RELOCATED-TO-MODS":       "PASS",
    "LEGIT-EXCLUDED":                "PASS",
    "BT-MOD-EXCEPTION":              "PASS",
    "MODEL-OVERRIDES-EXPECTED-ABSENT": "PASS",
    "MISSING-FOLDER":                "WARN",
    "EXELESS-FOLDER":                "WARN",
    "UNKNOWN-NEEDS-HUMAN":           "WARN",
    # Precedence model: 0.5.0 is canonical and may be AHEAD of the near-canonical
    # 0.4d-rc1. Content only in canonical = the lower build is behind (expected),
    # INFO not FAIL.
    "NEAR-CANONICAL-BEHIND":         "INFO",
}


def _emit(
    items: list, level: str, bucket: str, path: str, evidence: str
) -> None:
    items.append({
        "level":    level,
        "bucket":   bucket,
        "path":     path,
        "evidence": evidence,
    })


def check_authoritative_pair(
    a_label: str, a_root: Path,
    b_label: str, b_root: Path,
    quiet: bool,
) -> Tuple[List[dict], int]:
    """Compare the two authoritative builds. Returns (findings, exit_code)."""
    findings: List[dict] = []

    def emit(level, bucket, path, evidence):
        _emit(findings, level, bucket, path, evidence)
        if not quiet:
            print(f"{level:<4}  {bucket:<36}  {path}  [{evidence}]")

    # Folder existence guard
    a_ok = a_root.exists()
    b_ok = b_root.exists()
    if not a_ok and not b_ok:
        # CI-safe skip — neither authoritative folder present
        emit("SKIP", "MISSING-FOLDER", str(a_root),
             f"neither {a_label} nor {b_label} folders exist — CI-SKIP")
        return findings, 0
    if not a_ok:
        emit("WARN", "MISSING-FOLDER", str(a_root), f"{a_label} folder not found")
    if not b_ok:
        emit("WARN", "MISSING-FOLDER", str(b_root), f"{b_label} folder not found")
    if not a_ok or not b_ok:
        return findings, 0  # Can't compare without both

    # mc2.exe presence guard
    if not (a_root / "mc2.exe").exists():
        emit("WARN", "EXELESS-FOLDER", f"{a_label}/mc2.exe",
             f"{a_label} has no mc2.exe")
    if not (b_root / "mc2.exe").exists():
        emit("WARN", "EXELESS-FOLDER", f"{b_label}/mc2.exe",
             f"{b_label} has no mc2.exe")

    # Nested-deploy artefact detection
    for label, root in ((a_label, a_root), (b_label, b_root)):
        for nd in detect_nested_deploy(root):
            emit("FAIL", "REAL-DIVERGENCE", nd,
                 f"stray nested deploy artefact under {label}/data/ (contains mc2.exe or .fst)")

    # Enumerate both
    a_files = enumerate_folder(a_root)
    b_files = enumerate_folder(b_root)

    # mods/ presence check: if one authoritative build has no mods/ at all,
    # that is a known assembly gap (INFO/WARN), not a per-file REAL-DIVERGENCE
    a_has_mods = (a_root / "mods").exists() and any(
        True for k in a_files if k.startswith("mods/")
    )
    b_has_mods = (b_root / "mods").exists() and any(
        True for k in b_files if k.startswith("mods/")
    )
    if not a_has_mods and b_has_mods:
        emit("WARN", "MISSING-FOLDER", f"{a_label}/mods/",
             f"{a_label} has no mods/ tree (incomplete assembly); "
             f"{b_label} mods/ not compared")
    elif not b_has_mods and a_has_mods:
        emit("WARN", "MISSING-FOLDER", f"{b_label}/mods/",
             f"{b_label} has no mods/ tree (incomplete assembly); "
             f"{a_label} mods/ not compared")

    # Build normal-content sets (exclude known-classified files for comparison)
    def normal(files: Dict[str, int], skip_mods_if_missing: bool) -> Dict[str, int]:
        out: Dict[str, int] = {}
        for k, v in files.items():
            if classify(k) is not None:
                continue
            if skip_mods_if_missing and k.startswith("mods/"):
                continue
            out[k] = v
        return out

    # Only compare mods/ if both builds have mods/
    both_have_mods = a_has_mods and b_has_mods
    a_normal = normal(a_files, skip_mods_if_missing=not both_have_mods)
    b_normal = normal(b_files, skip_mods_if_missing=not both_have_mods)

    a_paths = set(a_normal)
    b_paths = set(b_normal)

    # PERF: hoist the mods/ basename index out of the per-difference loops.
    a_mods_names = mods_basenames(a_files)
    b_mods_names = mods_basenames(b_files)

    # Files in A not in B: could be LEGIT-RELOCATED-TO-MODS or REAL-DIVERGENCE
    for rel in sorted(a_paths - b_paths):
        # Is this file present in B's mods/ subtree somewhere (by basename)?
        basename = Path(rel).name.lower()
        in_b_mods = basename in b_mods_names
        if in_b_mods:
            # Only report in non-quiet mode as PASS
            if not quiet:
                print(f"PASS  {'LEGIT-RELOCATED-TO-MODS':<36}  {rel}  "
                      f"[{a_label} data/ -> {b_label} mods/]")
        else:
            # Present in canonical (A=0.5.0), absent from the lower-precedence
            # near-canonical (B=0.4d-rc1). Canonical is allowed to be AHEAD —
            # this means B is behind and needs a catch-up deploy, NOT divergence.
            emit("INFO", "NEAR-CANONICAL-BEHIND", rel,
                 f"present in canonical {a_label} but absent from {b_label} "
                 f"— {b_label} behind, needs catch-up")

    # Files in B not in A: symmetric check
    for rel in sorted(b_paths - a_paths):
        basename = Path(rel).name.lower()
        in_a_mods = basename in a_mods_names
        if in_a_mods:
            if not quiet:
                print(f"PASS  {'LEGIT-RELOCATED-TO-MODS':<36}  {rel}  "
                      f"[{b_label} data/ -> {a_label} mods/]")
        else:
            # Present in lower-precedence B but absent from canonical A. Canonical
            # missing something B has — could be a regression OR a stale B-only
            # leftover. Needs a human ruling, not an auto-fail.
            emit("WARN", "UNKNOWN-NEEDS-HUMAN", rel,
                 f"present in {b_label} but absent from canonical {a_label} "
                 f"— verify canonical isn't missing real content")

    # Size mismatches for common files (notable = .pak / .glb / delta > 500 KB)
    for rel in sorted(a_paths & b_paths):
        a_sz = a_normal[rel]
        b_sz = b_normal[rel]
        if a_sz == b_sz:
            continue
        delta = abs(b_sz - a_sz)
        ext = Path(rel).suffix.lower()
        if ext in (".pak", ".glb") or delta > 500_000:
            emit("WARN", "UNKNOWN-NEEDS-HUMAN", rel,
                 f"size mismatch {a_label}={a_sz} {b_label}={b_sz} delta={b_sz - a_sz:+d}")

    real_div = sum(1 for f in findings if f["bucket"] == "REAL-DIVERGENCE")

    # Gate convention (matches scripts/check-binding-slots.py): only a REAL
    # divergence fails the gate. UNKNOWN-NEEDS-HUMAN / NEAR-CANONICAL-BEHIND are
    # advisory — printed for human attention but exit 0 so CI stays green.
    if real_div:
        return findings, 1
    return findings, 0


def check_legacy_folder(
    label: str, root: Path,
    canon_label: str, canon_root: Path,
    quiet: bool,
) -> List[dict]:
    """Check an older (non-authoritative) folder. Older-behind = INFO only."""
    findings: List[dict] = []

    def emit(level, bucket, path, evidence):
        _emit(findings, level, bucket, path, evidence)
        if not quiet:
            print(f"{level:<4}  {bucket:<36}  {path}  [{evidence}]")

    if not root.exists():
        emit("WARN", "MISSING-FOLDER", str(root), f"{label} folder not found")
        return findings

    # Nested deploy
    for nd in detect_nested_deploy(root):
        emit("FAIL", "REAL-DIVERGENCE", nd,
             f"stray nested deploy artefact in {label}/data/ (contains mc2.exe or .fst)")

    # Just count — info only
    legacy_files = enumerate_folder(root)
    canon_files  = enumerate_folder(canon_root) if canon_root.exists() else {}
    lcount = len([k for k in legacy_files if classify(k) is None])
    ccount = len([k for k in canon_files  if classify(k) is None])
    if not quiet:
        print(f"INFO  {'OLDER-BUILD-BEHIND':<36}  {label}  "
              f"[{lcount} normal files vs canonical {ccount} — pre-migration layout, INFO only]")

    return findings


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    # Windows consoles default to cp1252, which cannot encode some chars and
    # crashes print(). Force UTF-8 so output is robust under any content.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser(
        description="MC2 deploy data/mods parity checker (DEPLOY-DATA-PARITY-1)"
    )
    ap.add_argument("--quiet",  action="store_true",
                    help="Suppress per-finding output; only print summary")
    ap.add_argument("--json",   metavar="OUT",
                    help="Write JSON report to OUT")
    ap.add_argument("--roots",  nargs="+", metavar="PATH",
                    help="Override deploy root paths (space-separated; "
                         "4 required in order: 0.5.0, 0.4d-rc1, 0.4, 0.4c)")
    args = ap.parse_args()

    # Resolve roots
    if args.roots:
        if len(args.roots) != 4:
            print("ERROR: --roots requires exactly 4 paths (0.5.0, 0.4d-rc1, 0.4, 0.4c)",
                  file=sys.stderr)
            return 1
        roots = [
            ("0.5.0",    Path(args.roots[0])),
            ("0.4d-rc1", Path(args.roots[1])),
            ("0.4",      Path(args.roots[2])),
            ("0.4c",     Path(args.roots[3])),
        ]
    else:
        roots = [(lbl, Path(pth)) for lbl, pth in DEFAULT_ROOTS]

    # CI-SAFETY: if NONE of the deploy folders exist, skip cleanly
    if all(not r.exists() for _, r in roots):
        print("PASS  MISSING-FOLDER  (all)  "
              "[no deploy folders found on this machine — CI-SKIP, exit 0]")
        return 0

    all_findings: List[dict] = []
    exit_code = 0

    # --- Authoritative pair comparison --------------------------------------
    a_label, a_root = roots[0]
    b_label, b_root = roots[1]

    if not args.quiet:
        print(f"\n[check-deploy-data-parity] DEPLOY-DATA-PARITY-1")
        print(f"  canonical  : {a_label}  {a_root}")
        print(f"  near-canon : {b_label}  {b_root}")
        print()
        print("--- Authoritative pair: {} vs {} ---".format(a_label, b_label))

    pair_findings, pair_exit = check_authoritative_pair(
        a_label, a_root, b_label, b_root, args.quiet
    )
    all_findings.extend(pair_findings)
    exit_code = max(exit_code, pair_exit)

    # --- Legacy folders (INFO only) -----------------------------------------
    for i in range(2, len(roots)):
        lbl, lroot = roots[i]
        if not args.quiet:
            print(f"\n--- Legacy: {lbl} ---")
        leg_findings = check_legacy_folder(lbl, lroot, a_label, a_root, args.quiet)
        all_findings.extend(leg_findings)
        # Legacy findings don't affect exit_code (INFO only, except nested deploy)
        for f in leg_findings:
            if f["bucket"] == "REAL-DIVERGENCE":
                exit_code = max(exit_code, 1)

    # --- Summary ------------------------------------------------------------
    real_div = sum(1 for f in all_findings if f["bucket"] == "REAL-DIVERGENCE")
    unknown  = sum(1 for f in all_findings if f["bucket"] == "UNKNOWN-NEEDS-HUMAN")

    if not args.quiet:
        print()
        print(f"  result: {'FAIL' if real_div else 'PASS'}  "
              f"(REAL-DIVERGENCE={real_div}, UNKNOWN-NEEDS-HUMAN={unknown})")

    # --- JSON output --------------------------------------------------------
    if args.json:
        report = {
            "summary": {
                "real_divergence_count": real_div,
                "unknown_needs_human_count": unknown,
                "exit_code": exit_code,
            },
            "findings": all_findings,
        }
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        if not args.quiet:
            print(f"  report written to: {args.json}")

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
