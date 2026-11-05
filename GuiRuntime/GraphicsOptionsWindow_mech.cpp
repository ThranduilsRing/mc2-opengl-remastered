// Graphics options window: env gates, mech and VFX sections.
// Declared in GraphicsOptionsWindow_internal.h.
#include "GraphicsOptionsWindow_internal.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

namespace GraphicsOptionsWindow {

// ─────────────────────────────────────────────────────────────────────────────
// Env gate table — baked at startup, shown read-only.
// defaultOn=true means the feature is enabled when the var is absent/non-zero.
// ─────────────────────────────────────────────────────────────────────────────
struct EnvGate {
    const char* var;
    const char* desc;
    bool        defaultOn;
    bool        inverted;   // true = presence of var DISABLES the feature
};

static const EnvGate kEnvGates[] = {
    { "MC2_GPU_DRIVEN",             "GPU-driven rendering master gate",          false, false },
    { "MC2_GPU_CULL",               "GPU frustum cull",                          false, false },
    { "MC2_GPU_CULL_SUBSTRATE",     "GPU substrate (default-on)",                true,  false },
    { "MC2_GPU_PARTICLES",          "GPU particle system",                       false, false },
    { "MC2_MATERIAL_GPU",           "MaterialGpu lookup path",                   false, false },
    { "MC2_RENDER_WATER_FASTPATH",  "Fast water render path",                    false, false },
    { "MC2_OBJECT_ID_BUFFER",       "Object-ID picking buffer",                  false, false },
    { "MC2_HDRI_SKY",               "HDRI skybox",                               false, false },
    { "MC2_DISABLE_GOSFX",          "FX/particles (var disables)",               false, true  },
    { "MC2_RENDER_CONTRACT_ASSERT", "Render-contract GL state validation",        false, false },
    { "MC2_IMGUI",                  "Dear ImGui overlay (default-on)",           true,  false },
};
static const int kEnvGateCount = static_cast<int>(sizeof(kEnvGates) / sizeof(kEnvGates[0]));
void drawEnvGatesSection() {
    ImGui::TextDisabled("Baked at startup — changes require restart.");
    ImGui::Spacing();

    if (!ImGui::BeginTable("##envtbl", 3,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("##st",   ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("##var",  ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableSetupColumn("##desc", ImGuiTableColumnFlags_WidthStretch);

    for (const auto & g : kEnvGates) {
        const auto st = mc2::gate::boolState(g.var);   // the name comes from the table above
        const bool present = st && st->set;

        bool enabled;
        if (g.inverted) {
            // Present → feature disabled.  Absent → feature enabled.
            enabled = !present;
        } else if (g.defaultOn) {
            enabled = !present || st->on;
        } else {
            enabled = present && st->on;
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (enabled)
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "[ON] ");
        else
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[off]");

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(g.var);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", g.desc);
    }

    ImGui::EndTable();
}


void drawMechSection() {
    // BT2018 imported-skin albedo correction (imported BT mechs ONLY; stock untouched).
    // Imported skins are sRGB-authored PBR albedo; the legacy lighting path samples
    // them un-linearized, so they read washed/flat. Gamma decodes sRGB->linear;
    // Scale brings dark skins back to legacy brightness parity.
    ImGui::SeparatorText("BT2018 Imported Skin");
    {
        float ig = batcher_getImportedMechGamma();
        if (ImGui::SliderFloat("Imported Albedo Gamma##mech", &ig, 1.0f, 2.4f, "%.2f"))
            batcher_setImportedMechGamma(ig);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("sRGB->linear exponent, IMPORTED BT mechs only.\n"
                              "2.2 = full decode (over-darkens dark skins in legacy\n"
                              "lighting); lower eases it. Stock mechs unaffected.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impgamma")) batcher_setImportedMechGamma(2.2f);

        float is = batcher_getImportedMechAlbedoScale();
        if (ImGui::SliderFloat("Imported Albedo Scale##mech", &is, 0.25f, 2.5f, "%.2f"))
            batcher_setImportedMechAlbedoScale(is);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Post-gamma brightness, IMPORTED BT mechs only.\n"
                              "Raise to pull dark skins toward legacy parity.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impscale")) batcher_setImportedMechAlbedoScale(1.0f);

        float ao = batcher_getImportedMechAoStrength();
        if (ImGui::SliderFloat("Imported AO Strength##mech", &ao, 0.0f, 1.0f, "%.2f"))
            batcher_setImportedMechAoStrength(ao);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ambient-occlusion multiply, IMPORTED BT mechs with AO only.\n"
                              "0 = no-op (pre-AO look); 1 = full. Default 0.5. Mechs without\n"
                              "an AO map are unaffected (white/no-op).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impao")) batcher_setImportedMechAoStrength(0.5f);

        float ns = batcher_getImportedMechNormalStrength();
        if (ImGui::SliderFloat("Imported Normal Strength##mech", &ns, 0.0f, 1.0f, "%.2f"))
            batcher_setImportedMechNormalStrength(ns);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tangent-space normal-map strength, IMPORTED BT mechs with a\n"
                              "normal map only. 0 = no-op (geometric normal / fallback-proven);\n"
                              "1 = full map. Default 1.0. Mechs without a normal map are\n"
                              "unaffected (v_normal fallback).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##impnrm")) batcher_setImportedMechNormalStrength(1.0f);
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Mech Lighting");

    bool specOn = batcher_getMechSpecularEnabled() != 0;
    if (ImGui::Checkbox("Specular##mech", &specOn))
        batcher_setMechSpecularEnabled(specOn ? 1 : 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blinn specular sheen on mech hull (legacy path).\n"
                          "No effect when StandardLit GGX is ON (Blinn path bypassed).\n"
                          "Kill-switch: MC2_MECH_SPECULAR_V1=0.");
    ImGui::SameLine();
    float ss = batcher_getMechSpecularStrength();
    if (ImGui::SliderFloat("Strength##mechspec", &ss, 0.0f, 2.0f, "%.2f"))
        batcher_setMechSpecularStrength(ss);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mechspec")) batcher_setMechSpecularStrength(0.05f);

    ImGui::Spacing();
    ImGui::SeparatorText("PBR Material");

    float mr = batcher_getMechMetalRoughness();
    if (ImGui::SliderFloat("Metal roughness##mech", &mr, 0.04f, 1.0f, "%.2f"))
        batcher_setMechMetalRoughness(mr);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PBR roughness for armour. 0.04=mirror, 1.0=matte. Default: 0.85");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mechmr")) batcher_setMechMetalRoughness(0.85f);

    float gr = batcher_getMechGlassRoughness();
    if (ImGui::SliderFloat("Glass roughness##mech", &gr, 0.04f, 1.0f, "%.2f"))
        batcher_setMechGlassRoughness(gr);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PBR roughness for cockpit glass. Default: 0.25");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##mechgr")) batcher_setMechGlassRoughness(0.25f);

    ImGui::TextDisabled("ambient/specular strength also settable via visual_tuning.json");

}


