"""Signed fence footprint regression: a size-only audit missed reversed corners."""
import json
import math
import struct

from tools.blender_pipeline import fence_bundle
from tools.blender_pipeline.recipes import fence_gen


def test_rail_faces_point_outward():
    # A size/bounds check cannot detect an inside-out tube. Check both actual rails.
    for radius, sides in ((0.28, 6), (0.14, 4)):
        for quad in fence_gen.run_tube(0, 3, -42, 0, radius, sides):
            p, q, r = quad[:3]
            u = [q[i]-p[i] for i in range(3)]
            v = [r[i]-p[i] for i in range(3)]
            normal = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
            center = [sum(vertex[i] for vertex in quad)/4 for i in range(3)]
            assert normal[0]*center[0] + normal[2]*(center[2]-3) > 0


def test_recipe_matches_positive_stock_run():
    for name, v in fence_gen.VARIANTS.items():
        pitch = 128 / 3 * (math.sqrt(2) if name == "wirefencediag" else 1)
        assert abs(fence_gen.RUN_SIGN * v["y0"] - pitch) < 1e-9
        assert v["y1"] == 0 and v["cx"] == 0
        sheet, _, _, _ = fence_gen.build(name)
        assert {p[1] for q in sheet for p in q} == {v["y0"], v["y1"]}


def test_bundle_rejects_reversed_run(tmp_path):
    mo = tmp_path / "data/model_overrides"
    mo.mkdir(parents=True)
    (mo / "models.json").write_text(json.dumps({"overrides": [{
        "type": "model", "replaces": "staticProp:wirefence", "source": "fence.glb",
        "renderOnly": True, "fallback": "stock", "scale": 1.0}]}))
    for reverse in (False, True):
        bounds = [-0.6325, 128 / 3 + 0.6325]
        if reverse:
            bounds = [-bounds[1], -bounds[0]]
        g = {"meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 0}, "indices": 1}]}],
             "accessors": [{"min": [-0.6325, -15.9699, bounds[0]], "max": [0.6325, 0, bounds[1]]}, {"count": 12}]}
        body = json.dumps(g).encode()
        (mo / "fence.glb").write_bytes(struct.pack("<4sIII4s", b"glTF", 2, 20 + len(body), len(body), b"JSON") + body)
        errors = fence_bundle.validate(tmp_path, tmp_path, tmp_path)
        assert bool(errors) == reverse
        if reverse:
            assert "positive stock run" in errors[0]
