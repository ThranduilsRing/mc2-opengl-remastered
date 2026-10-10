"""run_tidy_safe: selection and command assembly (no clang-tidy, no build)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_tidy_safe as rts  # noqa: E402


def test_only_third_party_gates_and_lane2_files_are_excluded():
    for rel in ("3rdparty/x/y.cpp", "Gates/gate.cpp", "code/gamecam.cpp", "code/mission_update.cpp", "code/missionbegin.cpp",
                "mclib/mlr/mlrlightmap.cpp", "mclib/tgl_transform.cpp", "mclib/mlr/mlrclipper.cpp",
                "GameOS/gameos/gos_terrain_indirect_dispatch.cpp", "editor/EditorInspector.cpp"):
        assert rts.is_excluded(rel), rel
    for rel in ("mclib/crater.cpp", "code/missiongui_input.cpp", "mclib/txmmgr_render.cpp", "mclib/bdactor.cpp", "mclib/mech3d.cpp",
                "code/mech_move.cpp", "GameOS/gameos/gos_postprocess_fbo.cpp", "GameOS/gameos/gameos_graphics_draw.cpp"):
        assert not rts.is_excluded(rel), rel


def test_select_skips_headers_dirty_unbuilt_and_excluded():
    db = {"mclib/crater.cpp", "mclib/dir.cpp", "code/gamecam.cpp", "mclib/edited.cpp"}
    paths = [os.path.join(rts.ROOT, p) for p in (
        "mclib/crater.cpp", "mclib/dir.h", "code/gamecam.cpp", "mclib/edited.cpp", "mclib/unknown.cpp")]
    kept, skipped = rts.select(paths, db, dirty={"mclib/edited.cpp"})
    assert kept == ["mclib/crater.cpp"]
    reasons = dict(skipped)
    assert "header" in reasons["mclib/dir.h"] or "translation unit" in reasons["mclib/dir.h"]
    assert "excluded" in reasons["code/gamecam.cpp"]
    assert "uncommitted" in reasons["mclib/edited.cpp"]
    assert "compile_commands" in reasons["mclib/unknown.cpp"]


def test_tidy_command_runs_only_the_safe_set_on_the_main_file():
    cmd = rts.tidy_command("clang-tidy", "build64-ninja", "mclib/crater.cpp")
    checks = [c for c in cmd if c.startswith("--checks=")][0]
    assert checks.startswith("--checks=-*,")
    assert "--header-filter=^$" in cmd and "--fix" in cmd and cmd[-1] == "mclib/crater.cpp"
    for off in ("default-member-init", "loop-convert", "use-auto"):
        assert off not in checks
    assert "--fix" not in rts.tidy_command("clang-tidy", "t", "a.cpp", fix=False)
