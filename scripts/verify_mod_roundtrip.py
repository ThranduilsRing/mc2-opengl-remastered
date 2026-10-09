"""
scripts/verify_mod_roundtrip.py -- modder end-to-end round-trip verifier (S16)
==============================================================================
Proves the mc2mod substrate (tools/mc2mod/mc2mod.py) is a LOSSLESS round-trip:

    pack -> verify-lite -> install -> [playtest] -> uninstall -> byte-identical restore

The core, deterministic guarantee this asserts is *byte-identical restore*: an
independent recursive hash of the deploy tree taken BEFORE install must equal the
hash taken AFTER uninstall. No residue, no missing files, no mutation. This is
checked here independently of the substrate's own receipt logic so the test is a
real adversary to the tooling, not a tautology.

Why a sandbox deploy
--------------------
`mc2mod install` hard-refuses the canonical game deploy roots (v0.4 / 0.4c) by
design. This verifier therefore installs into a throwaway sandbox deploy dir
(system temp). That is correct and intentional -- we never mutate the live game
to test the tooling.

The playtest step
-----------------
`--playtest` runs the canonical smoke harness (scripts/run_smoke.py) as an
ENGINE-LIVENESS gate against the live game deploy. Honesty note: the engine
cannot run from a bare sandbox (no exe/data), and we must not subvert the
canonical-root install guard, so the playtest proves "engine is healthy" -- it
does NOT prove "this mod loads in-engine" (that needs a junction-mirrored deploy,
out of scope for this slice). Playtest is OFF by default so the lossless core
runs fast and dependency-free.

Exit codes
----------
  0  all stages passed (round-trip is byte-identical; playtest passed if run)
  1  a round-trip stage failed (pack/verify/install/uninstall/byte-diff)
  2  playtest (smoke) failed
  3  bad invocation / environment

Python 3 stdlib only. No emoji.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
MC2MOD = REPO_ROOT / "tools" / "mc2mod" / "mc2mod.py"
RUN_SMOKE = SCRIPT_DIR / "run_smoke.py"
LIVE_DEPLOY = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4")


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def log(msg: str) -> None:
    print(f"[roundtrip] {msg}", file=sys.stderr, flush=True)


def stage(name: str) -> None:
    log("")
    log(f"=== STAGE: {name} ===")


# ---------------------------------------------------------------------------
# Hashing / tree snapshot
# ---------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def snapshot_tree(root: Path) -> Dict[str, str]:
    """
    Return {posix-relative-path: sha256} for every file under root.
    A missing root yields an empty snapshot (the pre-install state when the
    mods dir does not yet exist).
    """
    snap: Dict[str, str] = {}
    if not root.exists():
        return snap
    root_str = str(root)
    for dirpath, _dirs, files in os.walk(root_str):
        for fname in files:
            abs_path = Path(dirpath) / fname
            # relpath on strings avoids pathlib's case-folding surprises on NTFS.
            rel = os.path.relpath(str(abs_path), root_str).replace(os.sep, "/")
            snap[rel] = sha256_file(abs_path)
    return snap


def diff_snapshots(before: Dict[str, str], after: Dict[str, str]) -> Tuple[List[str], List[str], List[str]]:
    """Return (added, removed, changed) relative paths."""
    bkeys, akeys = set(before), set(after)
    added = sorted(akeys - bkeys)
    removed = sorted(bkeys - akeys)
    changed = sorted(p for p in (bkeys & akeys) if before[p] != after[p])
    return added, removed, changed


# ---------------------------------------------------------------------------
# mc2mod subprocess wrapper
# ---------------------------------------------------------------------------

def run_mc2mod(args: List[str]) -> Tuple[int, str]:
    """Run mc2mod.py with args; return (rc, combined-output). Output is echoed."""
    cmd = [sys.executable, str(MC2MOD)] + args
    log("$ mc2mod " + " ".join(args))
    proc = subprocess.run(cmd, capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    out = (proc.stdout or "") + (proc.stderr or "")
    for line in out.splitlines():
        log("  | " + line)
    return proc.returncode, out


# ---------------------------------------------------------------------------
# Synthetic fixture mod
# ---------------------------------------------------------------------------

def make_fixture_mod(dest: Path) -> Path:
    """
    Create a tiny, deterministic synthetic mod project under dest and return its
    directory. Exercises nested dirs, a binary file, and the dot-prefix / src /
    out exclusion rules (those entries must NOT survive the round-trip).
    """
    mod_dir = dest / "roundtrip-fixture"
    (mod_dir / "data" / "tgl").mkdir(parents=True, exist_ok=True)
    (mod_dir / "src").mkdir(parents=True, exist_ok=True)
    (mod_dir / "out").mkdir(parents=True, exist_ok=True)

    (mod_dir / "mod.json").write_text(
        '{\n'
        '  "schema": "mc2-mod/1",\n'
        '  "id": "roundtrip-fixture",\n'
        '  "name": "Round-Trip Fixture",\n'
        '  "version": "1.0.0",\n'
        '  "dependencies": []\n'
        '}\n',
        encoding="utf-8",
    )
    (mod_dir / "readme.txt").write_text("round-trip fixture payload\n", encoding="utf-8")
    (mod_dir / "data" / "tgl" / "stub.ini").write_text("[Appearance]\nName=stub\n", encoding="utf-8")
    # Deterministic 4 KiB binary payload (no Date/random -- byte-stable).
    (mod_dir / "data" / "blob.bin").write_bytes(bytes((i * 7 + 3) & 0xFF for i in range(4096)))

    # Entries that MUST be excluded by pack (ruling C4 + src/out):
    (mod_dir / ".hidden").write_text("should not survive\n", encoding="utf-8")
    (mod_dir / "src" / "source.psd").write_text("cook source\n", encoding="utf-8")
    (mod_dir / "out" / "scratch.tmp").write_text("regen artifact\n", encoding="utf-8")
    return mod_dir


# ---------------------------------------------------------------------------
# Verifier stages
# ---------------------------------------------------------------------------

def assert_installed(install_root: Path, package_files: List[str]) -> List[str]:
    """Independent check that declared files + receipt + package.json landed."""
    errs: List[str] = []
    if not install_root.is_dir():
        errs.append(f"install root missing: {install_root}")
        return errs
    receipt = install_root / ".install-receipt.json"
    if not receipt.is_file():
        errs.append(f"receipt missing: {receipt}")
    # The installer writes package.json into the install root separately from
    # files[]; verify it landed so a dropped-write regression is visible.
    if not (install_root / "package.json").is_file():
        errs.append(f"installer package.json missing: {install_root / 'package.json'}")
    for rel in package_files:
        if not (install_root / rel.replace("/", os.sep)).is_file():
            errs.append(f"declared file not installed: {rel}")
    return errs


def read_package_meta(package: Path) -> Tuple[str, List[str], List[str]]:
    """
    Return (id, declared_file_paths, raw_zip_members) from a .mc2mod.
    raw_zip_members is the actual archive contents (independent of files[]) so
    the caller can adversarially check exclusions against what was really packed.
    """
    import json
    import zipfile
    with zipfile.ZipFile(str(package), "r") as zf:
        members = [n for n in zf.namelist() if not n.endswith("/")]
        pkg = json.loads(zf.read("package.json").decode("utf-8"))
    declared = [e["path"] for e in pkg.get("files", [])]
    return str(pkg.get("id", "")), declared, members


def run_playtest(missions: List[str], duration: int) -> int:
    """Run the canonical smoke as an engine-liveness gate. Return smoke rc."""
    cmd = [
        sys.executable, str(RUN_SMOKE),
        "--duration", str(duration), "--keep-logs",
    ]
    for m in missions:
        cmd += ["--mission", m]
    log("ENGINE-LIVENESS (mod is in isolated sandbox, NOT in the smoke deploy):")
    log("$ " + " ".join(cmd))
    proc = subprocess.run(cmd, creationflags=(getattr(subprocess, "CREATE_NO_WINDOW", 0) if not (sys.stdout and sys.stdout.isatty()) else 0))
    return proc.returncode


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        prog="verify_mod_roundtrip",
        description="Verify the mc2mod pack/install/uninstall round-trip is byte-identical.",
    )
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--mod-dir", metavar="DIR",
                     help="Pack this mod project dir (default: synthetic fixture).")
    src.add_argument("--package", metavar="FILE",
                     help="Use a prebuilt .mc2mod package (skip pack stage).")
    ap.add_argument("--playtest", action="store_true",
                    help="Run a smoke engine-liveness gate against the live deploy.")
    ap.add_argument("--mission", action="append", default=[], metavar="NAME",
                    help="Playtest mission (repeatable; default: mc2_01 + mc2_24).")
    ap.add_argument("--duration", type=int, default=30,
                    help="Playtest per-mission seconds (default: 30).")
    ap.add_argument("--keep-temp", action="store_true",
                    help="Do not delete the sandbox deploy / temp dirs.")
    args = ap.parse_args(argv)

    if not MC2MOD.is_file():
        log(f"FATAL: mc2mod not found at {MC2MOD}")
        return 3

    work = Path(tempfile.mkdtemp(prefix="mc2_roundtrip_"))
    sandbox_deploy = work / "sandbox-deploy"
    sandbox_deploy.mkdir(parents=True, exist_ok=True)
    mods_root = sandbox_deploy / "mods"
    pkg_out = work / "dist"
    pkg_out.mkdir(parents=True, exist_ok=True)
    log(f"sandbox work dir: {work}")

    # Seed a co-resident sentinel so the byte-identical proof is NON-vacuous:
    # uninstall of the target mod must leave a neighbouring mod's files untouched.
    sentinel = mods_root / "co-resident-mod" / "keep.txt"
    sentinel.parent.mkdir(parents=True, exist_ok=True)
    sentinel.write_text("pre-existing neighbour mod -- must survive round-trip\n", encoding="utf-8")

    rc = 0
    try:
        # --- pack -------------------------------------------------------
        if args.package:
            package = Path(args.package).resolve()
            if not package.is_file():
                log(f"FATAL: package not found: {package}")
                return 3
            log(f"using prebuilt package: {package}")
        else:
            stage("pack")
            if args.mod_dir:
                mod_dir = Path(args.mod_dir).resolve()
                if not mod_dir.is_dir():
                    log(f"FATAL: mod-dir not found: {mod_dir}")
                    return 3
            else:
                mod_dir = make_fixture_mod(work / "fixture-src")
                log(f"built synthetic fixture: {mod_dir}")
            prc, _ = run_mc2mod(["pack", str(mod_dir), "--out", str(pkg_out)])
            if prc != 0:
                log("FAIL: pack returned nonzero")
                return 1
            packages = list(pkg_out.glob("*.mc2mod"))
            if len(packages) != 1:
                log(f"FAIL: expected exactly 1 package, found {len(packages)}")
                return 1
            package = packages[0]

        import json as _json
        import zipfile as _zipfile
        try:
            mod_id, package_files, zip_members = read_package_meta(package)
        except (_zipfile.BadZipFile, KeyError, _json.JSONDecodeError, OSError) as exc:
            log(f"FAIL: cannot read package metadata ({exc})")
            return 1
        log(f"package id={mod_id} declared-files={len(package_files)} "
            f"zip-members={len(zip_members)}")

        # Adversary check against the RAW archive members (not the self-reported
        # files[] manifest): excluded entries must never have been packed. Also
        # confirm the manifest and the archive agree (no member missing from
        # files[], no declared file absent from the archive).
        excluded = [m for m in zip_members
                    if m != "package.json"
                    and (m.startswith(".") or "/." in m
                         or m.startswith("src/") or m.startswith("out/"))]
        if excluded:
            log(f"FAIL: package leaked excluded entries into the archive: {excluded}")
            return 1
        member_set = {m for m in zip_members if m != "package.json"}
        declared_set = set(package_files)
        if member_set != declared_set:
            log(f"FAIL: manifest/archive mismatch. "
                f"in-zip-not-declared={sorted(member_set - declared_set)} "
                f"declared-not-in-zip={sorted(declared_set - member_set)}")
            return 1

        # --- verify-lite ------------------------------------------------
        stage("verify-lite")
        vrc, _ = run_mc2mod(["verify-lite", str(package)])
        if vrc != 0:
            log("FAIL: verify-lite returned nonzero")
            return 1

        # --- snapshot BEFORE --------------------------------------------
        stage("snapshot (before install)")
        before = snapshot_tree(mods_root)
        log(f"deploy mods/ tree: {len(before)} files before install")

        # --- install ----------------------------------------------------
        stage("install")
        irc, _ = run_mc2mod(["install", str(package), "--deploy", str(sandbox_deploy)])
        if irc != 0:
            log("FAIL: install returned nonzero")
            return 1
        install_root = mods_root / mod_id
        ierrs = assert_installed(install_root, package_files)
        if ierrs:
            for e in ierrs:
                log(f"FAIL: {e}")
            return 1
        log(f"install verified: {len(package_files)} files + receipt present")

        # --- playtest (optional) ----------------------------------------
        if args.playtest:
            stage("playtest (engine-liveness)")
            if not RUN_SMOKE.is_file():
                log(f"FATAL: run_smoke not found at {RUN_SMOKE}")
                return 3
            missions = args.mission or ["mc2_01", "mc2_24"]
            smoke_rc = run_playtest(missions, args.duration)
            if smoke_rc != 0:
                log(f"FAIL: playtest smoke returned {smoke_rc}")
                rc = 2  # continue to uninstall so we still leave a clean sandbox
            else:
                log("playtest PASS")
        else:
            log("playtest SKIPPED (pass --playtest for engine-liveness gate)")

        # --- uninstall --------------------------------------------------
        stage("uninstall")
        urc, _ = run_mc2mod(["uninstall", mod_id, "--deploy", str(sandbox_deploy)])
        if urc != 0:
            log("FAIL: uninstall returned nonzero")
            return 1

        # --- snapshot AFTER + byte-identical assertion ------------------
        stage("snapshot (after uninstall) + byte-identical check")
        after = snapshot_tree(mods_root)
        added, removed, changed = diff_snapshots(before, after)
        if added or removed or changed:
            log("FAIL: deploy tree NOT byte-identical after round-trip")
            for p in added:
                log(f"  RESIDUE (added): {p}")
            for p in removed:
                log(f"  LOST   (removed): {p}")
            for p in changed:
                log(f"  MUTATED (changed): {p}")
            return 1
        log(f"byte-identical restore CONFIRMED ({len(before)} files unchanged)")

        if rc == 0:
            log("")
            log("RESULT: PASS -- round-trip is lossless and byte-identical.")
        else:
            log("")
            log("RESULT: round-trip lossless, but PLAYTEST FAILED (rc=2).")
        return rc

    finally:
        if args.keep_temp:
            log(f"keeping temp dir: {work}")
        else:
            shutil.rmtree(str(work), ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
