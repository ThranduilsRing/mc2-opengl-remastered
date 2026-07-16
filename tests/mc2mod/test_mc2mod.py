"""
tests/mc2mod/test_mc2mod.py -- Unit tests for tools/mc2mod/mc2mod.py (S8 v1)

Covers: pack, verify-lite, install, uninstall, canonical guard.
Uses a tiny synthetic mod fixture created in a temp directory.
Python 3 stdlib only.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

# Make the tool importable from the test runner regardless of cwd
_TOOLS_DIR = str(Path(__file__).resolve().parents[2] / "tools" / "mc2mod")
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

import mc2mod  # noqa: E402


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _make_fixture_mod(base: str) -> str:
    """
    Create a minimal synthetic mod under base/.
    Returns path to the mod directory.
    """
    mod_dir = os.path.join(base, "fixture-mod")
    os.makedirs(os.path.join(mod_dir, "data", "tgl"), exist_ok=True)
    os.makedirs(os.path.join(mod_dir, "data", "missions"), exist_ok=True)
    os.makedirs(os.path.join(mod_dir, "src"), exist_ok=True)        # must be excluded
    os.makedirs(os.path.join(mod_dir, "out"), exist_ok=True)        # must be excluded
    os.makedirs(os.path.join(mod_dir, ".scratch"), exist_ok=True)   # dot-dir, must be excluded

    # mod.json
    with open(os.path.join(mod_dir, "mod.json"), "w") as fh:
        json.dump({
            "id": "fixture-mod",
            "name": "Fixture Mod",
            "version": "1.0.0",
            "dependencies": [],
        }, fh)

    # A data file that should be packed
    with open(os.path.join(mod_dir, "data", "tgl", "fake.ini"), "w") as fh:
        fh.write("[appearance]\nname=fake\n")

    # A mission stub
    with open(os.path.join(mod_dir, "data", "missions", "stub.abl"), "w") as fh:
        fh.write("state start; code endstate; endfsm.\n")

    # Files that must NOT appear in the package
    with open(os.path.join(mod_dir, "src", "source.blend"), "w") as fh:
        fh.write("blender source")
    with open(os.path.join(mod_dir, "out", "cook.log"), "w") as fh:
        fh.write("cook output")
    with open(os.path.join(mod_dir, ".scratch", "temp.txt"), "w") as fh:
        fh.write("scratch")
    # Dot-prefixed file at root of mod
    with open(os.path.join(mod_dir, ".modindex-cache"), "w") as fh:
        fh.write("cache")

    return mod_dir


# ---------------------------------------------------------------------------
# Test: pack
# ---------------------------------------------------------------------------

class TestPack(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.mod_dir = _make_fixture_mod(self.tmpdir)
        self.out_dir = os.path.join(self.tmpdir, "out_packages")
        os.makedirs(self.out_dir)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _run_pack(self) -> str:
        rc = mc2mod.main(["pack", self.mod_dir, "--out", self.out_dir])
        self.assertEqual(rc, 0, "pack should exit 0")
        pkgs = [f for f in os.listdir(self.out_dir) if f.endswith(".mc2mod")]
        self.assertEqual(len(pkgs), 1)
        return os.path.join(self.out_dir, pkgs[0])

    def test_pack_creates_archive(self):
        pkg = self._run_pack()
        self.assertTrue(os.path.isfile(pkg))

    def test_pack_filename(self):
        pkg = self._run_pack()
        self.assertIn("fixture-mod", os.path.basename(pkg))
        self.assertIn("1.0.0", os.path.basename(pkg))

    def test_pack_contains_package_json(self):
        pkg = self._run_pack()
        with zipfile.ZipFile(pkg, "r") as zf:
            self.assertIn("package.json", zf.namelist())

    def test_pack_schema(self):
        pkg = self._run_pack()
        with zipfile.ZipFile(pkg, "r") as zf:
            data = json.loads(zf.read("package.json"))
        self.assertEqual(data["schema"], mc2mod.PACKAGE_SCHEMA)
        self.assertEqual(data["id"], "fixture-mod")
        self.assertEqual(data["version"], "1.0.0")

    def test_pack_files_list_correct(self):
        pkg = self._run_pack()
        with zipfile.ZipFile(pkg, "r") as zf:
            data = json.loads(zf.read("package.json"))
        paths = {e["path"] for e in data["files"]}
        # Data files must be present
        self.assertIn("mod.json", paths)
        self.assertIn("data/tgl/fake.ini", paths)
        self.assertIn("data/missions/stub.abl", paths)
        # Excluded paths must be absent
        for p in paths:
            self.assertFalse(p.startswith("src/"), f"src/ should be excluded: {p}")
            self.assertFalse(p.startswith("out/"), f"out/ should be excluded: {p}")
            self.assertFalse(p.startswith("."), f"dot-prefixed should be excluded: {p}")
            self.assertNotIn("/.scratch/", f"/{p}/")

    def test_pack_excludes_dot_files(self):
        pkg = self._run_pack()
        with zipfile.ZipFile(pkg, "r") as zf:
            names = zf.namelist()
        for name in names:
            for part in name.split("/"):
                self.assertFalse(
                    part.startswith("."),
                    f"dot-prefixed entry found in archive: {name}"
                )

    def test_pack_sha256_entries(self):
        pkg = self._run_pack()
        with zipfile.ZipFile(pkg, "r") as zf:
            data = json.loads(zf.read("package.json"))
            for entry in data["files"]:
                raw = zf.read(entry["path"])
                actual = hashlib.sha256(raw).hexdigest()
                self.assertEqual(actual, entry["sha256"], f"sha256 mismatch for {entry['path']}")
                self.assertEqual(len(raw), entry["size"], f"size mismatch for {entry['path']}")


# ---------------------------------------------------------------------------
# Test: pack reproducibility (pinned zip timestamps)
# ---------------------------------------------------------------------------

class TestPackReproducible(unittest.TestCase):
    """
    pack must be reproducible: byte-identical payload input => byte-identical
    .mc2mod output (same sha256), regardless of wall-clock time or source-file
    mtimes.  Guards the pinned-ZipInfo behavior in mc2mod._write_zip_member.
    """

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.mod_dir = _make_fixture_mod(self.tmpdir)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _pack_into(self, subdir: str) -> str:
        out_dir = os.path.join(self.tmpdir, subdir)
        os.makedirs(out_dir)
        rc = mc2mod.main(["pack", self.mod_dir, "--out", out_dir])
        self.assertEqual(rc, 0)
        pkgs = [f for f in os.listdir(out_dir) if f.endswith(".mc2mod")]
        self.assertEqual(len(pkgs), 1)
        return os.path.join(out_dir, pkgs[0])

    def _touch_all_sources(self, epoch: float) -> None:
        """Set every payload file's atime/mtime to a fixed epoch."""
        for dirpath, _dirs, files in os.walk(self.mod_dir):
            for fname in files:
                os.utime(os.path.join(dirpath, fname), (epoch, epoch))

    def test_pack_twice_identical_sha256(self):
        # Perturb source mtimes between the two runs: a non-reproducible pack
        # (zipfile default: member timestamps from mtime / wall clock) would
        # produce different archive bytes here.
        self._touch_all_sources(1_000_000_000.0)   # 2001-09-09
        pkg_a = self._pack_into("out_a")
        self._touch_all_sources(1_500_000_000.0)   # 2017-07-14
        pkg_b = self._pack_into("out_b")

        self.assertEqual(
            _sha256_file(pkg_a), _sha256_file(pkg_b),
            "pack is not reproducible: same payload, different package sha256"
        )

    def test_pack_zip_timestamps_pinned(self):
        pkg = self._pack_into("out_pinned")
        with zipfile.ZipFile(pkg, "r") as zf:
            for zinfo in zf.infolist():
                self.assertEqual(
                    zinfo.date_time, mc2mod._REPRO_ZIP_DATE,
                    f"member {zinfo.filename} has unpinned timestamp "
                    f"{zinfo.date_time}"
                )

    def test_pack_reproducible_roundtrip_still_verifies(self):
        # The reproducible writer must not break archive integrity.
        pkg = self._pack_into("out_verify")
        rc = mc2mod.main(["verify-lite", pkg])
        self.assertEqual(rc, 0)


