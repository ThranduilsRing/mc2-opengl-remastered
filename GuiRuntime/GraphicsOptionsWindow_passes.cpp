// Graphics options window: draw-packet, render-pass and G-buffer sections.
// Declared in GraphicsOptionsWindow_internal.h.
#include "GraphicsOptionsWindow_internal.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

#include <algorithm>

namespace GraphicsOptionsWindow {

void drawDrawPacketsSection() {
    const bool anyError = (g_dpSnapshot.emitted != g_dpSnapshot.expected
                           || g_dpSnapshot.invalidRanges > 0
                           || g_dpSnapshot.overflow);
    if (anyError && g_dpSnapshot.frame > 0)
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "! packet integrity error");

    char buf[64];

    if (ImGui::BeginTable("##dphealth", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##dk",  ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##dv",  ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Frame");
        ImGui::TableSetColumnIndex(1);
        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u64((unsigned long long)g_dpSnapshot.frame));
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Emitted");
        ImGui::TableSetColumnIndex(1);
        {
            const bool ok = (g_dpSnapshot.emitted == g_dpSnapshot.expected);
            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(g_dpSnapshot.emitted));
            ImGui::TextColored(ok ? ImVec4(0.3f,1.f,0.3f,1.f)
                                  : ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Expected");
        ImGui::TableSetColumnIndex(1);
        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(g_dpSnapshot.expected));
        ImGui::TextUnformatted(buf);

        ImGui::EndTable();
    }

    ImGui::Separator();

