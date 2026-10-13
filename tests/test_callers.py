import json
from types import SimpleNamespace

import pytest

from tools import callers


def build(root, files):
    return callers.build_inventory(root, list(files), read_text=files.__getitem__)


@pytest.fixture
def root():
    return callers.ROOT / "tests/caller-fixture"


def item(inventory, path):
    return next(row for row in inventory["scripts"] if row["path"] == path)


def hits(inventory, path):
    row = item(inventory, path)
    return {hit["path"] for hit in row["callers_by_basename"] + row["callers_by_stem"]}


def test_live_sources_and_sorted_inventory(root):
    inventory = build(root, {
        "tools/target.py": "# inert\n", "scripts/runner.sh": "py -3 tools/target.py\n",
        "tests/importer.py": "import importlib\nimportlib.import_module('tools.target')\n",
        "CMakeLists.txt": "add_custom_target(run COMMAND py tools/target.py)\n",
        ".mcp.json": '{"command":"py","args":["-3","tools/target.py"]}\n',
        ".githooks/pre-commit": "#!/bin/sh\npy -3 tools/target.py\n",
        "scripts/check-contracts.sh": 'run_check "target" "py -3 tools/target.py"\n',
        "docs/readme.md": "py tools/target.py\n",
    })
    assert inventory["scripts"][0]["path"] == "scripts/check-contracts.sh"
    assert hits(inventory, "tools/target.py") == {"scripts/runner.sh", "tests/importer.py", "CMakeLists.txt",
          ".mcp.json", ".githooks/pre-commit", "scripts/check-contracts.sh"}
    assert item(inventory, "tools/target.py")["callers_by_stem"][0]["evidence"] == "import_module"


def test_qualified_imports_aliases_and_relative_imports(root):
    inventory = build(root, {
        "tools/target.py": "", "tools/pkg/caller.py": "from ..target import main\n",
        "tests/importer.py": "from tools.target import main\nfrom tools import target\n"
            "import tools.target\nfrom importlib import import_module as load\nload('tools.target')\n",
    })
    assert hits(inventory, "tools/target.py") == {"tests/importer.py", "tools/pkg/caller.py"}
    assert len(item(inventory, "tools/target.py")["callers_by_stem"]) == 5


def test_collision_path_identity_and_non_executable_mentions(root):
    inventory = build(root, {
        "tools/a/target.py": "", "tools/b/target.py": "", "tools/build.py": "",
        "scripts/run.sh": "py tools/a/target.py\npy target.py\n# py tools/b/target.py\necho py tools/b/target.py\npython -c 'tools/b/target.py'\n",
        "tests/importer.py": "build = False\nfixture = 'tools/b/target.py'\nimport tools.a.target\n",
        "CMakeLists.txt": "add_custom_target(build)\n# COMMAND py tools/b/target.py\n",
        "tests/fixture.json": '{"command":"py","args":["tools/b/target.py"]}',
    })
    assert hits(inventory, "tools/a/target.py") == {"scripts/run.sh", "tests/importer.py"}
    assert hits(inventory, "tools/b/target.py") == set()
    assert hits(inventory, "tools/build.py") == set()
    assert all(hit["line"] == 1 for hit in item(inventory, "tools/a/target.py")["callers_by_basename"])


def test_multiline_cmake_and_file_relative_spawn(root):
    inventory = build(root, {
        "tools/target.py": "", "tools/run.py": "from pathlib import Path\nimport subprocess\nimport sys\n"
            "HERE = Path(__file__).resolve().parent\nWORKER = HERE / 'target.py'\nsubprocess.run([sys.executable, str(WORKER)])\n",
        "CMakeLists.txt": "add_custom_target(run\n COMMAND ${Python3_EXECUTABLE}\n ${CMAKE_SOURCE_DIR}/tools/target.py\n VERBATIM\n)\n",
    })
    assert hits(inventory, "tools/target.py") == {"tools/run.py", "CMakeLists.txt"}


