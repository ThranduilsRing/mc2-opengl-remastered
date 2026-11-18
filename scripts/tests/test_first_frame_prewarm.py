"""First-frame hitch fix: the first drawn frame's lazy one-shots are realized during mission load (source pins)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def test_both_load_paths_prewarm_movers_and_hud_sheets():
    for rel in ("code/mission_init_load.cpp", "code/saveload_load.cpp"):
        src = read(rel)
        assert "prewarmMoverGpuTextures(*ObjectManager)" in src and "prewarmHudSheets()" in src, rel
        # before the pipeline prewarm, which waits idle and ends the load barrier
        assert src.index("prewarmHudSheets()") < src.index("mc2PrewarmMissionPipelines()"), rel


def test_prewarm_uses_the_same_lookup_as_the_first_flush():
    flush = read("GameOS/gameos/gos_mech_batcher_flush.cpp")
    warm = flush.split("void gpuMechPrewarmTextureSlot", 1)[1].split("void MechFlushFrame::collectRuns", 1)[0]
    assert "mc2::textures::sourceId(slot)" in warm and "gos_GetMaterialTextureId(handle)" in warm
    assert "mc2::textures::sourceId(slot)" in flush.split("void MechFlushFrame::collectRuns", 1)[1]


def test_planar_terrain_island_is_created_at_load_and_lazily_as_fallback():
    water = read("GameOS/gameos/vulkan_water_island_run.cpp")
    prewarm = water.split("mc2_vk_water_island_prewarm", 1)[1].split("mc2_vk_water_island_run", 1)[0]
    assert "waterplanar::prewarmTerrain(s->planarTerrain" in prewarm and "mapHasWater" in prewarm
    terrain = read("GameOS/gameos/vulkan_terrain_island_run.cpp")
    assert "if (!prewarmTerrain(state, target.w, target.h)) return false;" in terrain
