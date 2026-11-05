// EditorInspector: the Object Inspector section drawers drawImGui calls -- renderer features, copy-all, render explain,
// the per-kind panels, the pass sections and the material and gate tables. The orchestrator is in EditorInspector.cpp.
#include "EditorInspector_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"
#include <span>

namespace EditorInspector {

// Draw the Renderer Features window.
void drawFeaturesWindow() {
    if (s_featuresOpen) {
        ImGui::SetNextWindowSize(ImVec2(600, 440), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Renderer Features", &s_featuresOpen)) {
            ImGui::TextUnformatted("MC2_* feature gates  --  Ctrl+Shift+F to close");
            ImGui::TextUnformatted("Hover 'Feature' column for doc string.");
            ImGui::Separator();

            if (ImGui::BeginTable("##feats", 4,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                    | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Feature",
                    ImGuiTableColumnFlags_WidthFixed, 210.f);
                ImGui::TableSetupColumn("Env var",
                    ImGuiTableColumnFlags_WidthFixed, 210.f);
                ImGui::TableSetupColumn("Default",
                    ImGuiTableColumnFlags_WidthFixed,  52.f);
                ImGui::TableSetupColumn("Status",
                    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (const auto & d : RenderCore::kFeatureTable) {
                    ImGui::TableNextRow();

                    // Feature column — hover for doc string.
                    ImGui::TableSetColumnIndex(0);
                    if (d.kind == RenderCore::EnvVarKind::Retired)
                        ImGui::TextDisabled("%s", d.featureId);
                    else
                        ImGui::TextUnformatted(d.featureId);
                    if (ImGui::IsItemHovered() && d.doc)
                        ImGui::SetTooltip("%s", d.doc);

                    // Env var column.
                    ImGui::TableSetColumnIndex(1);
                    if (d.envVar)
                        ImGui::TextDisabled("%s", d.envVar);
                    else
                        ImGui::TextDisabled("(none)");

                    // Default column.
                    ImGui::TableSetColumnIndex(2);
                    if (d.kind == RenderCore::EnvVarKind::Retired)
                        ImGui::TextDisabled("--");
                    else if (!d.envVar)
                        ImGui::TextDisabled("ON");
                    else
                        ImGui::TextDisabled(d.defaultOn ? "ON" : "off");

                    // Status column.
                    ImGui::TableSetColumnIndex(3);
                    if (d.kind == RenderCore::EnvVarKind::Retired) {
                        ImGui::TextDisabled("[retired]");
                    } else if (!d.envVar) {
                        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "always-on");
                    } else {
                        const auto st = mc2::gate::boolState(d.envVar);   // the name comes from the registry table
                        bool on;
                        const char* src;
                        if (!st || !st->set) {
                            on  = d.defaultOn;
                            src = d.defaultOn ? "on (default)" : "off (default)";
                        } else if (!st->on) {
                            on  = false;
                            src = "off (forced)";
                        } else {
                            on  = true;
                            src = "on (forced)";
                        }
                        if (on)
                            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "%s", src);
                        else
                            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f), "%s", src);
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
}

// Draw the save-camera-bookmark and save-camera-view buttons.
void drawCameraBookmarkSave() {
    {
        static bool s_hzbSaved = false;
        static char s_bookmarkPath[512] = {0};
        static bool s_hzbFailed = false;
        ImGui::Spacing();
        // Primary: bookmark-schema JSON save.
        // Directory = MC2_BOOKMARK_SAVE_DIR or ./saved_bookmarks; the file is
        // directly consumable by MC2_VISUAL_BOOKMARK_CAPTURE.
        if (ImGui::Button("Save Camera Bookmark")) {
            mc2_save_camera_bookmark(s_bookmarkPath, static_cast<int>(sizeof(s_bookmarkPath)));
        }
        if (s_bookmarkPath[0]) {
            ImGui::TextDisabled("wrote: %s", s_bookmarkPath);
        }
        // Secondary: legacy HZB text dump (MC2_HZB_VIEW_FILE replay format).
        if (ImGui::Button("Save Camera View (HZB)")) {
            if (mc2_hzb_dump_camera_view() == 1) {
                s_hzbSaved = true;
                s_hzbFailed = false;
            } else {
                s_hzbSaved = false;
                s_hzbFailed = true;
            }
        }
        if (s_hzbSaved) {
            ImGui::SameLine();
            ImGui::TextDisabled("saved!");
        } else if (s_hzbFailed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "save FAILED (see stderr / path)");
        }
        ImGui::Spacing();
    }
}

// Draw the Copy All button and copy the inspector text to the clipboard.
void drawCopyAll(const RenderWorld::LookupResult& lk) {
    if (ImGui::Button("Copy All")) {
        char buf[3072];
        int  n = 0;
        n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "ObjID raw: 0x{:08X}  valid: {}{}{}\n", mc2::fmt::as_u32(lk.rawObjectId), mc2::fmt::safe(lk.isValid ? "yes" : "no"), mc2::fmt::safe((!lk.isValid && lk.lookupFailReason) ? "  reason: " : ""), mc2::fmt::safe((!lk.isValid && lk.lookupFailReason) ? lk.lookupFailReason : ""));
        n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "Kind: {}\n", mc2::fmt::safe(pickKindName(s_selection.pickKind)));
        n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "Handle: 0x{:08X}  idx {:d} / gen {:d}\n", mc2::fmt::as_u32(s_selection.handle.raw()), mc2::fmt::as_u32(s_selection.handle.index()), mc2::fmt::as_u32(s_selection.handle.generation()));
        if (lk.worldPosValid)
            n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "World: ({:.1f}, {:.1f}, {:.1f})\n", lk.worldX, lk.worldY, lk.worldZ);
        n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "Mesh: 0x{:08X}  Mat: 0x{:08X}  LOD: {:d}  Pipeline: {:d}\nDrawPacket: {:d}  PathReason: 0x{:08X}  GameObjID: {:d}\n", mc2::fmt::as_u32(lk.meshHandleBits), mc2::fmt::as_u32(lk.materialHandleBits), mc2::fmt::as_u32(static_cast<unsigned>(lk.lodLevel)), mc2::fmt::as_u32(static_cast<unsigned>(lk.pipelineId)), mc2::fmt::as_u32(lk.drawPacketIndex), mc2::fmt::as_u32(lk.pathReasonCode), mc2::fmt::as_u32(lk.gameObjectId));
        if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                && s_staticPropData.populated) {
            n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "RecipeIdx: {:d}  Shape: {}\n", mc2::fmt::as_i32(s_staticPropData.recipeIndex), mc2::fmt::safe(s_staticPropData.shapeName[0] ? s_staticPropData.shapeName : "(unknown)"));
            if (s_staticPropData.materialGpuPopulated) {
                const auto& mg = s_staticPropData.materialGpu;
                n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "MatIdx: {:d}  albedo: {:d}  normal: {:d}  mrTex: {:d}  emit: {:d}\nflags: 0x{:08X}  baseColor: {:.3f}  metallic: {:.3f}  rough: {:.3f}\n", mc2::fmt::as_u32(s_staticPropData.materialIdx), mc2::fmt::as_u32(mg.albedoTex), mc2::fmt::as_u32(mg.normalTex), mc2::fmt::as_u32(mg.metallicRoughnessTex), mc2::fmt::as_u32(mg.emissiveTex), mc2::fmt::as_u32(mg.flags), mg.baseColorFactor, mg.metallicFactor, mg.roughnessFactor);
            }
        }
        if (s_selection.kind == RenderWorld::RenderObjectKind::Mech
                && s_mechData.populated) {
            static const char* cnames[] = { "None","Light","Medium","Heavy","Assault" };
            static const char* snames[] = {
                "None","Sensor Q1","Sensor Q2","Sensor Q3","Sensor Q4","Visual"
            };
            const int ci  = (s_mechData.chassisClass >= 0 && s_mechData.chassisClass < 5)
                            ? s_mechData.chassisClass : 0;
            const int csi = (s_mechData.conStat >= 0 && s_mechData.conStat < 6)
                            ? s_mechData.conStat : 0;
            n += mc2::fmt::to_buf_n(buf+n, sizeof(buf)-n, "Variant: {}  Name: {}\nClass: {}  TeamID: {:d}  Pilot: {}\nStatus: {}{}{}\nSensor: {}\nArmor: {:.0f} / {:.0f}  Structure: {:.0f} / {:.0f}\n", mc2::fmt::safe(s_mechData.variantName[0] ? s_mechData.variantName : "(none)"), mc2::fmt::safe(s_mechData.longName[0]    ? s_mechData.longName    : "(none)"), mc2::fmt::safe(cnames[ci]), mc2::fmt::as_i32(s_mechData.teamId), mc2::fmt::safe(s_mechData.pilotName[0]   ? s_mechData.pilotName   : "(none)"), mc2::fmt::safe(s_mechData.destroyed ? "DESTROYED " : ""), mc2::fmt::safe(s_mechData.disabled  ? "disabled "  : ""), mc2::fmt::safe((!s_mechData.destroyed && !s_mechData.disabled && !s_mechData.crippled)
                    ? "OK" : (s_mechData.crippled ? "crippled" : "")), mc2::fmt::safe(snames[csi]), s_mechData.totalCurArmor, s_mechData.totalMaxArmor, s_mechData.totalCurStr, s_mechData.totalMaxStr);
        }
        ImGui::SetClipboardText(buf);
    }
}

