"""Second-compiler stage coverage and legacy MSVC command contracts, without temp dirs."""
from pathlib import Path
import json
import sys
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import build_all_targets as bat
import build_contract_harnesses as bch
import build_driver as bd


@pytest.fixture(autouse=True)
def production_seam(monkeypatch):
    seam = Mock(return_value=True)
    monkeypatch.setattr(bch, "build_and_run_order_seam", seam)
    return seam


def record_runs(monkeypatch, module):
    calls = []
    def run(cmd, root, stage, phase, *args, **kwargs):
        kwargs["log_dir"] = args[0] if args else kwargs.get("log_dir")
        calls.append((cmd, stage, phase, kwargs))
        return 0, ""
    monkeypatch.setattr(module.bd, "run", run)
    monkeypatch.setattr(module.bd, "verify_compiler", lambda *args: None)
    return calls


@pytest.mark.parametrize("preset", bd.PRESETS)
def test_selection_reaches_root_standalones_and_harness_child(monkeypatch, preset):
    calls = record_runs(monkeypatch, bat)
    children = []
    monkeypatch.setattr(bat.subprocess, "run", lambda cmd, **kw: children.append(cmd) or SimpleNamespace(returncode=0))
    assert bat.main(["--preset", preset, "--reconfigure", "--keep-going", "-j", "3"]) == 0
    assert calls[0][0][1:3] == ["--preset", preset]
    assert any(arg.endswith(bd.tree_name(bat.TREE, preset)) for arg in calls[1][0])
    assert ("/K" if preset == "clang-cl-nmake" else "-k") in calls[1][0]
    for src, tree, target in bat.STANDALONE:
        cfg, build = [c for c in calls if c[1] == target]
        assert cfg[0][cfg[0].index("-B") + 1].endswith(bd.tree_name(tree, preset))
        assert cfg[0][cfg[0].index("-G") + 1] == ("NMake Makefiles" if preset == "clang-cl-nmake" else "Ninja")
        assert cfg[3]["vcvars"] and build[3]["vcvars"]
        if bd.is_clang(preset):
            assert any("clangcl-toolchain.cmake" in arg for arg in cfg[0])
        else:
            assert "-DCMAKE_CXX_COMPILER=cl" in cfg[0]
        assert build[0][-2:] == ["--", "/K"] if preset == "clang-cl-nmake" else build[0][-3:] == ["--", "-k", "0"]
    assert children[0][-5:] == ["--preset", preset, "-j", "3", "--keep-going"]
    assert "--unit" in children[0]


def test_default_msvc_commands_unchanged(monkeypatch):
    calls = record_runs(monkeypatch, bat)
    children = []
    monkeypatch.setattr(bat.subprocess, "run", lambda cmd, **kw: children.append(cmd) or SimpleNamespace(returncode=0))
    assert bat.main(["--reconfigure", "--no-compile-cache"]) == 0
    root_cfg = calls[0][0]
    assert "--preset" not in root_cfg
    assert "-DMC2_COMPILE_CACHE=ON" not in root_cfg
    assert root_cfg[root_cfg.index("-B") + 1].endswith("/build-alltargets")
    assert calls[1][0][-2:] == ["all", "ui_editor"]
    for src, tree, target in bat.STANDALONE:
        cfg, build = [c for c in calls if c[1] == target]
        assert cfg[0][cfg[0].index("-G") + 1] == "Visual Studio 17 2022"
        assert cfg[0][cfg[0].index("-B") + 1].endswith(tree)
        assert "-A" in cfg[0] and not cfg[3]["vcvars"]
        assert "-k" not in build[0]
    assert children == [[sys.executable, str(ROOT / "tools" / "build_contract_harnesses.py"), "--unit"]]


