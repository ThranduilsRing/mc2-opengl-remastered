// Graphics options window: draw-packet, render-pass and G-buffer sections.
// Declared in GraphicsOptionsWindow_internal.h.
#include "GraphicsOptionsWindow_internal.h"

namespace GraphicsOptionsWindow {

void drawDrawPacketsSection() {
    // ── Frame health ──────────────────────────────────────────────────────────
    const bool anyError = (g_dpSnapshot.emitted != g_dpSnapshot.expected
                           || g_dpSnapshot.invalidRanges > 0
                           || g_dpSnapshot.overflow);
    if (anyError && g_dpSnapshot.frame > 0)
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "! packet integrity error");

    char buf[64];

    // Top table: frame + emitted vs expected
    if (ImGui::BeginTable("##dphealth", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##dk",  ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##dv",  ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Frame");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)g_dpSnapshot.frame);
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Emitted");
        ImGui::TableSetColumnIndex(1);
        {
            const bool ok = (g_dpSnapshot.emitted == g_dpSnapshot.expected);
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.emitted);
            ImGui::TextColored(ok ? ImVec4(0.3f,1.f,0.3f,1.f)
                                  : ImVec4(1.f,0.3f,0.3f,1.f), "%s", buf);
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Expected");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.expected);
        ImGui::TextUnformatted(buf);

        ImGui::EndTable();
    }

    ImGui::Separator();

    // Bottom table: diagnostic counters
    if (ImGui::BeginTable("##dpdiag", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##ddk", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("##ddv", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Distinct types");
        ImGui::TableSetColumnIndex(1);
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.distinctTypes);
        ImGui::TextUnformatted(buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Invalid ranges");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.invalidRanges > 0) {
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.invalidRanges);
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
        snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.skippedRanges);
        ImGui::TextDisabled("%s", buf);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("Mat mismatches");
        ImGui::TableSetColumnIndex(1);
        if (g_dpSnapshot.materialMismatches > 0) {
            snprintf(buf, sizeof(buf), "%u", g_dpSnapshot.materialMismatches);
            ImGui::TextColored(ImVec4(1.f,0.8f,0.3f,1.f), "%s", buf);
        } else {
            ImGui::TextDisabled("0");
        }

        ImGui::EndTable();
    }

    // ── Type Inspector ────────────────────────────────────────────────────────
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
    if (s_selTypeId < 0)             s_selTypeId = 0;
    if (s_selTypeId >= (int)typeCount) s_selTypeId = (int)typeCount - 1;
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
            snprintf(buf, sizeof(buf), "%u", desc.firstPacket);
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
            ImGui::TableSetColumnIndex(1);
            snprintf(buf, sizeof(buf), "%u", desc.packetCount);
            ImGui::TextUnformatted(buf);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
            ImGui::TableSetColumnIndex(1);
            switch (desc.alphaClass) {
                case 0:  ImGui::TextUnformatted("OPAQUE");     break;
                case 1:  ImGui::TextUnformatted("ALPHA_TEST"); break;
                default:
                    snprintf(buf, sizeof(buf), "?(%u)", desc.alphaClass);
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
            snprintf(buf, sizeof(buf),
                "typeId,firstPacket,packetCount,alphaClass\n%u,%u,%u,%s",
                (uint32_t)s_selTypeId, desc.firstPacket, desc.packetCount, alphaStr);
            ImGui::SetClipboardText(buf);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy type descriptor row as CSV.");
    } else {
        ImGui::TextDisabled("(typeId %d out of range)", s_selTypeId);
    }

    // ── Packet range minimap ──────────────────────────────────────────────────
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Packet Range Minimap##dp")) {
        const uint32_t total  = g_dpSnapshot.expected > 0u ? g_dpSnapshot.expected : 1u;
        const float    availW = ImGui::GetContentRegionAvail().x;
        const float    barH   = 14.f;
        const ImVec2   p0     = ImGui::GetCursorScreenPos();
        ImDrawList*    dl     = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(p0, ImVec2(p0.x + availW, p0.y + barH),
                          IM_COL32(45, 45, 45, 255));

