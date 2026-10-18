#include "GraphicsOptionsWindow_internal.h"

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
    if (ImGui::CollapsingHeader("Post-Process")) {
        gosPostProcess* pp = getGosPostProcess();
        if (pp) {
            // ── HDR post + grounding (live tuners) ──────────────────
            // All gates are plain runtime bools read every frame, so toggling
            // here takes effect immediately (env var seeds the startup state).
            ImGui::SeparatorText("Track V (Exposure / SSAO)");
            ImGui::Indent();
            ImGui::SliderFloat("Exposure##trackv", &pp->exposure_, 0.0f, 4.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("composite exposure multiplier (default 1.0)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##expo")) pp->exposure_ = 1.0f;
            ImGui::Unindent();

            ImGui::Checkbox("SSAO##trackv", &pp->ssaoEnabled_);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("MC2_SSAO. Half-res grounding AO.\nIndependent of the HDR master gate.");
            if (pp->ssaoEnabled_) {
                ImGui::Indent();
                // The radius range accounts for the MC2 world scale:
                // one map cell is 42.67 wu
                // (worldUnitsPerVertex 128 / MAPCELL_DIM 3), and the baked kernel
                // is not unit-normalised (mean |v| 0.289), so this slider's value
                // is ~3.5x the actual mean sample reach. The useful band is ~12-48.
                // Both this slider and gos_SetSsaoRadius cap the radius at
                // 64 wu (~1.5 map cells), past which
                // AO stops being a contact term and competes with the shadow cascades.
                ImGui::SliderFloat("Radius (wu)##ssao", &pp->aoRadius_,   0.1f, 64.0f);
                ImGui::SliderFloat("Strength##ssao",    &pp->aoStrength_, 0.0f, 2.0f);
                ImGui::SliderFloat("Bias##ssao",        &pp->aoBias_,     0.0f, 0.05f, "%.4f");
                ImGui::SliderFloat("Power##ssao",       &pp->aoPower_,    0.1f, 4.0f);
                ImGui::Checkbox("Bilateral blur##ssao", &pp->ssaoBlurEnabled_);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("OFF reproduces the pre-SSAO-BLUR-1 chain,\n"
                                      "which had NO denoise at all.");
                ImGui::SliderFloat("Blur depth sigma##ssao", &pp->aoBlurDepthSigma_,
                                   0.002f, 0.10f, "%.4f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Relative depth tolerance for edge-stopping.\n"
                                      "Lower = blur respects silhouettes more strictly\n"
                                      "but denoises less.");
                bool ssaoDbg = (pp->ssaoDebug_ != 0);
                if (ImGui::Checkbox("Debug: show AO buffer##ssao", &ssaoDbg))
                    pp->ssaoDebug_ = ssaoDbg ? 1 : 0;
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##ssao")) {
                    // Restore defaults with a nonzero AO sample footprint.
                    // Radius 3.0 puts every tap in the centre depth texel
                    // (mean reach 0.87 wu), producing zero occlusion.
                    pp->aoRadius_ = 24.0f; pp->aoStrength_ = 0.7f;
                    pp->aoBias_ = 0.0025f; pp->aoPower_ = 1.5f;
                    pp->ssaoBlurEnabled_ = true; pp->aoBlurDepthSigma_ = 0.10f;
                }
                ImGui::Unindent();
            }

            // post anti-aliasing tunables. pp->fxaa* members are
            // read every frame by endScene(), so edits take effect immediately.
            // Env (MC2_FXAA / _SUBPIX / _EDGE_THRESHOLD / _EDGE_THRESHOLD_MIN)
            // seeds startup; these sliders override live.
            ImGui::SeparatorText("FXAA (post anti-aliasing)");
            ImGui::Checkbox("Enable##fxaa", &pp->fxaaEnabled_);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("MC2_FXAA. Edge-smoothing on the composited scene (before UI).\n"
                                  "Fixes terrain / map-edge jaggies on straight lines. Does NOT\n"
                                  "fully kill camera-motion edge crawl — use render-scale/SSAA for that.");
            ImGui::BeginDisabled(!pp->fxaaEnabled_);
            ImGui::SliderFloat("Subpixel##fxaa",    &pp->fxaaSubpix_,           0.0f,   1.0f,   "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Subpixel aliasing removal. Higher = softer thin features.\n"
                                  "Sharper preset = 0.25, Stronger = 0.50.");
            ImGui::SliderFloat("Edge thresh##fxaa", &pp->fxaaEdgeThreshold_,    0.063f, 0.333f, "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Min local contrast to antialias. Lower = catches more edges.\n"
                                  "Sharper preset = 0.166, Stronger = 0.125.");
            ImGui::SliderFloat("Edge min##fxaa",    &pp->fxaaEdgeThresholdMin_, 0.0312f, 0.0833f, "%.4f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Dark-region contrast floor.\nSharper preset = 0.0833, Stronger = 0.0625.");
            if (ImGui::SmallButton("Sharper##fxaa")) {
                pp->fxaaSubpix_ = 0.25f; pp->fxaaEdgeThreshold_ = 0.166f; pp->fxaaEdgeThresholdMin_ = 0.0833f;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Stronger##fxaa")) {
                pp->fxaaSubpix_ = 0.50f; pp->fxaaEdgeThreshold_ = 0.125f; pp->fxaaEdgeThresholdMin_ = 0.0625f;
            }
            ImGui::EndDisabled();

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
                        "  (MC2_DEBUG_RENDERER=1 + MC2_TACTICAL_ARC_OVERLAY=1); here it is a passthrough.\n"
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
            if (pp->showShadowDebug_) {
                ImGui::Indent();
                ImGui::RadioButton("Static##sd",  &pp->shadowDebugMode_, 0); ImGui::SameLine();
                ImGui::RadioButton("Dynamic##sd", &pp->shadowDebugMode_, 1);
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
                    ImGui::TextDisabled("Resources:");
                    for (auto rid : { RenderCore::RenderResourceId::ShadowStaticMap,
                                      RenderCore::RenderResourceId::ShadowDynamicMap }) {
                        const RenderCore::RenderResourceDesc* r =
                            RenderCore::getRenderResource(rid);
                        if (r)
                            ImGui::Text("  %s  %ux%u  %s",
                                r->debugName ? r->debugName : "?",
                                r->width, r->height,
                                RenderCore::toString(r->format));
                        else
                            ImGui::TextDisabled("  %s  (not registered)",
                                RenderCore::toString(rid));
                    }
                    ImGui::TreePop();
                }
                ImGui::Unindent();
            }
            if (ImGui::CollapsingHeader("Shadow Tuning##pp")) {
                ImGui::Indent();
                ImGui::TextDisabled("Polygon offset (static shadow pass only):");
                ImGui::SliderFloat("Factor##bias", &pp->shadowBiasFactor_, 0.0f, 8.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("lower = shadow acne risk\nhigher = peter-panning risk\ndefault 2.0");
                ImGui::SliderFloat("Units##bias",  &pp->shadowBiasUnits_,  0.0f, 20.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("offset units\ndefault 4.0");
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset##bias"))
                    { pp->shadowBiasFactor_ = 2.0f; pp->shadowBiasUnits_ = 4.0f; }
                ImGui::Spacing();
                ImGui::TextDisabled("Softness (all shadow receivers):");
                {
                    float softness = gos_GetTerrainShadowSoftness();
                    if (ImGui::SliderFloat("Softness##sdtune", &softness, 0.5f, 8.0f))
                        gos_SetTerrainShadowSoftness(softness);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("higher = blurrier shadows\ndefault 0.9 (also in Terrain Tuning)");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset##sdtune")) gos_SetTerrainShadowSoftness(0.9f);
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

    ImGui::Separator();
    ImGui::TextDisabled("RAlt hotkeys still active.  Ctrl+Shift+G to close.");

    ImGui::End();
}

} // namespace GraphicsOptionsWindow
