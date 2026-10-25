"""Four-way deploy/engine priority contract using real cook bytes and fixture trees."""
import os
import hashlib
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
    bool read = false;
    bool user = userLooseBuildingAsset(argv[1], argv[2], [&] {
        read = true;
        return std::span<const unsigned char>{};
    });
    if (argc > 3 && read) return 42;
    return user ? 1 : 0;
}
''')
    exe = folder / ("probe.exe" if os.name == "nt" else "probe")
    subprocess.run(["clang++", "-std=c++20", "-O2", "-I", str(ROOT / "mclib"),
                    str(source), "-o", str(exe)], check=True, capture_output=True,
                   **console_child.child_kwargs(redirected=True))
    def user(path, owner="", no_stock_read=False):
        result = subprocess.run([str(exe), str(path), owner] + (["no-read"] if no_stock_read else []),
                                **console_child.child_kwargs())
        assert result.returncode in (0, 1)
        return result.returncode == 1
    return user


@pytest.mark.parametrize("case", ["fresh", "fresh-no-sidecar", "legacy", "legacy-no-source", "legacy-bc7", "legacy-alternate-cook", "user-ktx", "user-tga", "mod", "edited-reference"])
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
    current_hash = hashlib.sha256(output.read_bytes()).hexdigest()
    if case in ("legacy-bc7", "legacy-alternate-cook"):
        batch_cook._cook_one_bc7(source, output, batch_cook._DEFAULT_KTX_TOOL, 0)
    stamp = Path(str(output) + ".cookstamp")
    if case in ("legacy", "legacy-no-source", "legacy-bc7", "legacy-alternate-cook", "user-ktx"):
        stamp.unlink()
    original = output.read_bytes()
    stock_hashes = {rel.as_posix(): hashlib.sha256(source.read_bytes()).hexdigest()}
    inventory = dict(sha256=stock_hashes, cooked_sha256={
        rel.with_suffix(".ktx2").as_posix(): hashlib.sha256(original).hexdigest()})
    if case == "legacy-alternate-cook":
        inventory["cooked_sha256"][rel.with_suffix(".ktx2").as_posix()] = [
            current_hash, hashlib.sha256(original).hexdigest()]
    if case == "fresh-no-sidecar":
        output.unlink()
        stamp.unlink()
    if case == "legacy-no-source":
        installed.unlink()
    if case == "user-ktx":
        output.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
    if case == "user-tga":
        Image.new("RGBA", (4, 4), (1, 2, 3, 255)).save(installed)
    if case == "edited-reference":
        Image.new("RGBA", (4, 4), (1, 2, 3, 255)).save(source)
        installed.write_bytes(source.read_bytes())
    kit.reconcile_stock_textures(target, inventory)
    owner = "CVE-G" if case == "mod" else ""
    wins = priority_probe(output, owner) or priority_probe(installed, owner)
    assert wins == (case in ("user-ktx", "user-tga", "mod", "edited-reference"))
    if not wins:
        assert not priority_probe(output, no_stock_read=True)
        assert not priority_probe(installed, no_stock_read=True)
    if case == "mod":
        assert priority_probe(output, owner, no_stock_read=True)
    if case in ("legacy", "legacy-no-source", "legacy-bc7", "legacy-alternate-cook"):
        assert stamp.is_file() and output.read_bytes() == original
    # Repeated deploy must preserve edits, including edits after receipt creation.
    if case in ("fresh", "legacy-alternate-cook"):
        output.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        kit.reconcile_stock_textures(target, inventory)
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
    assert not check.grade("[BLDG_ART] foo|applied=watertower\n"
                           "[MODOVERRIDE] building 'City2StoryA': user assets take priority\n")["must_check_pass"]
    import merge_gate
    monkeypatch.setattr(merge_gate, "git", lambda *args: "head")
    monkeypatch.setattr(merge_gate, "log_lease_holders", lambda: None)
    stages = merge_gate.plan(6)
    stage = next(s for s in stages if s[0] == "kit-visibility")
    assert merge_gate.execute([stage], "head", runner=lambda *a, **kw: type("Result", (), {"returncode": 1})()) == 1