def test_default_run_goes_through_the_compile_cache(monkeypatch):
    calls = record_runs(monkeypatch, bat)
    fake_env = {"SCCACHE_DIR": "cache"}
    monkeypatch.setattr(bat.cn, "cache_cmake_args", lambda root: (["-DMC2_COMPILE_CACHE=ON"], fake_env))
    monkeypatch.setattr(bat.cn, "tree_uses_cache", lambda root, tree: True)
    monkeypatch.setattr(bat.cn, "tree_cache_current", lambda root, tree: True)
    monkeypatch.setattr(bat.cn, "compile_cache_env", lambda root: fake_env)
    children = []
    monkeypatch.setattr(bat.subprocess, "run", lambda cmd, **kw: children.append(cmd) or SimpleNamespace(returncode=0))
    assert bat.main(["--reconfigure", "-j", "2"]) == 0
    root_cfg = calls[0][0]
    assert "-DMC2_COMPILE_CACHE=ON" in root_cfg and "-DMC2_PCH=OFF" in root_cfg and "-DMC2_PCH=ON" not in root_cfg
    assert calls[0][3]["env"] is fake_env and calls[1][3]["env"]["SCCACHE_DIR"]
    assert calls[1][0][-3:] == ["all", "ui_editor", "mc2_authored_order_seam"]
    for src, tree, target in bat.STANDALONE:
        cfg, build = [c for c in calls if c[1] == target]
        assert cfg[0][cfg[0].index("-G") + 1] == "Ninja" and "-DMC2_COMPILE_CACHE=ON" in cfg[0]
        assert cfg[0][cfg[0].index("-B") + 1].endswith(bat.cc_tree(tree))
        assert cfg[3]["vcvars"] and cfg[3]["env"] is fake_env
    tail = children[0]
    assert "--compile-cache" in tail and tail[tail.index("--seam-tree") + 1] == bat.TREE and "--seam-prebuilt" in tail


@pytest.mark.parametrize("preset", [None, *bd.PRESETS])
def test_shared_corpus_receives_selection(monkeypatch, preset):
    calls = record_runs(monkeypatch, bch)
    verified = Mock()
    monkeypatch.setattr(bd, "verify_compiler", verified)
    argv = ["--unit", "--keep-going", "-j", "3", "--log-dir", "logs"] + (["--preset", preset] if preset else [])
    assert bch.main(argv) == 0
    assert [c[1:3] for c in calls] == [("mc2_tests", "configure"), ("mc2_tests", "build")]
    for cmd, stage, phase, kw in calls:
        tree = bd.tree_name("build64-tests", preset)
        assert any(arg.endswith(tree) for arg in cmd)
        assert kw["vcvars"] == bool(preset)
        assert kw["log_dir"] == "logs"
        if phase == "configure":
            if preset:
                assert ("NMake Makefiles" if preset == "clang-cl-nmake" else "Ninja") in cmd and kw["vcvars"]
                assert "-DCMAKE_BUILD_TYPE=RelWithDebInfo" in cmd
                assert (any("clangcl-toolchain.cmake" in arg for arg in cmd)) == bd.is_clang(preset)
            else:
                assert "-A" in cmd and "-G" not in cmd and not kw["vcvars"]
        else:
            if preset:
                assert cmd[-2:] == ["--", "/K"] if preset == "clang-cl-nmake" else cmd[-3:] == ["--", "-k", "0"]
            if preset == "clang-cl-nmake":
                assert "--parallel" not in cmd
            else:
                assert cmd[cmd.index("--parallel") + 1] == "3"
    if preset:
        verified.assert_called_once_with(ROOT / bd.tree_name("build64-tests", preset), preset)
    else:
        verified.assert_not_called()


def test_no_compiler_shares_any_default_tree():
    defaults = [bat.TREE, *[t for _, t, _ in bat.STANDALONE], "build64-tests", "build64-tests-asan"]
    all_trees = defaults + [bd.tree_name(t, p) for p in bd.PRESETS for t in defaults]
    assert len(all_trees) == len(set(all_trees))


