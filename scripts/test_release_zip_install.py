#!/usr/bin/env python3
"""test_release_zip_install.py — assemble release zips into a temp install and smoke-test it.

Proves that the public release artifacts are self-contained: a user who downloads
the zips and extracts them gets a working install.

Steps:
  1. Extract engine + gamedata zips into a temp directory (required).
  2. Overlay burnins-4x-pt1/pt2, art, tgl, movies (optional; skipped if absent).
  3. Verify the assembled tree has all required files and directories.
  4. Run a 1-mission smoke against the assembled install (--exe <tmp>/mc2.exe).
     No access to the dev deploy dir — this is the oracle for the actual artifact.
  5. Emit release_install_manifest.csv (relpath, sha256, bytes, zip_source).

Usage:
  py -3 scripts/test_release_zip_install.py
  py -3 scripts/test_release_zip_install.py --zip-dir release_assets
  py -3 scripts/test_release_zip_install.py --no-smoke          # tree verify only
  py -3 scripts/test_release_zip_install.py --keep-install      # don't delete temp dir
  py -3 scripts/test_release_zip_install.py --mission mc2_01 --duration 30

Exit codes:
  0  all checks pass (smoke pass or --no-smoke)
  1  tree verification failure or missing required zip
  2  smoke failure
"""
from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
RUN_SMOKE = REPO_ROOT / "scripts" / "run_smoke.py"
DEFAULT_ZIP_DIR = REPO_ROOT / "release_assets"
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import manifest_schema  # noqa: E402  (S12 unified identity block)
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)
from smoke_lib import nofocus, host_lease  # noqa: E402

# ---------------------------------------------------------------------------
# Assembly spec
# Triples: (zip_filename, required, label)
# Engine and gamedata are required; everything else is an optional overlay.
# Order matters — later zips overwrite earlier ones (overlays win).
# ---------------------------------------------------------------------------

ZIP_ORDER: list[tuple[str, bool, str]] = [
    ("mc2-remastered-engine.zip", True,  "engine"),
    ("mc2-gamedata.zip",          True,  "gamedata"),
    ("mc2-burnins-4x-pt1.zip",    False, "burnins-pt1"),
    ("mc2-burnins-4x-pt2.zip",    False, "burnins-pt2"),
    ("mc2-art.zip",               False, "art"),
    ("mc2-tgl.zip",               False, "tgl"),
    ("mc2-movies.zip",            False, "movies"),
]

# ---------------------------------------------------------------------------
# Required-tree spec (must be present after engine + gamedata extract)
# Derived from build_release.sh sanity checks and mechcmd2.cpp:971 sniffer path.
# ---------------------------------------------------------------------------

REQUIRED_FILES = [
    "mc2.exe",
    "license.txt",
    "EULA.txt",
    "LICENSES.md",
    "minprefs.cfg",   # sniffer self-heal CopyFile source
    "orgprefs.cfg",
    "system.cfg",
]
REQUIRED_DIRS = [
    "shaders",  # shaders/ must be non-empty; engine loads them by relative path
    "data",
]
# At least one .fst archive must exist at install root (fastfile data).
REQUIRED_FST = True

MANIFEST_NAME = "release_install_manifest.csv"
REPORT_NAME = "release_install_report.json"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def log(msg: str) -> None:
    print(f"[release-test] {msg}", flush=True)


