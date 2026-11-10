"""Geometry and held correspondence checks for the Carver V globe cook."""
import json
import re

import numpy as np
import pytest
from PIL import Image
from scipy.spatial.transform import Rotation

from tools import campaign_globe as globe
from tools.campaign_globe import ALIASES, NAMES, ROOT, SOURCE, lift, project


@pytest.mark.parametrize('ratio', [0., .3, .65])
def test_perspective_sphere_projection_roundtrip(ratio):
    geometry = [128., 116., 104., ratio]
    xy = np.array([[128, 116], [190, 153], [45, 91], [128, 14]])
    vectors = lift(xy, geometry)
    np.testing.assert_allclose(np.linalg.norm(vectors, axis=-1), 1, atol=1e-12)
    np.testing.assert_allclose(project(vectors, geometry), xy, atol=1e-10)


def test_registration_closes_every_view_without_reusing_alias_votes():
    registration = json.loads((SOURCE / 'registration.json').read_text())
    geometry = registration['geometry']
    poses = {name: Rotation.from_matrix(matrix) for name, matrix in registration['poses'].items()}
    assert set(poses) | set(ALIASES) == set(NAMES)
    graph = {name: set() for name in poses}
    for edge in registration['landmarks']:
        a, b = edge['a'], edge['b']
        graph[a].add(b)
        graph[b].add(a)
        relative = poses[b].inv() * poses[a]
        projected = project(relative.apply(lift(edge['xy_a'], geometry)), geometry)
        error = np.linalg.norm(projected - edge['xy_b'], axis=1)
        assert len(error) >= 8
        assert error.mean() < 2.0, (a, b, error.mean())
    connected = {next(iter(poses))}
    for _ in poses:
        connected |= {other for name in connected for other in graph[name]}
    assert connected == set(poses), 'Rejected edges must not leave an unconstrained view'
    # All non-leaf cameras participate in the same pose solve, rather than a
    # maximum-spanning tree whose accumulated drift is never measured.
    assert len(registration['landmarks']) >= len(poses)


def test_shipped_views_use_full_image_uvs_and_are_deployed(monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / 'scripts'))
    from deploy_payload import GAME_DATA_PAYLOAD
    for name in NAMES:
        path = ROOT / 'data/art/gui' / (name + '.tga')
        with Image.open(path) as image:
            assert image.size == (512, 512)
        assert path.stat().st_size < 1024 * 1024
        assert f'data/art/gui/{name}.tga' in GAME_DATA_PAYLOAD
        definition = (ROOT / 'data/defs/ui/game/legacy_imgui' / (name + '.fit')).read_text()
        image = re.search(r'GuiImage \{.*?\n\}', definition, re.S).group()
        assert 'uvWidth = 0' in image and 'uvHeight = 0' in image
        assert 'rect = "0,0,256,256"' in image
        # Legacy/mod atlas key remains available to existing VFS precedence.
        assert name in image.lower() and 'legacyTexture =' in image


def test_cook_is_deterministic_and_matches_shipped_pixels(tmp_path, monkeypatch):
    monkeypatch.setattr(globe, 'NAMES', ['mcl_cm_op1_1'])
    registration = json.loads((SOURCE / 'registration.json').read_text())
    globe.render(SOURCE, registration, SOURCE / 'maps', tmp_path, extension='tga')
    first = (tmp_path / 'mcl_cm_op1_1.tga').read_bytes()
    globe.render(SOURCE, registration, SOURCE / 'maps', tmp_path, extension='tga')
    assert first == (tmp_path / 'mcl_cm_op1_1.tga').read_bytes()
    with Image.open(tmp_path / 'mcl_cm_op1_1.tga') as actual, Image.open(ROOT / 'data/art/gui/mcl_cm_op1_1.tga') as shipped:
        assert actual.size == shipped.size
        # Same-environment bytes are exactly reproducible (asserted above). Against the shipped
        # file the cook differs only by float-library arithmetic (numpy/scipy/cv2 builds quantise
        # the float32 pipeline differently): measured 5.9% of pixels, max 1-2 levels, zero mean
        # bias, identical with cv2 SIMD off. Bound it so a real art change still fails.
        diff = np.abs(np.asarray(actual, np.int16) - np.asarray(shipped, np.int16))
        assert diff.max() <= 2
        assert (diff.max(axis=-1) > 0).mean() < .10
        assert diff.mean() < .1


def test_stock_to_render_landmarks_do_not_move():
    report = globe.landmark_errors(SOURCE, ROOT / 'data/art/gui')
    for name, result in report.items():
        assert result['landmarks'] >= 40, (name, result)
        assert result['mean_stock_px'] < 1.0, (name, result)
        assert result['p95_stock_px'] < 2.0, (name, result)
        assert result['rejected'] < result['landmarks'] * .1, (name, result)
