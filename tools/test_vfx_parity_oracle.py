import hashlib
import json
import struct
from pathlib import Path
import sys

from PIL import Image
import pytest
import vfx_parity_oracle as oracle

from vfx_parity_oracle import (acceptance, changed_pixels, compare, digest, montage,
                              region, source_paths, summarize)


def test_digest_is_decoded_rgba_with_dimensions():
    image = Image.new("RGB", (2, 1), (11, 22, 33))
    expected = hashlib.sha256(struct.pack("<II", 2, 1) + bytes([11, 22, 33, 255]) * 2).hexdigest()
    assert digest(image) == expected
    assert digest(image) == digest(image.convert("RGBA"))
    assert digest(image) != digest(Image.new("RGB", (1, 2), (11, 22, 33)))


def test_crop_excludes_outside_changes_and_includes_boundary_pixel():
    image = Image.new("RGBA", (4, 4), (0, 0, 0, 255))
    before = region(image, [1, 1, 3, 3])
    image.putpixel((3, 3), (255, 0, 0, 255))
    assert digest(before) == digest(region(image, [1, 1, 3, 3]))
    image.putpixel((2, 2), (255, 0, 0, 255))
    assert digest(before) != digest(region(image, [1, 1, 3, 3]))


@pytest.mark.parametrize("rect", [[-1, 0, 1, 1], [0, 0, 5, 1], [1, 1, 1, 2],
                                  [0, 0, 1.5, 2], [0, 0, 1]])
def test_crop_rejects_padding_or_empty_regions(rect):
    with pytest.raises(ValueError):
        region(Image.new("RGB", (4, 4)), rect)


def test_repeatability_and_parity_are_independent():
    black = Image.new("RGB", (3, 2))
    red = Image.new("RGB", (3, 2), "red")
    result = compare([black, black, red, red])
    assert result["repeatable"] and not result["cross_arm_equal"]
    assert not compare([black, red, black, black])["repeatable"]
    assert montage([black, black, red, red]).size == (9, 52)


def test_compare_requires_two_repeats_of_each_arm():
    black = Image.new("RGB", (3, 2))
    with pytest.raises(ValueError):
        compare([black] * 3)
    with pytest.raises(ValueError):
        compare([black] * 3 + [Image.new("RGB", (4, 2))])


@pytest.mark.parametrize("images", [
    {"mlr": ["a.png", "a.png"], "gpu": ["c.png", "d.png"]},
    {"mlr": ["a.png", "./a.png"], "gpu": ["c.png", "d.png"]},
    {"mlr": ["a.png", "b.png"], "gpu": ["a.png", "d.png"]},
    {"mlr": ["a.png"], "gpu": ["c.png", "d.png"]},
])
def test_same_capture_cannot_be_a_repeat_control(images):
    with pytest.raises(ValueError):
        source_paths(Path("."), images)


def test_four_distinct_sources_keep_arm_and_repeat_order():
    names = ["a.png", "b.png", "c.png", "d.png"]
    assert source_paths(Path("."), {"mlr": names[:2], "gpu": names[2:]}) == [Path(name).resolve() for name in names]


@pytest.fixture
def parity_case(monkeypatch):
    stamp = {"fingerprint": "5636f22bc357", "bookmark": "visible-effect", "frame": 154}
    effect = {"class": "ppc", "rect": [0, 0, 8, 8],
              "images": {"mlr": ["mlr1.png", "mlr2.png"], "gpu": ["gpu1.png", "gpu2.png"]},
              "evidence": {"captures": {}, "effect_free": {}, "suppressed": {}}}
    controls = {}
    for arm in ("mlr", "gpu"):
        effect["evidence"]["captures"][arm] = [
            {**stamp, "demonstration": "Visible blue-white PPC beam in this crop."} for _ in range(2)]
        for kind in ("effect_free", "suppressed"):
            name = f"{arm}-{kind}.png"
            effect["evidence"][kind][arm] = {**stamp, "image": name}
            controls[Path(name).resolve()] = Image.new("RGB", (8, 8))
        effect["evidence"]["suppressed"][arm]["effect_suppressed"] = True
    monkeypatch.setattr(Image, "open", lambda path: controls[path].copy())
    return effect, [Image.new("RGB", (8, 8), "red") for _ in range(4)], controls


def grade(case):
    effect, crops, _ = case
    return acceptance(effect, crops, Path("."), (8, 8))


def test_proven_nonempty_family_and_four_family_set_accept_parity(parity_case):
    result = grade(parity_case)
    assert result["status"] == "PARITY" and result["parity_accepted"]
    assert result["negative_control"]["verdict"] == "FAIL"
    assert result["effect_region_changed_pixels"] == {"mlr": [64, 64], "gpu": [64, 64]}
    rows = [{"class": family, **result} for family in ("missile", "ppc", "explosion", "smoke")]
    assert summarize(rows)["status"] == "PARITY"
    assert summarize(rows[:-1])["status"] == "DIAGNOSTIC"
    assert summarize(rows[:-1])["missing_families"] == ["smoke"]