        // Selected type range in yellow
        if (descOk && desc.packetCount > 0u) {
            const float x0 = p0.x + availW * ((float)desc.firstPacket / (float)total);
            float       x1 = p0.x + availW * ((float)(desc.firstPacket + desc.packetCount) / (float)total);
            if (x1 < x0 + 2.f) x1 = x0 + 2.f;
            dl->AddRectFilled(ImVec2(x0, p0.y + 2.f), ImVec2(x1, p0.y + barH - 2.f),
                              IM_COL32(255, 220, 50, 200));
        }

        // Border
        dl->AddRect(p0, ImVec2(p0.x + availW, p0.y + barH),
                    IM_COL32(110, 110, 110, 255));
        ImGui::Dummy(ImVec2(availW, barH));

        snprintf(buf, sizeof(buf), "type %d: [%u, %u) of %u total",
            s_selTypeId,
            descOk ? desc.firstPacket : 0u,
            descOk ? (desc.firstPacket + desc.packetCount) : 0u,
            total);
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
            // Header line: shape | typeId | recipe
            const char* sname = sel.shapeName[0] ? sel.shapeName : "(unnamed)";
            ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.f),
                "%s", sname);
            ImGui::SameLine();
            ImGui::TextDisabled("| typeId: %u | recipe: %d",
                sel.typeId, sel.recipeIndex);

            // Summary row: instances + materialIdx + alphaClass
            if (ImGui::BeginTable("##selpropsummary", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("##spsk", ImGuiTableColumnFlags_WidthFixed, 130.f);
                ImGui::TableSetupColumn("##spsv", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("Instances (vis)");
                ImGui::TableSetColumnIndex(1);
                snprintf(buf, sizeof(buf), "%u", sel.instanceCount);
                ImGui::TextUnformatted(buf);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("materialIdx");
                ImGui::TableSetColumnIndex(1);
                if (sel.materialIdx == 0xFFFFFFFFu)
                    ImGui::TextDisabled("(sentinel)");
                else {
                    snprintf(buf, sizeof(buf), "%u", sel.materialIdx);
                    ImGui::TextUnformatted(buf);
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("alphaClass");
                ImGui::TableSetColumnIndex(1);
                switch (sel.alphaClass) {
                    case 0:  ImGui::TextUnformatted("OPAQUE");      break;
                    case 1:  ImGui::TextUnformatted("ALPHA_TEST");  break;
                    default:
                        snprintf(buf, sizeof(buf), "?(%u)", sel.alphaClass);
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                        break;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("packetCount");
                ImGui::TableSetColumnIndex(1);
                snprintf(buf, sizeof(buf), "%u", sel.packetCount);
                ImGui::TextUnformatted(buf);

                ImGui::EndTable();
            }

            // Per-packet table
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
                        snprintf(buf, sizeof(buf), "%u", row.globalPacketIdx);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(1);
                        snprintf(buf, sizeof(buf), "%u", row.firstIndex);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(2);
                        snprintf(buf, sizeof(buf), "%u", row.indexCount);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(3);
                        snprintf(buf, sizeof(buf), "%d", row.baseVertex);
                        ImGui::TextUnformatted(buf);

                        ImGui::TableSetColumnIndex(4);
                        switch (row.pipelineId) {
                            case 0:  ImGui::TextDisabled("invalid");      break;
                            case 1:  ImGui::TextUnformatted("opaque");    break;
                            case 2:  ImGui::TextUnformatted("alpha_test"); break;
                            default:
                                snprintf(buf, sizeof(buf), "?(%u)", row.pipelineId);
                                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", buf);
                                break;
                        }

                        ImGui::TableSetColumnIndex(5);
                        snprintf(buf, sizeof(buf), "0x%02X", row.materialFlags);
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
                // Header
                int n = snprintf(buf, sizeof(buf),
                    "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                ImGui::SetClipboardText(buf);
                // Rows: build into a larger buffer
                char cbuf[1024]; int cn = 0;
                cn += snprintf(cbuf+cn, sizeof(cbuf)-cn,
                    "pktIdx,firstIndex,idxCount,baseVertex,pipeline,matFlags\n");
                for (uint32_t r = 0; r < sel.rowCount && cn < (int)sizeof(cbuf)-80; ++r) {
                    const DrawPacketPropRow& row = sel.rows[r];
                    const char* plName = row.pipelineId == 1 ? "opaque"
                                       : row.pipelineId == 2 ? "alpha_test" : "invalid";
                    cn += snprintf(cbuf+cn, sizeof(cbuf)-cn,
                        "%u,%u,%u,%d,%s,0x%02X\n",
                        row.globalPacketIdx, row.firstIndex, row.indexCount,
                        row.baseVertex, plName, row.materialFlags);
                }
                ImGui::SetClipboardText(cbuf);
                (void)n;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void drawRenderPassesSection() {
    gosPostProcess* pp = getGosPostProcess();

    // ── Shadow pass ───────────────────────────────────────────────────────────
    if (pp) {
        ImGui::Checkbox("Shadow Pass##rp", &pp->shadowsEnabled_);
        ImGui::SameLine();
        ImGui::TextDisabled("(also in Post-Process)");
    }

    // ── Cloud shadows (fullscreen procedural pass) ────────────────────────────
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

    // GPU objects (full GPU-driven batcher)
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

    const char* fxEnv = getenv("MC2_DISABLE_GOSFX");
    bool fxDisabled = (fxEnv && fxEnv[0] == '1');
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

// ─────────────────────────────────────────────────────────────────────────────

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

    // ── Scene Color (RGBA16F HDR) ─────────────────────────────────────────────
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

    // ── Scene Normals (RGBA16F world-space + shadow-skip) ─────────────────────
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

    // ── Shadow maps ───────────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextUnformatted("Shadow Maps  (DEPTH_COMPONENT24)");
    ImGui::TextDisabled("  Depth textures need GL_TEXTURE_COMPARE_MODE=GL_NONE to preview.");
    ImGui::TextDisabled("  Use the shadow debug overlay in Post-Process instead.");

    // ── Object-ID buffer ──────────────────────────────────────────────────────
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
    // WATER-REFLECTION-RESOURCE-1: substrate target. Renders BLACK until Phase C
    // (WATER-TERRAIN-REFLECTION-1) renders sky+terrain into it.
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

    // ── HZB depth pyramid (HZB-DEBUG-PREVIEW-1) ───────────────────────────────
    // Diagnostic preview of the reverse-Z Hi-Z pyramid. Each level is its own
    // R32F texture, so ImGui::Image previews a chosen level directly. R32F shows
    // in the red channel: reverse-Z near=1.0 = bright red, far/sky=0.0 = black.
    {
        ImGui::Spacing();
        ImGui::Separator();
        if (!pp->isHzbEnabled()) {
            ImGui::TextDisabled("HZB pyramid: off (set MC2_HZB_BUILD=1 to build + preview)");
        } else {
            const int mips = pp->getHzbMipCount();
            ImGui::Text("HZB Pyramid  (R32F per-level, %dx%d, %d mips, builds=%llu)",
                        pp->getHzbWidth(), pp->getHzbHeight(), mips,
                        (unsigned long long)pp->getHzbBuildCount());
            static int s_hzbMip = 0;
            if (s_hzbMip >= mips) s_hzbMip = mips > 0 ? mips - 1 : 0;
            if (mips > 1)
                ImGui::SliderInt("HZB mip##hzb", &s_hzbMip, 0, mips - 1);

            // Level dimensions via the same ceil ladder the runtime uses.
            int lw = pp->getHzbWidth(), lh = pp->getHzbHeight();
            for (int k = 0; k < s_hzbMip; ++k) {
                lw = (lw + 1) / 2; if (lw < 1) lw = 1;
                lh = (lh + 1) / 2; if (lh < 1) lh = 1;
            }
            unsigned int tex = pp->getHzbLevelTexture(s_hzbMip);
            if (tex) {
                ImGui::Text("  level %d = %dx%d", s_hzbMip, lw, lh);
                ImGui::Image((ImTextureID)(intptr_t)tex, sz, uv0, uv1);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("HZB level %d (%dx%d). R32F reverse-Z depth in red:\n"
                                      "near (closer) = bright, far/sky = black.\n"
                                      "Coarser levels store the MIN (farthest occluder).",
                                      s_hzbMip, lw, lh);
            } else {
                ImGui::TextDisabled("  HZB level texture not allocated");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────


} // namespace GraphicsOptionsWindow
