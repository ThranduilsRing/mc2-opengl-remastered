// tools/asset_viewer/ModWorkbenchPanel.cpp
// S2: stock vs override side-by-side panel with generation-sync.
// S3: warnings panel + appearance-verify control.
// S4: export draft bundle button.
// S5: LOD-chain table panel + central manifest merge.
#include "ModWorkbenchPanel.h"
#include "CentralManifestMerge.h"
#include "imgui.h"
#include <cctype>
#include <cfloat>
#include <cstring>
#include <string>
#include <utility>

void ModWorkbenchPanel::setDeployDir(const std::string& d) {
    stockPreview_.setDeployDir(d);
    overridePreview_.setDeployDir(d);
    deployDir_ = d;
    roster_.load(d);
}

void ModWorkbenchPanel::syncMeshes(ModWorkbench& wb) {
    if (wb.hasStock())    stockPreview_.setMeshData(wb.stockMesh());
    if (wb.hasOverride()) overridePreview_.setMeshData(wb.overrideMesh());
}

void ModWorkbenchPanel::draw(ModWorkbench& wb, const ImVec2& avail) {
    ImGui::TextUnformatted("Mod Workbench \xe2\x80\x94 drag a .glb/.gltf onto the window");
    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f),
        "Preview validates geometry/package. In-game lighting/material may differ (Backend A = v2).");
    ImGui::Separator();

    {
        auto& hrec = wb.record();
        int hblocks = 0, hwarns = 0;
        for (const auto& w : wb.warnings())
            (w.severity == WarnSeverity::Block ? hblocks : hwarns)++;
        ImGui::TextUnformatted("Health:");
        ImGui::SameLine();
        ImGui::Text("Key: %s:%s  Stock: %s  Appearance: %s  LODs: %zu  Validation: %d[B] %d[W]",
            hrec.overrideClass.c_str(),
            hrec.appearanceName.empty()? "--" : hrec.appearanceName.c_str(),
            wb.hasStock()? "ok" : "--",
            hrec.appearanceName.empty()? "--" : (hrec.appearanceVerified? "roster" : "free"),
            hrec.lods.size(), hblocks, hwarns);
        ImGui::Separator();
    }

    if (!wb.hasOverride()) {
        ImGui::TextDisabled("No override loaded. %s", wb.lastError().c_str());
        return;
    }

    ImGui::Text("Override: %s", wb.overridePath().c_str());

    ImGui::TextUnformatted("Stock prop (click to bind):");
    // The browser fills its parent; bound it so the loaded GLB stays visible.
    ImGui::BeginChild("stockPicker", ImVec2(0, 140), true);
    stockBrowser_.draw();   // filter box + scrollable .tgl list; tooltip shows full path
    ImGui::EndChild();
    if (stockBrowser_.hasSelection()) {
        std::string pick = stockBrowser_.takeSelection();
        wb.bindStock(pick);
    }
    if (wb.hasStock())
        ImGui::TextDisabled("bound stock: %s", wb.stockMesh().ok ? "ok" : "--");

    if (wb.generation() != lastSyncedGen_) {
        syncMeshes(wb);
        lastSyncedGen_ = wb.generation();
        appe_[0] = '\0';   // reprime appearance buffer from the (reset/suggested) record
    }

    // A lone GLB keeps its fitted camera until there is stock to compare.
    if (wb.hasStock()) {
        overridePreview_.orbitYaw()   = stockPreview_.orbitYaw();
        overridePreview_.orbitPitch() = stockPreview_.orbitPitch();
        overridePreview_.zoom()       = stockPreview_.zoom();
    }

    float half = (avail.x - 8.0f) * 0.5f;
    float h    = avail.y * 0.55f;

    ImGui::BeginChild("stock", ImVec2(half, h), true);
    ImGui::TextUnformatted("Stock");
    stockPreview_.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("override", ImVec2(half, h), true);
    ImGui::TextUnformatted("Override");
    overridePreview_.draw(ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    if (wb.hasStock()) {
        auto d = wb.computeDelta();
        ImGui::Text("footprint ratio (override/stock, max axis): %.2fx", d.maxRatio);
        ImGui::Text("pivot offset (GL): %.2f, %.2f, %.2f",
            d.pivotOffset[0], d.pivotOffset[1], d.pivotOffset[2]);
    }

    auto& rec = wb.record();
    if (appe_[0]=='\0' && !rec.appearanceName.empty()) strncpy(appe_, rec.appearanceName.c_str(), sizeof(appe_)-1);
    // Free-type field; verified state is DERIVED (roster pick or roster match = true).
    if (ImGui::InputText("Appearance key", appe_, sizeof(appe_))) {
        rec.appearanceName = appe_;
        rec.appearanceVerified = roster_.contains(rec.appearanceName);
    } else {
        rec.appearanceName = appe_;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh roster")) roster_.refresh(deployDir_);
    ImGui::TextDisabled("appearance source: %d data/tgl/*.ini scanned, %zu keys%s",
        roster_.scannedFileCount(), roster_.names().size(),
        rec.appearanceVerified ? "  [verified: roster match]" : "  [unverified: not in roster]");
    // Filterable roster picker.
    ImGui::InputText("filter##appearance", apFilter_, sizeof(apFilter_));
    if (ImGui::BeginListBox("##appearance_roster", ImVec2(-FLT_MIN, 120))) {
        std::string lf; for (char* p = apFilter_; *p; ++p) lf += (char)std::tolower((unsigned char)*p);
        for (const auto& n : roster_.names()) {
            std::string ln; for (char c : n) ln += (char)std::tolower((unsigned char)c);
            if (!lf.empty() && ln.find(lf) == std::string::npos) continue;
            if (ImGui::Selectable(n.c_str(), n == rec.appearanceName)) {
                strncpy(appe_, n.c_str(), sizeof(appe_)-1); appe_[sizeof(appe_)-1]='\0';
                rec.appearanceName = n;
                rec.appearanceVerified = true;          // explicit roster pick
            }
        }
        ImGui::EndListBox();
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Textures (assign to surface missing-texture warnings):");
    for (auto& slot : wb.textureSlots()) {
        char buf[260]; strncpy(buf, slot.path.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
        ImGui::PushID(slot.label.c_str());
        if (ImGui::InputText(slot.label.c_str(), buf, sizeof(buf))) slot.path = buf;
        ImGui::PopID();
    }
    wb.revalidateWithTextures();
    ImGui::Separator(); ImGui::TextUnformatted("Warnings:");
    if (wb.warnings().empty()) ImGui::TextDisabled("  none");
    for (const auto& w : wb.warnings()){
        bool blk = w.severity==WarnSeverity::Block;
        ImGui::TextColored(blk?ImVec4(1,0.4f,0.4f,1):ImVec4(1,0.8f,0.3f,1),
                           "  [%s] %s", blk?"BLOCK (mirror, advisory)":"WARN", w.message.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("LOD chain (LOD0 = the dropped GLB; add lower-detail entries):");
    {
        auto& lods = rec.lods;
        if (ImGui::BeginTable("lods", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("LOD"); ImGui::TableSetupColumn("Source GLB");
            ImGui::TableSetupColumn("Distance"); ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            int removeIdx = -1, moveUp = -1, moveDown = -1;
            for (int i = 0; i < (int)lods.size(); ++i) {
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", lods[i].lod);
                ImGui::TableSetColumnIndex(1);
                char sb[260]; strncpy(sb, lods[i].sourceRelPath.c_str(), sizeof(sb)-1); sb[sizeof(sb)-1]='\0';
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##src", sb, sizeof(sb))) lods[i].sourceRelPath = sb;
                ImGui::TableSetColumnIndex(2);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputFloat("##dist", &lods[i].distance, 0,0,"%.1f");
                ImGui::TableSetColumnIndex(3);
                bool ascOk = (i == 0) || (lods[i].distance > lods[i-1].distance);
                ImGui::TextColored(ascOk?ImVec4(0.6f,1,0.6f,1):ImVec4(1,0.5f,0.5f,1), ascOk?"ok":"order");
                ImGui::TableSetColumnIndex(4);
                if (ImGui::SmallButton("Up"))   moveUp = i; ImGui::SameLine();
                if (ImGui::SmallButton("Dn"))   moveDown = i; ImGui::SameLine();
                if (ImGui::SmallButton("X"))    removeIdx = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (removeIdx >= 0) lods.erase(lods.begin()+removeIdx);
            if (moveUp > 0)               std::swap(lods[moveUp], lods[moveUp-1]);
            if (moveDown >= 0 && moveDown+1 < (int)lods.size()) std::swap(lods[moveDown], lods[moveDown+1]);
            // Reindex lod by row order: entries start at 1 (LOD0 = record.source).
            for (int i = 0; i < (int)lods.size(); ++i) lods[i].lod = i + 1;
        }
        if (ImGui::Button("Add LOD")) rec.lods.push_back({(int)rec.lods.size()+1, "", 0.0f});
    }

    ImGui::Separator();
    static char bundleId[128]="my_override";
    static char outRoot[260]="data/model_overrides";
    static std::string exportMsg;
    ImGui::InputText("Bundle id", bundleId, sizeof(bundleId));
    ImGui::InputText("Out root (model_overrides dir)", outRoot, sizeof(outRoot));
    ImGui::BeginDisabled(wb.hasBlocking());
    if (ImGui::Button("Export draft bundle")){
        ExportResult r=wb.exportBundle(outRoot,bundleId);
        exportMsg=(r.ok?"OK: ":"FAILED: ")+r.message;
    }
    ImGui::EndDisabled();
    if (!exportMsg.empty()) ImGui::TextWrapped("%s", exportMsg.c_str());
    ImGui::TextDisabled("Writes <out>/<id>/{model.glb, models.generated.json}. Does NOT edit your central models.json.");

    ImGui::Separator();
    static char centralPath[260] = "data/model_overrides/models.json";
    static std::string mergeMsg;
    ImGui::InputText("Central models.json", centralPath, sizeof(centralPath));
    ImGui::BeginDisabled(wb.hasBlocking());
    if (ImGui::Button("Append/Merge to Central models.json")) {
        MergeResult r = MergeIntoCentralManifest(centralPath, wb.record());
        mergeMsg = (r.ok ? "OK: " : "FAILED: ") + r.message;
    }
    ImGui::EndDisabled();
    if (!mergeMsg.empty()) ImGui::TextWrapped("%s", mergeMsg.c_str());
    ImGui::TextDisabled("Writes a .bak first, preserves your other overrides, round-trip verified. Draft export above stays available.");
}
