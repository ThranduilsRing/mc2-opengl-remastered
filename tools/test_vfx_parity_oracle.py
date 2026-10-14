import hashlib
import json
import struct
from pathlib import Path
import sys

from PIL import Image
from vfx_oracle.evidence import extract
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
    raw_log = logged_observations().encode()
    monkeypatch.setattr(Path, "read_bytes", lambda path: raw_log)
    stamp = {"fingerprint": "5636f22bc357", "bookmark": "visible-effect", "frame": 154}
    effect = {"class": "ppc", "rect": [0, 0, 8, 8],
              "images": {"mlr": ["mlr1.png", "mlr2.png"], "gpu": ["gpu1.png", "gpu2.png"]},
              "evidence": {"experiment": "harvested", "captures": {}, "effect_free": {}, "suppressed": {}}}
    controls = {}
    for arm in ("mlr", "gpu"):
        effect["evidence"]["captures"][arm] = [
            {**stamp, "demonstration": "Visible blue-white PPC beam in this crop."} for _ in range(2)]
        for kind in ("effect_free", "suppressed"):
            name = f"{arm}-{kind}.png"
            effect["evidence"][kind][arm] = {**stamp, "image": name}
            controls[Path(name).resolve()] = Image.new("RGB", (8, 8))
        effect["evidence"]["suppressed"][arm]["effect_suppressed"] = True
        enabled = "1" if arm == "gpu" else "0"
        records = [*effect["evidence"]["captures"][arm],
                   effect["evidence"]["effect_free"][arm], effect["evidence"]["suppressed"][arm]]
        for index, record in enumerate(records):
            record["runtime"] = {
                "process": f"{arm}-{index}", "effect_age": 0.25,
                "rng": {"owner": "RenderWorld", "stream": "56465852", "version": 1, "seed": 42,
                        "algorithm": "LCG32-NR-next15", "state": 123, "draw_count": 9},
                "backend": {"requested": "gl", "drawn": "gl", "readback": "gl_fbo"},
                "assets": {"mc2.fx": "a" * 64}, "birth_transcript": extract(raw_log.decode())["birth_transcript"],
                "log_sha256": hashlib.sha256(raw_log).hexdigest(), "log": f"{arm}-{index}.log",
                "gates": {"MC2_GPU_PARTICLES": enabled, "MC2_VFX_ORACLE_RENDER": enabled,
                          "MC2_VFX_ORACLE_TUBE": enabled, "MC2_VFX_ORACLE_SHAPE": enabled,
                          "MC2_VFX_GPU_SIM_CARDCLOUD": "0", "MC2_VFX_GPU_SIM_COMPARE": "0"},
                "draw_probe": {"scope": "all_draw_flush_cull", "unchanged": True, "frozen_repeats": 2}}
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


@pytest.mark.parametrize("field,value", [
    ("rng", {"owner": "Effect", "stream": "legacy"}),
    ("rng", []), ("effect_age", 10**1000),
    ("effect_age", float("nan")), ("assets", {}), ("birth_transcript", ""),
    ("backend", {"requested": "gl", "drawn": "vk", "readback": "gl_fbo"}),
    ("gates", {}),
    ("process", "mlr-0"),
])
def test_incomplete_or_false_runtime_evidence_is_diagnostic(parity_case, field, value):
    parity_case[0]["evidence"]["captures"]["gpu"][0]["runtime"][field] = value
    assert grade(parity_case)["status"] == "DIAGNOSTIC"


def logged_observations():
    birth = "[VFX_BIRTH v1] spec=PPC stream=56465852 initial=42 state=123 draws=9"
    fields = ("scope=all owner=RenderWorld spec=PPC time=5 age=0.25 seed=0.2 initial=42 "
              "stream=56465852 state_before=123 state_after=123 draws_before=9 draws_after=9 "
              "legacy_before=10 legacy_after=10 legacy_state_before=" + "b" * 64
              + " legacy_state_after=" + "b" * 64 + " unchanged=1")
    return "\n".join([birth] + [f"[VFX_{phase}_RNG v1] {fields}"
                              for _ in range(2) for phase in ("DRAW", "FLUSH", "CULL")])


def bind_log(case, monkeypatch, log):
    raw_log = log.encode()
    monkeypatch.setattr(Path, "read_bytes", lambda path: raw_log)
    evidence = case[0]["evidence"]
    for arm in ("mlr", "gpu"):
        for record in [*evidence["captures"][arm], evidence["effect_free"][arm], evidence["suppressed"][arm]]:
            record["runtime"]["log_sha256"] = hashlib.sha256(raw_log).hexdigest()
            record["runtime"]["birth_transcript"] = extract(log)["birth_transcript"]


@pytest.mark.parametrize("missing", ["DRAW", "FLUSH", "CULL", "all"])
def test_manifest_claim_cannot_replace_logged_phase_observations(parity_case, monkeypatch, missing):
    lines = logged_observations().splitlines()
    log = "\n".join(line for line in lines if "_RNG v1]" not in line
                    or (missing != "all" and f"[VFX_{missing}_RNG v1]" not in line))
    bind_log(parity_case, monkeypatch, log)
    assert not grade(parity_case)["parity_accepted"]


