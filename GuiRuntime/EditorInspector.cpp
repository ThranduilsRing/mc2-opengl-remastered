#include "EditorInspector.h"
#include "imgui.h"
#include <cstdio>    // snprintf, fopen, fprintf, fclose
#include <cstdlib>   // getenv

// HZB camera-view dump: thin C helper defined in code/gamecam.cpp so we don't
// have to pull the full camera.h -> terrain.h chain into GuiRuntime.
extern "C" int mc2_hzb_dump_camera_view(void);
// Bookmark-schema camera save (BOOKMARK-SAVE-BUTTON-1): same thin-C pattern.
extern "C" void mc2_save_camera_bookmark(char* outPath, int outPathLen);
#include "../GameOS/gameos/debug_renderer.h"  // IMG-INSPECT-3
#include "../GameOS/gameos/gos_frame_pass_stats.h"  // FRAME-INSPECTOR-1
#include "../GameOS/gameos/gos_render_pass_timer.h"  // FRAME-INSPECTOR-1 (ms col)
#include "../GameOS/gameos/ibl_sh_runtime.h"   // V-IBL-STATIC-1: g_iblShStrength
#include "draw_packet_emitter.h"              // g_dpSelectedRecipeIndex
#include "../RenderCore/RendererFeatureRegistry.h"
#include "../RenderCore/RenderPassContract.h"  // RENDERPASS-CONTRACT-2.5 (descriptive table)
#include "RenderDebugView.h"                   // DEBUG-VIEW-REGISTRY-1

// Terrain tunable C-API used to be forward-declared here for an inline
// Terrain Pass tunables panel; those controls were moved to
// GuiRuntime/GraphicsOptionsWindow.cpp (the existing terrain tuning home),
// so the forward decls and the inline panel are gone. EditorInspector's
// Terrain Pass section is now read-only diagnostic only.

// V-MATERIAL-STATIC-0: forward-declared inventory contract — duplicating the
// struct keeps gui_runtime independent of gos_static_prop_batcher.h, which
// transitively pulls Stuff/Stuff.hpp (not visible in this TU). The single
// source of truth is GameOS/gameos/gos_static_prop_batcher.h; layout must
// match exactly. Sized-fields verified against that header.
struct StaticPropMaterialInventoryEntry {
    uint32_t materialIdx;
    uint32_t albedoTexLayer;
    uint32_t alphaGroup;
    uint32_t flags;
    uint32_t nodeIdx;
    uint32_t textureWidth;
    uint32_t textureHeight;
    uint32_t usageCount;
    char     textureName[64];
    bool     placeholder;
    float    metallicFactor;   // V-MATERIAL-PBR-1
    float    roughnessFactor;  // V-MATERIAL-PBR-1
};
uint32_t batcher_getStaticPropMaterialInventoryCount();
bool     batcher_getStaticPropMaterialInventoryEntry(
             uint32_t idx, StaticPropMaterialInventoryEntry* out);
// WATER-DEBUG-VIEWS-1: MDI water FS debug mode accessor (defined at global
// scope in GameOS/gameos/gameos_graphics.cpp). Declared here at file scope so
// the call inside namespace EditorInspector resolves to the global symbol
// (a block-scope extern would bind to EditorInspector::, causing LNK2019).
int gos_GetWaterFsDebugMode();

// DEBUG-VIEW-REGISTRY-1: runtime debug-mode getter/setter + view<->shaderMode helpers
// (gos_static_prop_batcher.cpp).
void batcher_setDebugMaterialMode(int shaderMode);
int  batcher_getDebugMaterialMode();
int             StaticPropViewToShaderMode(RenderDebugView view);
RenderDebugView StaticPropShaderModeToView(int shaderMode);

// MECH-SPINE-1: read-only accessors for mech pass-level state. Defined in
// gos_mech_batcher.cpp; declared here so the inspector can reference them
// without including engine-private mech batcher headers.
extern "C" uint32_t gos_getMechProgramId();
extern "C" uint32_t gos_getMechShadowProgramId();
extern "C" const char* gos_getMechTextureNameByNodeIdx(uint32_t nodeIdx);

// MECH-DEBUG-VIEWS-1: debug-mode getter/setter + view<->shaderMode helpers
// (gos_mech_batcher.cpp). Same pattern as StaticProp decls above.
// Not declared extern "C": pure C++ functions over RenderDebugView enum.
int             MechViewToFragDebugMode(RenderDebugView view);
RenderDebugView MechFragDebugModeToView(int shaderMode);
extern "C" void batcher_setMechDebugMode(int shaderMode);
extern "C" int  batcher_getMechDebugMode();

// MECH-NORMALS-FIX-1: live normal recompute API (gos_mech_batcher.cpp).
// Declared extern "C" to avoid including gos_mech_batcher.h which
// transitively pulls Stuff/Stuff.hpp (not visible in GuiRuntime TU).
extern "C" void  batcher_setMechNormalsMode(int mode);
extern "C" int   batcher_getMechNormalsMode();
extern "C" void  batcher_setMechNormalsSmoothDeg(float deg);
extern "C" float batcher_getMechNormalsSmoothDeg();
extern "C" void  batcher_rebuildMechNormals();
// MECH-AMBIENT-1: gated hemisphere ambient fill (per-flush uniform, no rebuild).
extern "C" void  batcher_setMechAmbientEnabled(int on);
extern "C" int   batcher_getMechAmbientEnabled();
extern "C" void  batcher_setMechAmbientStrength(float s);
extern "C" float batcher_getMechAmbientStrength();
// MECH-SPECULAR-V1: gated Blinn specular sheen (per-flush uniforms, no rebuild).
// Effective only when MC2_MECH_VIEWUNIFORMS=1 (shader has camera position data).
extern "C" void  batcher_setMechSpecularEnabled(int on);
extern "C" int   batcher_getMechSpecularEnabled();
extern "C" void  batcher_setMechSpecularStrength(float s);
extern "C" float batcher_getMechSpecularStrength();
extern "C" void  batcher_setMechMetalRoughness(float r);
extern "C" float batcher_getMechMetalRoughness();
extern "C" void  batcher_setMechGlassRoughness(float r);
extern "C" float batcher_getMechGlassRoughness();
extern "C" void  batcher_setMechGlassLumaThresh(float t);
extern "C" float batcher_getMechGlassLumaThresh();
extern "C" void  batcher_setMechGlassMaxChanThresh(float t);
extern "C" float batcher_getMechGlassMaxChanThresh();
extern "C" void  batcher_setMechSpecDebugMask(int on);
extern "C" int   batcher_getMechSpecDebugMask();