// Draw the render-explain section for a valid render-world pick.
void drawRenderExplain(const RenderWorld::LookupResult& lk) {
    if (s_selection.valid) {
        if (ImGui::CollapsingHeader("Render Explain##re", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Object
            const char* kindStr = (s_selection.kind == RenderWorld::RenderObjectKind::Mech)
                                  ? "Mech" : "StaticProp";
            ImGui::Text("Object:  %s  handle 0x%08X", kindStr, s_selection.handle.raw());

            // Fresh handle-direct probe — independent of pixel-pick data age.
            RenderWorld::ObjectRecordView rv{};
            const bool rvOk = RenderWorld::getObjectRecordView(s_selection.handle, &rv);
            if (!rvOk) {
                ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f),
                                   "  (handle stale -- object may be destroyed)");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Path:");

            // DrawPacket dispatch status (StaticProp only).
            if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp) {
                const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
                if (rs.valid) {
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f),
                                       "  dispatch       DrawPacket default ON");
                } else {
                    ImGui::TextDisabled("  dispatch       (no Render Spine data -- pick not resolved?)");
                }
            } else {
                ImGui::TextDisabled("  dispatch       (Mech batcher path)");
            }

            // Pipeline name. Prefer Render Spine row (authoritative) over stale
            // lk.pipelineId (which is 0/unknown for static props in the lookup).
            {
                const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
                const uint32_t pipeId =
                    (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                     && rs.valid && rs.rowCount > 0)
                    ? rs.rows[0].pipelineId
                    : static_cast<uint32_t>(lk.pipelineId);
                char pipeBuf[48];
                switch (pipeId) {
                    case 0:  mc2::fmt::to_buf(pipeBuf, "(unknown)"); break;
                    case 1:  mc2::fmt::to_buf(pipeBuf, "StaticPropOpaque (1)"); break;
                    case 2:  mc2::fmt::to_buf(pipeBuf, "StaticPropAlphaTest (2)"); break;
                    default: mc2::fmt::to_buf(pipeBuf, "?({:d})", mc2::fmt::as_u32(pipeId)); break;
                }
                ImGui::Text("  pipeline       %s", pipeBuf);
            }

            // Material summary (StaticProp + GPU material populated).
            if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                    && s_staticPropData.populated
                    && s_staticPropData.materialGpuPopulated) {
                const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
                const int32_t layer = (rs.valid && rs.rowCount > 0)
                                      ? rs.rows[0].texArrayLayer : -1;
                const bool matchOk  = (rs.valid && rs.rowCount > 0)
                                      && (rs.rows[0].albedoTex != 0xFFFFFFFFu)
                                      && (layer >= 0)
                                      && rs.rows[0].materialMatchesLegacy;
                char matBuf[128];
                mc2::fmt::to_buf(matBuf, "MaterialGpu  idx {:d}  albedo {:d}  layer {:d}  {}", mc2::fmt::as_u32(s_staticPropData.materialIdx), mc2::fmt::as_u32(s_staticPropData.materialGpu.albedoTex), mc2::fmt::as_i32(layer), mc2::fmt::safe(matchOk ? "OK" : "(mismatch)"));
                ImGui::Text("  material       %s", matBuf);
            } else if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp) {
                ImGui::TextDisabled("  material       (no GPU material data)");
            }

            // Factual lighting-model labels. These are descriptive of the
            // shader; if the static_prop .frag adds normal/PBR/IBL/emissive
            // sampling, update these strings in lockstep.
            if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp) {
                // Lighting model — directional + ambient via LightsData
                // SSBO 20 (see lighting.hglsl:43-56 + calc_light()).
                ImGui::Text("  lighting       per-vertex Gouraud (no PBR)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("calc_light() in shaders/include/lighting.hglsl\n"
                                      "drives AMBIENT/INFINITE/POINT/SPOT.\n"
                                      "No specular, no PBR, no IBL.");
                // Normal source — vertex-interpolated only; .frag does NOT
                // sample MaterialGpu::normalTex even when the bit is set.
                ImGui::Text("  normal source  vertex-interpolated (no normal map)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("static_prop.frag uses normalize(v_normal)\n"
                                      "only for the GBuffer1 write. normalTex is\n"
                                      "declared in MaterialGpu but unused today.");

                // Hemisphere ambient fill mode (default ON).
                // Read the published option (this panel only runs when
                // MC2_IMGUI_INSPECTOR is on).
                bool ambOn = mc2::options::current().staticPropAmbient;
                ImGui::Text("  ambient        %s", ambOn ? "hemisphere_v1" : "off");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("V-AMBIENT-STATIC-1 hemisphere ambient fill in\n"
                                      "static_prop.vert (skips window-flag nodes).\n"
                                      "options.cfg: StaticPropAmbient = TRUE.\n"
                                      "OFF -> u_ambientV1Strength=0.0 (byte-identical).");

                // Interactive debug view combo for StaticPropOpaque.
                {
                    // Use canonical helpers to avoid duplicating the mapping table.
                    RenderDebugView curView = StaticPropShaderModeToView(batcher_getDebugMaterialMode());
                    const char* curName = RenderDebugViewName(curView);
                    if (ImGui::BeginCombo("Debug View##sp", curName)) {
                        for (int i = 0; i < static_cast<int>(RenderDebugView::_Count); ++i) {
                            RenderDebugView v = RenderDebugView(i);
                            if (!RenderDebugViewSupported(v, kDebugViewMask_StaticPropOpaque))
                                continue;
                            bool selected = (v == curView);
                            if (ImGui::Selectable(RenderDebugViewName(v), selected)) {
                                batcher_setDebugMaterialMode(StaticPropViewToShaderMode(v));
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", RenderDebugViewDescription(v));
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("V-MATERIAL-DEBUG-1 debug view for StaticPropOpaque.\n"
                                          "Runtime selection only; starts at Final.");
                }

                // SH-L2 image-based ambient on the StaticPropOpaque lane.
                // Default-ON: unset env -> gate ON, uploads g_iblShStrength
                // (default 0.5). Explicit "=0" -> gate OFF -> uploads 0.0
                // -> shader `if (u_iblShStrength > 0.0)` short-circuits to
                // byte-identical pre-flip OFF behavior. ImGui slider below
                // tunes runtime strength when gate is on.
                // MUST match s_iblShEnabled lambda in gos_static_prop_batcher.cpp.
                bool iblOn = mc2::options::current().staticPropIblSh;
                if (iblOn) {
                    // Surface active per-mission SH set name.
                    const char* shSetName = ibl_sh_runtime_currentSetName();
                    ImGui::Text("  ibl sh         on (set=%s, strength=%.2f)",
                                shSetName ? shSetName : "default",
                                g_iblShStrength);
                } else {
                    ImGui::Text("  ibl sh         off (env-gated)");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("V-IBL-STATIC-1 SH-L2 image-based ambient in\n"
                                      "static_prop.vert (skips window-flag nodes).\n"
                                      "options.cfg: StaticPropIblSh = TRUE.\n"
                                      "Slider modulates strength when env=1.\n"
                                      "OFF -> u_iblShStrength=0.0 (byte-identical).");
                // Tunables (IBL SH strength + PBR V1 strength + roughness
                // override) live in Graphics Options > Static Prop Tuning.
                // This selection-driven block keeps the per-prop status text
                // for diagnostic readability; the sliders themselves are not
                // duplicated here.

                // Per-vertex Schlick-Fresnel + power-lobe
                // specular status (read-only here; slider in Graphics Options).
                bool pbrOn = mc2::options::current().staticPropPbr;
                if (pbrOn) {
                    ImGui::Text("  pbr v1         on (strength=%.2f)",
                                g_pbrV1Strength);
                } else {
                    ImGui::Text("  pbr v1         off (env-gated)");
                }

                // Roughness-override status.
                if (pbrOn) {
                    ImGui::Text("  pbr rough      %s (override=%.2f)",
                                g_pbrV1RoughnessOverrideEnabled
                                    ? "on" : "off",
                                g_pbrV1RoughnessOverrideValue);
                } else {
                    ImGui::Text("  pbr rough      off (env-gated)");
                }
                ImGui::TextDisabled("  (tuning sliders: Graphics Options > Static Prop Tuning)");
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Fallbacks:");

            // Key kill-switches / feature gates relevant to the static-prop path.
            struct FallbackRow { const char* label; const char* envVar; bool defaultOn; };
            static const FallbackRow kFallbacks[] = {
                { "material sample",     "MC2_MATERIAL_GPU_SAMPLE",         true  },
                { "object-ID buffer",    "MC2_OBJECT_ID_BUFFER",            true  },
            };
            const auto showRow = [](std::string_view label, bool on) {
                ImGui::Text("  %-22.*s", static_cast<int>(label.size()), label.data());
                ImGui::SameLine(0.f, 4.f);
                if (on)
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "ON");
                else
                    ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f), "OFF");
            };
            for (const auto& fb : kFallbacks) {
                const auto st = mc2::gate::boolState(fb.envVar);
                showRow(fb.label, (!st || !st->set) ? fb.defaultOn : st->on);
            }
            // The three static-prop lighting rows are options.cfg settings, not environment gates.
            showRow("ambient v1", mc2::options::current().staticPropAmbient);
            showRow("ibl sh",     mc2::options::current().staticPropIblSh);
            showRow("pbr v1",     mc2::options::current().staticPropPbr);
        }
    }
}

