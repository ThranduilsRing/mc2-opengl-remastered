"""Pins the detail-normal layer indices in terrain_tier1.frag against shaders/include/terrain_mat_layers.hglsl, and that every
option with a live-saving widget in GraphicsOptionsWindow.cpp is owned there (no stale draft widget in drawOptions)."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def defines(path):
    return {m[0]: int(m[1]) for m in re.findall(r"^#define\s+(MAT_LAYER_\w+)\s+(\d+)", path.read_text(encoding="utf-8"), re.M)}


def test_frag_normal_layer_indices_match_header():
    frag = defines(ROOT / "shaders/vulkan/terrain_tier1.frag")
    header = defines(ROOT / "shaders/include/terrain_mat_layers.hglsl")
    assert "MAT_LAYER_FOREST" in header and "MAT_LAYER_FOREST" in frag
    for name, idx in frag.items():
        assert header.get(name) == idx, f"{name}: frag {idx} vs header {header.get(name)}"


def test_front_end_edits_persisted_options_without_a_stale_draft():
    front = (ROOT / "GuiRuntime/PlayerFrontEnd.cpp").read_text(encoding="utf-8")
    options = front.split("void drawOptions() {", 1)[1].split("int restartIfRequested", 1)[0]
    assert "static mc2::options::RenderOptions" not in options
    assert "cfg::loadFromText(draft, cfg)" in options
    assert "isLiveWidgetKey" not in front
    assert "mc2::options::current()" not in options  # excludes test overrides from Save
    assert 'ImGui::Button("Save player settings") || changed' in options
    assert "saveRenderOptions(draft)" in options and "mc2::options::publish(draft)" in options
