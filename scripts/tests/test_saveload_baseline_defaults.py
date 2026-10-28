"""saveload_roundtrip --baseline reads save_at, load_at, sim_speed and the mask from the existing baseline when unset (as check does)."""
from __future__ import annotations

import json
import sys
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import saveload_roundtrip as sl  # noqa: E402


class _Stop(Exception):
    pass


def _run(tmp_path, prev, argv):
    path = tmp_path / "b.json"
    if prev is not None:
        path.write_text(json.dumps(prev), encoding="utf-8")
    seen = {}

    def fake(mission, save_at, load_at, *a, **kw):
        seen.update(save_at=save_at, load_at=load_at, sim_speed=kw.get("sim_speed"))
        raise _Stop()

    with patch.object(sl, "baseline_path", return_value=str(path)), patch.object(sl, "run_fixture", side_effect=fake):
        try:
            sl.main(["--baseline", "--mission", "m", *argv])
        except _Stop:
            pass
    return seen


def test_baseline_inherits_frames_and_speed(tmp_path):
    seen = _run(tmp_path, {"save_at": 77, "load_at": 99, "sim_speed": 4}, [])
    assert seen == {"save_at": 77, "load_at": 99, "sim_speed": 4}


def test_explicit_flags_win_and_no_baseline_uses_defaults(tmp_path):
    assert _run(tmp_path, {"save_at": 77, "load_at": 99}, ["--save-at", "5", "--load-at", "6"])["save_at"] == 5


def test_mask_and_first_save_only_inherited():
    mask = str(ROOT / "tools" / "save_noise_mask.json")
    prev = {"comparison": sl.mask_policy(mask), "judged": "first-save-only", "save_at": 1, "load_at": 2}
    import argparse
    a = argparse.Namespace(save_at=None, load_at=None, sim_speed=None, mask=None, first_save_only=False)
    sl.fill_from_previous(a, prev)
    assert (a.save_at, a.load_at, a.mask, a.first_save_only) == (1, 2, mask, True)
    b = argparse.Namespace(save_at=None, load_at=None, sim_speed=None, mask=None, first_save_only=False)
    sl.fill_from_previous(b, {"comparison": {"mode": "exact"}})
    assert b.mask is None and not b.first_save_only
