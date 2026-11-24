"""Golden updates preserve checkout bytes and existing newline conventions."""
import importlib.util
import os
from pathlib import Path
import sys

import pytest


@pytest.mark.parametrize("newline", ["\n", "\r\n"])
def test_update_preserves_unchanged_goldens_and_changed_style(tmp_path, monkeypatch, newline):
    source = Path(__file__).resolve().parents[2] / "tools/shader_reflect/reflect.py"
    spec = importlib.util.spec_from_file_location("reflect_update", source)
    reflect = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reflect)
    monkeypatch.setattr(reflect, "ROOT", tmp_path)
    monkeypatch.setattr(reflect, "EXPECTED_DIR", tmp_path / "expected")
    monkeypatch.setattr(reflect, "_find_tools", lambda: ("glslang", "spirv-cross"))
    monkeypatch.setattr(reflect, "_print_tool_versions", lambda *args: None)
    monkeypatch.setattr(reflect, "check_variant_coverage", lambda *args: [])
    monkeypatch.setattr(reflect, "reflect_spv", lambda *args: (True, "", {}))
    monkeypatch.delenv("CI", raising=False)
    spv = tmp_path / "fixture.spv"
    monkeypatch.setattr(reflect, "compile_to_spv", lambda *args, **kwargs: (True, "", spv))
    argv = ["reflect.py", "--update"]
    snapshots = []
    for name in ("unchanged", "changed"):
        shader = tmp_path / f"{name}.frag"
        shader.write_text("void main() {}", encoding="utf-8")
        argv.extend(["--shader", str(shader)])
        contract = reflect.normalize({}, shader.name, "frag", "default", [])
        golden = reflect.golden_path(shader.name, "default")
        golden.parent.mkdir(exist_ok=True)
        original = dict(contract, stage="vert") if name == "changed" else contract
        golden.write_bytes(reflect.serialize_contract(original).replace("\n", newline).encode())
        os.utime(golden, ns=(1_600_000_000_000_000_000,) * 2)
        snapshots.append((golden, golden.read_bytes(), golden.stat().st_mtime_ns, contract))
    monkeypatch.setattr(sys, "argv", argv)
    assert reflect.main() == 0
    unchanged, before, mtime, _ = snapshots[0]
    assert unchanged.read_bytes() == before
    assert unchanged.stat().st_mtime_ns == mtime
    changed, before, _, contract = snapshots[1]
    assert changed.read_bytes() != before
    assert changed.read_bytes() == reflect.serialize_contract(contract).replace("\n", newline).encode()
