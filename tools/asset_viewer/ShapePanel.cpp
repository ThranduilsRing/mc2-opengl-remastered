// tools/asset_viewer/ShapePanel.cpp
#include "ShapePanel.h"
#include "MeshPreview3D.h"
#include "imgui.h"
#include <cctype>
#include <cstdio>

static std::string lowerS(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

void ShapePanel::ensureBuilt(State& s, ShapeCategory cat) {
    if (s.built) return;
    s.built = true;   // once, regardless of success
    std::vector<std::string> names;
    if (TglMeshLoader::ensureFastFile(".")) names = TglMeshLoader::listTgl();
    s.entries = BuildShapeCatalog(cat, ".", names);
    refilter(s);
}

void ShapePanel::refilter(State& s) {
    s.shown.clear();
    const std::string needle = lowerS(s.filter);
    for (int i = 0; i < (int)s.entries.size(); ++i)
        if (needle.empty() || lowerS(s.entries[i].name).find(needle) != std::string::npos ||
            lowerS(s.entries[i].origin).find(needle) != std::string::npos)
            s.shown.push_back(i);
}

void ShapePanel::pick(State& s, int entryIdx) {
    s.selected = entryIdx;
    s.variant = 0;
    s.stats.clear();
    for (const auto& v : s.entries[entryIdx].variants) s.stats.push_back(MeasureMesh(LoadShapeVariant(v)));
    s.dirty = true;
}

void ShapePanel::select(ShapeCategory cat, const std::string& name) {
    State& s = st(cat);
    ensureBuilt(s, cat);
    const std::string n = lowerS(name);
    for (int i = 0; i < (int)s.entries.size(); ++i)
        if (lowerS(s.entries[i].name) == n) { pick(s, i); return; }
}

void ShapePanel::drawBrowser(ShapeCategory cat) {
    State& s = st(cat);
    ensureBuilt(s, cat);
    if (s.entries.empty()) {
        ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "No shapes found in this install");
        return;
    }
    if (ImGui::InputText("##shapefilter", s.filter, sizeof(s.filter))) refilter(s);
    ImGui::SameLine();
    if (ImGui::Button("x")) { s.filter[0] = '\0'; refilter(s); }
    ImGui::Text("%d / %d shapes", (int)s.shown.size(), (int)s.entries.size());
    ImGui::Separator();
    ImGui::BeginChild("##shapelist", ImVec2(0, 0), false);
    for (int i : s.shown) {
        const ShapeEntry& e = s.entries[i];
        char label[256];
        std::snprintf(label, sizeof(label), "%s  [%s]##%d", e.name.c_str(), e.origin.c_str(), i);
        if (ImGui::Selectable(label, s.selected == i)) pick(s, i);
    }
    ImGui::EndChild();
}

void ShapePanel::drawDetails(ShapeCategory cat, MeshPreview3D& preview) {
    State& s = st(cat);
    if (s.selected < 0) {
        ImGui::TextDisabled("Select a shape on the left.");
        return;
    }
    const ShapeEntry& e = s.entries[s.selected];
    if (s.dirty) {
        s.dirty = false;
        MeshData md = LoadShapeVariant(e.variants[s.variant]);
        preview.setMeshData(md);
        if (e.variants[s.variant].glb) preview.zoom() *= 1.9f;   // GLB bounds frame too tight under the default camera
        preview.modelRotDeg()[0] = e.variants[s.variant].glb ? 180.0f : -90.0f;   // GLB meshes come out upside-down under the stand-up rotation
    }
    ImGui::Text("%s", e.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", e.origin.c_str());
    if (ImGui::BeginTable("##variants", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Variant");
        ImGui::TableSetupColumn("Triangles");
        ImGui::TableSetupColumn("Vertices");
        ImGui::TableSetupColumn("Parts");
        ImGui::TableSetupColumn("Textures");
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)e.variants.size(); ++i) {
            const ShapeStats& t = s.stats[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[160];
            std::snprintf(lbl, sizeof(lbl), "%s##v%d", e.variants[i].role.c_str(), i);
            if (ImGui::Selectable(lbl, s.variant == i, ImGuiSelectableFlags_SpanAllColumns) && s.variant != i) {
                s.variant = i;
                s.dirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.variants[i].path.c_str());
            if (!t.ok) {
                ImGui::TableNextColumn();
                ImGui::TextColored({1.0f, 0.4f, 0.3f, 1.0f}, "%s", t.error.c_str());
                continue;
            }
            ImGui::TableNextColumn(); ImGui::Text("%d", t.tris);
            ImGui::TableNextColumn(); ImGui::Text("%d", t.verts);
            ImGui::TableNextColumn(); ImGui::Text("%d", t.parts);
            ImGui::TableNextColumn();
            if (t.textures.empty()) ImGui::TextDisabled("-");
            else {
                std::string joined;
                for (const auto& n : t.textures) joined += (joined.empty() ? "" : ", ") + n;
                ImGui::TextUnformatted(joined.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    preview.setCompactControls(true);
    preview.draw(ImGui::GetContentRegionAvail());
    preview.setCompactControls(false);
}
