"""Numerical proof for the temporary shimmer measurements."""
import numpy as np
import temporal_ab as ab


def test_static_and_alternating_luminance():
    black=np.zeros((4,5,3),dtype=np.uint8)
    white=np.full_like(black,255)
    assert ab.metrics([black,black,black]) == {'linear_luma_variance':0.0,'uncompensated_mad':0.0}
    result=ab.metrics([black,white,black,white])
    assert abs(result['linear_luma_variance']-.25)<1e-6
    assert abs(result['uncompensated_mad']-1)<1e-6


def test_orbit_track_and_mode_defaults():
    for mission in ab.MISSIONS:
        poses=[p for p in ab.track(mission)['bookmarks'] if p['name'].startswith('orbit_')]
        assert len(poses)==301
        assert poses[0]['rot']==poses[-1]['rot']
        assert all(p['proj']==20 and p['alt']==400 for p in poses)
