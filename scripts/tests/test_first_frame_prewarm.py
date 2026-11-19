"""First-frame hitch fix: the first drawn frame's lazy one-shots are realized during mission load (source pins).

Trigger paths: code/mission_init_load.cpp, code/saveload_load.cpp, code/objmgr_io.cpp,
GameOS/gameos/gos_mech_batcher_flush.cpp, GameOS/gameos/vulkan_water_island_run.cpp, GameOS/gameos/vulkan_terrain_island_run.cpp.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def test_both_load_paths_prewarm_movers_and_hud_sheets():
    for rel in ("code/mission_init_load.cpp", "code/saveload_load.cpp"):
        src = read(rel)
        movers = re.search(r"prewarmMoverGpuTextures\s*\(", src)
        hud = re.search(r"prewarmHudSheets\s*\(", src)
        pipes = re.search(r"mc2PrewarmMissionPipelines\s*\(", src)
        assert movers and hud and pipes, rel
        # before the pipeline prewarm, which waits idle and ends the load barrier
        assert movers.start() < pipes.start() and hud.start() < pipes.start(), rel


def test_prewarm_uses_the_same_lookup_as_the_first_flush():
    flush = read("GameOS/gameos/gos_mech_batcher_flush.cpp")
    warm = flush.split("void gpuMechPrewarmTextureSlot", 1)[1].split("void MechFlushFrame::collectRuns", 1)[0]
    assert "mc2::textures::sourceId(slot)" in warm and "gos_GetMaterialTextureId(handle)" in warm
    assert "mc2::textures::sourceId(slot)" in flush.split("void MechFlushFrame::collectRuns", 1)[1]


def test_mover_prewarm_pumps_events_and_stops_on_short_headroom():
    loop = read("code/objmgr_io.cpp").split("void prewarmMoverGpuTextures", 1)[1].split("\n}\n", 1)[0]
    assert loop.count("gpuMechPrewarmHasRoom()") == 1 and loop.count("pump_loading_events()") == 1
    assert "infra::headroom()" in read("GameOS/gameos/gos_mech_batcher_flush.cpp")


def test_planar_terrain_island_is_prewarmed_for_live_reflection_only_and_never_aborts_load():
    water = read("GameOS/gameos/vulkan_water_island_run.cpp")
    prewarm = water.split("mc2_vk_water_island_prewarm", 1)[1].split("mc2_vk_water_island_run", 1)[0]
    call = re.search(r"waterplanar::prewarmTerrain\(s->planarTerrain", prewarm)
    assert call and "reflect_live_active()" in prewarm[: call.start()]
    # a failed prewarm is ignored: lazy creation in the first drawn frame is the fallback
    assert re.search(r"\(void\)\s*mc2::waterplanar::prewarmTerrain", prewarm)
    assert re.search(r"if\s*\(\s*!\s*prewarmTerrain\(state,", read("GameOS/gameos/vulkan_terrain_island_run.cpp"))
