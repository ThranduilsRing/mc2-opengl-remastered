"""The negative matrix probe must target a shader the engine still loads."""
import importlib.util
from pathlib import Path
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_gl_variants", ROOT / "scripts/check-gl-shader-variants.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def test_native_renderer_has_no_gl_program_variants():
    assert checker.extractor.extract()["variants"] == []


def test_negative_probe_does_not_mistake_compute_for_a_fragment():
    records = [{"key": "compute", "stages": {"comp": "shaders/example.comp"}},
               {"key": "color", "stages": {"frag": "shaders/live.frag"}}]
    assert checker.regression_target(records) == ("shaders/live.frag", [records[1]])


def test_missing_negative_target_is_an_error():
    with pytest.raises(ValueError, match="no fragment"):
        checker.regression_target([])