def test_empty_effect_region_fails_even_with_repeatable_equal_arms(parity_case):
    effect, crops, controls = parity_case
    for path in controls:
        controls[path] = crops[0].copy()
    result = grade(parity_case)
    assert result["repeatable"] and result["cross_arm_equal"]
    assert result["status"] == "DIAGNOSTIC" and not result["parity_accepted"]
    assert any("empty effect region" in failure for failure in result["parity_failures"])


def test_stable_but_wrong_gpu_fails_parity(parity_case):
    _, crops, _ = parity_case
    crops[2:] = [Image.new("RGB", (8, 8), "blue") for _ in range(2)]
    result = grade(parity_case)
    assert result["repeatable"] and not result["parity_accepted"]
    assert "MLR/GPU pixels differ" in result["parity_failures"]


def test_suppressed_run_itself_fails_presence(parity_case):
    effect, crops, _ = parity_case
    crops[:] = [Image.new("RGB", (8, 8)) for _ in range(4)]
    result = grade(parity_case)
    assert result["negative_control"]["verdict"] == "FAIL"
    assert not result["parity_accepted"]


def test_negative_control_that_still_has_effect_rejects_parity(parity_case):
    effect, crops, controls = parity_case
    path = Path(effect["evidence"]["suppressed"]["gpu"]["image"]).resolve()
    controls[path] = crops[0].copy()
    result = grade(parity_case)
    assert result["negative_control"]["verdict"] == "PASS"
    assert not result["parity_accepted"]
    assert "suppressed negative control did not FAIL effect presence" in result["parity_failures"]


@pytest.mark.parametrize("field,value", [("fingerprint", ""), ("bookmark", ""),
                                        ("frame", None), ("demonstration", "")])
def test_missing_provenance_or_demonstration_rejects_parity(parity_case, field, value):
    effect, _, _ = parity_case
    effect["evidence"]["captures"]["gpu"][1][field] = value
    assert not grade(parity_case)["parity_accepted"]


@pytest.mark.parametrize("kind,field,value", [
    ("effect_free", "frame", 155), ("suppressed", "fingerprint", "abcdefabcdef"),
    ("suppressed", "effect_suppressed", False), ("effect_free", "image", "mlr1.png"),
])
def test_controls_require_matching_provenance_distinct_runs_and_suppression(parity_case, kind, field, value):
    effect, _, _ = parity_case
    effect["evidence"][kind]["mlr"][field] = value
    assert not grade(parity_case)["parity_accepted"]


def test_presence_threshold_excludes_noise_and_requires_more_than_sixteen_pixels(parity_case):
    _, crops, _ = parity_case
    black = Image.new("RGB", (8, 8))
    assert changed_pixels(Image.new("RGB", (8, 8), (8, 8, 8)), black) == 0
    small = black.copy()
    for x in range(8):
        for y in range(2):
            small.putpixel((x, y), (9, 0, 0))
    crops[:] = [small] * 4
    assert not grade(parity_case)["parity_accepted"]
    small.putpixel((0, 2), (9, 0, 0))
    assert grade(parity_case)["parity_accepted"]


def test_todays_fixture_is_diagnostic_even_if_every_repeat_matches():
    path = Path(__file__).parent / "vfx_oracle" / "regions.json"
    doc = json.loads(path.read_text(encoding="utf-8"))
    crops = [Image.new("RGB", (8, 8), "red")] * 4
    rows = [{"class": effect["class"], **acceptance(effect, crops, path.parent, (8, 8))}
            for effect in doc["effects"]]
    assert all(row["repeatable"] and row["status"] == "DIAGNOSTIC" for row in rows)
    assert summarize(rows) == {"status": "DIAGNOSTIC", "parity_accepted": False, "missing_families": []}


def test_cli_never_exits_success_for_todays_fixture_even_with_matching_pixels(monkeypatch, capsys):
    path = Path(__file__).parent / "vfx_oracle" / "regions.json"
    monkeypatch.setattr(sys, "argv", ["vfx_parity_oracle.py", str(path), "--out", "unused-output"])
    monkeypatch.setattr(Image, "open", lambda path: Image.new("RGB", (1920, 1080)))
    monkeypatch.setattr(Image.Image, "save", lambda *a, **k: None)
    monkeypatch.setattr(Path, "mkdir", lambda *a, **k: None)
    written = []
    monkeypatch.setattr(Path, "write_text", lambda self, text, **k: written.append(json.loads(text)))
    assert oracle.main() == 1
    assert written[0]["status"] == "DIAGNOSTIC" and not written[0]["parity_accepted"]
    assert all(row["repeatable"] and row["cross_arm_equal"] for row in written[0]["effects"])
    assert "DIAGNOSTIC: parity_accepted=False" in capsys.readouterr().out
