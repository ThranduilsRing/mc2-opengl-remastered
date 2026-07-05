#!/usr/bin/env python3
"""CLIFF-SMOOTH-1 unit tests (pure functions, no .pak needed).

Synthetic oracle: a giant TERRACED staircase — hard 100wu steps with flat
treads, exactly the mc2_17-class cliff the user screenshot shows. The base
--reauth recipe cannot fix it: max-drift 24 << step size and every terrace
edge is a pinned coarse extremum. CLIFF-SMOOTH-1 must melt the steps into a
continuous face while flats keep the scalar drift bound and TRUE summits stay
pinned.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from visual_heightfield import (  # noqa: E402
    upsample_corner_pinned, reauth_visual, detect_coarse_extrema,
    coarse_cliff_mask, cliff_weight_fine, striation_field, _crease_energy,
)

FACTOR = 4


def synth_terraced_cliff(side: int = 64, step_h: float = 100.0,
                         tread: int = 3, n_steps: int = 6,
                         start: int = 12):
    """Coarse heightfield: flat plain, then a staircase rising along +x in
    hard `step_h` steps every `tread` cells, then a summit plateau."""
    c = np.arange(side)
    terrace = np.clip((c - start) // tread, 0, n_steps).astype(np.float64) * step_h
    elev = np.tile(terrace, (side, 1))
    return elev


def _profile_second_diff_energy(prof: np.ndarray) -> float:
    s = prof[2:] - 2 * prof[1:-1] + prof[:-2]
    return float((s * s).mean())


# --- cliff mask ---------------------------------------------------------------

def test_cliff_mask_sees_single_cell_risers():
    """Central differences halve a 1-cell riser's slope (100wu -> 21deg); the
    one-sided mask must still catch the 38deg step."""
    elev = synth_terraced_cliff()
    m = coarse_cliff_mask(elev, cliff_slope_deg=30.0, dilate_iters=0)
    assert m.any(), "one-sided cliff mask missed 100wu risers"
    # risers live between start and start + tread*n_steps
    assert m[:, 11:31].any()
    # the far plain is not cliff
    assert not m[:, 40:].any()


def test_cliff_mask_excludes_water():
    elev = synth_terraced_cliff()
    water = np.zeros_like(elev, dtype=bool)
    water[:, :13] = True   # water lapping the cliff base
    m = coarse_cliff_mask(elev, water_coarse=water, cliff_slope_deg=30.0)
    assert not (m & water).any(), "dilated cliff ring leaked onto water"


def test_cliff_weight_feathered():
    elev = synth_terraced_cliff()
    m = coarse_cliff_mask(elev, cliff_slope_deg=30.0)
    w = cliff_weight_fine(m, FACTOR)
    assert w.min() >= 0.0 and w.max() <= 1.0
    # core is exactly 1, far plain is ~0, and values in between exist (feather)
    assert (w == 1.0).any()
    assert w[:, -8:].max() < 1e-6
    assert ((w > 0.05) & (w < 0.95)).any()


# --- melting ------------------------------------------------------------------

@pytest.fixture(scope="module")
def melted():
    elev = synth_terraced_cliff()
    protect = np.zeros_like(elev, dtype=bool)
    flat, _ = reauth_visual(elev, FACTOR, protect, max_drift=24.0, passes=150)
    work, info = reauth_visual(elev, FACTOR, protect, max_drift=24.0,
                               passes=150, cliff_drift=112.0,
                               cliff_slope_deg=30.0, cliff_melt_passes=300)
    return elev, flat, work, info


def test_steps_melt_into_continuous_face(melted):
    """The deliverable: staircase profile -> smooth curve. Second-difference
    energy along a cross-section through the face must collapse vs the
    cliff_drift=0 bake."""
    elev, flat, work, _info = melted
    r = elev.shape[0] // 2
    lo, hi = 10 * FACTOR, 32 * FACTOR       # the face span, fine indices
    before = _profile_second_diff_energy(flat[r * FACTOR, lo:hi])
    after = _profile_second_diff_energy(work[r * FACTOR, lo:hi])
    assert after < 0.35 * before, \
        f"terrace steps survived the melt: {before:.3f} -> {after:.3f}"


def test_melt_exceeds_scalar_drift_on_cliff(melted):
    """Proof the drift FIELD opened: on the face, |work - bilinear| must
    exceed the scalar 24wu bound (impossible pre-slice)."""
    elev, _flat, work, info = melted
    base = upsample_corner_pinned(elev, FACTOR)
    drift = np.abs(work - base)
    assert drift.max() > 24.0 * 1.25, "cliff drift cap never opened"
    assert drift.max() <= 112.0 * 1.25 + 1e-6, "cliff drift exceeded its own cap"
    assert info["effective_max_drift_wu"] == 112.0


def test_flats_keep_scalar_bound(melted):
    """Away from the cliff (weight ~ 0) the old 24wu contract still holds."""
    elev, _flat, work, _info = melted
    base = upsample_corner_pinned(elev, FACTOR)
    drift = np.abs(work - base)
    far = np.zeros_like(drift, dtype=bool)
    far[:, 40 * FACTOR:] = True             # plain, far from the face+feather
    assert drift[far].max() <= 24.0 * 1.25 + 1e-6


def test_summit_pinned_terraces_relaxed(melted):
    """Peaks still pinned: the summit-plateau extremum keeps its height; the
    terrace-step extrema (non-regional, on the cliff mask) are relaxed."""
    elev, _flat, work, info = melted
    assert info["extrema"]["relaxed_on_cliff"] > 0, "no terrace extrema relaxed"
    assert info["extrema"]["violations"] == 0
    # summit plateau is flat & regional -> its representative must be enforced
    ex = detect_coarse_extrema(elev)
    regional_peaks = [e for e in ex if e["kind"] == "peak" and e["is_regional"]]
    assert regional_peaks, "synthetic summit not detected as regional"
    for e in regional_peaks:
        mv = abs(float(work[e["r"] * FACTOR, e["c"] * FACTOR]) - e["h0"])
        assert mv <= max(0.5, 0.10 * e["relief"]) + 1e-6, \
            "regional summit melted — peaks must stay pinned"


def test_cliff_deterministic():
    elev = synth_terraced_cliff()
    protect = np.zeros_like(elev, dtype=bool)
    a, _ = reauth_visual(elev, FACTOR, protect, passes=30, cliff_drift=112.0,
                         cliff_melt_passes=60)
    b, _ = reauth_visual(elev, FACTOR, protect, passes=30, cliff_drift=112.0,
                         cliff_melt_passes=60)
    assert np.array_equal(a, b)


def test_cliff_noop_without_cliffs():
    """cliff_drift on a cliff-free map must be byte-identical to cliff off."""
    r = np.arange(48, dtype=np.float64)
    elev = 2.0 * r[None, :] + 0.0 * r[:, None]     # gentle 0.9deg ramp
    protect = np.zeros_like(elev, dtype=bool)
    off, io = reauth_visual(elev, FACTOR, protect, passes=30)
    on, i1 = reauth_visual(elev, FACTOR, protect, passes=30, cliff_drift=112.0)
    assert np.array_equal(off, on)
    assert i1["cliff"]["coarse_cliff_cells"] == 0
    assert i1["effective_max_drift_wu"] == 24.0


def test_protect_pin_survives_melt():
    """Roads/footprints stay ON the bilinear baseline even inside the mask."""
    elev = synth_terraced_cliff()
    protect = np.zeros_like(elev, dtype=bool)
    protect[30:34, 14:26] = True            # a road crossing the face
    work, _ = reauth_visual(elev, FACTOR, protect, passes=60,
                            cliff_drift=112.0, cliff_melt_passes=120)
    base = upsample_corner_pinned(elev, FACTOR)
    from visual_heightfield import _fine_mask
    pin = _fine_mask(protect, FACTOR)
    assert np.abs(work[pin] - base[pin]).max() < 1e-9


# --- striation (cliff-ness) ---------------------------------------------------

def test_striation_is_gradient_aligned():
    """On a uniform steep ramp (gradient along +x) the striation must vary
    LESS along the fall line (x) than across it (y): erosion runs DOWN the
    face, i.e. features elongate downslope."""
    V = 197
    x = np.arange(V, dtype=np.float64)
    surface = np.tile(25.0 * x, (V, 1))     # slope purely along +x
    s = striation_field(surface, seed=1337)
    inner = s[8:-8, 8:-8]
    gy, gx = np.gradient(inner)
    e_down = float((gx * gx).mean())        # along gradient (fall line)
    e_across = float((gy * gy).mean())      # along contours
    assert e_across > 2.0 * e_down, \
        f"striation not elongated downslope: across={e_across:.4f} down={e_down:.4f}"


def test_striation_biases_mountainify_on_cliff():
    """Under mountainify, cliff detail must differ from the isotropic bake and
    stay deterministic + water-clean."""
    elev = synth_terraced_cliff(side=64, step_h=100.0, tread=3)
    protect = np.zeros_like(elev, dtype=bool)
    iso, _ = reauth_visual(elev, FACTOR, protect, passes=60,
                           mountainify_amp=14.0, seed=1337)
    cl, info = reauth_visual(elev, FACTOR, protect, passes=60,
                             mountainify_amp=14.0, seed=1337,
                             cliff_drift=112.0, cliff_melt_passes=120)
    cl2, _ = reauth_visual(elev, FACTOR, protect, passes=60,
                           mountainify_amp=14.0, seed=1337,
                           cliff_drift=112.0, cliff_melt_passes=120)
    assert np.array_equal(cl, cl2)
    assert not np.array_equal(iso, cl)
    mi = info["mountainify"]
    assert mi["striation_rms_on_cliff_wu"] is not None
    assert mi["striation_rms_on_cliff_wu"] > 0.3


def test_landform_correlation_held():
    """KEEP THE SHAPE globally: melting a terraced face must not destroy the
    coarse landform correlation."""
    elev = synth_terraced_cliff()
    protect = np.zeros_like(elev, dtype=bool)
    _, info = reauth_visual(elev, FACTOR, protect, passes=150,
                            cliff_drift=112.0, cliff_melt_passes=300)
    assert info["landform_correlation"] >= 0.99


def test_crease_energy_metric_regression():
    flat = np.zeros((32, 32))
    assert _crease_energy(flat) == 0.0
