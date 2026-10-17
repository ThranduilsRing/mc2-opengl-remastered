"""Slang -> SPIR-V path of tools/shader_offline_build/build_variants.py.

Needs slangc + spirv-cross (Vulkan SDK); skipped without. Fixtures live in
scripts/tests/fixtures/slang (NOT part of the shipped shader set)."""
import json
import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, "fixtures", "slang")
BUILD = os.path.join(ROOT, "tools", "shader_offline_build", "build_variants.py")

sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools", "shader_offline_build"))
import shader_common  # noqa: E402
import slang_compile  # noqa: E402

if not shader_common.find_tool("slangc") or not shader_common.find_tool("spirv-cross"):
    pytest.skip("slangc/spirv-cross (Vulkan SDK) not available", allow_module_level=True)


def run_build(root, *extra):
    return subprocess.run(
        [sys.executable, "-I", BUILD, "--pilots", os.path.join(root, "pilots.json"),
         "--root", root, *extra], capture_output=True, text=True)


@pytest.fixture()
def work(tmp_path):
    dst = tmp_path / "fx"
    shutil.copytree(FIX, dst)
    return str(dst)


def sidecar(root, stage):
    with open(os.path.join(root, "out", f"slang_probe.{stage}.default.json"),
              encoding="utf-8") as f:
        return json.load(f)


def test_version_meets_pin():
    assert slang_compile.check_version(slang_compile.find_slangc()) is None


def test_compiles_and_emits_package(work):
    r = run_build(work)
    assert r.returncode == 0, r.stdout + r.stderr
    out = os.path.join(work, "out")
    for n in ("spirv_index.json", "spirv_package.json"):
        assert os.path.isfile(os.path.join(out, n))
    pkg = json.load(open(os.path.join(out, "spirv_package.json"), encoding="utf-8"))
    assert "slangc" in pkg["tools"]
    idx = {x["key"]: x["artifact"]
           for x in json.load(open(os.path.join(out, "spirv_index.json"),
                                   encoding="utf-8"))["records"]}
    assert set(idx) == {"slang_probe|frag|", "slang_probe|comp|"}
    for art in idx.values():
        with open(os.path.join(out, art), "rb") as f:
            assert f.read(4) == b"\x03\x02\x23\x07"  # SPIR-V magic
    with open(os.path.join(out, idx["slang_probe|frag|"]), "rb") as f:
        assert f.read(8)[4:6] == b"\x00\x00"  # version word 1.0, same as glslang -V
    assert run_build(work, "--check", "--quiet").returncode == 0


def test_frag_reflection_layout(work):
    assert run_build(work).returncode == 0
    b = sidecar(work, "frag")["bindings"]
    assert [(u["binding"], u["set"]) for u in b["ubos"]] == [(0, 0)]
    assert sorted((s["binding"], s["set"], s["type"]) for s in b["samplers"]) == [
        (1, 0, "texture2D"), (2, 0, "sampler")]
    assert sidecar(work, "frag")["language"] == "slang"


def test_comp_reflection_layout(work):
    assert run_build(work).returncode == 0
    ss = sidecar(work, "comp")["bindings"]["ssbos"]
    assert sorted((s["binding"], s["set"]) for s in ss) == [(0, 0), (1, 0)]


def test_broken_shader_fails_with_diagnostic(work):
    cfg = json.load(open(os.path.join(work, "pilots.json"), encoding="utf-8"))
    cfg["pilots"][0]["stages"] = {"frag": "broken.frag.slang"}
    json.dump(cfg, open(os.path.join(work, "pilots.json"), "w", encoding="utf-8"))
    r = run_build(work)
    assert r.returncode == 1
    text = r.stdout + r.stderr
    assert "slangc failed" in text and "undefined_symbol" in text
    assert not [f for f in os.listdir(os.path.join(work, "out")) if f.endswith(".spv")]


def test_artifact_checker_accepts_slang_and_detects_drift(work, tmp_path):
    """scripts/check-spirv-artifacts.py with a Slang pilot: in sync passes, edit fails."""
    assert run_build(work).returncode == 0
    mf = tmp_path / "tools" / "shader_offline_build"
    mf.mkdir(parents=True)
    shutil.copy(os.path.join(work, "pilots.json"), mf / "pilots.json")
    chk = os.path.join(ROOT, "scripts", "check-spirv-artifacts.py")

    def run(*extra):
        # pilots.json lives under <root>/tools/...; sources and out/ under <root>
        for item in os.listdir(work):
            dst = tmp_path / item
            if not dst.exists():
                (shutil.copytree if os.path.isdir(os.path.join(work, item))
                 else shutil.copy)(os.path.join(work, item), dst)
        return subprocess.run([sys.executable, "-I", chk, "--root", str(tmp_path),
                               *extra], capture_output=True, text=True)

    assert run("--quiet").returncode == 0
    with open(tmp_path / "probe.comp.slang", "a", encoding="utf-8") as f:
        f.write("// drift\n")
    r = run()
    assert r.returncode != 0 and "source drift" in (r.stdout + r.stderr)
