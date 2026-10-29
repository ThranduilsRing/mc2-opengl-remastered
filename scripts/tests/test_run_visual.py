"""Local regressions must survive low whole-frame MAD and malformed evidence."""
import copy
import importlib.util
import json
from pathlib import Path

from PIL import Image, ImageDraw
import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "run_visual.py"
spec = importlib.util.spec_from_file_location("run_visual", SCRIPT)
visual = importlib.util.module_from_spec(spec)
spec.loader.exec_module(visual)


@pytest.fixture
def config():
    return json.loads(visual.CONFIG.read_text(encoding="utf-8"))


def test_identical_is_clean(config):
    visual.validate_schema(config)
    image = Image.new("RGB", (128, 128), "#505050")
    assert visual.changed_boxes(image, image.copy(), config) == []


def test_missing_small_object_is_boxed_despite_low_mad(config):
    a = Image.new("RGB", (1024, 1024))
    b = a.copy()
    ImageDraw.Draw(b).rectangle((72, 72, 83, 83), fill="white")
    boxes = visual.changed_boxes(a, b, config)
    assert boxes == [{"region": "world", "rect": [64, 64, 96, 96], "changed_fraction": 144 / 1024}]
    assert 255 * 144 / (1024 * 1024) < 0.04  # Whole-frame MAD would hide it.


def test_region_specific_tolerance(config):
    a = Image.new("RGB", (128, 128))
    b = a.copy()
    ImageDraw.Draw(b).rectangle((0, 0, 15, 15), fill=(20, 20, 20))
    ImageDraw.Draw(b).rectangle((0, 110, 15, 125), fill=(20, 20, 20))
    assert {box["region"] for box in visual.changed_boxes(a, b, config)} == {"world"}


@pytest.mark.parametrize("mutation", ["gap", "overlap", "nan", "tile", "pose"])
def test_bad_schema(config, mutation):
    config = copy.deepcopy(config)
    if mutation == "gap":
        config["regions"][0]["rect"][2] = 0.5
    elif mutation == "overlap":
        config["regions"][0]["rect"][3] = 0.9
    elif mutation == "nan":
        config["regions"][0]["changed_fraction"] = float("nan")
    elif mutation == "tile":
        config["tile_px"] = 0
    else:
        config["scenes"][0]["pose"] = "absent"
    with pytest.raises(ValueError):
        visual.validate_schema(config)


def test_size_mismatch(config):
    with pytest.raises(ValueError, match="dimensions"):
        visual.changed_boxes(Image.new("RGB", (32, 32)), Image.new("RGB", (64, 64)), config)


def make_set(root, config):
    root.mkdir()
    (root / "set.json").write_text(json.dumps({"config": config}), encoding="utf-8")
    for scene in config["scenes"]:
        directory = root / scene["name"]
        directory.mkdir()
        image = directory / f"{scene['mission']}_{scene['pose']}.png"
        Image.new("RGB", (128, 128)).save(image)
        image.with_suffix(".json").write_text(json.dumps({"deterministic": True, "backend": "vulkan"}), encoding="utf-8")


def test_report_and_missing_sidecar(tmp_path, config):
    golden, candidate = tmp_path / "golden", tmp_path / "candidate"
    make_set(golden, config)
    make_set(candidate, config)
    out = tmp_path / "report"
    assert visual.compare(golden, candidate, out, config) == 0
    assert json.loads((out / "report.json").read_text())["status"] == "CLEAN"
    scene = config["scenes"][0]
    path, _ = visual.evidence(candidate, scene)
    with Image.open(path) as image:
        ImageDraw.Draw(image).rectangle((0, 0, 12, 12), fill="white")
        image.save(path)
    assert visual.compare(golden, candidate, out, config) == 1
    assert 'viewport' in (out / "index.html").read_text()
    assert json.loads((out / "report.json").read_text())["scenes"][0]["boxes"]
    path.with_suffix(".json").unlink()
    assert visual.compare(golden, candidate, out, config) == 2
    assert json.loads((out / "report.json").read_text())["status"] == "INCOMPLETE"


def test_interrupted_set_never_passes(tmp_path, config):
    golden, candidate = tmp_path / "golden", tmp_path / "candidate"
    make_set(golden, config)
    make_set(candidate, config)
    (candidate / "set.json").unlink()
    assert visual.compare(golden, candidate, tmp_path / "report", config) == 2
    report = json.loads((tmp_path / "report/report.json").read_text())
    assert report["status"] == "INCOMPLETE"
    assert all(row["boxes"] == [] for row in report["scenes"])
