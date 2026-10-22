"""The C++ / GLSL runtime schema and the IR vocabulary are one list: pin them together."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.content_pipeline import ir  # noqa: E402


def test_surface_classes_match_runtime_header():
    header = (ROOT / "RenderCore/SurfaceMaterial.h").read_text(encoding="utf-8")
    names = re.search(r"kSurfaceClassNames = \{(.*?)\};", header, re.S).group(1)
    assert tuple(re.findall(r'"([a-z_]+)"', names)) == ir.MATERIAL_CLASSES
    table = re.search(r"kSurfaceClassDefaults = \{\{(.*?)\}\};", header, re.S).group(1)
    pairs = [(float(a), float(b)) for a, b in re.findall(r"\{([0-9.]+)f, ([0-9.]+)f\}", table)]
    assert pairs == [ir.CLASS_DEFAULTS[c] for c in ir.MATERIAL_CLASSES]
    metal = re.search(r"surfaceClassMayBeMetallic\(SurfaceClass c\) \{(.*?)\}", header, re.S).group(1)
    assert {"bare_metal", "grating"} == set(ir.METALLIC_CLASSES)
    assert "BareMetal" in metal and "Grating" in metal


def test_glsl_class_constants_match():
    glsl = (ROOT / "shaders/include/surface_material.hglsl").read_text(encoding="utf-8")
    consts = dict(re.findall(r"const uint SURF_CLASS_([A-Z_]+)\s*=\s*(\d+)u;", glsl))
    assert {k.lower(): int(v) for k, v in consts.items()} == {c: i for i, c in enumerate(ir.MATERIAL_CLASSES)}
