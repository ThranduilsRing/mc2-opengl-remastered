"""Mover shadows stay crisp away from the contact point (owner 2026-11-09: "This shadow should stay crisper for longer
distance wise"). Static checks on the mover receiver: no short-blocker lift, no distance fade, and a mover sun cone
narrower than the static bake's, so the penumbra grows with blocker distance (contact hardening) but slowly."""
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADER = ROOT / "shaders/vulkan/include/mover_shadow.glsl"
FIT = ROOT / "GameOS/gameos/shadow_mover_fit.h"
BAKE = ROOT / "GameOS/gameos/shadow_bake_math.h"
STATE = ROOT / "GameOS/gameos/vulkan_mover_shadow.cpp"


def _constant(path, name):
    m = re.search(rf"constexpr\s+float\s+{name}\s*=\s*([0-9.]+)f", path.read_text(encoding="utf-8"))
    assert m, f"{name} not found in {path.name}"
    return float(m.group(1))


def _code(path):
    return "\n".join(line.split("//", 1)[0] for line in path.read_text(encoding="utf-8").splitlines())


def test_mover_sun_cone_is_narrower_than_the_bake():
    mover = _constant(FIT, "kMoverSunAngularRadiusDeg")
    assert 0.0 < mover <= 0.75 < _constant(BAKE, "kSunAngularRadiusDeg")
    assert re.search(r"sun\[3\]\s*=\s*std::tan\(\s*moverSunAngularRadiusDeg\(", _code(STATE))


def test_mover_receiver_has_no_short_blocker_lift_or_distance_fade():
    code = _code(SHADER)
    assert "lift" not in code, "the short-blocker lift softens mech-height casters up to 3x"
    assert "480.0" not in code and "fadeMax" not in code, "the distance fade washes the shadow out away from the feet"
    assert re.search(r"float radius = max\(meanD \* tanT,", code), "penumbra must stay meanD * tanT (contact hardening)"


def test_penumbra_model_is_crisper_but_still_contact_hardening():
    old_tan = math.tan(math.radians(_constant(BAKE, "kSunAngularRadiusDeg")))
    new_tan = math.tan(math.radians(_constant(FIT, "kMoverSunAngularRadiusDeg")))
    old = lambda d: d * old_tan * (1.0 + 2.0 * (1.0 - min(max(d / 200.0, 0.0), 1.0)))
    new = lambda d: d * new_tan
    for d in (25.0, 50.0, 100.0, 200.0):
        assert new(d) <= old(d) / 2.0
    assert new(25.0) < new(100.0) < new(400.0)


def test_rigid_casters_do_not_depend_on_nearby_mechs_or_bake_readiness():
    # Turrets and demoted owners are absent from the static bake. Their individual
    # leaves must survive even when no mech receives their shadow.
    code = _code(STATE)
    loop = code.split("for (const auto& prop:props) {", 1)[1].split("appendMechs(", 1)[0]
    assert "sweptShadowInView(sphere,sun,ground_,view)" in loop
    assert "props_.push_back(prop)" in loop
    assert "maskReady" not in loop, "bake readiness cannot suppress unbaked rigid casters"
    assert "propShadowReachesMover" not in loop, "a nearby mech cannot control a turret's shadow"


def test_coverage_grid_literals_follow_kMoverCoverageSide():
    side = int(re.search(r"constexpr\s+int\s+kMoverCoverageSide\s*=\s*(\d+)", FIT.read_text(encoding="utf-8")).group(1))
    glsl = _code(SHADER)
    assert f"uvec4 coverage[{side * side // 16}]" in glsl
    assert f"pc.xy * {side}.0" in glsl and f"uvec2({side - 1})" in glsl and f"* {side}u" in glsl
    live = (ROOT / "GameOS/gameos/vulkan_shadow_island_live.cpp").read_text(encoding="utf-8")
    kb = int(re.search(r"coverage_kb=(\d+)", live).group(1))
    assert kb == round(side * side / 1024), "the movers log line's coverage_kb is stale"
