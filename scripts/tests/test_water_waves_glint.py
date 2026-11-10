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


def test_wave_spectrum_is_short_crested_spread_and_bounded():
    n = int(re.search(r"const int\s+WAVE_TRAINS\s*=\s*(\d+)", SRC)[1])
    assert len(TRAINS) == n and 8 <= n <= 12
    for dx, dy, wavelength, slope in TRAINS:
        assert abs(math.hypot(dx, dy) - 1.0) < 2e-3
        assert 10.0 <= wavelength <= 700.0 and 0.0 < slope <= 0.06
    assert sum(1 for t in TRAINS if t[2] > 100.0) <= 2         # at most two long swells
    assert all(t[3] <= 0.035 for t in TRAINS if t[2] > 100.0)  # at low amplitude
    short = [t for t in TRAINS if t[2] <= 100.0]
    assert len(short) >= 8 and all(14.0 <= t[2] <= 70.0 for t in short)
    angles = [math.degrees(math.atan2(t[1], t[0])) for t in TRAINS]
    wind = sum(angles) / len(angles)
    assert all(abs(a - wind) <= 50.0 for a in angles) and max(angles) - min(angles) >= 60.0
    rms = math.sqrt(sum(0.5 * s * s for *_, s in TRAINS))
    assert 0.08 <= rms <= 0.2  # calm-to-breezy water (Cox-Munk range)


def test_slope_variance_is_conserved_across_zoom():
    total = sum(0.5 * s * s for *_, s in TRAINS)
    for footprint in (0.5, 2.0, 8.0, 30.0, 60.0, 120.0, 400.0, 1e9):
        resolved, unresolved = _split(footprint)
        assert abs(resolved + unresolved - total) < 1e-12


def test_waves_resolve_at_gameplay_zoom_and_become_roughness_far_away():
    resolved, unresolved = _split(2.0)      # ~2 wu per pixel: close gameplay zoom, every component (>= 14 wu) resolved
    assert unresolved < 1e-9 and resolved > 0.0
    resolved, unresolved = _split(1e9)      # Low/Potato path and the far horizon: roughness only
    assert resolved == 0.0 and unresolved > 0.0


def test_sun_and_lamp_specular_use_the_wave_normal_and_roughness():
    assert "waterGgxSpec(waveN, vdir, Ls, specAlpha2)" in SRC
    assert "waterLampSpec(WorldPos, waveN, vdir, specAlpha2)" in SRC
    # The reflection normal is the wave normal plus the actor ripple/wake slope (water-reactive-a), never a noise gradient.
    assert "reflect(-vdir, waveNormal)" in SRC and "PREC vec3  waveNormal = normalize(waveN + vec3(reactive.xy, 0.0));" in SRC
    # Spec AA comes before the coverage discard (derivatives defined) and is not temporal.
    assert SRC.index("float kernelVar") < SRC.index("if (shore <= 0.0) discard;")


def test_ripples_are_wave_spectrum_without_crest_brightening():
    assert "waterRippleNoise" not in SRC and "fbm23" not in SRC and "nzGrad" not in SRC
    assert "normalize(vec3(-waveSlope, 1.0))" in SRC
    assert "waveCrest" not in SRC and "glint * GLINT_GAIN" not in SRC and "col *= 1.0 + RIPPLE_GAIN" not in SRC
    assert "float(i) * 2.39996" in SRC


def test_vertex_swell_matches_long_trains_and_is_tiny_and_shore_faded():
    vs = (ROOT / "shaders/vulkan/water_geom.vert").read_text(encoding="utf-8")
    sw = [tuple(float(v) for v in m.groups())
          for m in re.finditer(r"vec4\(([-0-9.]+),\s*([-0-9.]+),\s*([0-9.]+),\s*([0-9.]+)\)",
                               vs[vs.index("kSwell[SWELL_TRAINS]"):vs.index("SWELL_G =")])]
    assert len(sw) == 2
    for s, t in zip(sw, TRAINS[:2]):
        assert s[:3] == t[:3]                      # same direction and wavelength as the fragment swells
        assert s[2] >= 2.5 * 128.0 * 0.9           # the one-cell mesh can carry it
    assert 0.3 <= sum(s[3] for s in sw) <= 1.0     # tiny, in world units
    assert re.search(r"SWELL_G = 30\.0", vs) and _const("WAVE_G") == 30.0
    assert "smoothstep(8.0, 40.0, WaterThickness)" in vs  # zero in shallow water: no shore z-fight
