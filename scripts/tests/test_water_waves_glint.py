"""WATER-WAVES-GLINT: the wave trains in water_geom.frag read at RTS zoom, and the specular AA conserves slope variance.

The shader moves each wave train's slope variance (slope^2 / 2) from the normal into the GGX roughness as the train
falls below the pixel footprint. Resolved weight r goes to the normal as r * slope (variance r^2 * slope^2 / 2) and
(1 - r^2) * slope^2 / 2 goes to roughness, so the total is the same at every zoom: distant water gets a wide highlight
instead of sparkle or a highlight that averages away.
"""
from pathlib import Path
import math
import re
import pytest

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


def _split(footprint, packets=None):
    resolved = unresolved = 0.0
    for i, (_, _, wavelength, slope) in enumerate(TRAINS):
        slope *= 1.0 if packets is None else packets[i]
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
    # WATER-POOL-FUSE-1 (bb3967a57): the lamp specular comes out of the fused pool light walk, still on the wave normal.
    assert "waterPoolLights(WorldPos, waveN, vdir, specAlpha2, lampSpec)" in SRC
    # The reflection normal is the wave normal plus the actor ripple/wake slope (water-reactive-a), never a noise gradient.
    assert "reflect(-vdir, waveNormal)" in SRC and "PREC vec3  waveNormal = normalize(waveN + vec3(reactive.xy, 0.0));" in SRC
    # Spec AA comes before the coverage discard (derivatives defined) and is not temporal.
    assert SRC.index("float kernelVar") < SRC.index("shore <= 0.0) discard;")


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


def test_coarse_mesh_does_not_turn_random_water_bits_into_triangle_slopes():
    vs = (ROOT / "shaders/vulkan/water_geom.vert").read_text(encoding="utf-8")
    assert "waveCos" not in vs and "unpackByte(rec.ctrl.w" not in vs
    assert re.search(r"float wz = \(shoreFlat \|\| WaterThickness >= 0.0\)\s*\? waterElevation", vs)
    # Stock +/-10-wu corner offsets on a 128-wu cell can change slope by 20/128
    # at a diagonal. Even the worst coherent swell corner difference is <1/10 of that.
    heights = [float(m[1]) for m in re.findall(
        r"vec4\([-0-9.]+,\s*[-0-9.]+,\s*([0-9.]+),\s*([0-9.]+)\)",
        vs[vs.index("kSwell[SWELL_TRAINS]"):vs.index("SWELL_G =")])]
    assert 2.0 * sum(heights) / 128.0 < 0.015


def _packet_noise(x, y):
    def fract(v):
        return v - math.floor(v)

    def h21(x, y):
        q = [fract(x * .1031), fract(y * .1031), fract(x * .1031)]
        d = sum(q[i] * (q[(i + 1) % 3] + 33.33) for i in range(3))
        q = [v + d for v in q]
        return fract((q[0] + q[1]) * q[2])

    ix, iy = math.floor(x), math.floor(y)
    fx, fy = _smoothstep(0, 1, fract(x)), _smoothstep(0, 1, fract(y))
    a = h21(ix, iy) * (1 - fx) + h21(ix + 1, iy) * fx
    b = h21(ix, iy + 1) * (1 - fx) + h21(ix + 1, iy + 1) * fx
    return a * (1 - fy) + b * fy


def test_stochastic_packets_are_bounded_continuous_and_conserve_local_variance():
    low, high, span = map(_const, ("WAVE_PACKET_MIN", "WAVE_PACKET_MAX", "WAVE_PACKET_SPAN"))
    assert 0.5 <= low < 1 < high <= 1.5 and span >= 6
    assert "float a = w.w * gain * packet * brkA;" in SRC and "float brkA = 1.0;" in SRC and "float packet = 1.0;" in SRC
    samples = []
    for x, y in ((0, 0), (128, 256), (-1234.7, 1904.4), (4401, -8123)):
        packets = [1.0 if i < 2 else low + (high - low) * _packet_noise(
            x / (span * w[2]) + i * 19.19, y / (span * w[2]) + i * 7.73)
            for i, w in enumerate(TRAINS)]
        samples.append(packets[2:])
        assert all(low <= p <= high for p in packets)
        total = sum(.5 * (w[3] * p) ** 2 for w, p in zip(TRAINS, packets))
        for footprint in (.5, 2, 8, 30, 60, 120, 400, 1e9):
            resolved, unresolved = _split(footprint, packets)
            assert abs(resolved + unresolved - total) < 1e-12
    assert max(samples[0]) - min(samples[0]) > .15
    assert samples[0] != samples[1]  # no repeating, globally uniform wave-train amplitudes
    for boundary in (-2, 0, 3):
        assert abs(_packet_noise(boundary - 1e-6, .37) - _packet_noise(boundary + 1e-6, .37)) < 1e-8


