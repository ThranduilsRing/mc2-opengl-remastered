import json
from pathlib import Path
from importlib.machinery import EXTENSION_SUFFIXES
from types import SimpleNamespace

import pytest


@pytest.fixture
def root():
    return Path('C:/host-inventory-fixture')

from tools import host_inventory as host


def collect(*, files=(), directories=(), commands=None, spec=None, env=None, patterns=None):
    return {row["tool"]: row for row in host.collect_inventory(
        which=lambda name: (commands or {}).get(name), find_spec=lambda name: spec,
        environ=env or {}, exists=lambda path: str(Path(path)) in files,
        is_dir=lambda path: str(Path(path)) in directories,
        glob_paths=lambda pattern: (patterns or {}).get(pattern, []),
    )}


def test_path_and_install_roots_off_path(root):
    pf = root / "Program Files"
    profile = root / "user"
    render = str(pf / "RenderDoc/renderdoccmd.exe")
    embedded = str(pf / "RenderDoc/qrenderdoc.exe")
    capture = str(profile / "Downloads/TracyProfiler/capture.exe")
    csv = str(profile / "Downloads/TracyProfiler/csvexport.exe")
    vs = str(pf / "Microsoft Visual Studio/2022/BuildTools")
    dump_pattern = str(Path(pf) / "Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe")
    dump = str(Path(vs) / "VC/Tools/MSVC/14.44/bin/Hostx64/x64/dumpbin.exe")
    rows = collect(files={render, embedded, capture, csv, dump}, env={"ProgramFiles": str(pf), "USERPROFILE": str(profile)},
                   patterns={dump_pattern: [dump]})
    for label, path in (("RenderDoc command", render), ("RenderDoc embedded API host", embedded),
                        ("Tracy capture", capture), ("Tracy csvexport", csv), ("dumpbin", dump)):
        assert rows[label]["path"] == path
        assert rows[label]["source"] == "install root"


def test_sdk_capture_scripts_and_env_overrides(root):
    sdk = str(root / "SDK")
    launcher = str(Path(sdk) / "Bin/gfxrecon-capture-vulkan.py")
    override = str(root / "custom-capture.py")
    layer = str(Path(sdk) / "Bin/VkLayer_khronos_validation.json")
    files = {launcher, override, layer}
    env = {"VULKAN_SDK": sdk}
    rows = collect(files=files, directories={sdk}, env=env)
    assert rows["gfxrecon-capture"]["path"] == launcher
    assert rows["Vulkan validation layer manifest"]["path"] == layer
    env["MC2_TOOL_GFXRECON_CAPTURE"] = override
    rows = collect(files=files, directories={sdk}, env=env, commands={"gfxrecon-capture": launcher})
    assert rows["gfxrecon-capture"]["path"] == override
    assert rows["gfxrecon-capture"]["source"] == "MC2_TOOL_GFXRECON_CAPTURE"
    env["MC2_TOOL_GFXRECON_CAPTURE"] = "missing"
    rows = collect(files=files, directories={sdk}, env=env)
    assert rows["gfxrecon-capture"]["path"] == launcher
    assert "Invalid" in rows["gfxrecon-capture"]["note"]


def test_sdk_install_root_without_environment(root):
    sdk = str(root / "SDK")
    rows = collect(directories={sdk}, patterns={"C:/VulkanSDK/*": [sdk]})
    assert rows["Vulkan SDK"]["path"] == sdk


def test_namespace_and_repository_shadow_are_rejected(root):
    namespace = SimpleNamespace(origin=None, submodule_search_locations=[str(host.ROOT / "tools/renderdoc")])
    assert collect(spec=namespace)["RenderDoc Python module"]["status"] == "not found"
    repo_module = str(host.ROOT / "tools/renderdoc/__init__.py")
    assert collect(files={repo_module}, spec=SimpleNamespace(origin=repo_module))["RenderDoc Python module"]["status"] == "not found"
    real = str(root / ("renderdoc" + EXTENSION_SUFFIXES[0]))
    assert collect(files={real}, spec=SimpleNamespace(origin=real))["RenderDoc Python module"]["path"] == real


def test_json_provenance_and_regenerated_board_rows(monkeypatch, capsys, root):
    rows = list(collect().values())
    provenance = {"utc": "2026-10-12T00:00:00+00:00", "source_sha": "a" * 40, "host": "fixture"}
    monkeypatch.setattr(host, "collect_inventory", lambda: rows)
    monkeypatch.setattr(host, "snapshot_provenance", lambda: provenance)
    assert host.main(["--json"]) == 0
    value = json.loads(capsys.readouterr().out)
    assert value["provenance"] == provenance
    markdown = host.render_markdown(rows, provenance)
    assert markdown == host.render_markdown(rows, provenance)
    assert "Exit 0" in markdown and "exit 2" in markdown
    assert all(row in markdown.split("## Proposed check-contracts.sh rows")[1] for row in host.BOARD_ROWS)
    assert all(note in markdown.split("## Harness tooling")[1] for note in host.HARNESS_NOTES)
    assert "[proof ledger]" in markdown