// Draw the lookup-result section.
void drawLookup(const RenderWorld::LookupResult& lk) {
    if (ImGui::CollapsingHeader("Lookup", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Mesh bits:     0x%08X", lk.meshHandleBits);
        ImGui::Text("Material bits: 0x%08X", lk.materialHandleBits);
        ImGui::Text("LOD:           %u",     static_cast<unsigned>(lk.lodLevel));
        ImGui::Text("Pipeline:      %u",     static_cast<unsigned>(lk.pipelineId));
        ImGui::Text("Draw packet:   %u",     lk.drawPacketIndex);
        ImGui::Text("Path reason:   0x%08X", lk.pathReasonCode);
        ImGui::Text("Game obj ID:   %u",     lk.gameObjectId);
    }
}

// Draw the static-prop game data section.
void drawStaticPropData() {
    if (ImGui::CollapsingHeader("StaticProp", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!s_staticPropData.populated) {
            ImGui::TextUnformatted("(no game data -- pick not resolved)");
        } else {
            ImGui::Text("Recipe idx: %d", s_staticPropData.recipeIndex);
            ImGui::Text("Shape:      %s",
                s_staticPropData.shapeName[0] ? s_staticPropData.shapeName : "(unknown)");
        }
    }
}

// Draw the static-prop render spine section.
void drawRenderSpine() {
    if (ImGui::CollapsingHeader("Render Spine##rs", ImGuiTreeNodeFlags_DefaultOpen)) {
        const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
        if (!rs.valid) {
            ImGui::TextDisabled("(not populated -- pick not resolved or no packet data)");
        } else {
            char buf[64];

            // Header summary
            if (ImGui::BeginTable("##rssum", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("##rsk", ImGuiTableColumnFlags_WidthFixed, 100.f);
                ImGui::TableSetupColumn("##rsv", ImGuiTableColumnFlags_WidthStretch);

                auto row2 = [&](const char* k, const char* v) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(k);
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(v);
                };

                mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(rs.typeId));
                row2("typeId", buf);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Batcher type slot (0..typeCount-1). Groups all\n"
                                      "instances that share geometry + pipeline.");

                mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_i32(rs.recipeIndex));
                row2("recipeIndex", buf);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Static-prop recipe/type index — NOT the\n"
                                      "RenderWorld instance slot (Handle idx).\n"
                                      "Multiple Handle slots share one recipeIndex.");

                mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(rs.packetCount));
                row2("packetCount", buf);
                mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(rs.instanceCount));
                row2("instances", buf);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Visible instances of this typeId this frame\n"
                                      "(count of snapshot entries with matching typeId).");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("dispatch");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(0.4f,1.f,0.4f,1.f), "DrawPacket default ON");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("materialIdx");
                ImGui::TableSetColumnIndex(1);
                if (rs.materialIdx == 0xFFFFFFFFu)
                    ImGui::TextDisabled("(sentinel)");
                else {
                    mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(rs.materialIdx));
                    ImGui::TextUnformatted(buf);
                }

                ImGui::EndTable();
            }

            // Per-packet rows
            if (rs.rowCount > 0) {
                ImGui::Spacing();
                ImGui::TextUnformatted("Packets:");
                if (ImGui::BeginTable("##rspkts", 8,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                        | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("pkt",      ImGuiTableColumnFlags_WidthFixed, 36.f);
                    ImGui::TableSetupColumn("pipeline", ImGuiTableColumnFlags_WidthFixed, 68.f);
                    ImGui::TableSetupColumn("matIdx",   ImGuiTableColumnFlags_WidthFixed, 52.f);
                    ImGui::TableSetupColumn("albedo",   ImGuiTableColumnFlags_WidthFixed, 52.f);
                    ImGui::TableSetupColumn("layer",    ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("match",    ImGuiTableColumnFlags_WidthFixed, 44.f);
                    ImGui::TableSetupColumn("baseV",    ImGuiTableColumnFlags_WidthFixed, 52.f);
                    ImGui::TableSetupColumn("baseInst", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (uint32_t r = 0; r < rs.rowCount; ++r) {
                        const DrawPacketPropRow& pr = rs.rows[r];
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(pr.globalPacketIdx));
                        ImGui::TextUnformatted(buf);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Global packet index in batcher sorted array.\n"
                                              "Not a RenderWorld drawPacketIndex.");

                        ImGui::TableSetColumnIndex(1);
                        switch (pr.pipelineId) {
                            case 0:  ImGui::TextDisabled("invalid");      break;
                            case 1:  ImGui::TextUnformatted("opaque");    break;
                            case 2:  ImGui::TextColored(ImVec4(1.f,0.85f,0.3f,1.f), "alpha_test"); break;
                            default:
                                mc2::fmt::to_buf(buf, "?({:d})", mc2::fmt::as_u32(pr.pipelineId));
                                ImGui::TextColored(ImVec4(1.f,0.5f,0.3f,1.f), "%s", buf);
                                break;
                        }

                        ImGui::TableSetColumnIndex(2);
                        if (rs.materialIdx == 0xFFFFFFFFu)
                            ImGui::TextDisabled("--");
                        else {
                            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(rs.materialIdx));
                            ImGui::TextUnformatted(buf);
                        }

                        ImGui::TableSetColumnIndex(3);
                        if (pr.albedoTex == 0xFFFFFFFFu)
                            ImGui::TextDisabled("--");
                        else {
                            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(pr.albedoTex));
                            ImGui::TextUnformatted(buf);
                        }

                        ImGui::TableSetColumnIndex(4);
                        if (pr.texArrayLayer < 0)
                            ImGui::TextDisabled("--");
                        else {
                            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_i32(pr.texArrayLayer));
                            ImGui::TextUnformatted(buf);
                        }

                        ImGui::TableSetColumnIndex(5);
                        if (pr.albedoTex == 0xFFFFFFFFu || pr.texArrayLayer < 0)
                            ImGui::TextDisabled("n/a");
                        else if (pr.materialMatchesLegacy)
                            ImGui::TextColored(ImVec4(0.4f,1.f,0.4f,1.f), "OK");
                        else
                            ImGui::TextColored(ImVec4(1.f,0.4f,0.3f,1.f), "DIFF");

                        ImGui::TableSetColumnIndex(6);
                        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_i32(pr.baseVertex));
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(7);
                        ImGui::TextDisabled("--");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Deferred — only valid during dispatch (v1+).\n"
                                              "Not available at emit time.");
                    }
                    ImGui::EndTable();
                }
            }
        }
    }
}