    if (ImGui::BeginTable("##dpdiag", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##ddk", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##ddv", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Distinct types");
        ImGui::TableSetColumnIndex(1);
        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(g_dpSnapshot.distinctTypes));
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Invalid ranges");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.invalidRanges > 0) {
            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(g_dpSnapshot.invalidRanges));
            ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
        } else {
            ImGui::TextUnformatted("0");
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Overflow");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.overflow)
            ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "YES");
        else
            ImGui::TextDisabled("no");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Skipped ranges");
        ImGui::TableSetColumnIndex(1);
        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(g_dpSnapshot.skippedRanges));
        ImGui::TextDisabled("%s", buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Mat mismatches");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.materialMismatches > 0) {
            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(g_dpSnapshot.materialMismatches));
            ImGui::TextColored(ImVec4(1.f,0.8f,0.3f,1.f), "%s", buf);
        } else {
            ImGui::TextDisabled("0");
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Type Inspector");

    const uint32_t typeCount = g_dpSnapshot.typeDescCount;
    if (typeCount == 0) {
        ImGui::TextDisabled("(geometry not finalized — load a mission)");
        return;
    }

    static int s_selTypeId = 0;
    if (s_selTypeId >= (int)typeCount) s_selTypeId = 0;

    ImGui::SetNextItemWidth(110.f);
    ImGui::InputInt("TypeId##dp", &s_selTypeId, 1, 10);
    s_selTypeId = std::clamp(s_selTypeId, 0, (int)typeCount - 1);
    ImGui::SameLine();
    ImGui::TextDisabled("/ %u", typeCount - 1u);

    RenderCore::StaticPropTypeDesc desc{};
    const bool descOk = batcher_getStaticPropTypeDesc((uint32_t)s_selTypeId, &desc);

    if (descOk) {
        if (ImGui::BeginTable("##dptype", 2,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("##dtk", ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("##dtv", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("firstPacket");
            ImGui::TableSetColumnIndex(1);
            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(desc.firstPacket));
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
            ImGui::TableSetColumnIndex(1);
            mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(desc.packetCount));
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
            ImGui::TableSetColumnIndex(1);
            switch (desc.alphaClass) {
                case 0:  ImGui::TextUnformatted("OPAQUE");     break;
                case 1:  ImGui::TextUnformatted("ALPHA_TEST"); break;
                default:
                    mc2::fmt::to_buf(buf, "?({:d})", mc2::fmt::as_u32(desc.alphaClass));
                    ImGui::TextColored(ImVec4(1.f,0.8f,0.3f,1.f), "%s", buf);
                    break;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("materialIdx");
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("per-instance");

            ImGui::EndTable();
        }

        if (ImGui::SmallButton("Copy CSV##dp")) {
            const char* alphaStr = desc.alphaClass == 0 ? "OPAQUE"
                                 : desc.alphaClass == 1 ? "ALPHA_TEST" : "?";
            mc2::fmt::to_buf(buf, "typeId,firstPacket,packetCount,alphaClass\n{:d},{:d},{:d},{}", mc2::fmt::as_u32((uint32_t)s_selTypeId), mc2::fmt::as_u32(desc.firstPacket), mc2::fmt::as_u32(desc.packetCount), mc2::fmt::safe(alphaStr));
            ImGui::SetClipboardText(buf);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy type descriptor row as CSV.");
    } else {
        ImGui::TextDisabled("(typeId %d out of range)", s_selTypeId);
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Packet Range Minimap##dp")) {
        const uint32_t total  = g_dpSnapshot.expected > 0u ? g_dpSnapshot.expected : 1u;
        const float    availW = ImGui::GetContentRegionAvail().x;
        const float    barH   = 14.f;
        const ImVec2   p0     = ImGui::GetCursorScreenPos();
        ImDrawList*    dl     = ImGui::GetWindowDrawList();

        dl->AddRectFilled(p0, ImVec2(p0.x + availW, p0.y + barH),
                          IM_COL32(45, 45, 45, 255));

        if (descOk && desc.packetCount > 0u) {
            const float x0 = p0.x + availW * ((float)desc.firstPacket / (float)total);
            float       x1 = p0.x + availW * ((float)(desc.firstPacket + desc.packetCount) / (float)total);
            if (x1 < x0 + 2.f) x1 = x0 + 2.f;
            dl->AddRectFilled(ImVec2(x0, p0.y + 2.f), ImVec2(x1, p0.y + barH - 2.f),
                              IM_COL32(255, 220, 50, 200));
        }

        dl->AddRect(p0, ImVec2(p0.x + availW, p0.y + barH),
                    IM_COL32(110, 110, 110, 255));
        ImGui::Dummy(ImVec2(availW, barH));

        mc2::fmt::to_buf(buf, "type {:d}: [{:d}, {:d}) of {:d} total", mc2::fmt::as_i32(s_selTypeId), mc2::fmt::as_u32(descOk ? desc.firstPacket : 0u), mc2::fmt::as_u32(descOk ? (desc.firstPacket + desc.packetCount) : 0u), mc2::fmt::as_u32(total));
        ImGui::TextDisabled("%s", buf);
    }

    // ── Selected Prop Packet Inspector ────────────────────────────────────────
    // Populated each frame by gameosmain after emitStaticPropDrawPackets.
    // Selection: Ctrl+Shift+Click a static prop in-game (Object Inspector).
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Selected Prop##dp")) {
        const DrawPacketSelectedPropSnapshot& sel = g_dpSelProp;

        if (!sel.valid) {
            ImGui::TextDisabled("(no static prop selected)");
            ImGui::TextDisabled("Ctrl+Shift+Click a prop in-game to inspect.");
        } else {
            const char* sname = sel.shapeName[0] ? sel.shapeName : "(unnamed)";
            ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.f),
                "%s", sname);
            ImGui::SameLine();
            ImGui::TextDisabled("| typeId: %u | recipe: %d",
                sel.typeId, sel.recipeIndex);

            if (ImGui::BeginTable("##selpropsummary", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("##spsk", ImGuiTableColumnFlags_WidthFixed, 130.f);
                ImGui::TableSetupColumn("##spsv", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Instances (vis)");
                ImGui::TableSetColumnIndex(1);
                mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(sel.instanceCount));
                ImGui::TextUnformatted(buf);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("materialIdx");
                ImGui::TableSetColumnIndex(1);
                if (sel.materialIdx == 0xFFFFFFFFu)
                    ImGui::TextDisabled("(sentinel)");
                else {
                    mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(sel.materialIdx));
                    ImGui::TextUnformatted(buf);
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
                ImGui::TableSetColumnIndex(1);
                switch (sel.alphaClass) {
                    case 0:  ImGui::TextUnformatted("OPAQUE");      break;
                    case 1:  ImGui::TextUnformatted("ALPHA_TEST");  break;
                    default:
                        mc2::fmt::to_buf(buf, "?({:d})", mc2::fmt::as_u32(sel.alphaClass));
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                        break;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
                ImGui::TableSetColumnIndex(1);
                mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(sel.packetCount));
                ImGui::TextUnformatted(buf);

                ImGui::EndTable();
            }

            if (sel.rowCount > 0) {
                ImGui::Spacing();
                ImGui::TextUnformatted("Packets:");
                if (ImGui::BeginTable("##selproppkts", 7,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                        | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("pktIdx",    ImGuiTableColumnFlags_WidthFixed, 48.f);
                    ImGui::TableSetupColumn("firstIdx",  ImGuiTableColumnFlags_WidthFixed, 64.f);
                    ImGui::TableSetupColumn("idxCount",  ImGuiTableColumnFlags_WidthFixed, 64.f);
                    ImGui::TableSetupColumn("baseVtx",   ImGuiTableColumnFlags_WidthFixed, 56.f);
                    ImGui::TableSetupColumn("pipeline",  ImGuiTableColumnFlags_WidthFixed, 74.f);
                    ImGui::TableSetupColumn("matFlags",  ImGuiTableColumnFlags_WidthFixed, 62.f);
                    ImGui::TableSetupColumn("baseInst",  ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (uint32_t r = 0; r < sel.rowCount; ++r) {
                        const DrawPacketPropRow& row = sel.rows[r];
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(row.globalPacketIdx));
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(1);
                        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(row.firstIndex));
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(2);
                        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_u32(row.indexCount));
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(3);
                        mc2::fmt::to_buf(buf, "{:d}", mc2::fmt::as_i32(row.baseVertex));
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(4);
                        switch (row.pipelineId) {
                            case 0:  ImGui::TextDisabled("invalid");      break;
                            case 1:  ImGui::TextUnformatted("opaque");    break;
                            case 2:  ImGui::TextUnformatted("alpha_test"); break;
                            default:
                                mc2::fmt::to_buf(buf, "?({:d})", mc2::fmt::as_u32(row.pipelineId));
                                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                                break;
                        }

                        ImGui::TableSetColumnIndex(5);
                        mc2::fmt::to_buf(buf, "0x{:02X}", mc2::fmt::as_u32(row.materialFlags));
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(6);
                        ImGui::TextDisabled("--");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("baseInstance populated at dispatch (v1+)");
                    }

                    ImGui::EndTable();
                }
            }

            if (ImGui::SmallButton("Copy CSV##selp")) {
                int n = mc2::fmt::to_buf(buf, "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                ImGui::SetClipboardText(buf);
                char cbuf[1024]; int cn = 0;
                cn += mc2::fmt::to_buf_n(cbuf+cn, sizeof(cbuf)-cn, "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                for (uint32_t r = 0; r < sel.rowCount && cn < (int)sizeof(cbuf)-80; ++r) {
                    const DrawPacketPropRow& row = sel.rows[r];
                    const char* plName = row.pipelineId == 1 ? "opaque"
                                       : row.pipelineId == 2 ? "alpha_test" : "invalid";
                    cn += mc2::fmt::to_buf_n(cbuf+cn, sizeof(cbuf)-cn, "{:d},{:d},{:d},{:d},{},0x{:02X}\n", mc2::fmt::as_u32(row.globalPacketIdx), mc2::fmt::as_u32(row.firstIndex), mc2::fmt::as_u32(row.indexCount), mc2::fmt::as_i32(row.baseVertex), mc2::fmt::safe(plName), mc2::fmt::as_u32(row.materialFlags));
                }
                ImGui::SetClipboardText(cbuf);
                (void)n;
            }
        }
    }
}


void drawRenderPassesSection() {
    gosPostProcess* pp = getGosPostProcess();

    if (pp) {
        ImGui::Checkbox("Shadow Pass##rp", &pp->shadowsEnabled_);
        ImGui::SameLine();
        ImGui::TextDisabled("(also in Post-Process)");
    }

    if (pp && ImGui::CollapsingHeader("Cloud Shadows##rp")) {
        bool cloudOn = pp->getCloudShadowEnabled();
        if (ImGui::Checkbox("Enable##cloud", &cloudOn))
            pp->setCloudShadowEnabled(cloudOn);

        float strength = pp->getCloudStrength();
        if (ImGui::SliderFloat("Strength##cloud", &strength, 0.0f, 0.6f, "%.3f"))
            pp->setCloudStrength(strength);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Max darkening amplitude. 0=clear, 0.15=default.");

        float scale = pp->getCloudScale();
        if (ImGui::SliderFloat("Cloud Scale##cloud", &scale, 0.0001f, 0.003f, "%.4f"))
            pp->setCloudScale(scale);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World-XY frequency. Smaller = bigger clouds. Default 0.0006.");

        float scroll[2] = { pp->getCloudScrollX(), pp->getCloudScrollY() };
        if (ImGui::SliderFloat2("Scroll Speed##cloud", scroll, -0.05f, 0.05f, "%.4f")) {
            pp->setCloudScrollX(scroll[0]);
            pp->setCloudScrollY(scroll[1]);
        }

        float thresh[2] = { pp->getCloudThreshLo(), pp->getCloudThreshHi() };
        if (ImGui::SliderFloat2("Coverage (lo/hi)##cloud", thresh, 0.0f, 1.0f, "%.3f")) {
            pp->setCloudThreshLo(thresh[0]);
            pp->setCloudThreshHi(thresh[1]);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smoothstep band on the FBM noise. Default 0.3 / 0.7.");

        int octaves = pp->getCloudOctaves();
        if (ImGui::SliderInt("Octaves##cloud", &octaves, 1, 6))
            pp->setCloudOctaves(octaves);

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##cloud")) {
            pp->setCloudShadowEnabled(true);
            pp->setCloudStrength(0.15f);
            pp->setCloudScale(0.0006f);
            pp->setCloudScrollX(0.012f);
            pp->setCloudScrollY(0.005f);
            pp->setCloudThreshLo(0.3f);
            pp->setCloudThreshHi(0.7f);
            pp->setCloudOctaves(4);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("GPU Geometry Passes");

    // Draw Mechs: hides mechs entirely (gates both GPU submit and CPU fallback).
    ImGui::Checkbox("Draw Mechs##rp", &g_drawMechs);
    ImGui::SameLine();
    // GPU Mechs: switches between GPU batcher and CPU legacy path (mechs still visible).
    ImGui::Checkbox("GPU path##rp", &g_useGpuMechs);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("GPU Mechs ON = GPU batcher (PBR lit).\n"
                          "GPU Mechs OFF = CPU legacy path (older shading, dimmer).\n"
                          "Use 'Draw Mechs' to hide mechs entirely.");

    if (g_useGpuObjects) {
        ImGui::Checkbox("GPU Objects##rp",  &g_useGpuObjects);
    } else {
        ImGui::Checkbox("GPU Objects##rp",  &g_useGpuObjects);
        ImGui::SameLine();
        // Legacy static props only active when GPU Objects batcher is off
        ImGui::Checkbox("GPU Static Props##rp", &g_useGpuStaticProps);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("FX / Particles");

    bool fxDisabled = mc2::gate::get<"MC2_DISABLE_GOSFX">();
    if (fxDisabled) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "[off]  FX/Particles");
        ImGui::SameLine();
        ImGui::TextDisabled("MC2_DISABLE_GOSFX=1 — requires restart to re-enable");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[ON]   FX/Particles");
        ImGui::SameLine();
        ImGui::TextDisabled("(set MC2_DISABLE_GOSFX=1 to disable)");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Static Props Debug Mode");

    static const char* kPropModes[] = {
        "0: Normal",
        "1: Addr-gradient",
        "2: Addr-hash",
        "3: WHITE",
        "4: ARGB only",
        "5: TEX only",
        "6: HIGHLIGHT only",
        "7: TEX+HIGHLIGHT",
    };
    int target = gos_GpuPropsGetDebugMode();
    if (ImGui::Combo("##propdbg", &target, kPropModes, IM_ARRAYSIZE(kPropModes))) {
        int cur   = gos_GpuPropsGetDebugMode();
        int steps = (target - cur + 8) % 8;
        for (int i = 0; i < steps; ++i)
            gos_GpuPropsCycleDebugMode();
    }
}


void drawGBufferPreview() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) {
        ImGui::TextDisabled("(post-process system unavailable)");
        return;
    }

    // Compute thumbnail width from available content region.
    // Aspect ratio: fixed 16:9 approximation.
    const float availW = ImGui::GetContentRegionAvail().x;
    const float thumbW = (availW * 0.5f) - ImGui::GetStyle().ItemSpacing.x;
    const float thumbH = thumbW * (9.0f / 16.0f);

    // UV flip: OpenGL textures have (0,0) at bottom-left; ImGui expects top-left.
    const ImVec2 uv0(0.0f, 1.0f);
    const ImVec2 uv1(1.0f, 0.0f);
    const ImVec2 sz(thumbW, thumbH);

    {
        unsigned int tex = pp->getSceneColorTexture();
        if (tex) {
            ImGui::TextUnformatted("Scene Color  (RGBA16F, HDR)");
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("HDR scene output before tone-mapping.\nValues above 1.0 are bloom sources.");
        } else {
            ImGui::TextDisabled("Scene Color: not allocated");
        }
    }

    ImGui::SameLine();

    {
        unsigned int tex = pp->getSceneNormalTexture();
        if (tex) {
            ImGui::TextUnformatted("Scene Normals  (RGBA16F)");
            // Column-layout: label is already on the SameLine'd column.
            // SetCursorPosY to align label with opposite column.
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("World-space normals (RGB = XYZ mapped 0-1).\n"
                                  "Alpha = shadow-skip flag (0=skip, 1=apply).\n"
                                  "Clear sentinel: (0.5, 0.5, 1.0, 0.0) = flat-up.");
        } else {
            ImGui::TextDisabled("Scene Normals: not allocated");
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Shadow Maps  (DEPTH_COMPONENT24)");
    ImGui::TextDisabled("  Depth textures need GL_TEXTURE_COMPARE_MODE=GL_NONE to preview.");
    ImGui::TextDisabled("  Use the shadow debug overlay in Post-Process instead.");

    {
        unsigned int tex = pp->getSceneObjectIdTex();
        if (tex) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Object-ID Buffer  (R32UI)");
            ImGui::TextDisabled("  Uint texture — needs a hash-to-RGB visualization shader.");
            ImGui::TextDisabled("  Use Ctrl+Shift+I + click to inspect individual pixels.");
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("Object-ID buffer: not active (set MC2_OBJECT_ID_BUFFER=1)");
        }
    }

    // ── Water Reflection (1/4-res RGBA16F) ────────────────────────────────────
    // Reflection preview stays black without sky and terrain content.
    {
        unsigned int tex = pp->getWaterReflectionTexture();
        ImGui::Spacing();
        if (tex) {
            ImGui::Text("Water Reflection  (1/4-res RGBA16F, %dx%d)",
                        pp->getWaterReflectionWidth(), pp->getWaterReflectionHeight());
            ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Quarter-res water reflection target (WATER-REFLECTION-RESOURCE-1).\n"
                                  "BLACK until Phase C renders sky+terrain into it; substrate only.");
        } else {
            ImGui::TextDisabled("Water Reflection: not allocated");
        }
    }

    // ── HZB depth pyramid  ───────────────────────────────
    // Native Vulkan pyramid dimensions and render-only culling status.
    {
        ImGui::Spacing();
        ImGui::Separator();
        if (!pp->isHzbEnabled()) {
            ImGui::TextDisabled("HZB culling: off (MC2_HZB_BUILD=0)");
        } else {
            const int mips = pp->getHzbMipCount();
            ImGui::Text("VK HZB Pyramid (R32F per-level, %dx%d, %d mips, builds=%llu)",
                        pp->getHzbWidth(), pp->getHzbHeight(), mips,
                        (unsigned long long)pp->getHzbBuildCount());
        }
    }
}



} // namespace GraphicsOptionsWindow