def fail(msg: str, code: int = 1) -> None:
    print(f"[release-test] FAIL: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_zip(zip_path: Path, dest: Path, label: str) -> list[str]:
    """Extract zip into dest, returning list of relative paths extracted."""
    extracted: list[str] = []
    with zipfile.ZipFile(zip_path, "r") as zf:
        names = {name.replace("\\", "/") for name in zf.namelist()}
        for required in ("license.txt", "EULA.txt", "LICENSES.md"):
            if required not in names:
                raise ValueError(f"{zip_path.name}: missing {required}")
        for info in zf.infolist():
            if info.is_dir():
                continue
            # Normalise separator (7-zip on Windows may embed backslashes).
            rel = info.filename.replace("\\", "/")
            if Path(rel).is_absolute() or ":" in rel or ".." in Path(rel).parts:
                raise ValueError(f"unsafe archive path: {rel}")
            out = dest / Path(rel)
            out.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
            extracted.append(rel)
    log(f"  [{label}] {len(extracted)} files extracted")
    return extracted


def verify_tree(install: Path) -> list[str]:
    """Return list of failure strings (empty = pass)."""
    failures: list[str] = []

    if (install / "mc2-launcher.exe").exists():
        failures.append("retired mc2-launcher.exe must not ship; launch mc2.exe")
    for path in install.iterdir():
        name = path.name.lower()
        if path.is_file() and (".bak" in name or name.startswith(("harness_", "h2prefs"))):
            failures.append(f"developer artifact must not ship: {path.name}")
        if path.suffix.lower() == ".cfg" and name not in {"minprefs.cfg", "orgprefs.cfg", "system.cfg"}:
            failures.append(f"non-factory configuration must not ship: {path.name}")

    dlls = (REPO_ROOT / "scripts/release-dlls.txt").read_text().splitlines()
    for dll in dlls:
        if not (install / dll).is_file():
            failures.append(f"required DLL missing: {dll}")
    allowed = {dll.lower() for dll in dlls}
    for dll in install.glob("*.dll"):
        if dll.name.lower() not in allowed:
            failures.append(f"unreviewed DLL: {dll.name}")

    for fname in REQUIRED_FILES:
        if not (install / fname).is_file():
            failures.append(f"required file missing: {fname}")

    for dname in REQUIRED_DIRS:
        d = install / dname
        if not d.is_dir():
            failures.append(f"required directory missing: {dname}/")
        elif not any(d.iterdir()):
            failures.append(f"required directory is empty: {dname}/")

    if REQUIRED_FST:
        fsts = list(install.glob("*.fst"))
        if not fsts:
            failures.append("no *.fst files found at install root (fastfile data missing)")

    # Shader sanity: at least one .vert and one .frag must exist.
    shaders = install / "shaders"
    if not any(p.stat().st_size > 0 for p in (shaders / "vulkan").glob("*.spv")):
        failures.append("shaders/vulkan/ contains no non-empty baked .spv files")
    if shaders.is_dir():
        if not any(shaders.glob("**/*.vert")):
            failures.append("shaders/ contains no .vert files")
        if not any(shaders.glob("**/*.frag")):
            failures.append("shaders/ contains no .frag files")

    # data/ sanity: missions subdir must exist (needed to load any mission).
    if not (install / "data" / "missions").is_dir():
        failures.append("data/missions/ missing — no missions to smoke")

    return failures


def emit_manifest(install: Path, zip_sources: dict[str, str], out_path: Path) -> int:
    """Walk install tree, write relpath/sha256/bytes/zip_source CSV. Returns file count."""
    rows: list[tuple[str, str, int, str]] = []
    for p in sorted(install.rglob("*")):
        if p.is_dir():
            continue
        rel = p.relative_to(install).as_posix()
        size = p.stat().st_size
        digest = sha256_file(p)
        source = zip_sources.get(rel, "unknown")
        rows.append((rel, digest, size, source))
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["relpath", "sha256", "bytes", "zip_source"])
        w.writerows(rows)
    return len(rows)


