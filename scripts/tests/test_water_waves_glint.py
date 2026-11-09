"""WATER-WAVES-GLINT: the wave trains in water_geom.frag read at RTS zoom, and the specular AA conserves slope variance.

The shader moves each wave train's slope variance (slope^2 / 2) from the normal into the GGX roughness as the train
falls below the pixel footprint. Resolved weight r goes to the normal as r * slope (variance r^2 * slope^2 / 2) and
(1 - r^2) * slope^2 / 2 goes to roughness, so the total is the same at every zoom: distant water gets a wide highlight
instead of sparkle or a highlight that averages away.
"""
from pathlib import Path
import math
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / "shaders/vulkan/water_geom.frag").read_text(encoding="utf-8")


def _const(name):
    return float(re.search(r"const float\s+" + name + r"\s*=\s*([0-9.]+)", SRC)[1])


TRAINS = [tuple(float(v) for v in m.groups())
          for m in re.finditer(r"vec4\(([-0-9.]+),\s*([-0-9.]+),\s*([0-9.]+),\s*([0-9.]+)\)",
                               SRC[SRC.index("kWaveTrain[WAVE_TRAINS]"):SRC.index("WAVE_G ")])]
LO, HI = _const("WAVE_RESOLVE_LO"), _const("WAVE_RESOLVE_HI")


def _smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def _split(footprint):
    resolved = unresolved = 0.0
    for _, _, wavelength, slope in TRAINS:
        r = _smoothstep(LO, HI, wavelength / footprint)
        resolved += 0.5 * (r * slope) ** 2
        unresolved += 0.5 * slope * slope * (1.0 - r * r)
    return resolved, unresolved


def test_wave_trains_are_unit_directions_with_bounded_slope():
    assert len(TRAINS) == int(re.search(r"const int\s+WAVE_TRAINS\s*=\s*(\d+)", SRC)[1]) == 4
    for dx, dy, wavelength, slope in TRAINS:
        assert abs(math.hypot(dx, dy) - 1.0) < 2e-3
        assert 100.0 <= wavelength <= 1000.0 and 0.0 < slope <= 0.15
    rms = math.sqrt(sum(0.5 * s * s for *_, s in TRAINS))
    assert 0.08 <= rms <= 0.2  # calm-to-breezy water (Cox-Munk range), crests catch the sun without tilting the sea


def test_slope_variance_is_conserved_across_zoom():
    total = sum(0.5 * s * s for *_, s in TRAINS)
    for footprint in (0.5, 2.0, 8.0, 30.0, 60.0, 120.0, 400.0, 1e9):
        resolved, unresolved = _split(footprint)
        assert abs(resolved + unresolved - total) < 1e-12


def test_waves_resolve_at_gameplay_zoom_and_become_roughness_far_away():
    resolved, unresolved = _split(4.0)      # ~4 wu per pixel: gameplay zoom
    assert unresolved < 1e-9 and resolved > 0.0
    resolved, unresolved = _split(1e9)      # Low/Potato path and the far horizon: roughness only
    assert resolved == 0.0 and unresolved > 0.0


def test_sun_and_lamp_specular_use_the_wave_normal_and_roughness():
    assert "waterGgxSpec(waveN, vdir, Ls, specAlpha2)" in SRC
    assert "waterLampSpec(WorldPos, waveN, vdir, specAlpha2)" in SRC
    assert "reflect(-vdir, waveNormal)" in SRC and "PREC vec3  waveNormal = waveN;" in SRC
    # Spec AA comes before the coverage discard (derivatives defined) and is not temporal.
    assert SRC.index("float kernelVar") < SRC.index("if (shore <= 0.0) discard;")


def test_ripples_are_wind_aligned_world_space_trains_not_noise():
    assert "waterRippleNoise" not in SRC and "fbm23" not in SRC and "nzGrad" not in SRC
    assert "normalize(vec3(-waveSlope, 1.0))" in SRC
    # Crests stay parallel: every train within 10 degrees of the mean direction.
    mx, my = (sum(t[i] for t in TRAINS) / len(TRAINS) for i in (0, 1))
    mean = math.atan2(my, mx)
    for dx, dy, *_ in TRAINS:
        assert abs(math.degrees(math.atan2(dy, dx) - mean)) < 10.0
