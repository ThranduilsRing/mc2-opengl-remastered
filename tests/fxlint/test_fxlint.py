"""
tests/fxlint/test_fxlint.py -- Unit tests for tools/fxlint/fxlint.py
(FX-DEFS-SIDECAR-1, VFX-MODERNIZATION-PROPOSAL-1 slice 1).

Covers: clean lint on sample_good_mod (0 errors), every FXD_* error code on
sample_bad_mod (intentional-failure fixture -- one distinct mistake per
file), single-file mode, folder mode (mod-root walk to data/effects/defs/),
and the --catalog cross-reference (FXD_EFFECT_NAME_UNRESOLVED).

Python 3 stdlib only (unittest).
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

_TOOLS_DIR = str(Path(__file__).resolve().parents[2] / "tools" / "fxlint")
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

import fxlint  # noqa: E402

_FIXTURES = Path(__file__).resolve().parent / "fixtures"
_GOOD_MOD = _FIXTURES / "sample_good_mod"
_BAD_MOD = _FIXTURES / "sample_bad_mod"
_CATALOG = _FIXTURES / "sample_catalog.json"


class TestFxlintGoodMod(unittest.TestCase):
    def test_clean_lint_zero_errors(self):
        files = fxlint.find_def_files(str(_GOOD_MOD))
        self.assertEqual(len(files), 1)
        names = fxlint.load_catalog_names(str(_CATALOG))
        findings = fxlint.lint_file(files[0], names)
        errors = [f for f in findings if f.is_error]
        self.assertEqual(errors, [], f"expected 0 errors, got: {[str(e) for e in errors]}")

    def test_main_exit_code_zero(self):
        rc = fxlint.main([str(_GOOD_MOD), "--catalog", str(_CATALOG)])
        self.assertEqual(rc, 0)


class TestFxlintBadMod(unittest.TestCase):
    def _codes_for(self, filename: str, catalog=None):
        path = _BAD_MOD / "data" / "effects" / "defs" / filename
        findings = fxlint.lint_file(str(path), catalog)
        return {f.code for f in findings}

    def test_missing_effect_name(self):
        self.assertIn("FXD_MISSING_EFFECT_NAME", self._codes_for("missing_effect_name.fxdef.json"))

    def test_typo_curve_key_suggests_fix(self):
        findings = fxlint.lint_file(
            str(_BAD_MOD / "data/effects/defs/typo_curve_key.fxdef.json"), None)
        codes = {f.code for f in findings}
        self.assertIn("FXD_UNKNOWN_CURVE", codes)
        msg = next(f.message for f in findings if f.code == "FXD_UNKNOWN_CURVE")
        self.assertIn("did you mean 'alpha'", msg)

    def test_unresolved_effect_name_requires_catalog(self):
        # Without --catalog, this file is actually error-free (no catalog to
        # cross-ref against) -- confirms the check is opt-in, not silently
        # assumed.
        codes_no_catalog = self._codes_for("unresolved_effect_name.fxdef.json", catalog=None)
        self.assertNotIn("FXD_EFFECT_NAME_UNRESOLVED", codes_no_catalog)

        names = fxlint.load_catalog_names(str(_CATALOG))
        codes_with_catalog = self._codes_for("unresolved_effect_name.fxdef.json", catalog=names)
        self.assertIn("FXD_EFFECT_NAME_UNRESOLVED", codes_with_catalog)

    def test_bad_blend_value(self):
        self.assertIn("FXD_BAD_BLEND", self._codes_for("bad_blend_value.fxdef.json"))

    def test_curve_bad_value(self):
        self.assertIn("FXD_CURVE_BAD_VALUE", self._codes_for("curve_bad_value.fxdef.json"))

    def test_unsafe_texture_path(self):
        self.assertIn("FXD_TEXTURE_UNSAFE_PATH", self._codes_for("unsafe_texture_path.fxdef.json"))

    def test_malformed_json(self):
        self.assertIn("FXD_PARSE_ERROR", self._codes_for("malformed_json.fxdef.json"))

    def test_folder_mode_finds_all_bad_defs(self):
        files = fxlint.find_def_files(str(_BAD_MOD))
        # 7 intentionally-broken fixture files.
        self.assertEqual(len(files), 7)

    def test_main_exit_code_nonzero(self):
        rc = fxlint.main([str(_BAD_MOD)])
        self.assertEqual(rc, 1)


class TestFxlintMisc(unittest.TestCase):
    def test_single_file_mode(self):
        one = _GOOD_MOD / "data" / "effects" / "defs" / "Fireball.fxdef.json"
        files = fxlint.find_def_files(str(one))
        self.assertEqual(files, [str(one)])

    def test_unknown_top_level_key_is_warning_not_error(self):
        # A well-formed def with a stray/unrecognized top-level key should
        # warn (tolerated at runtime) but NOT count as an error.
        import json
        import tempfile
        import os
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "Weird.fxdef.json")
            with open(p, "w", encoding="utf-8") as fh:
                json.dump({"effect": "Fireball", "totallyMadeUpKey": 1}, fh)
            findings = fxlint.lint_file(p, None)
            warn = [f for f in findings if f.code == "FXD_UNKNOWN_TOP_KEY"]
            self.assertEqual(len(warn), 1)
            self.assertFalse(warn[0].is_error)


if __name__ == "__main__":
    unittest.main()