@pytest.mark.parametrize("asan", [False, True])
def test_default_corpus_commands_and_output_unchanged(monkeypatch, capsys, asan):
    calls = []
    def run(cmd, **kwargs):
        calls.append((cmd, kwargs))
        return SimpleNamespace(returncode=0, stdout="", stderr="")
    monkeypatch.setattr(bch.subprocess, "run", run)
    monkeypatch.setattr(bch, "cmake_exe", lambda: "cmake")
    monkeypatch.setattr(bch.time, "monotonic", lambda: 0)
    driver = Mock()
    monkeypatch.setattr(bd, "run", driver)
    assert bch.main(["--asan"] if asan else ["--unit"]) == 0
    tree = ROOT / ("build64-tests-asan" if asan else "build64-tests")
    assert [c[0] for c in calls] == [
        ["cmake", "-S", str(ROOT / "tests/unit"), "-B", str(tree), "-A", "x64", "-DMC2_ASAN=" + ("ON" if asan else "OFF")],
        ["cmake", "--build", str(tree), "--config", "RelWithDebInfo", "--target", "mc2_tests"],
    ]
    expected_kwargs = dict(capture_output=True, text=True, cwd=ROOT,
                           **bch.console_child.child_kwargs(redirected=True))
    assert all(kw == expected_kwargs for _, kw in calls)
    driver.assert_not_called()
    assert capsys.readouterr().out == "  [PASS] mc2_tests: built in 0s\n" + (
        "" if asan else "  [PASS] mc2_authored_order_seam\n")


@pytest.mark.parametrize("argv,python_runs", [
    (["--run", "--unit"], True),
    (["--run", "--unit-only"], False),
    (["--run", "--asan"], False),
    (["--run", "--only", "camera_frustum_harness"], False),
])
def test_python_suite_selection_remains_compatible(monkeypatch, argv, python_runs, production_seam):
    monkeypatch.setattr(bch, "build_and_run_unit_tests", lambda *a, **kw: (True, "passed"))
    python = Mock(return_value=False)
    monkeypatch.setattr(bch, "run_python_harnesses", python)
    assert bch.main(argv) == (1 if python_runs else 0)
    assert python.call_count == int(python_runs)
    assert production_seam.call_count == int("--asan" not in argv and "--only" not in argv)


def test_root_preset_trees_match_cli_and_keep_full_product_scope():
    presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    configure = {p["name"]: p for p in presets["configurePresets"]}
    build = {p["name"]: p for p in presets["buildPresets"]}
    for name in bd.PRESETS:
        assert configure[name]["binaryDir"] == "${sourceDir}/" + bd.tree_name(bat.TREE, name)
        assert build[name]["configurePreset"] == name
        assert build[name]["targets"] == ["all", "ui_editor"]
    variables = configure["windows-alltargets"]["cacheVariables"]
    assert variables["CMAKE_BUILD_TYPE"] == "RelWithDebInfo"
    assert variables["MC2_CXX_STANDARD"] == "23"
    for flag in ["MC2_EDITOR", "MC2_EDITOR_IN_ALL", "ENABLE_MC2FX", "ENABLE_MC2FX_PREVIEW", "ENABLE_MC2WEAPON_GUI"]:
        assert variables[flag] == "ON"


def test_root_configure_failure_does_not_skip_later_stages(monkeypatch, capsys):
    calls = record_runs(monkeypatch, bat)
    original = bd.run
    def run(cmd, root, stage, phase, *args, **kwargs):
        rc, output = original(cmd, root, stage, phase, *args, **kwargs)
        return (1 if stage == "root" and phase == "configure" else rc), output
    monkeypatch.setattr(bd, "run", run)
    child = Mock(return_value=SimpleNamespace(returncode=0))
    monkeypatch.setattr(bat.subprocess, "run", child)
    assert bat.main(["--preset", "clang-cl-dev", "--reconfigure", "--keep-going"]) == 1
    assert {c[1] for c in calls} == {"root", *[t for _, _, t in bat.STANDALONE]}
    assert not any(c[1:3] == ("root", "build") for c in calls)
    child.assert_called_once()
    assert "FAILED: root tree" in capsys.readouterr().err


