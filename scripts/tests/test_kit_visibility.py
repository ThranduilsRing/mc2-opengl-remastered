"""Four-way deploy/engine priority contract using real cook bytes and fixture trees."""
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools/mc2texcook"))
import kit_payload as kit
import batch_cook
import mc2texcook
import console_child
import importlib.util


@pytest.fixture(scope="module")
def priority_probe(tmp_path_factory):
    folder = tmp_path_factory.mktemp("kit-priority-probe")
    source = folder / "probe.cpp"
    source.write_text('''#include "loose_asset_priority.h"
int main(int argc, char** argv) {
    return userLooseBuildingAsset(argv[1], argv[2], {}) ? 1 : 0;
}
''')
    exe = folder / ("probe.exe" if os.name == "nt" else "probe")
    subprocess.run(["clang++", "-std=c++20", "-O2", "-I", str(ROOT / "mclib"),
                    str(source), "-o", str(exe)], check=True, capture_output=True,
                   **console_child.child_kwargs(redirected=True))
    def user(path, owner=""):
        result = subprocess.run([str(exe), str(path), owner],
                                **console_child.child_kwargs())
        assert result.returncode in (0, 1)
        return result.returncode == 1
    return user


@pytest.mark.parametrize("case", ["fresh", "legacy", "user-ktx", "user-tga", "mod"])
def test_four_way_kit_priority(tmp_path, priority_probe, case):
    from PIL import Image
    stock = tmp_path / "stock"
    target = tmp_path / "install"
    rel = Path("data/tgl/128/building.tga")
    source = stock / rel
    installed = target / rel
    source.parent.mkdir(parents=True)
    installed.parent.mkdir(parents=True)
    Image.new("RGBA", (4, 4), (80, 40, 20, 255)).save(source)
    installed.write_bytes(source.read_bytes())
    output = installed.with_suffix(".ktx2")
    batch_cook._cook_one(source, output, "albedo", 0)
    stamp = Path(str(output) + ".cookstamp")
    if case in ("legacy", "user-ktx"):
        stamp.unlink()
    original = output.read_bytes()
    if case == "user-ktx":
        output.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
    if case == "user-tga":
        Image.new("RGBA", (4, 4), (1, 2, 3, 255)).save(installed)
    kit.reconcile_stock_textures(target, stock)
    owner = "CVE-G" if case == "mod" else ""
    wins = priority_probe(output, owner) or priority_probe(installed, owner)
    assert wins == (case in ("user-ktx", "user-tga", "mod"))
    if case == "legacy":
        assert stamp.is_file() and output.read_bytes() == original
    # Repeated deploy must preserve edits, including edits after receipt creation.
    if case == "fresh":
        output.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        kit.reconcile_stock_textures(target, stock)
        assert priority_probe(output)
        Image.new("RGBA", (4, 4), (1, 2, 3, 255)).save(installed)
        assert priority_probe(installed)


def test_gate_rejects_zero_overrides_and_missing_evidence(monkeypatch):
    spec = importlib.util.spec_from_file_location("kit_check", ROOT / "scripts/check-kit-visibility.py")
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    assert not check.grade("")["must_check_pass"]
    assert not check.grade("[BLDG_ART] foo|applied=-\n")["must_check_pass"]
    assert check.grade("[BLDG_ART] foo|applied=watertower\n")["must_check_pass"]
    import merge_gate
    monkeypatch.setattr(merge_gate, "git", lambda *args: "head")
    monkeypatch.setattr(merge_gate, "log_lease_holders", lambda: None)
    stages = merge_gate.plan(6)
    stage = next(s for s in stages if s[0] == "kit-visibility")
    assert merge_gate.execute([stage], "head", runner=lambda *a, **kw: type("Result", (), {"returncode": 1})()) == 1
