"""--via-menu / --cold-cache: the env a player-path run gets, the log parse, the cold-cache verdict, and the Tracy hook. No engine launched."""
from __future__ import annotations

import sys
import pytest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]
ROOT = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(ROOT / "tools"))

from smoke_lib import cold_cache, via_menu  # noqa: E402


def test_tracy_intro_default_and_explicit_skip(monkeypatch):
    import tracy_capture_compare as tracy
    monkeypatch.setenv("MC2_MENU_CANARY_SKIP_INTRO", "1")
    calls = []
    monkeypatch.setattr(tracy, "capture_trace", lambda *a, **kw: calls.append(kw) or 1)
    for flags, skip in [([], False), (["--skip-intro"], True)]:
        monkeypatch.setattr(sys, "argv", ["tracy", "capture", "--install", "unused",
                                        "-o", "unused.tracy", "--via-menu", *flags])
        assert tracy.main() == 1
        assert calls[-1]["via_menu_boot"] and calls[-1]["skip_intro"] is skip
        env = tracy.game_env("vk", True, None, None, [], True, skip)
        assert (env.get("MC2_MENU_CANARY_SKIP_INTRO") == "1") is skip


def test_via_menu_env_drops_smoke_mode_and_boots_the_campaign():
    env = {"MC2_SMOKE_MODE": "1", "MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_BOOT_TO_MISSION": "x", "KEEP": "1"}
    via_menu.apply_via_menu_env(env)
    assert "MC2_SMOKE_MODE" not in env and "MC2_SMOKE_FIXED_TIMESTEP" not in env and "MC2_BOOT_TO_MISSION" not in env
    assert env["MC2_BOOT_TO_BAY"] == "campaign" and env["MC2_LOG"] == "1" and env["MC2_NO_CURSOR_GRAB"] == "1" and env["KEEP"] == "1"
    assert "MC2_MENU_CANARY_SKIP_INTRO" not in env
    env["MC2_MENU_CANARY_SKIP_INTRO"] = "1"
    via_menu.apply_via_menu_env(env)
    assert "MC2_MENU_CANARY_SKIP_INTRO" not in env   # videos play by default, even if the caller's env skipped them
    via_menu.apply_via_menu_env(env, skip_intro=True)
    assert env["MC2_MENU_CANARY_SKIP_INTRO"] == "1"


def test_bay_mission_walks_the_front_end():
    env = via_menu.apply_via_menu_env({}, bay_mission="mc2_03")
    assert env["MC2_BOOT_TO_MISSION"] == "mc2_03" and env["MC2_BOOT_TO_SCREEN"] == "bay" and env["MC2_SOAK_AUTO_PURCHASE"] == "1"
    assert int(env["MC2_SOAK_WIN_AFTER_SEC"]) > 3600


def test_game_argv_has_no_mission_or_smoke_args():
    argv = via_menu.game_argv(Path("x/mc2.exe"))
    assert "--mission" not in argv and "--duration" not in argv


LOG = [
    (0.4, "[TIME] t=  0.40s  phase=window_created"),
    (3.25, "[VIA_MENU v1] event=main_menu"),
    (3.30, "[VIDEO] init: opened A:/x/data/movies/msft.bik (640x480, fps=30.00, audio=1)"),
    (3.60, "[VIDEO v1] event=pre_video site=campaign_start name=none"),
    (4.00, "[MISSION] t=  0.00s  phase=mission_load_start"),
    (11.50, "[MISSION] t=  7.31s  phase=terrain_ready"),
    (12.10, "[FIRST60 v1] n=60 avg_ms=8.10 p50_ms=7.90 p99_ms=15.20 max_ms=22.00 first_ms=9.00 load_frame_ms=7300.00"),
]


def test_parse_stamped_reads_menu_load_and_first60():
    st = via_menu.parse_stamped(LOG)
    assert st.time_to_menu_s == 3.25
    assert st.mission_load_ms == 7310.0
    assert st.time_to_first60_s == 12.1
    assert st.first60 == {"n": 60, "avg": 8.1, "p50": 7.9, "p99": 15.2, "max": 22.0, "first": 9.0}
    assert st.load_frame_ms == 7300.0 and st.time_to_campaign_start_s == 3.6
    assert st.videos_opened == ["A:/x/data/movies/msft.bik"] and st.mission_load_wall_ms is None
    assert st.video_skips == 0 and st.complete() and not st.crashed


def test_parse_stamped_incomplete_without_first60_and_flags_crash():
    st = via_menu.parse_stamped(LOG[:3] + [(5.0, "MC2 Remastered Crash Report")])
    assert not st.complete() and st.crashed


def test_cold_cache_verdict_needs_a_slowdown(tmp_path, monkeypatch):
    f = tmp_path / "big.fst"
    f.write_bytes(b"\0" * (9 * 1024 * 1024))
    reads = iter([(9, 0.001), (9, 0.001), (9, 0.010)])          # prime, hot, after-flush: 10x slower = really flushed
    monkeypatch.setattr(cold_cache, "_read_all", lambda p, chunk=0: next(reads))
    monkeypatch.setattr(cold_cache, "purge_standby_list", lambda: (True, "standby list purged"))
    r = cold_cache.make_cold(tmp_path)
    assert r.ok and r.method == "purge_standby_list"
    reads = iter([(9, 0.001), (9, 0.001), (9, 0.0011)])          # no slowdown: not claimed cold
    monkeypatch.setattr(cold_cache, "_read_all", lambda p, chunk=0: next(reads))
    r = cold_cache.make_cold(tmp_path)
    assert not r.ok and "not flushed" in r.note


@pytest.mark.skipif(sys.platform != "win32", reason="Windows administrator standby-cache purge")
def test_cold_cache_without_admin_reports_it(tmp_path, monkeypatch):
    f = tmp_path / "big.fst"
    f.write_bytes(b"\0" * (9 * 1024 * 1024))
    monkeypatch.setattr(cold_cache, "is_admin", lambda: False)
    r = cold_cache.make_cold(tmp_path, evict_roots=[])          # no eviction roots: nothing to read
    assert r.method == "none" and "admin" in r.note


def test_tracy_via_menu_env_has_no_smoke_mode():
    import tracy_capture_compare as capture
    env = capture.game_env("vk", True, None, None, [], via_menu_boot=True)
    assert "MC2_SMOKE_MODE" not in env and env["MC2_BOOT_TO_BAY"] == "campaign"
    assert capture.game_env("vk", True, None, None, [])["MC2_SMOKE_MODE"] == "1"