@pytest.mark.parametrize("run_suite", [False, True])
def test_failed_corpus_is_not_reported_as_success(monkeypatch, run_suite):
    monkeypatch.setattr(bch, "build_and_run_unit_tests", lambda *a, **kw: (False, "build failed"))
    python = Mock()
    monkeypatch.setattr(bch, "run_python_harnesses", python)
    assert bch.main(["--unit"] + (["--run"] if run_suite else [])) == 1
    python.assert_not_called()


def test_unknown_requested_harness_cannot_be_silently_skipped(monkeypatch):
    monkeypatch.setattr(bch, "REGISTERED_HARNESSES", ["known"])
    build = Mock()
    monkeypatch.setattr(bch, "build_and_run_unit_tests", build)
    with pytest.raises(SystemExit) as exc:
        bch.main(["--only", "known", "--only", "typo"])
    assert exc.value.code == 2
    build.assert_not_called()


@pytest.mark.parametrize("evidence", ["", 'set(CMAKE_CXX_COMPILER_ID "MSVC")',
                                     'set(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER "clang.exe")'])
def test_compiler_evidence_rejects_fallback(monkeypatch, evidence):
    monkeypatch.setattr(Path, "glob", lambda self, pattern: [SimpleNamespace(read_text=lambda **kw: evidence)] if "CXX" in pattern else [])
    with pytest.raises(RuntimeError):
        bd.verify_compiler("build-clangcl-alltargets", "clang-cl-dev")


def test_verified_clangcl_evidence(monkeypatch, capsys):
    evidence = '\n'.join(f'set({key} "{value}")' for key, value in {
        "CMAKE_CXX_COMPILER_ID": "Clang", "CMAKE_CXX_COMPILER": "C:/LLVM/bin/clang-cl.exe",
        "CMAKE_CXX_COMPILER_FRONTEND_VARIANT": "MSVC", "CMAKE_CXX_COMPILER_VERSION": "22.1.0"}.items())
    monkeypatch.setattr(Path, "glob", lambda self, pattern: [SimpleNamespace(read_text=lambda **kw: evidence)] if "CXX" in pattern else [])
    bd.verify_compiler("build-clangcl-alltargets", "clang-cl-dev")
    assert "Clang 22.1.0" in capsys.readouterr().out


@pytest.mark.parametrize("layout", ["config", "flat", "missing"])
def test_preset_run_uses_only_selected_corpus_tree(monkeypatch, layout):
    record_runs(monkeypatch, bch)
    tree = ROOT / bd.tree_name("build64-tests", "clang-cl-dev")
    binary = "mc2_tests" + (".exe" if bch.os.name == "nt" else "")
    selected = tree / "RelWithDebInfo" / binary if layout == "config" else tree / binary
    monkeypatch.setattr(Path, "is_file", lambda path: path == selected if layout != "missing" else "build64-tests" in str(path))
    runtime = Mock(return_value=SimpleNamespace(returncode=0))
    monkeypatch.setattr(bch.subprocess, "run", runtime)
    ok, info = bch.build_and_run_unit_tests("RelWithDebInfo", True, only=["camera_frustum_harness"], preset="clang-cl-dev")
    assert ok == (layout != "missing")
    if layout == "missing":
        runtime.assert_not_called()
        assert "requested compiler binary missing" in info
    else:
        assert runtime.call_args.args[0] == [str(selected), "--no-breaks", "--test-case=camera_frustum_harness/*"]


