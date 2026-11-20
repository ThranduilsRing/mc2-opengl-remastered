"""A documented MC2_* gate must be read from the env/gate table, not hardcoded off.

`static const bool s_x = false;` next to a gate's comment or log tag looks like a switch but no env var can ever turn it
on (MC2_BRAIN_FREEZE_TRACE, MC2_AIMED_FIRE_FIXTURE and MC2_LINKUP_TRACE all shipped as dead switches). This test finds
every such hardcoded-false constant in GameOS/, mclib/ and code/ (recursively) whose +-WINDOW source lines name a registered or documented gate (the full name, or
its name without the MC2_ prefix such as a "[BRAIN_FREEZE_TRACE]" log tag) and compares it with KNOWN_OFFENDERS.

Ratchet: a NEW offender fails the test; a fixed one must be deleted from KNOWN_OFFENDERS (the list may only shrink).
Fix an offender with `static const bool s_x = mc2::gate::get<"MC2_NAME">();`, never by editing the allowlist upwards.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WINDOW = 30
SCAN_DIRS = ("GameOS", "mclib", "code")
HARDCODED = re.compile(r"static\s+(?:const|constexpr)\s+bool\s+(\w+)\s*=\s*false\s*(?:\|\|\s*false\s*)?;")
NAME = re.compile(r"MC2_[A-Z0-9_]{4,}")

# (file relative to the repo root, variable, n-th constant of that name in the file, 0-based) -> gate named nearby. Only ever delete lines.
KNOWN_OFFENDERS = {
    # known dead gates (documented env var, no read)
    ("code/mech_update.cpp", "s_linkupTrace", 0): "MC2_LINKUP_TRACE",
    ("code/warrior_combat.cpp", "s_aimedFireFixture", 0): "MC2_AIMED_FIRE_FIXTURE",
    # heuristic hits (a gate name is only nearby); review each, delete the line once it is a gate read or judged intentional
    ("code/controlgui_objectives.cpp", "s_on", 0): "MC2_IMGUI",
    ("code/controlgui_objectives.cpp", "s_on", 1): "MC2_IMGUI",
    ("code/controlgui_objectives.cpp", "s_on", 2): "MC2_IMGUI",
    ("code/controlgui_update.cpp", "s_cgSplitOn", 0): "MC2_BOOT_INFO",
    ("code/gamecam.cpp", "s_forcePos", 0): "MC2_HZB_VIEW_FILE",
    ("code/gamecam.cpp", "s_hzbForceHorizon", 0): "MC2_HZB_VIEW_FILE",
    ("code/gamecam.cpp", "s_vanishProbe", 0): "MC2_VIEW_UNIFORMS",
    ("code/infowindow.cpp", "s_on", 0): "MC2_IMGUI",
    ("code/infowindow.cpp", "s_scrollPin", 0): "MC2_BOOT_INFO",
    ("code/infowindow.cpp", "s_scrollPin", 1): "MC2_BOOT_INFO",
    ("code/loadscreen.cpp", "s_legacyBanner", 0): "MC2_LEGACY_LOAD_BANNER",
    ("code/logisticsdata_init.cpp", "s_smartLoadTrace", 0): "MC2_SMART_LOAD",
    ("code/missiongui_orders.cpp", "s_tgtGuard", 0): "MC2_DIAG_TAGS",
    ("code/missiongui_update.cpp", "s_cursorTargetTrace", 0): "MC2_DIAG_TAGS",
    ("code/mover_combat.cpp", "s_tgtGuard", 0): "MC2_DIAG_TAGS",
    ("code/objmgr_pick.cpp", "s_cursorTargetTrace", 0): "MC2_DIAG_TAGS",
    ("code/objmgr_update.cpp", "s_gpuCullLifecycle", 0): "MC2_GOM_RECON",
    ("code/pausewindow.cpp", "s_on", 0): "MC2_IMGUI",
    # found by the recursive GameOS/mclib/code scan (harness-guards-2), recorded as-is, not reviewed
    ("GameOS/gameos/gameos_graphics_hud.cpp", "s_hudDrawDumpEnv", 0): "MC2_VULKAN",
    ("GameOS/gameos/gameos_graphics_hud.cpp", "s_uiPassMeasure", 0): "MC2_VULKAN",
    ("GameOS/gameos/gameos_graphics_texture.cpp", "s_magentaTrace", 0): "MC2_DIAG_TAGS",
    ("GameOS/gameos/gameos_graphics_tparams.cpp", "enabled", 0): "MC2_HEIGHT",
    ("GameOS/gameos/gameos_graphics_tparams.cpp", "s_fatal", 0): "MC2_FRAMECTX_MISMATCH_FATAL",
    ("GameOS/gameos/gameosmain.cpp", "s_useRenderFrameDriver", 0): "MC2_RENDER_PASS_TIME",
    ("GameOS/gameos/gameosmain_frame.cpp", "s_capTrace", 0): "MC2_HEARTBEAT",
    ("GameOS/gameos/gameosmain_frame.cpp", "s_dpCompareEnabled", 0): "MC2_DRAW_PACKET_V3",
    ("GameOS/gameos/gameosmain_frame.cpp", "s_v3Enabled", 0): "MC2_DRAW_PACKET_V3",
    ("GameOS/gameos/gos_mech_batcher_geometry.cpp", "s_mechRestoreTrace", 0): "MC2_DIAGFE",
    ("GameOS/gameos/gos_mech_batcher_geometry.cpp", "s_nodeTrace", 0): "MC2_DIAGFE",
    ("GameOS/gameos/gos_static_prop_registry_flush_range.cpp", "s_leafDiag", 0): "MC2_FORCE_DYNAMIC_TREES",
    ("GameOS/gameos/gos_static_prop_registry_flush_range.cpp", "s_traceMulti", 0): "MC2_REGFLUSH_GUARD2",
    ("GameOS/gameos/gos_static_prop_registry_flush_tail.cpp", "s_regflushTrace", 0): "MC2_REGFLUSH_GUARD2",
    ("GameOS/gameos/gos_static_prop_registry_flush_tail.cpp", "s_staticLightZeroProbe", 0): "MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE",
    ("GameOS/gameos/gos_terrain_indirect.cpp", "s", 0): "MC2_TERRAIN_INDIRECT",
    ("GameOS/gameos/gos_terrain_indirect.cpp", "s", 1): "MC2_TERRAIN_INDIRECT",
    ("GameOS/gameos/gos_terrain_indirect.cpp", "s", 2): "MC2_DRAWALPHA",
    ("GameOS/gameos/gos_terrain_indirect.cpp", "s", 3): "MC2_TERRAIN_INDIRECT_MINE",
    ("GameOS/gameos/gos_terrain_indirect_cement.cpp", "s_adoptTrace", 0): "MC2_DIAGF",
    ("GameOS/gameos/gos_terrain_lighting.cpp", "s_hazeParity", 0): "MC2_TERRAIN_LIGHTING_GPU",
    ("GameOS/gameos/gos_terrain_lighting.cpp", "s_poolDump", 0): "MC2_LIGHT_GRID",
    ("GameOS/gameos/gpu_cull_substrate.cpp", "s_substrateTrace", 0): "MC2_DIAGF",
    ("GameOS/gameos/gpu_driven_common.cpp", "s_enabled", 0): "MC2_GPU_DRIVEN",
    ("GameOS/gameos/gpu_driven_common.cpp", "s_enabled", 1): "MC2_GPU_DRIVEN",
    ("mclib/assimp_importer.cpp", "s_mechSkelTrace", 0): "MC2_GLTF_AXIS",
    ("mclib/bdactor_diag.cpp", "s_diagEnabled", 0): "MC2_FRAME_JOBS",
    ("mclib/bdactor_diag.cpp", "s_enabled", 0): "MC2_LIGHTBAKE",
    ("mclib/bdactor_lightbake.cpp", "s_bakeOrdProbe", 0): "MC2_LIGHTBAKE",
    ("mclib/bdactor_update.cpp", "s_beamDiag", 0): "MC2_LIGHT_CONE_NODES",
    ("mclib/bdactor_update.cpp", "s_coneDiag", 0): "MC2_CONE_SPILL_COUPLING",
    ("mclib/bdactor_update.cpp", "s_reach", 0): "MC2_LIGHT_CONE_DIAG",
    ("mclib/camera_matrix.cpp", "s_rzTrace", 0): "MC2_REVERSE_Z_TRACE",
    ("mclib/camera_project.cpp", "s_lodChunkPick", 0): "MC2_TERRAIN_LOD_CHUNK",
    ("mclib/camera_project.cpp", "s_pickParity", 0): "MC2_TERRAIN_LOD_CHUNK",
    ("mclib/cone_glow.cpp", "s", 0): "MC2_CONE_POOL_POLICY",
    ("mclib/cone_glow.cpp", "s", 1): "MC2_LIGHT_CONE_GLOW_ALPHA",
    ("mclib/cone_glow.cpp", "s", 2): "MC2_LIGHT_CONE_SPILL",
    ("mclib/cone_glow.cpp", "s", 3): "MC2_LIGHT_REALISM",
    ("mclib/cone_glow.cpp", "s", 4): "MC2_LIGHT_REALISM_CONE_FEATHER",
    ("mclib/gosfx/tube.cpp", "s_tpOn", 0): "MC2_FX_FORCE_SPAWN",
    ("mclib/mapdata.cpp", "s_probe", 0): "MC2_COLORMAP_CPU_RETIRE",
    ("mclib/mech3d_render.cpp", "s_fxCountLog", 0): "MC2_DIAGF",
    ("mclib/mech3d_render.cpp", "s_mechLodTrace", 0): "MC2_OBJECT_ID_BUFFER",
    ("mclib/mech3d_render.cpp", "s_mrtNull", 0): "MC2_DIAGF",
    ("mclib/mech3d_update.cpp", "s_animCadGuard", 0): "MC2_DIAG_TAGS",
    ("mclib/quad_draw.cpp", "s_dbc", 0): "MC2_WINDOWED",
    ("mclib/quad_draw.cpp", "s_m2dPzParity", 0): "MC2_M2D_PZ_PARITY",
    ("mclib/terrain_geometry.cpp", "s_blockFrustumFallback", 0): "MC2_VPL_CULL",
    ("mclib/terrain_geometry.cpp", "s_fastpathDropLog", 0): "MC2_FASTPATH_DROP_LOG",
    ("mclib/terrain_geometry.cpp", "s_s6CostOn", 0): "MC2_GPU_DRIVEN_WATER",
    ("mclib/terrain_render.cpp", "s_gateDiag", 0): "MC2_RENDER_WATER_FASTPATH",
    ("mclib/terrain_render.cpp", "s_waterDebugOn", 0): "MC2_THIN_DEBUG",
    ("mclib/terrain_render.cpp", "s_waterDebugOn", 1): "MC2_GPU_DRIVEN_WATER",
    ("mclib/terrain_render.cpp", "s_waterS6Trace", 0): "MC2_THIN_DEBUG",
    ("mclib/terrain_update.cpp", "s_noSkirts", 0): "MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS",
    ("mclib/terrain_update.cpp", "s_noStitch", 0): "MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS",
    ("mclib/vk_object_light_gather.cpp", "s_lightClampProbe", 0): "MC2_LIGHT_CLAMP_FIXTURE",
    ("mclib/vk_object_light_gather.cpp", "s_lightRowCensus", 0): "MC2_LIGHT_POS_COLUMN_FIX",
    ("mclib/vk_object_light_gather.cpp", "s_spotDiagPackEnabled", 0): "MC2_LIGHT_CLAMP_FIXTURE",
}


def _gate_names():
    names = set()
    for p in (ROOT / "Gates").glob("gate_rows_*.inc"):
        names.update(re.findall(r'"(MC2_[A-Z0-9_]{4,})"', p.read_text(encoding="utf-8", errors="replace")))
    doc = ROOT / "docs" / "tier1_env_vars.md"
    names.update(re.findall(r"`(MC2_[A-Z0-9_]{4,})", doc.read_text(encoding="utf-8", errors="replace")))
    return names


def _source_files():
    return sorted(p for d in SCAN_DIRS for p in (ROOT / d).rglob("*.cpp"))


def _offenders(files=None, names=None):
    names = _gate_names() if names is None else names
    bare = {n[4:]: n for n in names}
    found = {}
    for path in (files if files is not None else _source_files()):
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        seen = {}
        rel = Path(path).relative_to(ROOT).as_posix() if Path(path).is_relative_to(ROOT) else Path(path).name
        for m in HARDCODED.finditer(text):
            nth = seen[m.group(1)] = seen.get(m.group(1), -1) + 1
            line = text.count("\n", 0, m.start())
            win = "\n".join(lines[max(0, line - WINDOW):line + WINDOW + 1])
            hit = sorted({t for t in NAME.findall(win) if t in names} |
                         {full for b, full in bare.items() if re.search(r"(?<![A-Za-z0-9_])" + b + r"(?![A-Za-z0-9_])", win)})
            if hit:
                found[(rel, m.group(1), nth)] = hit[0]
    return found


def test_scanner_flags_a_hardcoded_gate(tmp_path):
    f = tmp_path / "x.cpp"
    f.write_text("// MC2_FAKE_TRACE=1 logs things\nstatic const bool s_t =\n\tfalse;\n"
                 "static const bool s_ok = mc2::gate::get<\"MC2_FAKE_TRACE\">();\n"
                 + "\n" * (WINDOW + 5) + "static const bool s_unrelated = false; // no gate named\n", encoding="utf-8")
    assert _offenders([f], {"MC2_FAKE_TRACE"}) == {("x.cpp", "s_t", 0): "MC2_FAKE_TRACE"}


def test_second_same_named_constant_is_a_distinct_offender(tmp_path):
    f = tmp_path / "y.cpp"
    f.write_text("// MC2_FAKE_TRACE\nstatic const bool s_a = false;\nstatic const bool s_a = false;\n", encoding="utf-8")
    assert sorted(_offenders([f], {"MC2_FAKE_TRACE"})) == [("y.cpp", "s_a", 0), ("y.cpp", "s_a", 1)]


def test_scan_covers_nested_dirs():
    rels = {p.relative_to(ROOT).parts[0] for p in _source_files()}
    assert rels == set(SCAN_DIRS)
    assert any(len(p.relative_to(ROOT).parts) > 2 for p in _source_files())


def test_no_new_hardcoded_false_gates():
    found = _offenders()
    new = {k: v for k, v in found.items() if k not in KNOWN_OFFENDERS}
    assert not new, "hardcoded-false constant next to a documented gate (read the gate instead): " + repr(new)
    fixed = sorted(set(KNOWN_OFFENDERS) - set(found))
    assert not fixed, "fixed or removed; delete from KNOWN_OFFENDERS (ratchet only shrinks): " + repr(fixed)
