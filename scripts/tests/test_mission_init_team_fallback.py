"""Regression guard (mc10 load crash): Mission::init must never leave a commander
named by commandersToLoad without a team, and callers of Commander::team must
tolerate null (logistics identity table names team == commander id)."""
import pathlib, re
ROOT = pathlib.Path(__file__).resolve().parents[2]

def _src(p):
    return (ROOT / p).read_text(encoding="utf-8", errors="replace")

def test_init_falls_back_to_home_team():
    s = _src("code/mission_init.cpp")
    i = s.index("falling back to home team")
    assert "setTeam(Team::home)" in s[i:i + 900]

def test_callers_null_check_team():
    assert re.search(r"getTeam\(\) \?", _src("code/artlry.cpp"))
    assert re.search(r"getTeam\(\) \?", _src("code/missiongui_style.cpp"))
    assert re.search(r"->team \?", _src("code/missiongui_support.cpp"))
