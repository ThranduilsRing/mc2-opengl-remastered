"""verify.py refuses a candidate drawn by a backend other than its golden's, unless --allow-backend-mismatch says so (logged, in the report).

WHY: the VK golden set replaces the GL one before GL is deleted (vk_golden_plan.md); a GL frame graded against a VK golden reports the backend
gap as a regression (or hides one), and nothing in the PASS/FAIL line said which backend drew the candidate.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_THIS = Path(__file__).resolve()
sys.path.insert(0, str(_THIS.parent))

import verify  # noqa: E402

VK_CAP = {"backend": {"requested": "vulkan", "drew": ["vulkan"]}}
GL_CAP = {"backend": {"requested": "gl", "drew": ["gl"]}}


def _golden(tmp_path: Path, body: dict) -> Path:
    d = tmp_path / "golden"
    d.mkdir()
    (d / "golden.json").write_text(json.dumps(body), encoding="utf-8")
    return d


def test_named_backend_matches():
    assert verify.golden_backend({"backend": "vulkan"}) == "vulkan"
    assert verify.golden_backend({"capture_env": {"backend": {"requested": "gl", "drew": ["gl"]}}}) == "gl"
    assert verify.golden_backend({}) is None


def test_same_backend_passes(tmp_path):
    rec = verify.check_backend(_golden(tmp_path, {"backend": "vulkan"}), VK_CAP, allow_mismatch=False)
    assert rec == {"golden": "vulkan", "candidate_drew": ["vulkan"], "mismatch_allowed": False}


def test_other_backend_refused(tmp_path):
    with pytest.raises(SystemExit, match="BACKEND MISMATCH"):
        verify.check_backend(_golden(tmp_path, {"backend": "vulkan"}), GL_CAP, allow_mismatch=False)


def test_unrecorded_candidate_refused(tmp_path):
    with pytest.raises(SystemExit, match="records no backend"):
        verify.check_backend(_golden(tmp_path, {"backend": "vulkan"}), {}, allow_mismatch=False)


def test_mixed_draw_refused(tmp_path):
    cap = {"backend": {"requested": "vulkan", "drew": ["vulkan", "gl"]}}
    with pytest.raises(SystemExit, match="BACKEND MISMATCH"):
        verify.check_backend(_golden(tmp_path, {"backend": "vulkan"}), cap, allow_mismatch=False)


def test_override_logged_and_recorded(tmp_path, capsys):
    rec = verify.check_backend(_golden(tmp_path, {"capture_env": {"backend": {"requested": "gl", "drew": ["gl"]}}}), VK_CAP,
                               allow_mismatch=True)
    assert rec["mismatch_allowed"] is True and rec["golden"] == "gl"
    assert "--allow-backend-mismatch" in capsys.readouterr().err


def test_legacy_golden_not_checked(tmp_path, capsys):
    rec = verify.check_backend(_golden(tmp_path, {}), GL_CAP, allow_mismatch=False)
    assert rec["golden"] is None
    assert "records no backend" in capsys.readouterr().err
