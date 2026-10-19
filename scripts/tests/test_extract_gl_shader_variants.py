"""Material-loader extraction must survive retiring a tessellated shader."""
import importlib.util
from pathlib import Path
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("extract_gl_variants", ROOT / "scripts/extract_gl_shader_variants.py")
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)


@pytest.mark.parametrize("condition, expected", [
    ('if (strcmp(shader, "gos_terrain") == 0) {', {"gos_terrain"}),
    ('if (strcmp(shader, "gos_terrain") == 0 || strcmp(shader, "shadow_terrain") == 0) {', {"gos_terrain", "shadow_terrain"}),
])
def test_tessellation_materials_follow_the_loader(condition, expected):
    assert extractor.material_tess_shaders(condition) == expected


def test_unknown_loader_shape_fails_instead_of_omitting_tessellation():
    with pytest.raises(extractor.ExtractError):
        extractor.material_tess_shaders("if (new_tess_policy(shader)) {")
