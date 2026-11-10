"""importer_source_key must change when the importer's vendored headers (3rdparty/include, tracy) or the cmake modules it includes change."""
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kit_payload as kit


def _git(root, *args):
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *args], check=True,
                   capture_output=True, **kit.console_child.child_kwargs(redirected=True))


@pytest.fixture
def repo(tmp_path):
    for rel, body in {
        "tools/tg_import_dump/CMakeLists.txt": "# importer\n",
        "tools/tg_import_dump/main.cpp": "int main(){}\n",
        "3rdparty/include/a.h": "// a\n",
        "3rdparty/tracy/tracy/Tracy.hpp": "// t\n",
        "3rdparty/assimp/x.cpp": "// x\n",
        "cmake/Mc2Standalone.cmake": "# m\n",
    }.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body)
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-q", "-m", "base")
    return tmp_path


@pytest.mark.parametrize("rel", ["3rdparty/include/a.h", "3rdparty/tracy/tracy/Tracy.hpp", "cmake/Mc2Standalone.cmake"])
def test_key_changes_with_vendored_and_cmake_inputs(repo, rel):
    before = kit.importer_source_key(repo)
    (repo / rel).write_text("// changed\n")
    _git(repo, "commit", "-q", "-am", "edit")
    assert kit.importer_source_key(repo) != before
