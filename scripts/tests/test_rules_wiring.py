"""Game-rules wiring: every sim consumer of mc2::rules::current() still reads its rule, and the harness refuses non-default rules.

The C++ helper tests (tests/unit/test_game_rules.cpp) re-derive the helpers and cannot see a removed read. Each row here pins one consumer
site by source pattern; the mutation test deletes the read from the real text and requires the same pattern to FAIL, so a regex that
would pass on a stripped site is itself caught.
"""
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools"))

# (file, pattern that must match, the part of the match that is the rules read and is removed by the mutation)
SITES = [
    ("code/mover_attack.cpp", r"called_shot::mode\([^;]*mc2::rules::current\(\)\.calledShots\);\s*aimLocation = called_shot::aim\(aimLocation, calledMode\)","mc2::rules::current().calledShots"),
    ("code/mover_attack.cpp", r"weightSkill\([^;]*mc2::rules::current\(\)\.pilotSkillWeight\)", "mc2::rules::current().pilotSkillWeight"),
    ("code/mech_weapons.cpp", r"called_shot::mode\([^;]*mc2::rules::current\(\)\.calledShots\);\s*aimLocation = called_shot::aim\(aimLocation, calledMode\)","mc2::rules::current().calledShots"),
    ("code/gvehicl_weapons.cpp", r"called_shot::mode\([^;]*mc2::rules::current\(\)\.calledShots\);\s*aimLocation = called_shot::aim\(aimLocation, calledMode\)","mc2::rules::current().calledShots"),
    ("code/turret.cpp", r"weightSkill\([^;]*mc2::rules::current\(\)\.pilotSkillWeight\)", "mc2::rules::current().pilotSkillWeight"),
    ("code/logisticsdata_save.cpp", r"payoutPct\s*=\s*mc2::rules::current\(\)\.cbillMultiplier;", "mc2::rules::current().cbillMultiplier"),
    ("code/logisticsdata_save.cpp", r"payoutCredited\s*=\s*mc2::rules::payoutScaled\(payoutBase,\s*payoutPct\);\s*[^\n]*\s*addCBills\(\s*payoutCredited\s*\)",
     "payoutScaled(payoutBase, payoutPct)"),
    ("code/salvagemecharea.cpp", r"costToSalvage\s*=\s*mc2::rules::salvagePriceFactor\(\s*mc2::rules::current\(\)\.salvage\s*\)", "mc2::rules::current().salvage"),
    ("code/mover_combat.cpp", r"aimLocation != -1\s*&&\s*called_shot::mode\([^;]*mc2::rules::current\(\)\.calledShots\)\s*!=\s*called_shot::kOff\)\s*\{\s*message = RADIO_CALLED_SHOT",
     "mc2::rules::current().calledShots"),
    ("code/missiongui_orders.cpp", r"calledShotsOff\(\)\s*\{\s*return called_shot::mode\([^;]*mc2::rules::current\(\)\.calledShots\)\s*==\s*called_shot::kOff",
     "mc2::rules::current().calledShots"),
    ("code/logisticsdata.cpp", r"\[GAME_RULES\] snapshot[^;]*salvage=\{\}\s*cbillMultiplier=\{\}[^;]*published\.salvage,\s*published\.cbillMultiplier", "published.cbillMultiplier"),
    ("code/prefs_io.cpp", r"mc2::options::cfg::get\(sourceText_,\s*current\[i\]\.key\)\.has_value\(\)", "mc2::options::cfg::get(sourceText_, current[i].key)"),
    ("code/warrior_combat.cpp", r"s_aimedFireFixture\s*&&\s*!MPlayer\s*&&\s*!mc2::net::active\(\)", "!MPlayer && "),
]
for _leg in ("Leg", "Arm", "Head"):
    SITES.append(("code/missiongui_orders.cpp", r"int MissionInterfaceManager::aim%s\(\)\s*\{\s*if \( calledShotsOff\(\) \)\s*\{\s*userInput->setMouseCursor\( mState_DONT \);\s*return 1;"
                  % _leg, "calledShotsOff()"))


def _text(rel):
    return (ROOT / rel).read_text(encoding="utf-8-sig", errors="replace")


@pytest.mark.parametrize("rel,pat,read", SITES, ids=[f"{s[0]}#{i}" for i, s in enumerate(SITES)])
def test_site_reads_its_rule(rel, pat, read):
    assert re.search(pat, _text(rel), re.S), f"{rel}: the rules read is gone or changed: {read}"


@pytest.mark.parametrize("rel,pat,read", SITES, ids=[f"{s[0]}#{i}" for i, s in enumerate(SITES)])
def test_mutation_removing_the_read_is_caught(rel, pat, read):
    text = _text(rel)
    m = re.search(pat, text, re.S)
    assert m, rel
    hit = m.group(0)
    assert read in hit, f"{rel}: pattern does not cover {read}"
    stripped = text[:m.start()] + hit.replace(read, "0", 1) + text[m.end():]
    assert not re.search(pat, stripped, re.S), f"{rel}: removing {read} still satisfies the pattern"


def test_runtime_rules_snapshot_is_the_only_publish_path_in_logisticsdata():
    assert "mc2::rules::rulesToPublish( networked, rules() )" in _text("code/logisticsdata.cpp")


# ---- harness refusal ----
import run_smoke  # noqa: E402
import sim_determinism as sim  # noqa: E402

DEFAULT = "[GAME_RULES] snapshot networked=false calledShots=1 pilotSkillWeight=100 salvage=1 cbillMultiplier=100"


def test_default_snapshot_is_accepted():
    assert run_smoke._nondefault_rules("noise\n" + DEFAULT + "\n") == []
    assert run_smoke._nondefault_rules("no snapshot at all") == []


@pytest.mark.parametrize("field,value", [("calledShots=1", "calledShots=0"), ("pilotSkillWeight=100", "pilotSkillWeight=150"),
                                         ("salvage=1", "salvage=2"), ("cbillMultiplier=100", "cbillMultiplier=75")])
def test_each_non_default_field_is_refused(field, value):
    bad = run_smoke._nondefault_rules(DEFAULT.replace(field, value))
    assert len(bad) == 1 and value in bad[0]


def test_defaults_match_the_engine_struct():
    h = _text("code/game_rules.h")
    for pat in (r"calledShots\s*=\s*kCalledFloors", r"kCalledFloors = 1", r"pilotSkillWeight\s*=\s*100", r"salvage\s*=\s*kSalvageMC2",
                r"kSalvageMC2 = 1", r"cbillMultiplier\s*=\s*100"):
        assert re.search(pat, h), pat


def test_sim_determinism_opt_in_sets_the_runner_env(monkeypatch):
    owner = "A:/Games/mc2-opengl/mc2-win64-v0.4"
    monkeypatch.delenv("MC2_SMOKE_ALLOW_NONDEFAULT_RULES", raising=False)
    assert sim.main(["check", "mc2_03", "--install", owner]) == sim.RC_OWNER_INSTALL
    assert "MC2_SMOKE_ALLOW_NONDEFAULT_RULES" not in __import__("os").environ
    assert sim.main(["check", "mc2_03", "--install", owner, "--allow-nondefault-rules"]) == sim.RC_OWNER_INSTALL
    assert __import__("os").environ["MC2_SMOKE_ALLOW_NONDEFAULT_RULES"] == "1"
    __import__("os").environ.pop("MC2_SMOKE_ALLOW_NONDEFAULT_RULES", None)  # sim.main set it directly; monkeypatch would restore "1"