// Draw the mech game data section.
void drawMechData() {
    if (ImGui::CollapsingHeader("Mech", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!s_mechData.populated) {
            ImGui::TextUnformatted("(no game data -- pick not resolved)");
        } else {
            static const char* s_classNames[] = { "None","Light","Medium","Heavy","Assault" };
            static const char* s_conStatNames[] = {
                "None","Sensor Q1","Sensor Q2","Sensor Q3","Sensor Q4","Visual"
            };
            const int ci  = (s_mechData.chassisClass >= 0 && s_mechData.chassisClass < 5)
                            ? s_mechData.chassisClass : 0;
            const int csi = (s_mechData.conStat >= 0 && s_mechData.conStat < 6)
                            ? s_mechData.conStat : 0;

            ImGui::Text("Variant:   %s", s_mechData.variantName[0] ? s_mechData.variantName : "(none)");
            ImGui::Text("Name:      %s", s_mechData.longName[0]    ? s_mechData.longName    : "(none)");
            ImGui::Text("Class:     %s", s_classNames[ci]);
            ImGui::Text("Team ID:   %ld", s_mechData.teamId);
            ImGui::Text("Pilot:     %s", s_mechData.pilotName[0]   ? s_mechData.pilotName   : "(none)");
            ImGui::Separator();
            if (!s_mechData.destroyed && !s_mechData.disabled && !s_mechData.crippled) {
                ImGui::Text("Status:    OK");
            } else {
                ImGui::Text("Status:    %s%s%s",
                    s_mechData.destroyed ? "DESTROYED " : "",
                    s_mechData.disabled  ? "disabled "  : "",
                    s_mechData.crippled  ? "crippled"   : "");
            }
            ImGui::Text("Sensor:    %s", s_conStatNames[csi]);
            ImGui::Separator();
            ImGui::Text("Armor:     %.0f / %.0f", s_mechData.totalCurArmor, s_mechData.totalMaxArmor);
            ImGui::Text("Structure: %.0f / %.0f", s_mechData.totalCurStr,   s_mechData.totalMaxStr);
        }
    }
}