namespace {

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_IMGUI_INSPECTOR");
        cached = (!v || v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

static const char* pickKindName(EditorInspector::InspectorPickKind k) {
    switch (k) {
        case EditorInspector::InspectorPickKind::StaticProp: return "StaticProp";
        case EditorInspector::InspectorPickKind::Mech:       return "Mech";
        case EditorInspector::InspectorPickKind::Terrain:    return "Terrain";
        default:                                              return "None";
    }
}

static EditorInspector::InspectorSelection     s_selection;
static EditorInspector::StaticPropInspectorData s_staticPropData;
static EditorInspector::MechInspectorData       s_mechData;
static EditorInspector::TerrainInspectorData    s_terrainData;
static EditorInspector::TerrainPassSnapshot     s_terrainPass;   // TERRAIN-SPINE-0
static EditorInspector::ShadowPassSnapshot      s_shadowPass;    // SHADOW-SPINE-0
static EditorInspector::VfxPassSnapshot         s_vfxPass;       // VFX-SPINE-0
static bool s_open = false;
static bool s_featuresOpen = false;
static bool s_frameInspectorOpen = false;   // FRAME-INSPECTOR-1
static bool s_framePrevCollect   = false;   // last SetCollect we issued

// FRAME-INSPECTOR-1: standalone "Frame Inspector" window. Live per-pass render
// stats (draws/instances/GL state) from gos_frame_pass_stats. Editor-only: the
// collect flag is flipped on only while this window is open, so the game build
// (env unset, tab never opened) keeps collection off = zero cost. Toggle:
// Ctrl+Shift+P. Mirrors the "Renderer Features" window pattern below.
static void drawFrameInspectorWindow() {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P))
        s_frameInspectorOpen = !s_frameInspectorOpen;

    // Drive the collector's runtime collect flag from window visibility.
    if (s_frameInspectorOpen != s_framePrevCollect) {
        gos_frame_pass_stats::SetCollect(s_frameInspectorOpen);
        gos_render_pass_timer::SetCollect(s_frameInspectorOpen);
        s_framePrevCollect = s_frameInspectorOpen;
    }
    if (!s_frameInspectorOpen) return;

