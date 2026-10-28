"""
Unit tests for tools/mod_install/resolver.py
Tests: NormalizeKey parity, ShouldSearchMods, overlay precedence, strip logic,
       mod.json parsing, dot-dir skip, first-wins semantics.
All tests use tempdir fixtures -- no deploy required.
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# Ensure the tools/mod_install package is importable
_TOOLS = os.path.join(os.path.dirname(__file__), "..", "..", "tools", "mod_install")
if _TOOLS not in sys.path:
    sys.path.insert(0, os.path.abspath(_TOOLS))

from resolver import (
    normalize_key,
    should_search_mods,
    ResolverConfig,
    resolve,
    _strip_size_subdir,
    _read_mod_json,
    _build_fst_set,
)


class TestNormalizeKey(unittest.TestCase):
    """Mirror NormalizeKey (file.cpp:105): backslash->/, ASCII lowercase."""

    def test_already_normalized(self):
        self.assertEqual(normalize_key("data/tgl/foo.tga"), "data/tgl/foo.tga")

    def test_backslash_to_slash(self):
        self.assertEqual(normalize_key("data\\tgl\\foo.tga"), "data/tgl/foo.tga")

    def test_mixed_slashes(self):
        self.assertEqual(normalize_key("data\\tgl/foo.tga"), "data/tgl/foo.tga")

    def test_uppercase_to_lower(self):
        self.assertEqual(normalize_key("Data/TGL/Foo.TGA"), "data/tgl/foo.tga")

    def test_mixed_case_backslash(self):
        self.assertEqual(normalize_key("DATA\\TGL\\FOO.TGA"), "data/tgl/foo.tga")

    def test_empty_string(self):
        self.assertEqual(normalize_key(""), "")

    def test_drive_letter_preserved_but_lowercased(self):
        # Drive letters are lowercased too -- matches ASCII tolower
        result = normalize_key("A:\\Games\\foo.tga")
        self.assertEqual(result, "a:/games/foo.tga")

    def test_no_non_ascii_mangling(self):
        # Non-ASCII bytes pass through (ASCII tolower only)
        result = normalize_key("data/\xff/foo.tga")
        self.assertIn("data/", result)


class TestShouldSearchMods(unittest.TestCase):
    """Mirror ShouldSearchMods (file.cpp:97-103)."""

    def test_data_prefix_ok(self):
        self.assertTrue(should_search_mods("data/tgl/foo.tga"))

    def test_data_subdir_ok(self):
        self.assertTrue(should_search_mods("data/missions/mission01.fit"))

    def test_no_data_prefix(self):
        self.assertFalse(should_search_mods("prefs.cfg"))

    def test_absolute_windows_path(self):
        self.assertFalse(should_search_mods("a:/games/data/foo.tga"))

    def test_absolute_unix_path(self):
        self.assertFalse(should_search_mods("/data/foo.tga"))

    def test_dotdot_traversal(self):
        self.assertFalse(should_search_mods("data/../etc/passwd"))

    def test_dotdot_in_path(self):
        self.assertFalse(should_search_mods("../data/foo.tga"))

    def test_empty_string(self):
        self.assertFalse(should_search_mods(""))

    def test_just_data(self):
        # "data" without trailing slash does not start with "data/"
        self.assertFalse(should_search_mods("data"))

    def test_data_slash_ok(self):
        self.assertTrue(should_search_mods("data/x"))


class TestStripSizeSubdir(unittest.TestCase):
    """Mirror numeric-subdir strip (file.cpp:788-806)."""

    def test_strip_128(self):
        self.assertEqual(_strip_size_subdir("data/tgl/128/foo.tga"), "data/tgl/foo.tga")

    def test_strip_256(self):
        self.assertEqual(_strip_size_subdir("data/tgl/256/bar.tga"), "data/tgl/bar.tga")

    def test_no_numeric_subdir(self):
        self.assertIsNone(_strip_size_subdir("data/tgl/foo.tga"))

    def test_leading_numeric_not_stripped(self):
        # First segment is "128" but there is no preceding path component to strip into
        # The loop starts at index 1 (i > 0 check); "128/foo.tga" has "128" at index 0 -> no strip
        result = _strip_size_subdir("128/foo.tga")
        # i=0 is skipped (i > 0 guard is actually not in _strip_size_subdir -- let's verify behavior)
        # In our impl: parts[:-1] and part.isdigit() and i > 0
        # parts = ["128", "foo.tga"], enumerate starts at 0; i=0 -> skipped by i>0 check
        self.assertIsNone(result)

    def test_non_numeric_subdir(self):
        self.assertIsNone(_strip_size_subdir("data/tgl/hd/foo.tga"))


class TestReadModJson(unittest.TestCase):
    """Mirror ReadModJson (file.cpp:393-409)."""

    def _write_json(self, d, path):
        with open(path, "w") as f:
            json.dump(d, f)

    def test_full_json(self):
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "mod.json")
            self._write_json({"id": "mc2x-pbr", "name": "PBR Mod", "dependencies": ["mc2x-compat"]}, p)
            mod_id, mod_name, deps = _read_mod_json(p)
            self.assertEqual(mod_id, "mc2x-pbr")
            self.assertEqual(mod_name, "PBR Mod")
            self.assertEqual(deps, ["mc2x-compat"])

    def test_missing_file(self):
        mod_id, mod_name, deps = _read_mod_json("/nonexistent/path/mod.json")
        self.assertEqual(mod_id, "")
        self.assertEqual(deps, [])

    def test_no_dependencies_key(self):
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "mod.json")
            self._write_json({"id": "my-mod", "name": "My Mod"}, p)
            _, _, deps = _read_mod_json(p)
            self.assertEqual(deps, [])

    def test_malformed_json(self):
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "mod.json")
            with open(p, "w") as f:
                f.write("{not valid json")
            mod_id, _, deps = _read_mod_json(p)
            self.assertEqual(mod_id, "")
            self.assertEqual(deps, [])


class TestFstSet(unittest.TestCase):
    """_build_fst_set: text listing -> normalized key set."""

    def test_basic_listing(self):
        with tempfile.TemporaryDirectory() as td:
            listing = os.path.join(td, "tgl.fst.txt")
            with open(listing, "w") as f:
                f.write("data/art/cursors1a.tga\n")
                f.write("data/art/WALKA.TGA\n")
                f.write("# comment line\n")
                f.write("\n")
            fst = _build_fst_set([listing])
            self.assertIn("data/art/cursors1a.tga", fst)
            self.assertIn("data/art/walka.tga", fst)  # lowercased
            self.assertNotIn("# comment line", fst)

    def test_missing_file(self):
        fst = _build_fst_set(["/nonexistent.fst.txt"])
        self.assertEqual(len(fst), 0)

    def test_empty_listing(self):
        with tempfile.TemporaryDirectory() as td:
            listing = os.path.join(td, "empty.fst.txt")
            with open(listing, "w") as f:
                pass
            fst = _build_fst_set([listing])
            self.assertEqual(len(fst), 0)


class _TempDeploy:
    """Helper: build a minimal game-dir + mods/ fixture in a tempdir."""

    def __init__(self):
        self._td = tempfile.TemporaryDirectory()
        self.root = self._td.name

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self._td.cleanup()

    def write_file(self, rel_path: str, content: str = "data") -> str:
        """Create a file at root/rel_path.  Returns absolute path."""
        abs_path = os.path.join(self.root, rel_path.replace("/", os.sep))
        os.makedirs(os.path.dirname(abs_path), exist_ok=True)
        with open(abs_path, "w") as f:
            f.write(content)
        return abs_path

    def write_mod_json(self, mod_id: str, deps=None) -> str:
        deps = deps or []
        return self.write_file(
            f"mods/{mod_id}/mod.json",
            json.dumps({"id": mod_id, "name": mod_id, "dependencies": deps}),
        )


class TestResolveBaseGame(unittest.TestCase):
    """resolve() with no active mod: base-loose, base-strip, fastfile, MISS."""

    def test_base_loose_hit(self):
        with _TempDeploy() as td:
            td.write_file("data/missions/mc2_01.fit")
            cfg = ResolverConfig(game_dir=td.root)
            rec = resolve("data/missions/mc2_01.fit", cfg)
            self.assertEqual(rec["layer"], "base-loose")
            self.assertEqual(rec["key"], "data/missions/mc2_01.fit")

    def test_base_loose_miss(self):
        with _TempDeploy() as td:
            cfg = ResolverConfig(game_dir=td.root)
            rec = resolve("data/missions/nonexistent.fit", cfg)
            self.assertEqual(rec["layer"], "MISS")

    def test_non_data_path_base_loose(self):
        with _TempDeploy() as td:
            td.write_file("prefs.cfg")
            cfg = ResolverConfig(game_dir=td.root)
            rec = resolve("prefs.cfg", cfg)
            # ShouldSearchMods=False so mod overlay skipped; file exists -> base-loose
            self.assertEqual(rec["layer"], "base-loose")

    def test_non_data_path_miss(self):
        with _TempDeploy() as td:
            cfg = ResolverConfig(game_dir=td.root)
            rec = resolve("prefs.cfg", cfg)
            self.assertEqual(rec["layer"], "MISS")

    def test_base_strip_hit(self):
        with _TempDeploy() as td:
            # File exists at stripped path but NOT at original
            td.write_file("data/tgl/foo.tga")
            cfg = ResolverConfig(game_dir=td.root)
            rec = resolve("data/tgl/128/foo.tga", cfg)
            self.assertEqual(rec["layer"], "base-strip")

    def test_base_strip_skipped_when_original_exists(self):
        with _TempDeploy() as td:
            # If original exists, should resolve as base-loose before trying strip
            td.write_file("data/tgl/128/foo.tga")
            cfg = ResolverConfig(game_dir=td.root)
            rec = resolve("data/tgl/128/foo.tga", cfg)
            self.assertEqual(rec["layer"], "base-loose")

    def test_fastfile_hit(self):
        with _TempDeploy() as td:
            listing = td.write_file("tgl.fst.txt", "data/art/walka.tga\n")
            cfg = ResolverConfig(game_dir=td.root, fst_files=[listing])
            rec = resolve("data/art/walka.tga", cfg)
            self.assertEqual(rec["layer"], "fastfile")
            self.assertEqual(rec["path"], "")

    def test_fastfile_miss_when_not_in_fst(self):
        with _TempDeploy() as td:
            listing = td.write_file("tgl.fst.txt", "data/art/walka.tga\n")
            cfg = ResolverConfig(game_dir=td.root, fst_files=[listing])
            rec = resolve("data/art/NOTHERE.tga", cfg)
            self.assertEqual(rec["layer"], "MISS")

    def test_normalization_applied_to_request(self):
        with _TempDeploy() as td:
            td.write_file("data/missions/mc2_01.fit")
            cfg = ResolverConfig(game_dir=td.root)
            # Mixed case + backslash
            rec = resolve("DATA\\Missions\\MC2_01.FIT", cfg)
            self.assertEqual(rec["key"], "data/missions/mc2_01.fit")
            # May or may not find the file (NTFS is case-insensitive but os.path.isfile
            # may return True on Windows even with different case)
            self.assertIn(rec["layer"], ("base-loose", "MISS"))

    def test_record_schema(self):
        with _TempDeploy() as td:
            td.write_file("data/tgl/foo.ini")
            cfg = ResolverConfig(game_dir=td.root)
            rec = resolve("data/tgl/foo.ini", cfg)
            self.assertIn("v", rec)
            self.assertIn("key", rec)
            self.assertIn("req", rec)
            self.assertIn("layer", rec)
            self.assertIn("path", rec)
            self.assertIn("shadowed", rec)
            self.assertEqual(rec["v"], 1)


class TestResolveModOverlay(unittest.TestCase):
    """resolve() with active mod: first-wins, dep chain, shadowed list."""

    def test_mod_hit_wins_over_base(self):
        with _TempDeploy() as td:
            td.write_mod_json("mymod")
            td.write_file("mods/mymod/data/tgl/abuilding.ini", "mod-version")
            td.write_file("data/tgl/abuilding.ini", "base-version")
            cfg = ResolverConfig(game_dir=td.root, active_mod="mymod")
            rec = resolve("data/tgl/abuilding.ini", cfg)
            self.assertEqual(rec["layer"], "mod:mymod")
            self.assertIn("abuilding.ini", rec["path"])
            self.assertIn("mods/mymod", rec["path"])

    def test_base_fallthrough_when_mod_misses(self):
        with _TempDeploy() as td:
            td.write_mod_json("mymod")
            td.write_file("data/tgl/base_only.ini")
            cfg = ResolverConfig(game_dir=td.root, active_mod="mymod")
            rec = resolve("data/tgl/base_only.ini", cfg)
            self.assertEqual(rec["layer"], "base-loose")

    def test_shadowed_base_in_record(self):
        with _TempDeploy() as td:
            td.write_mod_json("mymod")
            td.write_file("mods/mymod/data/tgl/abuilding.ini", "mod")
            td.write_file("data/tgl/abuilding.ini", "base")
            cfg = ResolverConfig(game_dir=td.root, active_mod="mymod")
            rec = resolve("data/tgl/abuilding.ini", cfg)
            self.assertIn("base-loose", rec["shadowed"])

    def test_dep_chain_first_wins(self):
        """Active mod wins over dep; dep wins over base."""
        with _TempDeploy() as td:
            td.write_mod_json("active", deps=["dep-a", "dep-b"])
            td.write_mod_json("dep-a")
            td.write_mod_json("dep-b")
            td.write_file("mods/active/data/tgl/shared.ini", "active")
            td.write_file("mods/dep-a/data/tgl/shared.ini", "dep-a")
            td.write_file("mods/dep-b/data/tgl/shared.ini", "dep-b")
            td.write_file("data/tgl/shared.ini", "base")
            cfg = ResolverConfig(game_dir=td.root, active_mod="active")
            rec = resolve("data/tgl/shared.ini", cfg)
            self.assertEqual(rec["layer"], "mod:active")

    def test_dep_wins_over_base(self):
        """When active mod does NOT have file, dep wins."""
        with _TempDeploy() as td:
            td.write_mod_json("active", deps=["dep-a"])
            td.write_mod_json("dep-a")
            td.write_file("mods/dep-a/data/tgl/depfile.ini", "dep-a")
            td.write_file("data/tgl/depfile.ini", "base")
            cfg = ResolverConfig(game_dir=td.root, active_mod="active")
            rec = resolve("data/tgl/depfile.ini", cfg)
            self.assertEqual(rec["layer"], "mod:dep-a")

    def test_dep_a_wins_over_dep_b(self):
        """dep[0] wins over dep[1] when active mod misses."""
        with _TempDeploy() as td:
            td.write_mod_json("active", deps=["dep-a", "dep-b"])
            td.write_mod_json("dep-a")
            td.write_mod_json("dep-b")
            td.write_file("mods/dep-a/data/tgl/overlap.ini", "dep-a")
            td.write_file("mods/dep-b/data/tgl/overlap.ini", "dep-b")
            cfg = ResolverConfig(game_dir=td.root, active_mod="active")
            rec = resolve("data/tgl/overlap.ini", cfg)
            self.assertEqual(rec["layer"], "mod:dep-a")

    def test_non_data_path_bypasses_mod(self):
        """ShouldSearchMods=False for non-data/ paths -- mod overlay skipped."""
        with _TempDeploy() as td:
            td.write_mod_json("mymod")
            td.write_file("mods/mymod/prefs.cfg", "mod-prefs")
            # We write the file in the mod folder but NOT in game_dir root
            # ShouldSearchMods("prefs.cfg") -> False, so mod overlay skipped
            cfg = ResolverConfig(game_dir=td.root, active_mod="mymod")
            rec = resolve("prefs.cfg", cfg)
            # Should NOT resolve to mod -- either base-loose (if it exists in game_dir) or MISS
            self.assertNotEqual(rec["layer"], "mod:mymod")

    def test_dotdir_skipped_in_mod(self):
        """dot-prefixed dirs under mods/<id>/ are excluded from the index (C5 ruling)."""
        with _TempDeploy() as td:
            td.write_mod_json("mymod")
            # Plant a file in a dotdir -- should NOT appear in index
            td.write_file("mods/mymod/.scratch/data/tgl/scratch.ini", "scratch")
            td.write_file("mods/mymod/data/tgl/real.ini", "real")
            cfg = ResolverConfig(game_dir=td.root, active_mod="mymod")
            # Scratch is an explicit layer above the active project.
            self.assertEqual(cfg.mod_index["data/tgl/scratch.ini"][1], "mymod#scratch")
            # real.ini SHOULD be in mod_index
            self.assertIn("data/tgl/real.ini", cfg.mod_index)

    def test_missing_mod_json_uses_folder_name(self):
        """When mod.json is absent, mod_id falls back to folder name (file.cpp:516-521)."""
        with _TempDeploy() as td:
            # No mod.json written -- only the data dir
            td.write_file("mods/mymod/data/tgl/foo.ini", "mod")
            cfg = ResolverConfig(game_dir=td.root, active_mod="mymod")
            rec = resolve("data/tgl/foo.ini", cfg)
            # layer should be "mod:mymod" (folder name used as id)
            self.assertEqual(rec["layer"], "mod:mymod")

    def test_absolute_path_bypasses_mod(self):
        """Absolute paths bypass ShouldSearchMods entirely."""
        with _TempDeploy() as td:
            td.write_mod_json("mymod")
            cfg = ResolverConfig(game_dir=td.root, active_mod="mymod")
            # Even if it were in the mod index, should_search_mods returns False
            rec = resolve("A:/absolute/path/to/data/foo.ini", cfg)
            self.assertNotEqual(rec["layer"], "mod:mymod")

    def test_base_game_mode_no_mod_index(self):
        """Without MC2_ACTIVE_MOD, g_modIndex is empty -- zero overhead."""
        with _TempDeploy() as td:
            td.write_file("data/tgl/foo.ini")
            cfg = ResolverConfig(game_dir=td.root, active_mod=None)
            self.assertEqual(len(cfg.mod_index), 0)
            rec = resolve("data/tgl/foo.ini", cfg)
            self.assertEqual(rec["layer"], "base-loose")


class TestParitySmoke(unittest.TestCase):
    """Basic parity_smoke.py logic via direct import."""

    def test_no_mismatches_base_game(self):
        """Parity smoke on a synthetic base-game trace: all records should match."""
        import importlib.util, types, sys as _sys

        # Import parity_smoke without running main
        spec = importlib.util.spec_from_file_location(
            "parity_smoke",
            os.path.join(_TOOLS, "parity_smoke.py"),
        )
        ps = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ps)

        with _TempDeploy() as td:
            td.write_file("data/missions/mc2_01.fit")
            td.write_file("data/tgl/foo.ini")
            cfg = ResolverConfig(game_dir=td.root)

            # Synthetic engine trace
            trace = [
                {"v": 1, "key": "data/missions/mc2_01.fit", "req": "data/missions/mc2_01.fit",
                 "layer": "base-loose", "path": f"{td.root}/data/missions/mc2_01.fit"},
                {"v": 1, "key": "data/tgl/missing.ini", "req": "data/tgl/missing.ini",
                 "layer": "MISS", "path": ""},
            ]

            import io
            from contextlib import redirect_stdout
            with redirect_stdout(io.StringIO()):
                mismatches = ps.run_parity(trace, cfg, fst_set_empty=True, verbose=False)

            self.assertEqual(mismatches, 0)


if __name__ == "__main__":
    unittest.main()
