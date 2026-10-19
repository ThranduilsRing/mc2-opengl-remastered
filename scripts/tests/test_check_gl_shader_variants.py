"""The negative matrix probe must target a shader the engine still loads."""
import importlib.util
from pathlib import Path
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_gl_variants", ROOT / "scripts/check-gl-shader-variants.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def test_negative_probe_follows_a_live_fragment_and_covers_all_its_variants():
    records = checker.extractor.extract()["variants"]
    fragment, targets = checker.regression_target(records)
    assert "static_prop" not in fragment
    assert targets
    assert targets == [v for v in records if fragment in v["stages"].values()]
    assert (ROOT / fragment).is_file()


def test_negative_probe_does_not_mistake_compute_for_a_fragment():
    records = [{"key": "compute", "stages": {"comp": "shaders/example.comp"}},
               {"key": "color", "stages": {"frag": "shaders/live.frag"}}]
    assert checker.regression_target(records) == ("shaders/live.frag", [records[1]])


def test_missing_negative_target_is_an_error():
    with pytest.raises(ValueError, match="no fragment"):
        checker.regression_target([])