def emit_report(out_path: Path, *, verdict: str, install: Path,
                zip_labels: list[str], mission: str, file_count: int,
                tree_failures: list[str], smoke_rc: int | None,
                manifest_path: Path | None) -> None:
    """Write release_install_report.json carrying the S12 identity block.

    The report is the formal release gate's machine-readable verdict: a release
    is publishable iff verdict==PASS. It joins with smoke run manifests and the
    visual golden sets on the same identity fields (exe.sha256 / git.commit).
    """
    exe = install / "mc2.exe"
    ident = manifest_schema.identity_block(
        generator="release_zip_install",
        exe_path=str(exe) if exe.is_file() else None,
        repo_root=str(REPO_ROOT),
        deploy_target=str(install),
        zip_set=zip_labels,
    )
    rep = manifest_schema.report_summary(
        verdict=verdict,
        missions=[mission],
        artifact_dir=str(out_path.parent),
        extra={
            "tree_ok": not tree_failures,
            "tree_failures": tree_failures,
            "file_count": file_count,
            "smoke_rc": smoke_rc,
            "manifest_csv": (manifest_path.name if manifest_path else None),
            "generated_utc": datetime.datetime.now(
                datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        },
    )
    doc = manifest_schema.attach({"kind": "release-install"}, ident, rep)
    out_path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    log(f"report: {out_path} (verdict={verdict})")


def first_launch(exe: Path, output: Path) -> int:
    """Exercise factory prefs without smoke mode bypassing normal first startup."""
    if (exe.parent / "options.cfg").exists():
        log("FAIL: release contains personal options.cfg instead of factory first-run defaults")
        return 1
    env = os.environ.copy()
    env.pop("MC2_SMOKE_MODE", None)
    env["MC2_WINDOW_OFFSCREEN"] = "1"
    try:
        with (output / "first_launch.log").open("w") as log_file:
            result = nofocus.run([str(exe)], cwd=str(exe.parent), env=env,
                                 stdout=log_file, stderr=subprocess.STDOUT, timeout=120)
    except subprocess.TimeoutExpired:
        log("FAIL: factory first launch timed out")
        return 1
    finally:
        host_lease.release()
    options = exe.parent / "options.cfg"
    text = (output / "first_launch.log").read_text(errors="replace")
    if "[BUILD_FINGERPRINT v1]" not in text or "[RENDER_BACKEND] FATAL" in text:
        log("FAIL: first-launch build fingerprint missing or Vulkan fatal")
        return 1
    if result.returncode or not options.is_file():
        return result.returncode or 1
    if options.read_bytes() != (exe.parent / "minprefs.cfg").read_bytes():
        log("FAIL: first launch did not preserve factory preferences")
        return 1
    return 0


def run_smoke(exe: Path, mission: str, duration: int) -> int:
    """Run run_smoke.py against the assembled install. Returns process exit code."""
    cmd = [
        sys.executable,
        str(RUN_SMOKE),
        "--mission", mission,
        "--duration", str(duration),
        "--keep-logs",
        "--exe", str(exe),
        "--ref-exe", str(exe),
    ]
    log(f"smoke: {' '.join(cmd)}")
    result = subprocess.run(cmd, **console_child.child_kwargs())
    return result.returncode


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Assemble release zips into a temp install and smoke-test it."
    )
    ap.add_argument(
        "--zip-dir",
        type=Path,
        default=DEFAULT_ZIP_DIR,
        help=f"Directory containing release zips (default: {DEFAULT_ZIP_DIR})",
    )
    ap.add_argument(
        "--no-smoke",
        action="store_true",
        help="Skip the smoke run; verify tree only.",
    )
    ap.add_argument(
        "--keep-install",
        action="store_true",
        help="Do not delete the temp install directory after the run.",
    )
    ap.add_argument(
        "--mission",
        default="mc2_01",
        help="Mission to smoke (default: mc2_01)",
    )
    ap.add_argument(
        "--duration",
        type=int,
        default=15,
        help="Smoke duration in seconds (default: 15)",
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Directory to write release_install_manifest.csv (default: --zip-dir)",
    )
    args = ap.parse_args()

    zip_dir: Path = args.zip_dir.resolve()
    out_dir: Path = (args.out_dir or zip_dir).resolve()

    if not zip_dir.is_dir():
        fail(f"--zip-dir not found: {zip_dir}")

    # ------------------------------------------------------------------
    # 1. Check required zips exist before doing any work.
    # ------------------------------------------------------------------
    log(f"zip-dir: {zip_dir}")
    missing_required: list[str] = []
    present_zips: list[tuple[str, bool, str]] = []
    for fname, required, label in ZIP_ORDER:
        p = zip_dir / fname
        if p.is_file():
            present_zips.append((fname, required, label))
            log(f"  found [{label}]: {fname} ({p.stat().st_size // 1024 // 1024} MB)")
        elif required:
            missing_required.append(fname)
            log(f"  MISSING (required): {fname}")
        else:
            log(f"  absent (optional): {fname}")

    if missing_required:
        fail(f"required zips missing: {', '.join(missing_required)}")

    # ------------------------------------------------------------------
    # 2. Extract into a temp directory.
    # ------------------------------------------------------------------
    tmp_root = Path(tempfile.mkdtemp(prefix="mc2_release_test_"))
    log(f"temp install: {tmp_root}")
    if not args.keep_install:
        log("  (will be deleted after run; use --keep-install to retain)")

    try:
        # Track which zip each file came from (last writer wins for overlays).
        zip_sources: dict[str, str] = {}
        for fname, _required, label in present_zips:
            p = zip_dir / fname
            log(f"extracting {fname} ...")
            paths = extract_zip(p, tmp_root, label)
            for rel in paths:
                zip_sources[rel] = label

        # ------------------------------------------------------------------
        # 3. Verify required tree.
        # ------------------------------------------------------------------
        out_dir.mkdir(parents=True, exist_ok=True)
        report_path = out_dir / REPORT_NAME
        manifest_path = out_dir / MANIFEST_NAME
        zip_labels = [label for _f, _r, label in present_zips]

        log("verifying assembled tree ...")
        failures = verify_tree(tmp_root)
        file_count = sum(1 for _ in tmp_root.rglob('*') if _.is_file())
        if failures:
            for f in failures:
                print(f"[release-test]   FAIL: {f}", file=sys.stderr)
            emit_report(report_path, verdict="FAIL", install=tmp_root,
                        zip_labels=zip_labels, mission=args.mission,
                        file_count=file_count, tree_failures=failures,
                        smoke_rc=None, manifest_path=None)
            fail(f"{len(failures)} tree verification failure(s) — see above", code=1)
        log(f"  tree OK — {file_count} files assembled")

        # ------------------------------------------------------------------
        # 4. Emit manifest.
        # ------------------------------------------------------------------
        count = emit_manifest(tmp_root, zip_sources, manifest_path)
        log(f"manifest: {manifest_path} ({count} entries)")

        # ------------------------------------------------------------------
        # 5. Smoke run.
        # ------------------------------------------------------------------
        if args.no_smoke:
            log("--no-smoke: skipping smoke run")
            emit_report(report_path, verdict="PASS", install=tmp_root,
                        zip_labels=zip_labels, mission=args.mission,
                        file_count=file_count, tree_failures=[],
                        smoke_rc=None, manifest_path=manifest_path)
            log("PASS (tree verify only)")
            return

        exe = tmp_root / "mc2.exe"
        if not exe.is_file():
            fail("mc2.exe not found in assembled install (should have been caught by verify)")

        log("running normal first launch (MC2_SMOKE_MODE unset) ...")
        rc = first_launch(exe, out_dir)
        if rc == 0:
            log(f"running smoke: mission={args.mission} duration={args.duration}s ...")
            rc = run_smoke(exe, args.mission, args.duration)
        if any(p.is_file() for p in (tmp_root / "crashes").rglob("*")):
            log("FAIL: crash artifacts present")
            rc = 1
        emit_report(report_path, verdict="PASS" if rc == 0 else "FAIL",
                    install=tmp_root, zip_labels=zip_labels,
                    mission=args.mission, file_count=file_count,
                    tree_failures=[], smoke_rc=rc, manifest_path=manifest_path)
        if rc != 0:
            fail(f"smoke exited {rc}", code=2)

        log("PASS")

    finally:
        if not args.keep_install:
            shutil.rmtree(tmp_root, ignore_errors=True)
        else:
            log(f"install retained at: {tmp_root}")


if __name__ == "__main__":
    main()