// Draw the experimental mech normals controls.
void drawMechNormals() {
    if (ImGui::CollapsingHeader("Mech Normals (experimental)##mn")) {
        static const char* s_normModeNames[] = {
            "0  Cooked (default)",
            "1  Faceted",
            "2  Smoothed (recompute)",
        };
        int curMode = batcher_getMechNormalsMode();
        if (curMode < 0 || curMode > 2) curMode = 0;
        if (ImGui::BeginCombo("Normal Mode##mn", s_normModeNames[curMode])) {
            for (int i = 0; i < 3; ++i) {
                bool sel = (i == curMode);
                if (ImGui::Selectable(s_normModeNames[i], sel)) {
                    batcher_setMechNormalsMode(i);
                    batcher_rebuildMechNormals();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Normal source for GPU mech geometry.\n"
                              "Cooked: use ASE-loader normals (default, byte-identical).\n"
                              "Faceted: geometric face normal per triangle (flat shading).\n"
                              "Smoothed: angle-threshold smooth — lower angle = more hard edges.\n"
                              "Changing the mode rebuilds the mech VBO (occasional stall).");

        // Smooth Angle slider — only relevant when mode == 2.
        {
            float curDeg = batcher_getMechNormalsSmoothDeg();
            const bool modeIsSmooth = (batcher_getMechNormalsMode() == 2);
            if (!modeIsSmooth) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SliderFloat("Smooth Angle##mn", &curDeg, 1.0f, 179.0f, "%.1f deg")) {
                batcher_setMechNormalsSmoothDeg(curDeg);
                batcher_rebuildMechNormals();
            }
            if (!modeIsSmooth) {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Angle threshold for mode 2 Smoothed.\n"
                                  "Faces meeting at a shared vertex blend normals only\n"
                                  "when the angle between them is <= this value.\n"
                                  "Lower value = more hard edges preserved.\n"
                                  "Changing the angle rebuilds the mech VBO.");
        }
    }
}

// Draw the experimental mech lighting controls.
void drawMechLighting() {
    if (ImGui::CollapsingHeader("Mech Lighting (experimental)##ml")) {
        // Blinn specular sheen + glass/cockpit heuristic.
        // Per-flush uniforms — no VBO rebuild. Requires MC2_MECH_VIEWUNIFORMS=1
        // (camera position data); no effect on the default shader variant.
        ImGui::Separator();
        bool specOn = batcher_getMechSpecularEnabled() != 0;
        if (ImGui::Checkbox("Specular (metal sheen)##ml", &specOn))
            batcher_setMechSpecularEnabled(specOn ? 1 : 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Conservative Blinn specular sheen.\n"
                              "Requires MC2_MECH_VIEWUNIFORMS=1 (camera data);\n"
                              "no effect otherwise. Gate MC2_MECH_SPECULAR_V1=1\n"
                              "sets default-ON. OFF = byte-identical default.");
        {
            if (!specOn) ImGui::BeginDisabled();
            float specStr = batcher_getMechSpecularStrength();
            if (ImGui::SliderFloat("Spec Strength##ml", &specStr, 0.0f, 4.0f, "%.2f"))
                batcher_setMechSpecularStrength(specStr);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Overall specular multiplier (0..4).\n"
                                  "Glass pixels get an additional 1.6x multiplier.");
            float metalR = batcher_getMechMetalRoughness();
            if (ImGui::SliderFloat("Metal Roughness##ml", &metalR, 0.04f, 1.0f, "%.2f"))
                batcher_setMechMetalRoughness(metalR);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Roughness for non-glass surfaces (0.04..1.0).\n"
                                  "Lower = tighter glint; default 0.85 (broad, conservative).");
            float glassR = batcher_getMechGlassRoughness();
            if (ImGui::SliderFloat("Glass Roughness##ml", &glassR, 0.04f, 1.0f, "%.2f"))
                batcher_setMechGlassRoughness(glassR);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Roughness for glass/cockpit pixels (0.04..1.0).\n"
                                  "Default 0.25 — tighter highlight for cockpit glass.");
            float lumaT = batcher_getMechGlassLumaThresh();
            if (ImGui::SliderFloat("Glass Luma <##ml", &lumaT, 0.0f, 1.0f, "%.3f"))
                batcher_setMechGlassLumaThresh(lumaT);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pixel classified as glass if luminance < this.\n"
                                  "Dark pixels only — no hue/blue detection.\n"
                                  "Default 0.12.");
            float maxChanT = batcher_getMechGlassMaxChanThresh();
            if (ImGui::SliderFloat("Glass MaxChan <##ml", &maxChanT, 0.0f, 1.0f, "%.3f"))
                batcher_setMechGlassMaxChanThresh(maxChanT);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pixel classified as glass if max(R,G,B) < this\n"
                                  "(AND luma < threshold above). Default 0.18.");
            bool maskViz = batcher_getMechSpecDebugMask() != 0;
            if (ImGui::Checkbox("Show Cockpit Mask##ml", &maskViz))
                batcher_setMechSpecDebugMask(maskViz ? 1 : 0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Visualize glass classification:\n"
                                  "green = glass-classified, grey = metal.\n"
                                  "Debug modes 1-9 override this when active.");
            if (!specOn) ImGui::EndDisabled();
        }
    }
}

