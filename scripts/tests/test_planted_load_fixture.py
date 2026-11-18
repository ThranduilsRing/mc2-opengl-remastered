"""Pins the planted-load quickload fixture (code/missiongui_update.cpp updateSaveFixtures).

capture --load arms MC2_BOOT_QUICKSAVE_AT at 10^9 and sets QUICKLOAD_AT: the C++ must treat that as a
planted load (load fires with no quicksave, re-save follows). saveload_roundtrip's save-at 0 and the
normal save-then-load pair keep their behaviour.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import campaign_save_roundtrip as csr  # noqa: E402


def _block():
    src = (ROOT / "code" / "missiongui_update.cpp").read_text(encoding="utf-8")
    return src[src.index("const bool planted"):src.index("g_resaveAfterQuickload = (s_qsAt > 0)")]


def test_capture_load_env_is_planted_sentinel():
    env = csr.campaign_env("c", 1.0, 10**9, 900)
    assert int(env["MC2_BOOT_QUICKSAVE_AT"]) >= 1_000_000_000 and env["MC2_BOOT_QUICKLOAD_AT"] == "900"


def test_fixture_fires_planted_load_without_quicksave():
    b = _block()
    assert "planted = s_qsAt >= 1000000000L" in b
    assert re.search(r"s_qsAt <= 0 \|\| planted \|\| s_qsIssuedAt >= 0", b)
    assert "s_qsAt > 0 && !planted && s_qsIssuedAt >= 0" in b


def test_normal_pair_and_save_at_zero_unchanged():
    b = _block()
    assert "std::max(s_qlAt, s_qsIssuedAt + requestedGap)" in b  # save-then-load gap kept
    src = (ROOT / "code" / "missiongui_update.cpp").read_text(encoding="utf-8")
    assert "g_resaveAfterQuickload = (s_qsAt > 0);" in src  # save-at 0 -> no re-save
