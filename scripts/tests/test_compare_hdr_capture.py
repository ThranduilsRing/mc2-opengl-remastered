import importlib.util
from pathlib import Path

import numpy as np
import pytest

SPEC = importlib.util.spec_from_file_location(
    "compare_hdr_capture", Path(__file__).resolve().parents[2] / "tools/compare_hdr_capture.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def pair(tmp_path):
    raw = tmp_path / "test.frame-431.f16.raw"
    ppm = tmp_path / "test.frame-431.sdr.ppm"
    # Distinct rows detect a missing bottom-up conversion; last pixel is HDR.
    scene = np.array([[[0, 0, 0, 1], [1, 1, 1, 1]],
                      [[0.21586, 0.21586, 0.21586, 1], [2, 2, 2, 1]]], dtype="<f2")
    scene[::-1].tofile(raw)
    ppm.write_bytes(b"P6\n# frame=431 mode=0 dither=0 scene-only\n2 2\n255\n" +
                    bytes([0, 0, 0, 255, 255, 255, 128, 128, 128, 0, 0, 0]))
    return raw, ppm


def test_same_frame_orientation_and_paper_white(tmp_path):
    result = MODULE.compare(*pair(tmp_path))
    assert result["passed"] and result["compared"] == 3 and result["excluded"] == 1
    assert result["max_codes"] == 0


@pytest.mark.parametrize("error,passed", [(1, True), (2, False)])
def test_one_code_bar(tmp_path, error, passed):
    raw, ppm = pair(tmp_path)
    data = bytearray(ppm.read_bytes())
    data[-12] = error
    ppm.write_bytes(data)
    result = MODULE.compare(raw, ppm)
    assert result["passed"] is passed
    assert result["violations"] == (0 if passed else 1)


@pytest.mark.parametrize("fault", ["frame", "header", "size", "nan", "empty"])
def test_reject_invalid_proof(tmp_path, fault):
    raw, ppm = pair(tmp_path)
    if fault == "frame":
        other = tmp_path / "test.frame-432.sdr.ppm"
        other.write_bytes(ppm.read_bytes())
        ppm = other
    elif fault == "header":
        ppm.write_bytes(ppm.read_bytes().replace(b"dither=0", b"dither=1"))
    elif fault == "size":
        raw.write_bytes(raw.read_bytes()[:-1])
    else:
        scene = np.fromfile(raw, dtype="<f2")
        scene[:] = np.nan if fault == "nan" else 2
        scene.tofile(raw)
    with pytest.raises(ValueError):
        MODULE.compare(raw, ppm)