@pytest.mark.parametrize("map_span", [12800, 25600, 51200])
def test_baked_packet_parity(map_span):
    """Float32 analytic noise versus RGBA8 lattice hashes and smooth bilinear sampling.

    This is a numerical/mapping oracle, not a GPU compiler conformance test.
    Include map edges: clamp-to-edge must meet the same envelope error bound.
    """
    np = pytest.importorskip("numpy")
    f = np.float32
    bake = (ROOT / "shaders/vulkan/water_wave_packets.comp.slang").read_text()
    wavelengths = [float(v) for v in re.search(r"wavelengths\[10\] = \{([^}]+)", bake)[1].split(",")]
    assert wavelengths == [w[2] for w in TRAINS[2:]]
    assert "j = tid.z * 4 + c" in bake and "float(j + 2)" in bake

    def fract(v):
        return v - np.floor(v)

    def h(x, y):
        qx, qy = fract(x * f(.1031)), fract(y * f(.1031))
        d = (qx * (qy + f(33.33)) + qy * (qx + f(33.33))) + qx * (qx + f(33.33))
        a, b = qx + d, qy + d
        return fract((a + b) * a)

    def noise(x, y):
        ix, iy = np.floor(x), np.floor(y)
        fx, fy = fract(x), fract(y)
        fx, fy = fx * fx * (f(3) - f(2) * fx), fy * fy * (f(3) - f(2) * fy)
        a, b, c, d = h(ix, iy), h(ix + f(1), iy), h(ix, iy + f(1)), h(ix + f(1), iy + f(1))
        return (a * (f(1) - fx) + b * fx) * (f(1) - fy) + (c * (f(1) - fx) + d * fx) * fy

    span, half = f(map_span), f(map_span / 2)
    size = int(span / f(93)) + 3
    assert size * size * 12 <= 48 * 1024 * 1024
    rng = np.random.default_rng(431)
    grid = rng.uniform(0, span, (2, 100000)).astype(f)
    # Explicit corners and both outer rows/columns, in addition to interior samples.
    edge = np.linspace(0, span, 4097, dtype=f)
    grid = np.concatenate((grid, np.array([edge, edge * 0]), np.array([edge, edge * 0 + span]),
                           np.array([edge * 0, edge]), np.array([edge * 0 + span, edge])), axis=1)
    x, y = grid[0] - half, half - grid[1]
    errors = []
    for j, wavelength in enumerate(wavelengths):
        scale, ox, oy = f(6) * f(wavelength), f(j + 2) * f(19.19), f(j + 2) * f(7.73)
        analytic = noise(x / scale + ox, y / scale + oy)
        lattice = np.array([x / scale + ox, y / scale + oy])
        origin = np.floor(np.array([-half / scale + ox, -half / scale + oy]))
        cell, weight = np.floor(lattice), fract(lattice)
        weight = weight * weight * (f(3) - f(2) * weight)
        weight = np.rint(weight * f(256)) / f(256)
        assert np.all(cell >= origin[:, None])
        assert np.all(cell + 1 - origin[:, None] < size)
        texels = []
        for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            v = h(cell[0] + f(dx), cell[1] + f(dy))
            texels.append(np.rint(v * f(255)) / f(255))
        a, b, c, d = texels
        wx, wy = weight
        sampled = (a * (f(1) - wx) + b * wx) * (f(1) - wy) + (c * (f(1) - wx) + d * wx) * wy
        error = np.abs(analytic - sampled)
        errors.append(float(error.max()))
        print(f"rgba8 train={j + 2} samples={error.size} max={error.max():.9f} mean={error.mean():.9f}")
    assert max(errors) <= 2 / 255
