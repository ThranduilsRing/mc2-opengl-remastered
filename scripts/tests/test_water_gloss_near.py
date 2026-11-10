"""WATER-GLOSS-NEAR: close, steeply viewed water gets a reflectance floor and a tighter calm roughness by day.

Owner 2026-11-10 (mc2_01_saved_205644): "the near water should be a bit glossier ... generally the vibe is good".
Far water and night water must not change; the LEAN / specular-AA roughness terms stay in place.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / "shaders/vulkan/water_geom.frag").read_text(encoding="utf-8")


def _const(name):
    return float(re.search(r"const float\s+" + name + r"\s*=\s*([0-9.]+)", SRC)[1])


F0, FLOOR = _const("REFL_F0"), _const("NEAR_REFL_FLOOR")
START, END = _const("NEAR_GLOSS_START"), _const("NEAR_GLOSS_END")


def _smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def _fres(cos_v, dist, night_dim):
    """Mirror of the shader: Schlick, then max() with the day/near floor."""
    near = (1.0 - _smoothstep(START, END, dist)) * (1.0 - min(max(night_dim / 0.6, 0.0), 1.0))
    schlick = F0 + (1.0 - F0) * (1.0 - cos_v) ** 5
    return schlick, max(schlick, FLOOR * near)


def test_near_day_water_reflects_more():
    base, new = _fres(0.6, 3000.0, 0.0)  # gameplay camera looking down at the foreground sea
    assert base < 0.05 and new >= 2.0 * base
    assert FLOOR <= 0.2  # a sheen, not a mirror


def test_far_water_is_unchanged():
    for cos_v in (0.05, 0.2, 0.6, 0.9):
        base, new = _fres(cos_v, END + 1.0, 0.0)
        assert new == base
    # grazing water inside the near range keeps its (already higher) Schlick value
    base, new = _fres(0.05, 3000.0, 0.0)
    assert new == base


def test_night_water_is_unchanged():
    for night in (0.6, 0.9):
        for cos_v in (0.2, 0.6, 0.9):
            base, new = _fres(cos_v, 1000.0, night)
            assert new == base


def test_shader_wiring_keeps_lean_and_specular_aa():
    assert "(1.0 - clamp(F.f3.z / 0.6, 0.0, 1.0))" in SRC
    assert "fres = max(fres, NEAR_REFL_FLOOR * nearGloss);" in SRC
    assert "float alpha0 = mix(WATER_ALPHA0, NEAR_ALPHA0, nearGloss);" in SRC
    assert re.search(r"specAlpha2 = clamp\(alpha0 \* alpha0\s*\n\s*\+ 2\.0 \* \(waveVar", SRC)
    assert "min(2.0 * kernelVar, 0.18)" in SRC
    assert 0.0 < _const("NEAR_ALPHA0") <= _const("WATER_ALPHA0")
    assert SRC.index("float nearGloss") < SRC.index("if (shore <= 0.0) discard;")
