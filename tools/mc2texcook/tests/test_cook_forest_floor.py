import os
import ast
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import cook_forest_floor as cff  # noqa: E402


def test_deploy_forest_source_matches_cooker():
    deploy = Path(__file__).resolve().parents[3] / "scripts/deploy_payload.py"
    constants = {n.targets[0].id: n.value.value for n in ast.parse(deploy.read_text(encoding="utf-8")).body
                 if isinstance(n, ast.Assign) and isinstance(n.targets[0], ast.Name) and isinstance(n.value, ast.Constant)}
    assert constants["FOREST_FLOOR_SRC"] == cff.SOURCE_DIR


def test_forest_texels_are_unit_normals_at_array_size():
    src = np.random.default_rng(1).integers(100, 156, (4096, 4096, 3), dtype=np.uint8)
    src[..., 2] = 255
    a = cff.forest_texels(src)
    assert a.shape == (cff.SIZE, cff.SIZE, 4) and a.dtype == np.uint8 and (a[..., 3] == 128).all()
    n = a[..., :3].astype(np.float64) / 127.5 - 1.0
    assert abs(np.linalg.norm(n, axis=-1).mean() - 1.0) < 0.02
