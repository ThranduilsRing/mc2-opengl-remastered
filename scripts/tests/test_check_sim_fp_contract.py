"""check-sim-fp-contract.py: every sim TU is /fp:precise with no contraction; the rejection probes must fire."""
import importlib.util
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "check-sim-fp-contract.py")

spec = importlib.util.spec_from_file_location("check_sim_fp_contract", SCRIPT)
fp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fp)

SIM_SRC = os.path.join(fp.ROOT, "mclib", "stuff", "affinematrix.cpp")
MSVC = "cl.exe /nologo /TP /O2 /fp:precise -c " + SIM_SRC
CLANG = "C:/LLVM/bin/clang-cl.exe /nologo /TP /O2 /fp:precise /clang:-ffp-contract=off -c " + SIM_SRC


def _db(tmp_path, commands):
    path = os.path.join(str(tmp_path), "compile_commands.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump([{"directory": str(tmp_path), "command": c, "file": SIM_SRC} for c in commands], fh)
    return path


def test_selftest_probes_fire():
    assert fp.selftest() == 0


def test_clean_databases_pass(tmp_path):
    n, bad = fp.check_db(_db(tmp_path, [MSVC, CLANG]))
    assert n == 2 and bad == []


def test_each_forbidden_flag_on_a_sim_tu_is_rejected(tmp_path):
    for extra in ("/fp:fast", "/fp:contract", "/fp:strict", "/clang:-ffp-contract=fast", "-ffp-contract=on",
                  "/clang:-ffast-math", "/clang:-ffp-model=fast"):
        n, bad = fp.check_db(_db(tmp_path, [CLANG + " " + extra]))
        assert n == 1 and bad, extra


def test_clang_without_contract_off_is_rejected(tmp_path):
    n, bad = fp.check_db(_db(tmp_path, [CLANG.replace(" /clang:-ffp-contract=off", "")]))
    assert bad and "ffp-contract" in bad[0]


def test_non_sim_tu_is_ignored(tmp_path):
    path = os.path.join(str(tmp_path), "compile_commands.json")
    other = os.path.join(fp.ROOT, "3rdparty", "imgui", "imgui.cpp")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump([{"directory": str(tmp_path), "command": "cl.exe /fp:fast -c " + other, "file": other}], fh)
    assert fp.check_db(path) == (0, [])


def test_repo_cmake_sources_are_clean():
    assert fp.source_arm() == []