// Draw the mech snapshot panel.
void drawMechSnapshot() {
    {
    const RenderSnapshot* snap = getLastRenderSnapshot();
    const bool gateOn = snap && (snap->mechSnapshotCount > 0
                                 || snap->mechMatValid > 0
                                 || snap->mechMatSentinel > 0);
    const ImGuiTreeNodeFlags snapFlags = gateOn
        ? (ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap)
        : ImGuiTreeNodeFlags_AllowOverlap;
    bool msOpen = ImGui::CollapsingHeader("Mech Snapshot##ms", snapFlags);
    if (msOpen) {
        if (!snap || !gateOn) {
            ImGui::TextDisabled("(no data -- set MC2_SNAPSHOT_MECH_EXTRACT=1)");
        } else {
            // Frame summary
            ImGui::Text("rows=%u  mat_valid=%u  mat_sentinel=%u",
                snap->mechSnapshotCount, snap->mechMatValid, snap->mechMatSentinel);

            const bool anyMismatch = (snap->mechCountMismatch      != 0u
                                   || snap->mechHandleMismatch     != 0u
                                   || snap->mechObjectIdMismatch   != 0u
                                   || snap->mechTexHandleMismatch  != 0u
                                   || snap->mechMaterialIdxMismatch != 0u);
            if (anyMismatch) {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
                    "countMis=%u handleMis=%u objectIdMis=%u texMis=%u matMis=%u",
                    snap->mechCountMismatch, snap->mechHandleMismatch,
                    snap->mechObjectIdMismatch, snap->mechTexHandleMismatch,
                    snap->mechMaterialIdxMismatch);
            } else {
                ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f),
                    "countMis=0 handleMis=0 objectIdMis=0 texMis=0 matMis=0");
            }


            // Per-selected-mech row (match by objectIdRaw == handle.raw())
            ImGui::Separator();
            const uint32_t selRaw = s_selection.handle.raw();
            bool rowFound = false;
            for (uint32_t i = 0u; i < snap->mechPackets.size(); ++i) {
                const ExtractedMechPacket& row = snap->mechPackets[i];
                if (row.objectIdRaw != selRaw) continue;
                rowFound = true;
                ImGui::Text("Row %u:  (extracted this frame)", i);
                ImGui::Text("  handle      0x%08X", row.objectIdRaw);
                {
                    const char* texName =
                        gos_getMechTextureNameByNodeIdx(row.texHandle);
                    if (texName && *texName)
                        ImGui::Text("  texHandle   %u  (%s)", row.texHandle, texName);
                    else
                        ImGui::Text("  texHandle   %u", row.texHandle);
                }
                if (row.materialIdx == 0xFFFFFFFFu)
                    ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                        "  materialIdx sentinel (0xFFFFFFFF)");
                else
                    ImGui::Text("  materialIdx %u", row.materialIdx);
                ImGui::Text("  typeLodIdx  %u", row.typeLodIdx);
                ImGui::Text("  renderFlags 0x%02X", row.renderFlags);
                break;
            }
            if (!rowFound)
                ImGui::TextDisabled("  (selected handle NOT in snapshot this frame)");
        }
    }
    } // Mech Snapshot block
}

// Draw the terrain pick data section.
void drawTerrainData() {
    if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!s_terrainData.populated) {
            ImGui::TextUnformatted("(terrain pick not resolved)");
        } else {
            ImGui::Text("World:  (%.1f, %.1f, %.1f)",
                s_terrainData.worldX, s_terrainData.worldY, s_terrainData.worldZ);
            ImGui::Text("Tile:   row %-4d  col %d",
                s_terrainData.tileRow, s_terrainData.tileCol);
            ImGui::Text("Cell:   row %-4d  col %d",
                s_terrainData.cellRow, s_terrainData.cellCol);
            if (s_terrainData.terrainType >= 0)
                ImGui::Text("Type:   %d", s_terrainData.terrainType);
            else
                ImGui::TextDisabled("Type:   (n/a v1)");
        }
    }
}

// Draw the terrain pass section.
void drawTerrainPass() {
    if (ImGui::CollapsingHeader("Terrain Pass##tp", ImGuiTreeNodeFlags_DefaultOpen)) {
        const TerrainPassSnapshot& ts = s_terrainPass;
        ImGui::Text("View: id=%u (%s)", ts.currentViewId,
                    ts.currentViewName ? ts.currentViewName : "");
        if (ts.viewUniformsBoundForTerrain) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "ViewUniforms (binding=3): consumed");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "ViewUniforms (binding=3): NOT consumed (legacy uniforms)");
        }
        ImGui::Separator();
        ImGui::Text("Programs:");
        // Read-only readout of the MDI water FS debug mode
        // (live control is deferred to Graphics Options > Water per panel discipline).
        // ::-qualified -> global symbol (decl at file top), not EditorInspector::.
        ImGui::BulletText("water FS debug mode:  %d  (MC2_WATER_DEBUG_MODE; 0=Final)",
                          ::gos_GetWaterFsDebugMode());
        ImGui::Separator();
        ImGui::Text("Last flush stats:");
        ImGui::BulletText("draw buckets:    %u", ts.bucketCount);
        ImGui::BulletText("verts (expanded): %u", ts.vertCount);
        ImGui::BulletText("thin records:    %u", ts.thinRecCount);
        ImGui::BulletText("recipes:         %u", ts.recipeCount);
        if (ts.overflow) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "OVERFLOWED last flush");
        }
        ImGui::Separator();
        ImGui::Text("Tessellation: %s", ts.tessellationOn ? "ON (always)" : "off");
        // Tuning controls (matNormalBoost, tintStrengthScale, NfH strength,
        // Lighting V1, resample factor, debug-mode picker) all live in the
        // GraphicsOptionsWindow Terrain section. This panel stays read-only
        // diagnostic — no live controls — so the Object Inspector and the
        // Graphics Options window aren't duplicated views of the same state.
        ImGui::TextDisabled("(tuning controls: Graphics Options > Terrain)");
    }
}

