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


def test_options_draft_reload_and_failed_save_contract():
    # drawOptions owns local ImGui state, outside the preferences unit harness.
    source = (ROOT / "GuiRuntime/PlayerFrontEnd.cpp").read_text(encoding="utf-8")
    body = source.split("void drawOptions() {", 1)[1].split('ImGui::TextWrapped("SMAA', 1)[0]
    body = re.sub(r"\s+", " ", body)
    for declaration in ("static mc2::options::RenderOptions draft;",
                        "static std::optional<mc2::options::RenderOptions> seen;",
                        "static bool dirty = false;"):
        assert declaration in body
    assert ('if (!seen || (!dirty && *seen != mc2::options::current())) { '
            'draft = {}; mc2::options::cfg::loadFromText(draft, '
            'mc2::options::cfg::readTextFile("options.cfg")); '
            'seen = mc2::options::current(); }') in body
    # Failure preserves dirty/draft for retry; only successful publication syncs seen.
    assert ('dirty |= changed; const bool committed = mc2::options::cfg::editCommitted('
            'changed, ImGui::IsAnyItemActive(), wasEditing); '
            'if (ImGui::Button("Save player settings") || (dirty && committed)) { '
            'if (!saveRenderOptions || !saveRenderOptions(draft)) '
            'error = "Could not save options.cfg."; else { '
            'mc2::options::publish(draft); seen = mc2::options::current(); dirty = false;') in body
    assert body.count("dirty = false;") == 2  # initialization and successful save only
