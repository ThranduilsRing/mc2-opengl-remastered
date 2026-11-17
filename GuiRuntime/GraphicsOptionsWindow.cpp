#include "GraphicsOptionsWindow_internal.h"
#include "PlayerFrontEnd.h"
#include "Gates/render_options.h"
#include "../GameOS/gameos/vk_render_infra.h"

namespace GraphicsOptionsWindow {

static bool s_open = false;

void setOpen(bool open) { s_open = open; }
bool isOpen()           { return s_open; }

void draw() {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G))
        s_open = !s_open;

    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(500, 700), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Options  [Ctrl+Shift+G]", &s_open)) {
        ImGui::End();
        return;
    }

    // ── Post-Process ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Water quality", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto opts = mc2::options::current();
        const char* reflectionNames[] = {"Off (sky/environment)", "Low", "Medium", "High"};
        const char* detailNames[] = {"Low", "High"};
        bool changed = ImGui::Combo("Reflection quality", &opts.reflectionQuality, reflectionNames, 4);
        changed |= ImGui::Combo("Water detail", &opts.waterDetail, detailNames, 2);
        static bool saveFailed = false;
        if (changed) {
            saveFailed = !PlayerFrontEnd::saveRenderOptions || !PlayerFrontEnd::saveRenderOptions(opts);
            if (!saveFailed) mc2::options::publish(opts);
        }
        ImGui::TextUnformatted("Saved automatically; applies live.");
        if (saveFailed) ImGui::TextUnformatted("Could not save water quality.");
    }
    PlayerFrontEnd::drawOptions();
    if (ImGui::CollapsingHeader("Post-Process")) {
        gosPostProcess* pp = getGosPostProcess();
        if (pp) {
            // ── HDR post + grounding (live tuners) ──────────────────
            // All gates are plain runtime bools read every frame, so toggling
            // here takes effect immediately (env var seeds the startup state).
            ImGui::SeparatorText("Exposure");
            ImGui::Indent();
            ImGui::SliderFloat("Exposure##trackv", &pp->exposure_, 0.0f, 4.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("composite exposure multiplier (default 1.0)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##expo")) pp->exposure_ = 1.0f;
            ImGui::Unindent();

            ImGui::SeparatorText("Anti-aliasing");
            const char* aaNames[] = {"Off", "SMAA 1x", "FXAA", "CMAA2", "FSR 3.1 native AA"};
            int aa = mc2::options::current().spatialAA;
            static bool aaSaveFailed = false;
            if (ImGui::Combo("Anti-aliasing", &aa, aaNames, IM_ARRAYSIZE(aaNames))) {
                aaSaveFailed = !mc2::options::saveSpatialAA || !mc2::options::saveSpatialAA(aa);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("SMAA 1x is the default: spatial AA without history or jitter.\nFSR 3.1 is optional temporal AA; FXAA uses less memory.");
            if (aaSaveFailed) ImGui::TextUnformatted("Could not save anti-aliasing setting.");

            ImGui::SeparatorText("Texture quality");
            {
                const char* tqNames[] = {"Auto (by VRAM)", "Low", "Medium", "High", "Ultra"};
                auto opts = mc2::options::current();
                int tq = opts.textureQuality + 1;
                static bool tqSaveFailed = false;
                if (ImGui::Combo("Texture quality", &tq, tqNames, IM_ARRAYSIZE(tqNames))) {
                    opts.textureQuality = tq - 1;
                    tqSaveFailed = !PlayerFrontEnd::saveRenderOptions || !PlayerFrontEnd::saveRenderOptions(opts);
                    if (!tqSaveFailed) mc2::options::publish(opts);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Road/runway/cement tile resolution and overlay normal+ORM maps. Applies to missions loaded after the change. Auto picks by VRAM (512 MB or less = Low).");
                ImGui::Text("Active tier: %d", mc2::options::textureTier());
                if (tqSaveFailed) ImGui::TextUnformatted("Could not save texture quality.");
            }

            // view-mode selector.
            // Gated on MC2_VIEWMODE_FRAMEWORK (resolved once at init); when OFF
            // the combo is hidden and endScene() forces Visual (byte-identical).
            if (gos_IsViewmodeFrameworkEnabled()) {
                ImGui::SeparatorText("View Mode");
                // Index == RenderCore::ViewMode value (0..5) so the combo
                // selection maps straight to the mode int.
                const char* kModeNames[] = {
                    "Visual (normal)",
                    "Object ID Debug",
                    "Tactical Overlay",
                    "Thermal (placeholder)",
                    "Infrared (n/a)",
                    "Low Light",
                };
                static const int kModeCount = 6;
                int curMode = gos_GetSelectedViewMode();
                if (curMode < 0 || curMode >= kModeCount) curMode = 0;
                if (ImGui::Combo("##viewmode", &curMode, kModeNames, kModeCount))
                    gos_SetSelectedViewMode(curMode);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Visual: normal rendered output (default).\n"
                        "Object ID Debug: per-pixel ID colorization (needs MC2_OBJECT_ID_BUFFER=1).\n"
                        "Tactical Overlay: drawn in-scene by the debug renderer\n"
                        "  (MC2_TACTICAL_ARC_OVERLAY=1); here it is a passthrough.\n"
                        "Thermal: luminance->iron-palette PLACEHOLDER (not real IR).\n"
                        "Infrared: no implementer (passthrough).\n"
                        "Low Light: night-vision luminance boost + green tint.");
                // live tuners (only meaningful in Low Light).
                if (curMode == 5) {
                    ImGui::Indent();
                    float gain = gos_GetLowLightGain();
                    if (ImGui::SliderFloat("Gain##lowlight", &gain, 0.1f, 16.0f))
                        gos_SetLowLightGain(gain);
                    ImGui::Unindent();
                }
            }

            ImGui::SeparatorText("Legacy / shared");

            ImGui::Checkbox("Shadows##pp",      &pp->shadowsEnabled_);
            ImGui::Checkbox("Shadow Debug##pp", &pp->showShadowDebug_);
            ImGui::Checkbox("Motion Vectors##pp", &pp->showMotionVectors_);
            if (pp->showShadowDebug_) {
                ImGui::Indent();
                if (ImGui::TreeNodeEx("Registries##sd")) {
                    ImGui::TextDisabled("Views:");
                    uint32_t nv = RenderCore::getViewCount();
                    bool foundShadow = false;
                    for (uint32_t i = 0; i < nv; ++i) {
                        const RenderCore::EngineView* v = RenderCore::getViewByIndex(i);
                        if (!v) continue;
                        if (v->kind == RenderCore::ViewKind::ShadowStatic ||
                            v->kind == RenderCore::ViewKind::ShadowDynamic) {
                            ImGui::Text("  id=%u  %s  %dx%d",
                                v->id, v->debugName ? v->debugName : "?",
                                v->viewport[2], v->viewport[3]);
                            foundShadow = true;
                        }
                    }
                    if (!foundShadow)
                        ImGui::TextDisabled("  (none — load a mission first)");
                    ImGui::TreePop();
                }
                ImGui::Unindent();
            }
            ImGui::Separator();
            ImGui::Checkbox("Screen Shadows##pp", &pp->screenShadowEnabled_);
            if (pp->screenShadowEnabled_) {
                ImGui::Indent();
                ImGui::RadioButton("Normal##scr",    &pp->screenShadowDebug_, 0); ImGui::SameLine();
                ImGui::RadioButton("Visualize##scr", &pp->screenShadowDebug_, 1);
                ImGui::Unindent();
            }
            ImGui::Separator();
            ImGui::Checkbox("Shorelines", &pp->shorelineEnabled_);
        } else {
            ImGui::TextDisabled("(post-process system unavailable)");
        }
    }

    // ── Terrain ───────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Terrain"))
        drawTerrainSection();

    // ── Draw Packets ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Draw Packets"))
        drawDrawPacketsSection();

    // ── Render Passes ─────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Render Passes"))
        drawRenderPassesSection();

    // ── Terrain Tuning ────────────────────────────────────────────────────────
    // Heavy section (15 sliders) — collapsed by default so it doesn't push other
    // sections off-screen on first open.
    if (ImGui::CollapsingHeader("Terrain Tuning"))
        drawTerrainTuningSection();

    // ── Static Prop Tuning ────────────────────────────────────────────────────
    // IBL SH + PBR V1 strength + roughness override. Consolidated here so
    // the controls are reachable without picking a prop.
    if (ImGui::CollapsingHeader("Static Prop Tuning"))
        drawStaticPropTuningSection();

    // ── Mech ──────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Mech"))
        drawMechSection();

    // ── Water ─────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Water"))
        drawWaterSection();

    // ── VFX Tuning ────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("VFX Tuning"))
        drawVfxTuningSection();

    // ── HUD ───────────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("HUD")) {
        float scale = gos_GetHudScale();
        if (ImGui::SliderFloat("Scale##hud", &scale, 0.5f, 1.5f, "%.2f"))
            gos_SetHudScale(scale);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##hud")) gos_SetHudScale(1.0f);
    }

    // ── GBuffer Preview ───────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("GBuffer Preview"))
        drawGBufferPreview();

    // ── Env Gates ─────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Env Gates"))
        drawEnvGatesSection();

    // ── Debug Overlays ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Debug Overlays")) {
        if (ImGui::Button("Advance ProjectZ Overlay"))
            projectz_overlay_advance();
        ImGui::SameLine();
        ImGui::TextDisabled("(cycles predicates)");
    }

    // ── Visual Tuning Profile ─────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Visual Tuning Profile")) {
        const char* path    = visualTuning_getProfilePath();
        const char* mission = visualTuning_getActiveMission();
        bool        hasFile = visualTuning_hasProfileFile();
        int         keys    = visualTuning_getAppliedKeyCount();
        ImGui::TextDisabled("File: %s", path);
        if (hasFile) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Loaded");
            ImGui::SameLine();
            ImGui::Text("mission=%s  keys=%d", mission[0] ? mission : "(none)", keys);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No profile file (data/visual_tuning.json)");
        }
        if (ImGui::Button("Reset to Profile"))
            visualTuning_applyProfile(mission[0] ? mission : nullptr);
        ImGui::SameLine();
        ImGui::TextDisabled("re-applies profile; ImGui sliders override after");

        static int s_saveFlash = 0;
        if (mission[0]) {
            if (ImGui::Button("Set as Mission Defaults")) {
                s_saveFlash = visualTuning_saveCurrentToMission() ? 120 : -120;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("writes current slider state to JSON for '%s'", mission);
        }
        if (s_saveFlash > 0) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Saved!");
            s_saveFlash--;
        } else if (s_saveFlash < 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Write failed!");
            s_saveFlash++;
        }
    }

    mc2vk::infra::panel();
    ImGui::Separator();
    ImGui::TextDisabled("RAlt hotkeys still active.  Ctrl+Shift+G to close.");

    ImGui::End();
}

} // namespace GraphicsOptionsWindow
