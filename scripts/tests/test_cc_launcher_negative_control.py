"""Negative control for the shared compile cache: an object whose compile was replayed from a cache entry filled by a DIFFERENT
(since deleted) checkout must still rebuild when a header of THIS checkout changes. Real ninja, a fake cl, the real launcher."""
import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import cc_launcher as cl
import console_child
import configure_ninja as cn

DEAD_ROOT = "E:/mc2-archive-2099-01/builds/gone-lane"  # matches the archived-build pattern; never exists

FAKE_CL = textwrap.dedent('''
    import sys
    # Replays what a cache hit filled by the dead checkout would print, then writes the object.
    print("Note: including file:   {dead}/mclib/h.h")
    open(sys.argv[2], "w").write("obj")
''').format(dead=DEAD_ROOT)

BUILD_NINJA = textwrap.dedent('''
    rule cc
      command = "{py}" -S -E "{launcher}" "{own}" "{py}" "{fake}" $in $out
      deps = msvc
      msvc_deps_prefix = Note: including file:
    build a.obj: cc a.c
''')


def test_remap_foreign_dead_root(tmp_path):
    own = tmp_path / "own"
    (own / "mclib").mkdir(parents=True)
    (own / "mclib" / "h.h").write_text("x")
    got = cl.remap(f"{DEAD_ROOT}/mclib/h.h", str(own), [], [])
    assert Path(got) == own / "mclib" / "h.h"
    # Other drives and spellings of an archived build are covered; system headers are not touched.
    assert Path(cl.remap("E:\\mc2-archive-x\\builds\\old\\mclib\\h.h", str(own), [], [])) == own / "mclib" / "h.h"
    assert cl.remap("C:/Program Files/sdk/vector", str(own), [], []) == "C:/Program Files/sdk/vector"


@pytest.mark.skipif(not os.path.exists(cn.NINJA), reason="ninja.exe not installed")
def test_header_edit_rebuilds_after_cache_replay_from_deleted_tree(tmp_path):
    own = tmp_path / "treeB"
    (own / "mclib").mkdir(parents=True)
    (own / "mclib" / "h.h").write_text("v1")
    (own / "a.c").write_text("int x;")
    fake = tmp_path / "fake_cl.py"
    fake.write_text(FAKE_CL)
    py = sys.executable.replace("\\", "/")
    (own / "build.ninja").write_text(BUILD_NINJA.format(
        py=py, launcher=cl.__file__.replace("\\", "/"), own=str(own).replace("\\", "/"), fake=str(fake).replace("\\", "/")))

    def ninja(*args):
        r = subprocess.run([cn.NINJA, "-C", str(own), *args], capture_output=True, text=True,  # background-launch: allow ninja on a temp tree, windowless
                           **console_child.child_kwargs(redirected=True))
        assert r.returncode == 0, r.stdout + r.stderr
        return r.stdout

    ninja()                                   # first build records the replayed (remapped) header dependency
    assert "no work to do" in ninja("-n")     # a path that stayed foreign would be dirty forever
    header = own / "mclib" / "h.h"
    header.write_text("v2")
    later = header.stat().st_mtime + 10
    os.utime(header, (later, later))
    assert "a.obj" in ninja("-n")             # the negative control: editing this tree's header dirties the object