// Draw the shadow pass section.
void drawShadowPass() {
    if (ImGui::CollapsingHeader("Shadow Pass##sp", ImGuiTreeNodeFlags_DefaultOpen)) {
        const ShadowPassSnapshot& sp = s_shadowPass;
        ImGui::Text("Shadows enabled: %s", sp.shadowsEnabled ? "yes" : "no");
        ImGui::Text("Static light matrix built: %s", sp.staticLightMatrixBuilt ? "yes" : "no");
        ImGui::Text("Shadow map size: %d   Dyn shadow map size: %d",
                    sp.shadowMapSize, sp.dynShadowMapSize);
        if (sp.viewUniformsBoundForShadow) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "ViewUniforms (binding=3): consumed");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "ViewUniforms (binding=3): NOT consumed (legacy shadow matrices)");
        }
        ImGui::Separator();
        ImGui::Text("Caster counts (last flushShadow):");
        ImGui::BulletText("mech         types=%u  instances=%u",
                          sp.mechShadowTypesDrawn, sp.mechShadowInstDrawn);
        ImGui::BulletText("static-prop  types=%u  instances=%u",
                          sp.staticPropShadowTypesDrawn, sp.staticPropShadowInstDrawn);
        ImGui::Separator();
        ImGui::TextDisabled("PipelineDesc: legacy (shadow pass not on registry)");
    }
}

// Draw the VFX pass section.
void drawVfxPass() {
    if (ImGui::CollapsingHeader("VFX Pass##vfx", ImGuiTreeNodeFlags_DefaultOpen)) {
        const VfxPassSnapshot& vs = s_vfxPass;
        // GPU particles enabled status (driven by MC2_GPU_PARTICLES env var).
        if (vs.gpuParticlesEnabled) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "GPU particles: enabled (MC2_GPU_PARTICLES=1)");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "GPU particles: DISABLED (default OFF; set MC2_GPU_PARTICLES=1; legacy CPU FX only)");
        }
        ImGui::Text("Verbose log gate (MC2_GPU_PARTICLES_LOG): %s",
                    vs.gpuParticlesLogEnabled ? "ON" : "off");
        if (vs.initFailed) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Bridge init: FAILED (program compile/link error)");
        }
        ImGui::Text("Camera basis set this frame: %s",
                    vs.cameraSetThisFrame ? "yes" : "no (using last-known)");
        if (vs.viewUniformsBoundForVfx) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "ViewUniforms (binding=3): consumed");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "ViewUniforms (binding=3): NOT consumed (legacy gosFX path)");
        }
        ImGui::Separator();
        // Active debug mode (read-only; driven by
        // MC2_VFX_DEBUG_MODE). Mode 0 is byte-identical default output.
        {
            static const char* kVfxModes[] = {
                "0 Final (default)", "1 Albedo", "2 Alpha",
                "3 ParticleKind", "4 Overdraw" };
            int m = vs.debugMode;
            const char* name = (m >= 0 && m < 5) ? kVfxModes[m] : "?";
            if (m == 0) {
                ImGui::Text("Debug view (MC2_VFX_DEBUG_MODE): %s", name);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                    "Debug view (MC2_VFX_DEBUG_MODE): %s [ACTIVE]", name);
            }
        }
        ImGui::Separator();
        ImGui::Text("Blend modes (per-group): alpha (0) or additive (1) — set per BeginGroup");
        ImGui::Separator();
        ImGui::Text("Particle buffers:");
        ImGui::BulletText("CPU staging budget:   %u records", vs.perFrameBudget);
        ImGui::BulletText("GL SSBO capacity:     %u records (binding=14)", vs.ssboCapacityRecords);
        if (vs.overflowReported) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "OVERFLOW reported (staging exceeded budget — record dropped)");
        }
        ImGui::Separator();
        ImGui::Text("Process-lifetime aggregates:");
        ImGui::BulletText("emit total:                %llu", vs.emitTotal);
        ImGui::BulletText("flush total:               %llu", vs.flushTotal);
        ImGui::BulletText("non-empty flushes:         %llu", vs.nonemptyFlushTotal);
        ImGui::BulletText("records flushed total:     %llu", vs.recordsFlushedTotal);
        ImGui::BulletText("records per flush (max):   %u",   vs.recordsPerFlushMax);
        ImGui::BulletText("trail spawn total:         %llu", vs.trailSpawnTotal);
        ImGui::BulletText("trail head total:          %llu", vs.trailHeadTotal);
        ImGui::Separator();
        ImGui::Text("Active particle kinds (GpuTrailKind enum):");
        // TODO: wire per-kind draw counts — not currently tracked by Batcher.
        // gosFX legacy specs use distinct names (Card/PertCloud/PointCloud/etc.)
        // but the GPU path collapses them into a single SSBO; per-kind counts
        // would require a new counter and are out of scope.
        ImGui::BulletText("None         (no trail)         draws: n/a");
        ImGui::BulletText("MissileSmoke (handle 41)         draws: n/a");
        ImGui::BulletText("PpcBolt      (handle 33)         draws: n/a");
        ImGui::Separator();
        ImGui::TextDisabled("PipelineDesc: legacy (VFX not on registry; object-IDs prohibited)");
    }
}

// Draw the render-pass contracts section.
void drawRenderPassContracts() {
    if (ImGui::CollapsingHeader("Render Pass Contracts##rpc")) {
        ImGui::TextDisabled("Descriptive only -- imperative dispatch unchanged.");
        if (ImGui::BeginTable("rpc_table", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("id");
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("owner");
            ImGui::TableSetupColumn("viewUni");
            ImGui::TableSetupColumn("pipeDesc");
            ImGui::TableSetupColumn("snapAuth");
            ImGui::TableSetupColumn("kill-switch");
            ImGui::TableHeadersRow();
            for (const auto & c : RenderCore::kRenderPassContracts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<unsigned>(c.id));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.ownerSubsystem);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.viewUniformsBound ? "Y" : "-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.pipelineDescRegistered ? "Y" : "-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.snapshotRowAuthoritative ? "Y" : "-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.killSwitchEnv ? c.killSwitchEnv : "(none)");
            }
            ImGui::EndTable();
        }
    }
}