def test_c_compiler_fallback_is_rejected(monkeypatch):
    cxx = '\n'.join(f'set({key} "{value}")' for key, value in {
        "CMAKE_CXX_COMPILER_ID": "Clang", "CMAKE_CXX_COMPILER": "clang-cl.exe",
        "CMAKE_CXX_COMPILER_FRONTEND_VARIANT": "MSVC"}.items())
    def glob(self, pattern):
        evidence = cxx if "CXX" in pattern else 'set(CMAKE_C_COMPILER_ID "MSVC")'
        return [SimpleNamespace(read_text=lambda **kw: evidence)]
    monkeypatch.setattr(Path, "glob", glob)
    with pytest.raises(RuntimeError, match="C compiler does not match"):
        bd.verify_compiler("build-clangcl-alltargets", "clang-cl-dev")


@pytest.mark.parametrize("failure", ["configure", "compiler", "build"])
def test_failed_corpus_stage_prevents_runtime(monkeypatch, capsys, failure):
    calls = record_runs(monkeypatch, bch)
    original = bd.run
    def run(cmd, root, stage, phase, *args, **kwargs):
        original(cmd, root, stage, phase, *args, **kwargs)
        return (1, "bad stage") if phase == failure else (0, "")
    monkeypatch.setattr(bd, "run", run)
    if failure == "compiler":
        monkeypatch.setattr(bd, "verify_compiler", Mock(side_effect=RuntimeError("bad compiler")))
    runtime = Mock()
    monkeypatch.setattr(bch.subprocess, "run", runtime)
    assert bch.main(["--run", "--unit", "--preset", "clang-cl-dev", "--keep-going"]) == 1
    assert [c[2] for c in calls] == (["configure", "build"] if failure == "build" else ["configure"])
    runtime.assert_not_called()
    assert "[FAIL] mc2_tests:" in capsys.readouterr().out


def test_vcvars_preserves_quoted_paths_repo_cwd_and_failure_code(monkeypatch):
    run = Mock(return_value=SimpleNamespace(returncode=7))
    monkeypatch.setattr(bd.subprocess, "run", run)
    rc, _ = bd.run(["C:/Program Files/CMake/cmake.exe", "--preset", "clang-cl-dev"],
                   "A:/Games/mc2-s1", "root", "configure", vcvars=True)
    assert rc == 7
    command = run.call_args.args[0]
    assert command.startswith(f'call "{bd.cn.VCVARS}" >nul && "C:/Program Files/CMake/cmake.exe"')
    assert run.call_args.kwargs["cwd"] == "A:/Games/mc2-s1"
    assert run.call_args.kwargs["shell"] is True


def test_cache_launcher_rewrites_replayed_include_paths(tmp_path, monkeypatch):
    """A cache hit replays the first checkout's /showIncludes lines; the launcher maps them onto this checkout."""
    sys.path.insert(0, str(ROOT / "scripts"))
    import cc_launcher as cl
    own, other = tmp_path / "own", tmp_path / "other"
    for base in (own, other):
        (base / "mclib").mkdir(parents=True)
        (base / "mclib" / "a.h").write_text("x")
    (own / "bld" / "generated").mkdir(parents=True)
    (own / "bld" / "generated" / "fp.h").write_text("x")
    monkeypatch.setenv("SCCACHE_BASEDIRS", f"{own};{other}")
    roots = cl.roots_from_env(str(own))
    assert roots == [str(other)]
    incs = cl.include_dirs(["/nologo", f"-I{own / 'bld' / 'generated'}", "/I", str(own / "mclib")])
    assert Path(cl.remap(str(other / "mclib" / "a.h"), str(own), roots, incs)) == own / "mclib" / "a.h"
    assert Path(cl.remap(str(other / "oldbld" / "generated" / "fp.h"), str(own), roots, incs)) == own / "bld" / "generated" / "fp.h"
    assert cl.remap("C:/Program Files/sdk/x.h", str(own), roots, incs) == "C:/Program Files/sdk/x.h"