// GPU particle look tuning. All scales default 1.0 (no-op,
// byte-identical default frame). Look-only — no emission/lifetime/sorting
// change. Tuning is invisible when MC2_GPU_PARTICLES=0 (legacy CPU FX) or in
// scenes with no routed (Card/CardCloud/Point/Shard/Tube) particles.
void drawVfxTuningSection() {
    // Single source of truth: the same reader the batcher uses (default OFF,
    // opt in with MC2_GPU_PARTICLES=1), not a private re-parse of the env var.
    const bool particlesOn = mc2::particles::Batcher::is_enabled();
    if (particlesOn) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "GPU particles: ON (MC2_GPU_PARTICLES=1)");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
            "GPU particles: OFF (default; set MC2_GPU_PARTICLES=1 — sliders have no effect)");
    }

    ImGui::BeginDisabled(!particlesOn);

    // Debug view selector (also exposed via MC2_VFX_DEBUG_MODE / inspector).
    ImGui::SeparatorText("Debug view");
    {
        static const char* kVfxModes[] = {
            "0 Final", "1 Albedo", "2 Alpha", "3 ParticleKind", "4 Overdraw" };
        int mode = gos_vfx_getDebugMode();
        if (ImGui::Combo("Mode##vfxdbg", &mode, kVfxModes, 5))
            gos_vfx_setDebugMode(mode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("particle_billboard.frag u_debugMode. Mode 0 = "
                              "byte-identical default. Diagnostic only.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxdbg")) gos_vfx_setDebugMode(0);
    }

    // Intensity scales. Defaults 1.0 = byte-identical.
    ImGui::SeparatorText("Intensity");
    {
        float b = gos_vfx_getBrightness();
        // Slider range matches the backend clamp (0..8) so a profile value
        // above 4 is not silently reduced on first touch.
        if (ImGui::SliderFloat("Brightness##vfx", &b, 0.0f, 8.0f, "%.2f"))
            gos_vfx_setBrightness(b);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Global RGB scale on all particles. Default 1.0 (no-op).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxb")) gos_vfx_setBrightness(1.0f);

        float ab = gos_vfx_getAdditiveBrightness();
        if (ImGui::SliderFloat("Additive brightness##vfx", &ab, 0.0f, 8.0f, "%.2f"))
            gos_vfx_setAdditiveBrightness(ab);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Extra RGB scale applied ONLY to additive groups\n"
                              "(flares/explosions). Default 1.0 (no-op). The\n"
                              "highest-value lever for pre-bloom additive overdraw.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxab")) gos_vfx_setAdditiveBrightness(1.0f);

        float a = gos_vfx_getAlphaScale();
        if (ImGui::SliderFloat("Opacity##vfx", &a, 0.0f, 2.0f, "%.2f"))
            gos_vfx_setAlphaScale(a);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Alpha (opacity) scale on all particles. Default 1.0 (no-op).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxa")) gos_vfx_setAlphaScale(1.0f);

        // depth-fade enable + world-unit fade band.
        ImGui::Separator();
        bool softOn = gos_vfx_getSoftEnabled() != 0;
        if (ImGui::Checkbox("Soft particles##vfx", &softOn))
            gos_vfx_setSoftEnabled(softOn ? 1 : 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Depth-fade alpha particles where they meet opaque geometry\n"
                              "(no hard intersection lines). Default OFF (options.cfg VfxSoftParticles).\n"
                              "Alpha groups only; additive flashes/lasers unaffected.");
        float sd = gos_vfx_getSoftDistance();
        if (ImGui::SliderFloat("Soft fade dist##vfx", &sd, 0.0f, 200.0f, "%.1f"))
            gos_vfx_setSoftDistance(sd);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World-unit fade band for soft particles. Larger = softer "
                              "intersection. Default 30.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxsd")) gos_vfx_setSoftDistance(30.0f);

        bool litOn = gos_vfx_getLitEnabled() != 0;
        if (ImGui::Checkbox("Lit particles##vfx", &litOn))
            gos_vfx_setLitEnabled(litOn ? 1 : 0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tint alpha smoke/dust by the scene sun + ambient so it\n"
                              "reads as lit volume. Default OFF (options.cfg VfxLitParticles).\n"
                              "Alpha groups only; additive flashes/lasers stay emissive.");
        float ls = gos_vfx_getLitStrength();
        if (ImGui::SliderFloat("Lit strength##vfx", &ls, 0.0f, 1.0f, "%.2f"))
            gos_vfx_setLitStrength(ls);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = unlit (byte-identical), 1 = fully scene-lit. Default 0.7.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##vfxls")) gos_vfx_setLitStrength(0.7f);
    }

    if (ImGui::SmallButton("Reset all##vfx")) {
        gos_vfx_setBrightness(1.0f);
        gos_vfx_setAdditiveBrightness(1.0f);
        gos_vfx_setAlphaScale(1.0f);
    }
    ImGui::TextDisabled("Defaults (1.0) are byte-identical. Look-only: no "
                        "emission/lifetime/timing change.");

    ImGui::EndDisabled();
}



} // namespace GraphicsOptionsWindow
