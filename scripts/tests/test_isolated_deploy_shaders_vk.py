"""deploy_payload rewrites shaders_vk/*.spv, so make-isolated-deploy must give each install private copies
(otherwise the next deploy refuses on the hardlink and lanes re-copied by hand)."""
import importlib.util
import os
import sys
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1]


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / filename)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def test_shaders_vk_is_copied_not_linked(tmp_path):
    guard = _load("chi_guard", "check-hardlink-isolation.py")
    iso = _load("iso_deploy", "make-isolated-deploy.py")
    src = tmp_path / "src"
    (src / "shaders_vk").mkdir(parents=True)
    (src / "shaders_vk" / "mesh_lit.vert.spv").write_bytes(b"spv")
    (src / "data").mkdir()
    (src / "data" / "x.bin").write_bytes(b"d")
    to_link, to_copy, _, _ = iso.plan(src, guard)
    assert [r for r, _ in to_copy] == ["shaders_vk/mesh_lit.vert.spv"]
    assert [r for r, _ in to_link] == ["data/x.bin"]