@pytest.mark.parametrize("claim", [None, {}, {"scope": "all_draw_flush_cull", "unchanged": False,
                                           "frozen_repeats": 999}])
def test_logged_observations_determine_proof_not_manifest_probe(parity_case, claim):
    for arm in ("mlr", "gpu"):
        for record in parity_case[0]["evidence"]["captures"][arm]:
            record["runtime"]["draw_probe"] = claim
    result = grade(parity_case)
    assert result["parity_accepted"]
    assert result["observed_draw_probes"]["gpu"][0]["phase_samples"] == {"draw": 2, "flush": 2, "cull": 2}


@pytest.mark.parametrize("before,after", [
    ("state_after=123", "state_after=124"), ("draws_after=9", "draws_after=10"),
    ("legacy_after=10", "legacy_after=11"), ("unchanged=1", "unchanged=0"),
    ("legacy_state_after=" + "b" * 64, "legacy_state_after=" + "c" * 64),
    ("state_before=123 ", ""), ("draws_before=9", "draws_before=-1"),
    ("state_after=123", "state_after=4294967296"),
    ("legacy_state_before=" + "b" * 64 + " ", ""),
    ("scope=all", "class=CardCloud"), ("owner=RenderWorld", "owner=Effect"),
    ("initial=42", "initial=43"), ("age=0.25", "age=0.5"),
])
def test_logged_mutation_missing_snapshot_or_wrong_scope_rejects_claims(parity_case, monkeypatch, before, after):
    # Valid hashes keep these cases focused on snapshot validation.
    bind_log(parity_case, monkeypatch, logged_observations().replace(before, after))
    assert not grade(parity_case)["parity_accepted"]


def test_phases_must_repeat_the_same_frozen_state(parity_case, monkeypatch):
    lines = logged_observations().splitlines()
    log = "\n".join(line.replace("time=5", "time=6") if "VFX_CULL_RNG" in line else line for line in lines)
    bind_log(parity_case, monkeypatch, log)
    assert not grade(parity_case)["parity_accepted"]
    bind_log(parity_case, monkeypatch, "\n".join(lines[:4]))
    assert not grade(parity_case)["parity_accepted"]


@pytest.mark.parametrize("phase", ["DRAW", "FLUSH", "CULL"])
def test_valid_observations_cannot_hide_another_rng_mutation(parity_case, monkeypatch, phase):
    log = logged_observations()
    changed = next(line for line in log.splitlines() if f"[VFX_{phase}_RNG v1]" in line)
    bind_log(parity_case, monkeypatch, log + "\n" + changed.replace("state_after=123", "state_after=124"))
    assert not grade(parity_case)["parity_accepted"]


def test_each_capture_needs_its_own_hash_bound_observations(parity_case, monkeypatch):
    record = parity_case[0]["evidence"]["captures"]["gpu"][0]["runtime"]
    log = "\n".join(line for line in logged_observations().splitlines() if "VFX_FLUSH_RNG" not in line).encode()
    record["log_sha256"] = hashlib.sha256(log).hexdigest()
    missing = Path(record["log"]).resolve()
    monkeypatch.setattr(Path, "read_bytes", lambda path: log if path == missing else logged_observations().encode())
    result = grade(parity_case)
    assert not result["parity_accepted"]
    assert any("logged zero-RNG" in failure for failure in result["parity_failures"])


@pytest.mark.parametrize("field,value", [("effect_age", 0.5), ("birth_transcript", "d" * 64),
                                        ("assets", {"mc2.fx": "d" * 64})])
def test_repeat_runtime_inputs_must_agree(parity_case, field, value):
    parity_case[0]["evidence"]["captures"]["gpu"][1]["runtime"][field] = value
    assert not grade(parity_case)["parity_accepted"]


@pytest.mark.parametrize("field,value", [("seed", 43), ("state", 124), ("draw_count", 10),
                                        ("seed", True), ("state", -1), ("draw_count", 2**64)])
def test_rng_values_and_repeat_equality_are_required(parity_case, field, value):
    parity_case[0]["evidence"]["captures"]["gpu"][1]["runtime"]["rng"][field] = value
    assert not grade(parity_case)["parity_accepted"]


def test_old_provenance_without_runtime_evidence_stays_diagnostic(parity_case):
    del parity_case[0]["evidence"]["captures"]["gpu"][1]["runtime"]
    assert not grade(parity_case)["parity_accepted"]


def test_changed_bound_log_and_reused_process_log_are_rejected(parity_case, monkeypatch):
    monkeypatch.setattr(Path, "read_bytes", lambda path: b"tampered log")
    assert not grade(parity_case)["parity_accepted"]


def test_eight_process_ids_cannot_reuse_one_engine_log(parity_case):
    parity_case[0]["evidence"]["captures"]["gpu"][1]["runtime"]["log"] = "gpu-0.log"
    assert not grade(parity_case)["parity_accepted"]


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