// Draw the static-prop material inventory section.
void drawMaterialInventory() {
    if (ImGui::CollapsingHeader("Material Inventory##matinv")) {
        const uint32_t invCount = batcher_getStaticPropMaterialInventoryCount();
        ImGui::TextDisabled("Static-prop lane only -- read-only snapshot.");
        ImGui::Text("Materials: %u", static_cast<unsigned>(invCount));
        if (invCount == 0u) {
            ImGui::TextDisabled("(empty -- mission not loaded or MC2_MATERIAL_GPU=0)");
        } else if (ImGui::BeginTable("matinv_table", 10,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                                     ImVec2(0.f, 240.f))) {
            ImGui::TableSetupColumn("idx");
            ImGui::TableSetupColumn("layer");
            ImGui::TableSetupColumn("grp");
            ImGui::TableSetupColumn("node");
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("dims");
            ImGui::TableSetupColumn("uses");
            ImGui::TableSetupColumn("flags");
            ImGui::TableSetupColumn("rough");
            ImGui::TableSetupColumn("metal");
            ImGui::TableHeadersRow();
            unsigned placeholderCount = 0u;
            for (uint32_t i = 0; i < invCount; ++i) {
                StaticPropMaterialInventoryEntry e{};
                if (!batcher_getStaticPropMaterialInventoryEntry(i, &e)) continue;
                if (e.placeholder) ++placeholderCount;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", e.materialIdx);
                ImGui::TableNextColumn(); ImGui::Text("%u", e.albedoTexLayer);
                ImGui::TableNextColumn(); ImGui::Text("%s", e.alphaGroup == 0u ? "OFF" : "ON");
                ImGui::TableNextColumn();
                if (e.nodeIdx == 0xFFFFFFFFu) ImGui::TextDisabled("--");
                else                          ImGui::Text("%u", e.nodeIdx);
                ImGui::TableNextColumn();
                if (e.placeholder) ImGui::TextColored(ImVec4(1.f,0.55f,0.3f,1.f),
                                                       "%s", e.textureName);
                else               ImGui::TextUnformatted(e.textureName);
                ImGui::TableNextColumn();
                if (e.textureWidth == 0u) ImGui::TextDisabled("--");
                else                      ImGui::Text("%ux%u",
                                                       e.textureWidth, e.textureHeight);
                ImGui::TableNextColumn(); ImGui::Text("%u", e.usageCount);
                ImGui::TableNextColumn(); ImGui::Text("0x%08X", e.flags);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", e.roughnessFactor);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", e.metallicFactor);
            }
            ImGui::EndTable();
            ImGui::Separator();
            if (placeholderCount > 0u) {
                ImGui::TextColored(ImVec4(1.f,0.55f,0.3f,1.f),
                    "Missing/placeholder textures: %u of %u",
                    placeholderCount, invCount);
            } else {
                ImGui::TextDisabled("Missing/placeholder textures: 0");
            }
        }
    }
}

// Draw the material handle lookup section.
void drawMaterialRwLookup(const RenderWorld::LookupResult& lk) {
    if (ImGui::CollapsingHeader("Material (RW handle lookup)")) {
        RenderCore::MaterialHandle mh;
        mh.bits = lk.materialHandleBits;
        if (mh.bits == 0u) {
            ImGui::TextDisabled("Handle:        (unknown)");
            // For static props: handle bits are not yet populated by the RenderWorld
            // lookup path. Render Spine materialIdx is the authoritative source.
            if (s_selection.pickKind == InspectorPickKind::StaticProp)
                ImGui::TextDisabled("  (use Render Spine for authoritative materialIdx)");
        } else {
            ImGui::Text("Handle idx:    %u", mh.index());
            ImGui::Text("Handle gen:    %u", mh.generation());
            ImGui::Text("Handle raw:    0x%08X", mh.raw());
        }
        ImGui::Spacing();
        if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                && s_staticPropData.populated) {
            if (!s_staticPropData.materialGpuActive) {
                ImGui::TextDisabled("Source:        legacy texArrayLayer");
                ImGui::TextDisabled("(MC2_MATERIAL_GPU not active)");
            } else if (!s_staticPropData.materialGpuPopulated) {
                ImGui::TextColored(ImVec4(1.f,0.55f,0.3f,1.f),
                    "Source:        MaterialGpu");
                ImGui::TextDisabled("  handle bits unpopulated (see Render Spine)");
            } else {
                const auto& mg = s_staticPropData.materialGpu;
                ImGui::Text("Source:        MaterialGpu");
                ImGui::Text("matIdx:        %u / gen %u",
                    s_staticPropData.materialIdx, s_staticPropData.materialGen);
                ImGui::Separator();
                auto texLabel = [](uint32_t t, std::span<char> b) {
                    if (t == 0xFFFFFFFFu) mc2::fmt::to_buf_n(b.data(), b.size(), "(absent)");
                    else mc2::fmt::to_buf_n(b.data(), b.size(), "{:d}", mc2::fmt::as_u32(t));
                };
                char tb[24];
                texLabel(mg.albedoTex, tb);
                ImGui::Text("albedoTex:     %s", tb);
                texLabel(mg.normalTex, tb);
                ImGui::Text("normalTex:     %s", tb);
                texLabel(mg.metallicRoughnessTex, tb);
                ImGui::Text("mrTex:         %s", tb);
                texLabel(mg.emissiveTex, tb);
                ImGui::Text("emissiveTex:   %s", tb);
                ImGui::Separator();
                ImGui::Text("flags:         0x%08X%s%s%s%s%s%s",
                    mg.flags,
                    (mg.flags & 1u)  ? " AlphaTest"  : "",
                    (mg.flags & 2u)  ? " NormalMap"  : "",
                    (mg.flags & 4u)  ? " MetalRough" : "",
                    (mg.flags & 8u)  ? " Emissive"   : "",
                    (mg.flags & 16u) ? " DblSided"   : "",
                    (mg.flags & 32u) ? " Window"     : "");
                ImGui::Text("baseColor:     %.3f", mg.baseColorFactor);
                ImGui::Text("metallic:      %.3f", mg.metallicFactor);
                ImGui::Text("roughness:     %.3f", mg.roughnessFactor);
            }
        } else {
            ImGui::TextDisabled("(MaterialGpu data: StaticProp only)");
        }
    }
}

// Draw the environment gates section.
void drawEnvGates() {
    if (ImGui::CollapsingHeader("Env Gates")) {
        struct GateInfo { const char* name; bool defaultOn; };
        static const GateInfo kGates[] = {
            { "MC2_IMGUI",            true  },
            { "MC2_IMGUI_INSPECTOR",  true  },
            { "MC2_OBJECT_ID_BUFFER", true  },
        };
        for (const auto& g : kGates) {
            const auto st = mc2::gate::boolState(g.name);
            bool on;
            const char* src;
            if (!st || !st->set) {
                on  = g.defaultOn;
                src = g.defaultOn ? "(default ON)" : "(default OFF)";
            } else if (!st->on) {
                on  = false;
                src = "(=0)";
            } else {
                on  = true;
                src = "(=1)";
            }
            ImGui::Text("%s", g.name);
            ImGui::SameLine();
            if (on)
                ImGui::TextColored(ImVec4(0.4f,1.f,0.4f,1.f), "ON  %s", src);
            else
                ImGui::TextColored(ImVec4(1.f,0.55f,0.3f,1.f), "OFF %s", src);
        }
    }
}

}  // namespace EditorInspector
