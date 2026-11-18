"""deps_guard: objects whose recorded header deps name another checkout are found and deleted; local and SDK deps are kept."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import cc_launcher  # noqa: E402
import configure_ninja  # noqa: E402
import deps_guard  # noqa: E402

ROOT = "A:/Games/mc2-slot03"
DEPS = """\
foreign.obj: #deps 3, deps mtime 1 (VALID)
    C:/Program Files (x86)/Windows Kits/10/Include/um/windows.h
    G:/mc2-wt/alpha-order-1/code/objmgr.h
    A:/Games/mc2-slot03/code/local.h

slotfix.obj: #deps 1, deps mtime 1 (VALID)
    A:\\Games\\mc2-slot06\\GameOS\\gameos.hpp

local.obj: #deps 3, deps mtime 1 (VALID)
    A:/Games/mc2-slot03/code/a.h
    A:/Games/mc2-slot03/build64-ninja/gen/b.h
    ../mclib/c.h

sdk.obj: #deps 2, deps mtime 1 (VALID)
    C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14/include/vector
    C:/vcpkg/installed/x64-windows/include/zlib.h

dead.obj: #deps 1, deps mtime 1 (VALID)
    A:/Games/mc2-slot01/code/x.h
"""


def test_parse_and_foreign():
    found = deps_guard.foreign_objects(deps_guard.parse_deps(DEPS), ROOT)
    assert set(found) == {"foreign.obj", "slotfix.obj", "dead.obj"}
    assert found["foreign.obj"] == "G:/mc2-wt/alpha-order-1/code/objmgr.h"


def test_is_foreign_ignores_local_sdk_and_prefix_lookalike():
    assert not deps_guard.is_foreign("A:/Games/mc2-slot03/code/x.h", ROOT)
    assert not deps_guard.is_foreign("a:\\games\\MC2-SLOT03\\code\\x.h", ROOT)
    assert not deps_guard.is_foreign("C:/Program Files (x86)/Windows Kits/10/Include/um/windows.h", ROOT)
    assert not deps_guard.is_foreign("C:/vcpkg/installed/x64-windows/include/zlib.h", ROOT)
    assert deps_guard.is_foreign("A:/Games/mc2-slot030/code/x.h", ROOT)   # not a path prefix of the checkout
    assert deps_guard.is_foreign("A:/Games/mc2-main/code/x.h", ROOT)


def test_guard_deletes_foreign_objects_only(tmp_path, monkeypatch):
    tree = tmp_path / "build64-ninja"
    tree.mkdir()
    (tree / ".ninja_deps").write_bytes(b"x")
    for name in ("foreign.obj", "slotfix.obj", "local.obj", "sdk.obj"):   # dead.obj does not exist on disk
        (tree / name).write_bytes(b"o")
    monkeypatch.setattr(deps_guard, "dump_deps", lambda t: DEPS)
    assert set(deps_guard.scan(str(tree), ROOT)) == {"foreign.obj", "slotfix.obj"}
    assert deps_guard.guard_before(str(tree), ROOT) == 2
    assert sorted(p.name for p in tree.glob("*.obj")) == ["local.obj", "sdk.obj"]
    assert deps_guard.refusal_after(str(tree), ROOT) is None
    assert deps_guard.guard_before(str(tree), ROOT) == 0


def test_refusal_names_the_fix(tmp_path, monkeypatch):
    tree = tmp_path / "t"
    tree.mkdir()
    (tree / ".ninja_deps").write_bytes(b"x")
    (tree / "foreign.obj").write_bytes(b"o")
    monkeypatch.setattr(deps_guard, "dump_deps", lambda t: DEPS)
    msg = deps_guard.refusal_after(str(tree), ROOT)
    assert "[DEPS_GUARD]" in msg and "foreign.obj" in msg and "objmgr.h" in msg


def test_launcher_flags_unmappable_foreign_path():
    line = b"Note: including file: G:\\mc2-wt\\nowhere\\code\\does_not_exist_here.h\r\n"
    out, foreign = cc_launcher.rewrite_output(line, ROOT, [], ())
    assert foreign and b"does_not_exist_here.h" in out
    sdk = b"Note: including file: C:\\Program Files (x86)\\Windows Kits\\10\\Include\\um\\windows.h\r\n"
    assert cc_launcher.rewrite_output(sdk, ROOT, [], ())[1] is False


def test_launcher_revision_tracks_cache_stamp(tmp_path):
    tree = tmp_path / "build64-ninja"
    tree.mkdir()
    cache = tree / "CMakeCache.txt"
    cache.write_text("cc_launcher.py\n", encoding="utf-8")   # a tree from before the stamp is not current
    assert not configure_ninja.tree_cache_current(str(tmp_path), "build64-ninja")
    cache.write_text(f"MC2_CC_LAUNCHER_REV:STRING={configure_ninja.launcher_rev()}\n", encoding="utf-8")
    assert configure_ninja.tree_cache_current(str(tmp_path), "build64-ninja")
    cache.write_text("MC2_CC_LAUNCHER_REV:STRING=0000\n", encoding="utf-8")
    assert not configure_ninja.tree_cache_current(str(tmp_path), "build64-ninja")
