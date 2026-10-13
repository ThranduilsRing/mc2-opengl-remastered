"""Exercise the merged firewall with unsanctioned calls and owner exceptions."""
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
namespace = {"__name__": "firewall_tests"}
source = (ROOT / "scripts/check-no-raw-gl-from-game.sh").read_text(encoding="utf-8").split("<<'PY'\n", 1)[1]
exec(compile(source.rsplit("\nPY", 1)[0], "check-no-raw-gl-from-game.sh", "exec"), namespace)
AXES = json.loads((ROOT / "scripts/raw-gl-state.allowlist.json").read_text(encoding="utf-8"))


def diff(path, added):
    return f"+++ b/{path}\n" + "\n".join("+" + line for line in added.splitlines())


@pytest.mark.parametrize("axis,call", [("depthfunc", "glDepthFunc(GL_LESS);"),
    ("depthmask", "glDepthMask(1);"), ("colormask", "glColorMask(1,1,1,1);"),
    ("blendfunc", "glBlendFunc(GL_ONE,GL_ZERO);"), ("blendfunc", "glBlendEquation(GL_ADD);"),
    ("fbobind", "glBindFramebuffer(GL_FRAMEBUFFER, newFbo);")])
def test_backend_calls_require_an_owner(axis, call):
    check = namespace["state_problems"]
    assert check(diff("GameOS/gameos/new_renderer.cpp", call), AXES)
    assert not check(diff(AXES[axis]["paths"][0], call), AXES)
    assert not check(diff(AXES[axis]["prefixes"][0] + "test.cpp", call), AXES)


def test_fbo_tag_and_restore_exceptions():
    check = namespace["state_problems"]
    path = "GameOS/gameos/new_renderer.cpp"
    assert not check(diff(path, "// FBO-OWNER: reflection target\nglBindFramebuffer(GL_FRAMEBUFFER, target);"), AXES)
    assert not check(diff(path, "glBindFramebuffer(GL_FRAMEBUFFER, savedFbo);"), AXES)
    assert check(diff(path, "// FBO-OWNER: reflection target\n\nglBindFramebuffer(GL_FRAMEBUFFER, target);"), AXES)


def test_game_firewall_ignores_comments_but_rejects_real_calls():
    check = namespace["game_problems"]
    assert check({"code/x.cpp": "glClear(GL_COLOR_BUFFER_BIT);"}, set())
    assert not check({"code/x.cpp": "/* glClear(0); */\n// glClear(0);"}, set())
    assert not check({"code/x.cpp": "glClear(0);"}, {"code/x.cpp"})
    assert not check({"code/x.cpp": "auto point = glToMC2(position);"}, set())