    ImGui::SetNextWindowSize(ImVec2(680, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Frame Inspector", &s_frameInspectorOpen)) {
        ImGui::TextUnformatted("Per-pass render stats (last frame)  --  Ctrl+Shift+P to close");

        const auto& agg = gos_frame_pass_stats::GetFrameAggregates();
        ImGui::Text("chunks=%u  spBatches=%u  mechInst=%u  vfx=%u",
                    agg.visibleTerrainChunks, agg.staticPropBatches,
                    agg.mechBatchInstances, agg.vfxCount);
        ImGui::Separator();

        if (ImGui::BeginTable("##framepass", 9,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Pass",  ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("ms",    ImGuiTableColumnFlags_WidthFixed,  56.f);
            ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed,  60.f);
            ImGui::TableSetupColumn("Inst",  ImGuiTableColumnFlags_WidthFixed,  60.f);
            ImGui::TableSetupColumn("FBO",   ImGuiTableColumnFlags_WidthFixed,  48.f);
            ImGui::TableSetupColumn("VP",    ImGuiTableColumnFlags_WidthFixed,  92.f);
            ImGui::TableSetupColumn("Depth(t/m)",
                                             ImGuiTableColumnFlags_WidthFixed,  80.f);
            ImGui::TableSetupColumn("Blend", ImGuiTableColumnFlags_WidthFixed,  48.f);
            ImGui::TableSetupColumn("Cull",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            const int n = gos_frame_pass_stats::PassCount();
            for (int p = 0; p < n; ++p) {
                const auto& r = gos_frame_pass_stats::GetPassRow(p);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (r.ran)
                    ImGui::TextUnformatted(gos_frame_pass_stats::PassKey(p));
                else
                    ImGui::TextDisabled("%s", gos_frame_pass_stats::PassKey(p));

                // ms column (index 1): last-window mean GPU ms from the timer.
                // The pass enum is shared (gos_frame_pass_stats records via
                // gos_render_pass_timer::Pass), so p indexes both. Blank when no
                // GPU sample was harvested in the last window.
                const auto tp = static_cast<gos_render_pass_timer::Pass>(p);
                ImGui::TableSetColumnIndex(1);
                if (gos_render_pass_timer::HasSample(tp))
                    ImGui::Text("%.2f", gos_render_pass_timer::LastMs(tp));
                else
                    ImGui::TextDisabled("-");

                if (!r.ran) {
                    // Pass did not run this frame: leave the rest blank.
                    for (int c = 2; c < 9; ++c) {
                        ImGui::TableSetColumnIndex(c);
                        ImGui::TextDisabled("--");
                    }
                    continue;
                }
                ImGui::TableSetColumnIndex(2); ImGui::Text("%u", r.drawCount);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%u", r.instanceCount);
                ImGui::TableSetColumnIndex(4); ImGui::Text("%u", r.fbo);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%dx%d", r.viewport[2], r.viewport[3]);
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%d/%d", r.depthTest ? 1 : 0, r.depthMask ? 1 : 0);
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%d", r.blend ? 1 : 0);
                ImGui::TableSetColumnIndex(8);
                ImGui::Text("%d", r.cull ? 1 : 0);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    // The window can be closed via the title-bar X; re-sync the collect flag on
    // the next call (handled at the top of this function via s_framePrevCollect).
}

}  // namespace

void EditorInspector::onCtrlShiftClick(int mouseX, int mouseY) {
    if (!isEnabled()) return;
    // Coords are recorded; missiongui.cpp calls setPickResult after
    // running tryGameplayPick to populate the full selection.
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};
    s_selection.screenX = mouseX;
    s_selection.screenY = mouseY;
    g_dpSelectedRecipeIndex = -1;  // cleared until setStaticPropData confirms
    s_open = true;
}

void EditorInspector::setPickResult(int mouseX, int mouseY,
                                    const RenderWorld::LookupResult& lookup) {
    if (!isEnabled()) return;
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};
    s_selection.screenX = mouseX;
    s_selection.screenY = mouseY;
    s_selection.valid   = lookup.isValid;
    s_selection.lookup  = lookup;
    // Clear bridge until setStaticPropData fires with the confirmed recipeIndex.
    // Non-StaticProp picks never call setStaticPropData, so they stay at -1.
    g_dpSelectedRecipeIndex = -1;
    if (lookup.isValid) {
        s_selection.kind   = lookup.kind;
        s_selection.handle = lookup.handle;
        s_selection.hasSelection = true;
        if (lookup.kind == RenderWorld::RenderObjectKind::StaticProp)
            s_selection.pickKind = InspectorPickKind::StaticProp;
        else if (lookup.kind == RenderWorld::RenderObjectKind::Mech)
            s_selection.pickKind = InspectorPickKind::Mech;
        else
            s_selection.pickKind = InspectorPickKind::None;
    }
    s_open = true;
}

void EditorInspector::setStaticPropData(const StaticPropInspectorData& sd) {
    s_staticPropData = sd;
    // Publish recipeIndex so gameosmain can fill g_dpSelProp each frame.
    g_dpSelectedRecipeIndex = sd.populated ? sd.recipeIndex : -1;
}

void EditorInspector::setMechData(const MechInspectorData& md) {
    s_mechData = md;
}

void EditorInspector::setTerrainData(const TerrainInspectorData& td) {
    if (!isEnabled()) return;
    s_terrainData = td;
    s_selection.hasSelection     = td.populated;
    s_selection.pickKind         = InspectorPickKind::Terrain;
    s_selection.lookup.worldX      = td.worldX;
    s_selection.lookup.worldY      = td.worldY;
    s_selection.lookup.worldZ      = td.worldZ;
    s_selection.lookup.worldPosValid = td.populated;
    // NB: s_selection.valid stays false (no RenderWorld lookup).
}

void EditorInspector::setTerrainPassSnapshot(const TerrainPassSnapshot& ts) {
    // TERRAIN-SPINE-0: pass-level snapshot. Always accept (no enable gate so the
    // snapshot is current even when the inspector window is closed; drawImGui is
    // already gated by isEnabled()).
    s_terrainPass = ts;
}

void EditorInspector::setShadowPassSnapshot(const ShadowPassSnapshot& sp) {
    // SHADOW-SPINE-0: same always-accept policy as the terrain snapshot.
    s_shadowPass = sp;
}

void EditorInspector::setVfxPassSnapshot(const VfxPassSnapshot& vs) {
    // VFX-SPINE-0: same always-accept policy as the terrain / shadow snapshots.
    s_vfxPass = vs;
}

void EditorInspector::clear() {
    s_selection      = InspectorSelection{};
    s_staticPropData = StaticPropInspectorData{};
    s_mechData       = MechInspectorData{};
    s_terrainData    = TerrainInspectorData{};
    g_dpSelectedRecipeIndex = -1;
    s_open = false;
}

void EditorInspector::drawImGui() {
    if (!isEnabled()) return;

    // Ctrl+Shift+I toggles the panel regardless of current open state.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I))
        s_open = !s_open;

    // Renderer Features — standalone window. Runs independent of Object Inspector.
    // Ctrl+Shift+F to toggle. Placed BEFORE if (!s_open) return.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F))
        s_featuresOpen = !s_featuresOpen;

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

                for (int i = 0; i < static_cast<int>(RenderCore::RendererFeature::COUNT); ++i) {
                    const RenderCore::EnvVarDesc& d = RenderCore::kFeatureTable[i];
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
                        const char* v = std::getenv(d.envVar);
                        bool on;
                        const char* src;
                        if (!v) {
                            on  = d.defaultOn;
                            src = d.defaultOn ? "on (default)" : "off (default)";
                        } else if (v[0] == '0') {
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

    // FRAME-INSPECTOR-1: live per-pass render stats window (Ctrl+Shift+P).
    // Independent of the Object Inspector; runs before the s_open early-return.
    drawFrameInspectorWindow();

    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(440, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Object Inspector", &s_open)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Ctrl+Shift+F -- Renderer Features   Ctrl+Shift+P -- Frame Inspector");
    ImGui::Separator();

    const auto& lk = s_selection.lookup;

    // Object-ID — shown even for invalid / failed picks.
    if (ImGui::CollapsingHeader("Object-ID", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Raw pixel:     0x%08X", lk.rawObjectId);
        if (lk.rawObjectId != 0u) {
            RenderCore::RenderObjectHandle hraw;
            hraw.bits = lk.rawObjectId;
            ImGui::Text("  idx [19:0]:  %u", hraw.index());
            ImGui::Text("  gen [31:20]: %u", hraw.generation());
        }
        if (lk.isValid) {
            ImGui::TextColored(ImVec4(0.4f,1.0f,0.4f,1.f), "Valid:         yes");
        } else {
            ImGui::TextColored(ImVec4(1.0f,0.55f,0.3f,1.f), "Valid:         no");
            if (lk.lookupFailReason)
                ImGui::Text("  Reason:      %s", lk.lookupFailReason);
        }
    }

    if (!s_selection.hasSelection) {
        ImGui::Spacing();
        ImGui::TextUnformatted("No valid object selected.");
        ImGui::TextUnformatted("Ctrl+Shift+LMB to pick.");
        ImGui::Text("Screen: (%d, %d)", s_selection.screenX, s_selection.screenY);
        ImGui::End();
        return;
    }

    // Generic header
    const char* kindName = pickKindName(s_selection.pickKind);
    ImGui::Separator();
    ImGui::Text("Kind:          %s", kindName);
    ImGui::Text("Handle raw:    0x%08X", s_selection.handle.raw());
    ImGui::Text("Handle idx:    %u / gen: %u",
        s_selection.handle.index(), s_selection.handle.generation());
    ImGui::Text("Screen:        (%d, %d)", s_selection.screenX, s_selection.screenY);
    if (lk.worldPosValid)
        ImGui::Text("World:         (%.1f, %.1f, %.1f)", lk.worldX, lk.worldY, lk.worldZ);
    else
        ImGui::TextDisabled("World:         (unavailable)");

    // Save Camera View (HZB) — dumps current eye position/rotation to saved_view.txt
    // so it can be replayed via MC2_HZB_VIEW_FILE. Uses a thin C helper in gamecam.cpp
    // to avoid pulling the camera.h -> terrain.h header chain into GuiRuntime.
    {
        static bool s_hzbSaved = false;
        static char s_bookmarkPath[512] = {0};
        static bool s_hzbFailed = false;
        ImGui::Spacing();
        // Primary: bookmark-schema JSON save (BOOKMARK-SAVE-BUTTON-1).
        // Directory = MC2_BOOKMARK_SAVE_DIR or ./saved_bookmarks; the file is
        // directly consumable by MC2_VISUAL_BOOKMARK_CAPTURE.
        if (ImGui::Button("Save Camera Bookmark")) {
            mc2_save_camera_bookmark(s_bookmarkPath, (int)sizeof(s_bookmarkPath));
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

    // Copy All — formats everything visible in the window to the clipboard.
    ImGui::Spacing();
    if (ImGui::Button("Copy All")) {
        char buf[3072];
        int  n = 0;
        n += std::snprintf(buf+n, sizeof(buf)-n,
            "ObjID raw: 0x%08X  valid: %s%s%s\n",
            lk.rawObjectId, lk.isValid ? "yes" : "no",
            (!lk.isValid && lk.lookupFailReason) ? "  reason: " : "",
            (!lk.isValid && lk.lookupFailReason) ? lk.lookupFailReason : "");
        n += std::snprintf(buf+n, sizeof(buf)-n, "Kind: %s\n", kindName);
        n += std::snprintf(buf+n, sizeof(buf)-n,
            "Handle: 0x%08X  idx %u / gen %u\n",
            s_selection.handle.raw(),
            s_selection.handle.index(), s_selection.handle.generation());
        if (lk.worldPosValid)
            n += std::snprintf(buf+n, sizeof(buf)-n,
                "World: (%.1f, %.1f, %.1f)\n", lk.worldX, lk.worldY, lk.worldZ);
        n += std::snprintf(buf+n, sizeof(buf)-n,
            "Mesh: 0x%08X  Mat: 0x%08X  LOD: %u  Pipeline: %u\n"
            "DrawPacket: %u  PathReason: 0x%08X  GameObjID: %u\n",
            lk.meshHandleBits, lk.materialHandleBits,
            static_cast<unsigned>(lk.lodLevel), static_cast<unsigned>(lk.pipelineId),
            lk.drawPacketIndex, lk.pathReasonCode, lk.gameObjectId);
        if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                && s_staticPropData.populated) {
            n += std::snprintf(buf+n, sizeof(buf)-n,
                "RecipeIdx: %d  Shape: %s\n",
                s_staticPropData.recipeIndex,
                s_staticPropData.shapeName[0] ? s_staticPropData.shapeName : "(unknown)");
            if (s_staticPropData.materialGpuPopulated) {
                const auto& mg = s_staticPropData.materialGpu;
                n += std::snprintf(buf+n, sizeof(buf)-n,
                    "MatIdx: %u  albedo: %u  normal: %u  mrTex: %u  emit: %u\n"
                    "flags: 0x%08X  baseColor: %.3f  metallic: %.3f  rough: %.3f\n",
                    s_staticPropData.materialIdx,
                    mg.albedoTex, mg.normalTex, mg.metallicRoughnessTex, mg.emissiveTex,
                    mg.flags, mg.baseColorFactor, mg.metallicFactor, mg.roughnessFactor);
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
            n += std::snprintf(buf+n, sizeof(buf)-n,
                "Variant: %s  Name: %s\n"
                "Class: %s  TeamID: %ld  Pilot: %s\n"
                "Status: %s%s%s\n"
                "Sensor: %s\n"
                "Armor: %.0f / %.0f  Structure: %.0f / %.0f\n",
                s_mechData.variantName[0] ? s_mechData.variantName : "(none)",
                s_mechData.longName[0]    ? s_mechData.longName    : "(none)",
                cnames[ci], s_mechData.teamId,
                s_mechData.pilotName[0]   ? s_mechData.pilotName   : "(none)",
                s_mechData.destroyed ? "DESTROYED " : "",
                s_mechData.disabled  ? "disabled "  : "",
                (!s_mechData.destroyed && !s_mechData.disabled && !s_mechData.crippled)
                    ? "OK" : (s_mechData.crippled ? "crippled" : ""),
                snames[csi],
                s_mechData.totalCurArmor, s_mechData.totalMaxArmor,
                s_mechData.totalCurStr,   s_mechData.totalMaxStr);
        }
        ImGui::SetClipboardText(buf);
    }
    ImGui::Separator();

    // Render Explain — consolidated one-line-per-fact render path summary.
    // Shown only for valid RenderWorld picks (not terrain). Detailed panels below.
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
            // lk.pipelineId (which is 0/unknown for static props in M1.5 lookup).
            {
                const DrawPacketSelectedPropSnapshot& rs = g_dpSelProp;
                const uint32_t pipeId =
                    (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp
                     && rs.valid && rs.rowCount > 0)
                    ? rs.rows[0].pipelineId
                    : static_cast<uint32_t>(lk.pipelineId);
                char pipeBuf[48];
                switch (pipeId) {
                    case 0:  std::snprintf(pipeBuf, sizeof(pipeBuf), "(unknown)"); break;
                    case 1:  std::snprintf(pipeBuf, sizeof(pipeBuf), "StaticPropOpaque (1)"); break;
                    case 2:  std::snprintf(pipeBuf, sizeof(pipeBuf), "StaticPropAlphaTest (2)"); break;
                    default: std::snprintf(pipeBuf, sizeof(pipeBuf), "?(%u)", pipeId); break;
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
                std::snprintf(matBuf, sizeof(matBuf),
                    "MaterialGpu  idx %u  albedo %u  layer %d  %s",
                    s_staticPropData.materialIdx,
                    s_staticPropData.materialGpu.albedoTex,
                    layer,
                    matchOk ? "OK" : "(mismatch)");
                ImGui::Text("  material       %s", matBuf);
            } else if (s_selection.kind == RenderWorld::RenderObjectKind::StaticProp) {
                ImGui::TextDisabled("  material       (no GPU material data)");
            }

            // V-LIGHTING-STATIC-0 — factual lighting-model labels.
            // Source: docs/static-prop-lighting-audit.md. These are
            // descriptive of the shader at HEAD 9a9d6eb0; if the static_prop
            // .frag adds normal/PBR/IBL/emissive sampling, update both the
            // audit doc and these strings in lockstep.
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

                // V-AMBIENT-STATIC-1: hemisphere ambient fill mode (default-OFF).
                // Inspect the env var directly (cheap; this panel only runs
                // when MC2_IMGUI_INSPECTOR is on).
                const char* ambEnv = std::getenv("MC2_STATIC_PROP_AMBIENT_V1");
                bool ambOn = (ambEnv != nullptr && ambEnv[0] != '0' && ambEnv[0] != '\0');
                ImGui::Text("  ambient        %s", ambOn ? "hemisphere_v1" : "off");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("V-AMBIENT-STATIC-1 hemisphere ambient fill in\n"
                                      "static_prop.vert (skips window-flag nodes).\n"
                                      "Gate: MC2_STATIC_PROP_AMBIENT_V1=1.\n"
                                      "OFF -> u_ambientV1Strength=0.0 (byte-identical).");

                // DEBUG-VIEW-REGISTRY-1: interactive debug view combo for StaticPropOpaque.
                // Replaces the prior read-only env-text display.
                {
                    // Use canonical helpers to avoid duplicating the mapping table.
                    RenderDebugView curView = StaticPropShaderModeToView(batcher_getDebugMaterialMode());
                    const char* curName = RenderDebugViewName(curView);
                    if (ImGui::BeginCombo("Debug View##sp", curName)) {
                        for (int i = 0; i < int(RenderDebugView::_Count); ++i) {
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
                                          "Initial value from MC2_STATIC_PROP_DEBUG_MATERIAL env var.\n"
                                          "ImGui selection overrides at runtime.");
                }

                // V-IBL-STATIC-1 + V-IBL-DEFAULT-FLIP (2026-05-27):
                // SH-L2 image-based ambient on the StaticPropOpaque lane.
                // Default-ON: unset env -> gate ON, uploads g_iblShStrength
                // (default 0.5). Explicit "=0" -> gate OFF -> uploads 0.0
                // -> shader `if (u_iblShStrength > 0.0)` short-circuits to
                // byte-identical pre-flip OFF behavior. ImGui slider below
                // tunes runtime strength when gate is on.
                // MUST match s_iblShEnabled lambda in gos_static_prop_batcher.cpp.
                const char* iblEnv = std::getenv("MC2_STATIC_PROP_IBL_SH");
                bool iblOn = !(iblEnv != nullptr && iblEnv[0] == '0');
                if (iblOn) {
                    // V-IBL-STATIC-2: surface active per-mission SH set name.
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
                                      "Gate: MC2_STATIC_PROP_IBL_SH=1 (env-authoritative).\n"
                                      "Slider modulates strength when env=1.\n"
                                      "OFF -> u_iblShStrength=0.0 (byte-identical).");
                // Tunables (IBL SH strength + PBR V1 strength + roughness
                // override) live in Graphics Options > Static Prop Tuning.
                // This selection-driven block keeps the per-prop status text
                // for diagnostic readability; the sliders themselves are not
                // duplicated here.

                // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe
                // specular status (read-only here; slider in Graphics Options).
                const char* pbrEnv = std::getenv("MC2_STATIC_PROP_PBR_V1");
                bool pbrOn = pbrEnv != nullptr && pbrEnv[0] != '0'
                                                && pbrEnv[0] != '\0';
                if (pbrOn) {
                    ImGui::Text("  pbr v1         on (strength=%.2f)",
                                g_pbrV1Strength);
                } else {
                    ImGui::Text("  pbr v1         off (env-gated)");
                }

                // V-MATERIAL-PBR-3-TUNE-UI: roughness-override status.
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
                { "legacy kill-switch",  "MC2_STATIC_PROP_LEGACY_DISPATCH", false },
                { "material sample",     "MC2_MATERIAL_GPU_SAMPLE",         true  },
                { "object-ID buffer",    "MC2_OBJECT_ID_BUFFER",            true  },
                { "ambient v1",          "MC2_STATIC_PROP_AMBIENT_V1",      false },
                { "debug material",      "MC2_STATIC_PROP_DEBUG_MATERIAL",  false },
                { "ibl sh",              "MC2_STATIC_PROP_IBL_SH",          true  },
                { "pbr v1",              "MC2_STATIC_PROP_PBR_V1",          false },
            };
            for (const auto& fb : kFallbacks) {
                const char* v = std::getenv(fb.envVar);
                bool on;
                if (!v)               on = fb.defaultOn;
                else if (v[0] == '0') on = false;
                else                  on = true;
                ImGui::Text("  %-22s", fb.label);
                ImGui::SameLine(0.f, 4.f);
                if (on)
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "ON");
                else
                    ImGui::TextColored(ImVec4(1.f, 0.55f, 0.3f, 1.f), "OFF");
            }
        }
    }

    // LookupResult
    if (ImGui::CollapsingHeader("Lookup", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Mesh bits:     0x%08X", lk.meshHandleBits);
        ImGui::Text("Material bits: 0x%08X", lk.materialHandleBits);
        ImGui::Text("LOD:           %u",     static_cast<unsigned>(lk.lodLevel));
        ImGui::Text("Pipeline:      %u",     static_cast<unsigned>(lk.pipelineId));
        ImGui::Text("Draw packet:   %u",     lk.drawPacketIndex);
        ImGui::Text("Path reason:   0x%08X", lk.pathReasonCode);
        ImGui::Text("Game obj ID:   %u",     lk.gameObjectId);
    }

    // Kind-specific
    if (s_selection.pickKind == InspectorPickKind::StaticProp) {
        if (ImGui::CollapsingHeader("StaticProp", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!s_staticPropData.populated) {
                ImGui::TextUnformatted("(no game data -- pick not resolved)");
            } else {
                ImGui::Text("Recipe idx: %d", s_staticPropData.recipeIndex);
                ImGui::Text("Shape:      %s",
                    s_staticPropData.shapeName[0] ? s_staticPropData.shapeName : "(unknown)");
            }
        }

        // Render Spine — reads from g_dpSelProp (filled by gameosmain each frame).
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

                    std::snprintf(buf, sizeof(buf), "%u", rs.typeId);
                    row2("typeId", buf);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Batcher type slot (0..typeCount-1). Groups all\n"
                                          "instances that share geometry + pipeline.");

                    std::snprintf(buf, sizeof(buf), "%d", rs.recipeIndex);
                    row2("recipeIndex", buf);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Static-prop recipe/type index — NOT the\n"
                                          "RenderWorld instance slot (Handle idx).\n"
                                          "Multiple Handle slots share one recipeIndex.");

                    std::snprintf(buf, sizeof(buf), "%u", rs.packetCount);
                    row2("packetCount", buf);
                    std::snprintf(buf, sizeof(buf), "%u", rs.instanceCount);
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
                        std::snprintf(buf, sizeof(buf), "%u", rs.materialIdx);
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
                            std::snprintf(buf, sizeof(buf), "%u", pr.globalPacketIdx);
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
                                    std::snprintf(buf, sizeof(buf), "?(%u)", pr.pipelineId);
                                    ImGui::TextColored(ImVec4(1.f,0.5f,0.3f,1.f), "%s", buf);
                                    break;
                            }

                            ImGui::TableSetColumnIndex(2);
                            if (rs.materialIdx == 0xFFFFFFFFu)
                                ImGui::TextDisabled("--");
                            else {
                                std::snprintf(buf, sizeof(buf), "%u", rs.materialIdx);
                                ImGui::TextUnformatted(buf);
                            }

                            ImGui::TableSetColumnIndex(3);
                            if (pr.albedoTex == 0xFFFFFFFFu)
                                ImGui::TextDisabled("--");
                            else {
                                std::snprintf(buf, sizeof(buf), "%u", pr.albedoTex);
                                ImGui::TextUnformatted(buf);
                            }

                            ImGui::TableSetColumnIndex(4);
                            if (pr.texArrayLayer < 0)
                                ImGui::TextDisabled("--");
                            else {
                                std::snprintf(buf, sizeof(buf), "%d", pr.texArrayLayer);
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
                            std::snprintf(buf, sizeof(buf), "%d", pr.baseVertex);
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
    } else if (s_selection.pickKind == InspectorPickKind::Mech) {
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

        // MECH-DEBUG-VIEWS-1: interactive debug view combo for the mech lane.
        // Shown in the Mech inspector context (a mech is picked), mirroring the
        // StaticProp combo's pick-context placement. NOT gated on
        // MC2_SNAPSHOT_MECH_EXTRACT — it is a render control, independent of
        // snapshot extraction. The selection is global (drives s_mechDebugMode
        // for all mechs), not per-picked-mech.
        if (ImGui::CollapsingHeader("Mech Debug View##mdv", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderDebugView curView = MechFragDebugModeToView(batcher_getMechDebugMode());
            const char* curName = RenderDebugViewName(curView);
            if (ImGui::BeginCombo("Debug View##mech", curName)) {
                for (int i = 0; i < int(RenderDebugView::_Count); ++i) {
                    RenderDebugView v = RenderDebugView(i);
                    if (!RenderDebugViewSupported(v, kDebugViewMask_Mech))
                        continue;
                    bool selected = (v == curView);
                    if (ImGui::Selectable(RenderDebugViewName(v), selected)) {
                        batcher_setMechDebugMode(MechViewToFragDebugMode(v));
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", RenderDebugViewDescription(v));
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("MECH-DEBUG-VIEWS-1 debug view for the mech lane.\n"
                                  "Legacy MC2_MECH_FRAG_DEBUG env var takes precedence\n"
                                  "when set. ImGui selection drives s_mechDebugMode\n"
                                  "only when env is unset. Default (mode 0) = Final\n"
                                  "(byte-identical to unmodified path).");
        }

        // MECH-NORMALS-FIX-1: live normal recompute controls. Default mode 0
        // (Cooked) is byte-identical to the unmodified path — no performance
        // cost, no visual change. Mode 1 (Faceted) and 2 (Smoothed) trigger
        // batcher_rebuildMechNormals() which re-uploads the geometry VBO from
        // the pristine staging copy with the current recompute applied.
        // The rebuild is infrequent (on user interaction only) so the stall is
        // acceptable.
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

        // MECH-AMBIENT-1: gated hemisphere ambient fill. Default OFF = byte-
        // identical (strength 0 uploaded). Per-flush uniform — changes take
        // effect next frame, no VBO rebuild. Best paired with Smoothed normals.
        if (ImGui::CollapsingHeader("Mech Lighting (experimental)##ml")) {
            bool ambOn = batcher_getMechAmbientEnabled() != 0;
            if (ImGui::Checkbox("Ambient Fill (hemisphere)##ml", &ambOn))
                batcher_setMechAmbientEnabled(ambOn ? 1 : 0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Conservative hemisphere ambient fill keyed on the\n"
                                  "world normal — lifts shadowed surfaces for readability.\n"
                                  "No PBR/material/team-mask. OFF = byte-identical default.\n"
                                  "Looks best with Mech Normals = Smoothed.");
            {
                float amt = batcher_getMechAmbientStrength();
                if (!ambOn) ImGui::BeginDisabled();
                if (ImGui::SliderFloat("Ambient Strength##ml", &amt, 0.0f, 2.0f, "%.2f"))
                    batcher_setMechAmbientStrength(amt);
                if (!ambOn) ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hemisphere fill strength (0..2). Default 0.5.\n"
                                      "Multiplies the mech's own albedo, so it tints\n"
                                      "with the paint. 0 = no fill.");
            }

            // MECH-SPECULAR-V1: Blinn specular sheen + glass/cockpit heuristic.
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

        // MECH-EXTRACTION-2: mech snapshot panel (gate: MC2_SNAPSHOT_MECH_EXTRACT=1).
        // Read-only view of RenderSnapshot mech counters + per-selected-mech row detail.
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

                // MECH-SPINE-1: pass-level program ids (legacy path — mech
                // is NOT on the PipelineDesc registry yet; surfacing the gap
                // here mirrors the TERRAIN-SPINE-0 "ViewUniforms not bound"
                // red-flag label).
                ImGui::Separator();
                const uint32_t mechProg   = gos_getMechProgramId();
                const uint32_t shadowProg = gos_getMechShadowProgramId();
                ImGui::Text("mech program       %u", mechProg);
                ImGui::Text("shadow_mech program %u", shadowProg);
                ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                    "PipelineDesc: legacy (not on registry)");
                ImGui::TextDisabled("pass: opaque + shadow (legacy mech path)");

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

    } else if (s_selection.pickKind == InspectorPickKind::Terrain) {
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
            static int s_drEnabled = -1;
            if (s_drEnabled < 0) {
                const char* v = std::getenv("MC2_DEBUG_RENDERER");
                s_drEnabled = (!v || v[0] != '0') ? 1 : 0;  // default-ON
            }
            if (!s_drEnabled)
                ImGui::TextDisabled("Highlight: off (MC2_DEBUG_RENDERER=0)");
        }
    }

    // TERRAIN-SPINE-0: pass-level (not selection-driven) view of the terrain
    // render spine. Visible regardless of pick state.
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
        ImGui::BulletText("surface (solid):      %u", ts.surfaceProgramId);
        ImGui::BulletText("thin records:         %u", ts.thinProgramId);
        ImGui::BulletText("water fast:           %u", ts.waterFastProgramId);
        // WATER-DEBUG-VIEWS-1: read-only readout of the MDI water FS debug mode
        // (live control is deferred to Graphics Options > Water per panel discipline).
        // ::-qualified -> global symbol (decl at file top), not EditorInspector::.
        ImGui::BulletText("water FS debug mode:  %d  (MC2_WATER_DEBUG_MODE; 0=Final)",
                          ::gos_GetWaterFsDebugMode());
        ImGui::BulletText("overlay:              %u", ts.overlayProgramId);
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

    // SHADOW-SPINE-0: pass-level (not selection-driven) view of the shadow
    // render spine. Mirrors the terrain header above.
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
        ImGui::Text("Programs:");
        ImGui::BulletText("terrain shadow:     %u", sp.terrainShadowProgramId);
        ImGui::BulletText("mech shadow:        %u", sp.mechShadowProgramId);
        ImGui::BulletText("static-prop shadow: %u", sp.staticPropShadowProgramId);
        ImGui::Separator();
        ImGui::Text("Caster counts (last flushShadow):");
        ImGui::BulletText("mech         types=%u  instances=%u",
                          sp.mechShadowTypesDrawn, sp.mechShadowInstDrawn);
        ImGui::BulletText("static-prop  types=%u  instances=%u",
                          sp.staticPropShadowTypesDrawn, sp.staticPropShadowInstDrawn);
        ImGui::Separator();
        ImGui::TextDisabled("PipelineDesc: legacy (shadow pass not on registry)");
    }

    // VFX-SPINE-0: pass-level view of the GPU particle / VFX render spine.
    // Mirrors the Shadow Pass header above. Read-only — no GL state, no VFX
    // mutation. VFX object-IDs are prohibited and intentionally absent.
    if (ImGui::CollapsingHeader("VFX Pass##vfx", ImGuiTreeNodeFlags_DefaultOpen)) {
        const VfxPassSnapshot& vs = s_vfxPass;
        // GPU particles enabled status (driven by MC2_GPU_PARTICLES env var).
        if (vs.gpuParticlesEnabled) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "GPU particles: enabled (MC2_GPU_PARTICLES default ON)");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                "GPU particles: DISABLED (MC2_GPU_PARTICLES=0 — legacy CPU FX only)");
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
        // VFX-DEBUG-VIEWS-1: active debug mode (read-only; driven by
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
        ImGui::Text("Program:");
        ImGui::BulletText("particle_billboard: %u", vs.particleProgramId);
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
        // would require a new counter and are out of scope for VFX-SPINE-0.
        ImGui::BulletText("None         (no trail)         draws: n/a");
        ImGui::BulletText("MissileSmoke (handle 41)         draws: n/a");
        ImGui::BulletText("PpcBolt      (handle 33)         draws: n/a");
        ImGui::Separator();
        ImGui::TextDisabled("PipelineDesc: legacy (VFX not on registry; object-IDs prohibited)");
    }

    // RENDERPASS-CONTRACT-2.5: descriptive table of pass-lane closure state.
    // Mirrors RendererFeatureRegistry: pure data, no scheduling, no callbacks.
    // Source of truth: RenderCore/RenderPassContract.h (kRenderPassContracts).
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
            for (int i = 0; i < RenderCore::kRenderPassContractCount; ++i) {
                const RenderCore::RenderPassContract& c = RenderCore::kRenderPassContracts[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)c.id);
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

    // V-MATERIAL-STATIC-0: read-only enumeration of the StaticPropOpaque
    // MaterialGpu table — one row per s_materialGpuTable entry. Sourced from
    // batcher_getStaticPropMaterialInventory*; never mutates the live table.
    // Designed for Track V planning: surfaces every distinct static-prop
    // material (albedo layer, source nodeIdx, texture name, pixel dims,
    // usage count, placeholder/missing-texture flag).
    if (ImGui::CollapsingHeader("Material Inventory##matinv")) {
        const uint32_t invCount = batcher_getStaticPropMaterialInventoryCount();
        ImGui::TextDisabled("Static-prop lane only -- read-only snapshot.");
        ImGui::Text("Materials: %u", (unsigned)invCount);
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
            ImGui::TableSetupColumn("rough");   // V-MATERIAL-PBR-1
            ImGui::TableSetupColumn("metal");   // V-MATERIAL-PBR-1
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
                ImGui::TableNextColumn(); ImGui::Text("%.2f", e.roughnessFactor);  // V-MATERIAL-PBR-1
                ImGui::TableNextColumn(); ImGui::Text("%.2f", e.metallicFactor);   // V-MATERIAL-PBR-1
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

    // Material — shown for any kind; GPU fields only when MC2_MATERIAL_GPU active.
    // For static props: handle bits come from RenderWorld lookup (currently unpopulated →
    // handle=0). Use Render Spine above for authoritative materialIdx + albedoTex.
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
                auto texLabel = [](uint32_t t, char* b, size_t n) {
                    if (t == 0xFFFFFFFFu) std::snprintf(b, n, "(absent)");
                    else std::snprintf(b, n, "%u", t);
                };
                char tb[24];
                texLabel(mg.albedoTex, tb, sizeof(tb));
                ImGui::Text("albedoTex:     %s", tb);
                texLabel(mg.normalTex, tb, sizeof(tb));
                ImGui::Text("normalTex:     %s", tb);
                texLabel(mg.metallicRoughnessTex, tb, sizeof(tb));
                ImGui::Text("mrTex:         %s", tb);
                texLabel(mg.emissiveTex, tb, sizeof(tb));
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

    // Env Gates — live read of all MC2_* flags (default-ON: absent = enabled).
    if (ImGui::CollapsingHeader("Env Gates")) {
        struct GateInfo { const char* name; bool defaultOn; };
        static const GateInfo kGates[] = {
            { "MC2_IMGUI",            true  },
            { "MC2_IMGUI_INSPECTOR",  true  },
            { "MC2_OBJECT_ID_BUFFER", true  },
            { "MC2_DEBUG_RENDERER",   true  },
        };
        for (const auto& g : kGates) {
            const char* v = std::getenv(g.name);
            bool on;
            const char* src;
            if (!v) {
                on  = g.defaultOn;
                src = g.defaultOn ? "(default ON)" : "(default OFF)";
            } else if (v[0] == '0') {
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

    ImGui::End();
}

void EditorInspector::flushDebugHighlight() {
    if (!isEnabled()) return;
    if (!s_open)      return;
    if (!s_selection.hasSelection) return;

    // 0xRRGGBBAA, opaque.
    constexpr uint32_t kStaticPropCol = 0xFFFFFFFFu;  // white
    constexpr uint32_t kMechCol       = 0xFFFF00FFu;  // yellow
    constexpr uint32_t kTerrainCol    = 0x44FF88FFu;  // green

    const auto pk = s_selection.pickKind;

    if ((pk == InspectorPickKind::StaticProp || pk == InspectorPickKind::Mech)
            && s_selection.valid) {
        const float radius   = (pk == InspectorPickKind::Mech) ? 4.f : 2.f;
        const uint32_t col   = (pk == InspectorPickKind::Mech) ? kMechCol : kStaticPropCol;
        DebugRenderer::Vec3 c{
            s_selection.lookup.worldX,
            s_selection.lookup.worldY,
            s_selection.lookup.worldZ
        };
        DebugRenderer::drawRingWorld(c, radius, 16, col);

    } else if (pk == InspectorPickKind::Terrain && s_terrainData.populated) {
        // Flat X/Z crosshair at terrain hit. No vertical arm - terrain
        // marker should look flush with the ground, not like an object bracket.
        const float x   = s_terrainData.worldX;
        const float y   = s_terrainData.worldY;
        const float z   = s_terrainData.worldZ;
        const float arm = 5.f;
        DebugRenderer::drawLineWorld({ x-arm, y, z     }, { x+arm, y, z     }, kTerrainCol);
        DebugRenderer::drawLineWorld({ x,     y, z-arm }, { x,     y, z+arm }, kTerrainCol);
    }
}