# ---------------------------------------------------------------------------
# Test: verify-lite
# ---------------------------------------------------------------------------

class TestVerifyLite(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.mod_dir = _make_fixture_mod(self.tmpdir)
        self.out_dir = os.path.join(self.tmpdir, "pkgs")
        os.makedirs(self.out_dir)
        mc2mod.main(["pack", self.mod_dir, "--out", self.out_dir])
        pkgs = [f for f in os.listdir(self.out_dir) if f.endswith(".mc2mod")]
        self.pkg = os.path.join(self.out_dir, pkgs[0])

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_verify_pass(self):
        rc = mc2mod.main(["verify-lite", self.pkg])
        self.assertEqual(rc, 0)

    def test_verify_fails_on_corruption(self):
        # Corrupt the zip: flip bytes in a data file entry
        corrupt_pkg = os.path.join(self.tmpdir, "corrupt.mc2mod")
        shutil.copy(self.pkg, corrupt_pkg)

        # Read the zip, modify a file, rewrite
        entries: dict[str, bytes] = {}
        with zipfile.ZipFile(corrupt_pkg, "r") as zf:
            for name in zf.namelist():
                entries[name] = zf.read(name)

        # Flip one byte in a data file (not package.json to keep JSON valid)
        for name in entries:
            if name != "package.json":
                data = bytearray(entries[name])
                data[0] ^= 0xFF
                entries[name] = bytes(data)
                break

        with zipfile.ZipFile(corrupt_pkg, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for name, data in entries.items():
                zf.writestr(name, data)

        rc = mc2mod.main(["verify-lite", corrupt_pkg])
        self.assertNotEqual(rc, 0, "verify-lite must fail on corruption")

    def test_verify_fails_on_missing_schema(self):
        # Build a zip with a package.json that has the wrong schema
        bad_pkg = os.path.join(self.tmpdir, "bad-schema.mc2mod")
        entries: dict[str, bytes] = {}
        with zipfile.ZipFile(self.pkg, "r") as zf:
            for name in zf.namelist():
                entries[name] = zf.read(name)

        pkg_data = json.loads(entries["package.json"])
        pkg_data["schema"] = "wrong-schema/99"
        entries["package.json"] = json.dumps(pkg_data).encode("utf-8")

        with zipfile.ZipFile(bad_pkg, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for name, data in entries.items():
                zf.writestr(name, data)

        rc = mc2mod.main(["verify-lite", bad_pkg])
        self.assertNotEqual(rc, 0)


# ---------------------------------------------------------------------------
# Test: install + uninstall
# ---------------------------------------------------------------------------

class TestInstallUninstall(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.mod_dir = _make_fixture_mod(self.tmpdir)
        self.pkg_dir = os.path.join(self.tmpdir, "pkgs")
        os.makedirs(self.pkg_dir)
        mc2mod.main(["pack", self.mod_dir, "--out", self.pkg_dir])
        pkgs = [f for f in os.listdir(self.pkg_dir) if f.endswith(".mc2mod")]
        self.pkg = os.path.join(self.pkg_dir, pkgs[0])
        self.deploy = os.path.join(self.tmpdir, "deploy")
        os.makedirs(self.deploy)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _install(self) -> int:
        return mc2mod.main(["install", self.pkg, "--deploy", self.deploy])

    def _uninstall(self, force: bool = False) -> int:
        argv = ["uninstall", "fixture-mod", "--deploy", self.deploy]
        if force:
            argv.append("--force")
        return mc2mod.main(argv)

    def test_install_creates_files(self):
        rc = self._install()
        self.assertEqual(rc, 0)
        install_root = os.path.join(self.deploy, "mods", "fixture-mod")
        self.assertTrue(os.path.isdir(install_root))
        self.assertTrue(
            os.path.isfile(os.path.join(install_root, "data", "tgl", "fake.ini"))
        )

    def test_install_creates_receipt(self):
        self._install()
        receipt_path = os.path.join(self.deploy, "mods", "fixture-mod", ".install-receipt.json")
        self.assertTrue(os.path.isfile(receipt_path))
        with open(receipt_path) as fh:
            receipt = json.load(fh)
        self.assertEqual(receipt["schema"], mc2mod.RECEIPT_SCHEMA)
        self.assertEqual(receipt["id"], "fixture-mod")
        self.assertIn("package_sha256", receipt)
        self.assertIn("installed_at", receipt)
        self.assertIn("files", receipt)

    def test_uninstall_removes_all_files(self):
        self._install()
        rc = self._uninstall()
        self.assertEqual(rc, 0)
        install_root = os.path.join(self.deploy, "mods", "fixture-mod")
        self.assertFalse(
            os.path.isdir(install_root),
            "install root should be removed after uninstall"
        )

    def test_uninstall_refuses_if_modified(self):
        self._install()
        # Modify an installed file
        fake_ini = os.path.join(self.deploy, "mods", "fixture-mod", "data", "tgl", "fake.ini")
        with open(fake_ini, "a") as fh:
            fh.write("# tampered\n")
        rc = self._uninstall(force=False)
        self.assertNotEqual(rc, 0, "uninstall must refuse if file was modified")

    def test_uninstall_force_removes_modified(self):
        self._install()
        fake_ini = os.path.join(self.deploy, "mods", "fixture-mod", "data", "tgl", "fake.ini")
        with open(fake_ini, "a") as fh:
            fh.write("# tampered\n")
        rc = self._uninstall(force=True)
        self.assertEqual(rc, 0)

    def test_roundtrip_byte_identical_deploy(self):
        """
        Snapshot deploy state before install, install, uninstall, re-snapshot:
        the two manifests must be identical.
        """
        def _snapshot(root: str) -> dict[str, tuple[int, str]]:
            result = {}
            for dirpath, dirs, files in os.walk(root):
                dirs[:] = sorted(dirs)
                for fname in sorted(files):
                    abs_path = os.path.join(dirpath, fname)
                    rel = os.path.relpath(abs_path, root).replace("\\", "/")
                    size = os.path.getsize(abs_path)
                    sha = _sha256_file(abs_path)
                    result[rel] = (size, sha)
            return result

        before = _snapshot(self.deploy)
        self._install()
        self._uninstall()
        after = _snapshot(self.deploy)

        diff = {k: (before.get(k), after.get(k)) for k in before.keys() | after.keys()
                if before.get(k) != after.get(k)}
        self.assertEqual(diff, {}, f"Deploy tree differs after install+uninstall: {diff}")


# ---------------------------------------------------------------------------
# Test: canonical guard
# ---------------------------------------------------------------------------

class TestCanonicalGuard(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.mod_dir = _make_fixture_mod(self.tmpdir)
        self.pkg_dir = os.path.join(self.tmpdir, "pkgs")
        os.makedirs(self.pkg_dir)
        mc2mod.main(["pack", self.mod_dir, "--out", self.pkg_dir])
        pkgs = [f for f in os.listdir(self.pkg_dir) if f.endswith(".mc2mod")]
        self.pkg = os.path.join(self.pkg_dir, pkgs[0])

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_canonical_guard_v04(self):
        # Force canonical path check by using the string directly (no resolve needed)
        # Patch _is_canonical_deploy to test the logic with the exact strings
        for canonical in mc2mod._CANONICAL_DEPLOY_ROOTS:
            self.assertTrue(
                mc2mod._is_canonical_deploy(canonical),
                f"Should detect canonical: {canonical}"
            )

    def test_install_refused_for_canonical(self):
        # Monkey-patch _is_canonical_deploy to return True for self.tmpdir
        original = mc2mod._is_canonical_deploy

        def _fake_guard(deploy_dir: str) -> bool:
            return True  # pretend any dir is canonical for this test

        mc2mod._is_canonical_deploy = _fake_guard
        try:
            rc = mc2mod.main(["install", self.pkg, "--deploy", self.tmpdir])
            self.assertNotEqual(rc, 0, "install must be refused for canonical deploys")
        finally:
            mc2mod._is_canonical_deploy = original

    def test_non_canonical_allowed(self):
        safe_deploy = os.path.join(self.tmpdir, "safe-deploy")
        os.makedirs(safe_deploy)
        self.assertFalse(mc2mod._is_canonical_deploy(safe_deploy))
        rc = mc2mod.main(["install", self.pkg, "--deploy", safe_deploy])
        self.assertEqual(rc, 0, "install must succeed for non-canonical deploy")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)