def test_fixture_locals_and_parameters_do_not_resolve_unrelated_commands(root):
    inventory = build(root, {
        "tools/target.py": "", "tests/runner.py": "import subprocess\n"
            "def run(cmd):\n subprocess.run(cmd)\n"
            "def fixture():\n cmd = ['py', 'tools/target.py']\n"
            "def shadow(subprocess):\n subprocess.run(['py', 'tools/target.py'])\n"
            "def shadow_sys(sys):\n subprocess.run([sys.executable, 'tools/target.py'])\n"
            "def unknown(exe):\n subprocess.run([exe, 'tools/target.py'])\n",
    })
    assert hits(inventory, "tools/target.py") == set()


def test_batch_comments_and_python_subprocess_command_positions(root):
    inventory = build(root, {
        "tools/target.py": "", "tools/unused.py": "",
        "scripts/run.cmd": "REM py tools/unused.py\n:: py tools/unused.py\n@rem py tools/unused.py\npy tools/target.py\n",
        "tests/runner.py": "import subprocess\nimport sys\nfixture = 'tools/unused.py'\n"
            "command = [sys.executable, '-B', 'tools/target.py']\nsubprocess.run(command)\n",
    })
    assert hits(inventory, "tools/target.py") == {"scripts/run.cmd", "tests/runner.py"}
    assert hits(inventory, "tools/unused.py") == set()


def test_generated_inventory_and_allowlists_never_become_evidence(root):
    files = {"tools/target.py": "", "scripts/run.sh": "py tools/target.py\n"}
    before = build(root, files)
    files["tests/callers.json"] = json.dumps(before)
    files["tests/callers_allowlist.json"] = '{"command":"py tools/target.py"}'
    files["tools/inventory.json"] = '{"command":"py tools/target.py"}'
    first = build(root, files)
    files["tests/callers.json"] = json.dumps(first)
    second = build(root, files)
    assert before == first == second


def test_allowlist_and_stale_check(tmp_path, monkeypatch, capsys):
    inventory = build(tmp_path, {"scripts/manual.py": "print('manual')\n"})
    assert callers.uncalled_scripts(inventory) == ["scripts/manual.py"]
    assert callers.check_inventory(inventory, {"scripts/manual.py": "Manual maintainer CLI"}) == []
    assert callers.check_inventory(inventory, {"scripts/removed.py": "deleted"})[-1] == "allowlist entry is not a tracked script: scripts/removed.py"
    monkeypatch.setattr(callers, "tracked_files", lambda root: ["scripts/manual.py"])
    (tmp_path / "tests").mkdir()
    (tmp_path / "tests/callers_allowlist.json").write_text('{"scripts/manual.py":"Manual maintainer CLI"}')
    assert callers.main(["--root", str(tmp_path)]) == 0
    assert callers.main(["--root", str(tmp_path), "--check"]) == 0
    (tmp_path / "tests/callers.json").write_text("{}")
    assert callers.main(["--root", str(tmp_path), "--check"]) == 1
    assert "stale" in capsys.readouterr().err
    (tmp_path / "tests/callers_allowlist.json").write_text('{"scripts/manual.py":""}')
    assert callers.main(["--root", str(tmp_path), "--check"]) == 2


def test_git_spawn_uses_console_child(monkeypatch, root):
    calls = []
    monkeypatch.setattr(type(root), "is_file", lambda path: path == root / "tools/target.py")
    monkeypatch.setattr(callers.console_child, "child_kwargs", lambda **kw: {"sentinel": kw["redirected"]})
    def run(args, **kwargs):
        calls.append((args, kwargs))
        return SimpleNamespace(stdout=b"tools/target.py\0")
    monkeypatch.setattr(callers.subprocess, "run", run)
    assert callers.tracked_files(root) == ["tools/target.py"]
    assert calls[0][1]["sentinel"] is True
    monkeypatch.setattr(type(root), "is_file", lambda path: False)
    assert callers.tracked_files(root) == []
