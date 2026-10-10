"""run_tidy_safe: selection and command assembly (no clang-tidy, no build)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_tidy_safe as rts  # noqa: E402


def test_owner_comment_pass_files_are_excluded():
    for rel in ("code/missiongui_input.cpp", "code/mission_update.cpp", "mclib/txmmgr_render.cpp",
                "mclib/bdactor.cpp", "mclib/mech3d.cpp", "code/mech_move.cpp",
                "GameOS/gameos/gos_postprocess_fbo.cpp", "GameOS/gameos/gos_terrain_indirect_pack.cpp",
                "GameOS/gameos/gos_terrain_lod_chunk_matalbedo.cpp", "GameOS/gameos/gameos_graphics_draw.cpp",
                "GameOS/gameos/gos_static_prop_batcher_state.cpp", "3rdparty/x/y.cpp"):
        assert rts.is_excluded(rel), rel
    assert not rts.is_excluded("mclib/crater.cpp")


def test_select_skips_headers_dirty_unbuilt_and_excluded():
    db = {"mclib/crater.cpp", "mclib/dir.cpp", "code/mission_update.cpp", "mclib/edited.cpp"}
    paths = [os.path.join(rts.ROOT, p) for p in (
        "mclib/crater.cpp", "mclib/dir.h", "code/mission_update.cpp", "mclib/edited.cpp", "mclib/unknown.cpp")]
    kept, skipped = rts.select(paths, db, dirty={"mclib/edited.cpp"})
    assert kept == ["mclib/crater.cpp"]
    reasons = dict(skipped)
    assert "header" in reasons["mclib/dir.h"] or "translation unit" in reasons["mclib/dir.h"]
    assert "excluded" in reasons["code/mission_update.cpp"]
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
