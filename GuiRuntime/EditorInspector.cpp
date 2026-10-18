#include "EditorInspector_internal.h"

namespace EditorInspector {
EditorInspector::InspectorSelection     s_selection;
EditorInspector::StaticPropInspectorData s_staticPropData;
EditorInspector::MechInspectorData       s_mechData;
EditorInspector::TerrainInspectorData    s_terrainData;
EditorInspector::TerrainPassSnapshot     s_terrainPass;
EditorInspector::ShadowPassSnapshot      s_shadowPass;
EditorInspector::VfxPassSnapshot         s_vfxPass;
bool s_open = false;
bool s_featuresOpen = false;

const char* pickKindName(InspectorPickKind k) {
    switch (k) {
        case InspectorPickKind::StaticProp: return "StaticProp";
        case InspectorPickKind::Mech:       return "Mech";
        case InspectorPickKind::Terrain:    return "Terrain";
        default:                                              return "None";
    }
}
}  // namespace EditorInspector

namespace {

static bool isEnabled() {
    static int cached = -1;
    if (cached < 0) {
        cached = mc2::gate::get<"MC2_IMGUI_INSPECTOR">();
    }
    return cached == 1;
}


static bool s_frameInspectorOpen = false;
static bool s_framePrevCollect   = false;   // last SetCollect we issued

// standalone "Frame Inspector" window. Live per-pass render
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

        if (ImGui::BeginTable("##framepass", 4,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Pass",  ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("ms",    ImGuiTableColumnFlags_WidthFixed,  56.f);
            ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed,  60.f);
            ImGui::TableSetupColumn("Inst",  ImGuiTableColumnFlags_WidthStretch);
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
                    for (int c = 2; c < 4; ++c) {
                        ImGui::TableSetColumnIndex(c);
                        ImGui::TextDisabled("--");
                    }
                    continue;
                }
                ImGui::TableSetColumnIndex(2); ImGui::Text("%u", r.drawCount);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%u", r.instanceCount);
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
    // pass-level snapshot. Always accept (no enable gate so the
    // snapshot is current even when the inspector window is closed; drawImGui is
    // already gated by isEnabled()).
    s_terrainPass = ts;
}

void EditorInspector::setShadowPassSnapshot(const ShadowPassSnapshot& sp) {
    // same always-accept policy as the terrain snapshot.
    s_shadowPass = sp;
}

void EditorInspector::setVfxPassSnapshot(const VfxPassSnapshot& vs) {
    // same always-accept policy as the terrain / shadow snapshots.
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


// Draws the Object Inspector, the Renderer Features window and the Frame Inspector. The toggles, the early returns,
// the window begin / end, the object-id header and the pick-kind chain stay here; each section body is a helper
// above, run in the original order.
void EditorInspector::drawImGui() {
    if (!isEnabled()) return;

    // Ctrl+Shift+I toggles the panel regardless of current open state.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I))
        s_open = !s_open;

    // Renderer Features — standalone window. Runs independent of Object Inspector.
    // Ctrl+Shift+F to toggle. Placed BEFORE if (!s_open) return.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F))
        s_featuresOpen = !s_featuresOpen;

    drawFeaturesWindow();

    // live per-pass render stats window (Ctrl+Shift+P).
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
    drawCameraBookmarkSave();

    // Copy All — formats everything visible in the window to the clipboard.
    ImGui::Spacing();
    drawCopyAll(lk);
    ImGui::Separator();

    // Render Explain — consolidated one-line-per-fact render path summary.
    // Shown only for valid RenderWorld picks (not terrain). Detailed panels below.
    drawRenderExplain(lk);

    // LookupResult
    drawLookup(lk);

    // Kind-specific
    if (s_selection.pickKind == InspectorPickKind::StaticProp) {
        drawStaticPropData();

        // Render Spine — reads from g_dpSelProp (filled by gameosmain each frame).
        drawRenderSpine();
    } else if (s_selection.pickKind == InspectorPickKind::Mech) {
        drawMechData();

        // live normal recompute controls. Default mode 0
        // (Cooked) is byte-identical to the unmodified path — no performance
        // cost, no visual change. Mode 1 (Faceted) and 2 (Smoothed) trigger
        // batcher_rebuildMechNormals() which re-uploads the geometry VBO from
        // the pristine staging copy with the current recompute applied.
        // The rebuild is infrequent (on user interaction only) so the stall is
        // acceptable.
        drawMechNormals();

        // gated hemisphere ambient fill. Default OFF = byte-
        // identical (strength 0 uploaded). Per-flush uniform — changes take
        // effect next frame, no VBO rebuild. Best paired with Smoothed normals.
        drawMechLighting();

        // mech snapshot panel (gate: MC2_SNAPSHOT_MECH_EXTRACT=1).
        // Read-only view of RenderSnapshot mech counters + per-selected-mech row detail.
        drawMechSnapshot();

    } else if (s_selection.pickKind == InspectorPickKind::Terrain) {
        drawTerrainData();
    }

    // pass-level (not selection-driven) view of the terrain
    // render spine. Visible regardless of pick state.
    drawTerrainPass();

    // pass-level (not selection-driven) view of the shadow
    // render spine. Mirrors the terrain header above.
    drawShadowPass();

    // pass-level view of the GPU particle / VFX render spine.
    // Mirrors the Shadow Pass header above. Read-only — no GL state, no VFX
    // mutation. VFX object-IDs are prohibited and intentionally absent.
    drawVfxPass();

    // descriptive table of pass-lane closure state.
    // Mirrors RendererFeatureRegistry: pure data, no scheduling, no callbacks.
    // Source of truth: RenderCore/RenderPassContract.h (kRenderPassContracts).
    drawRenderPassContracts();

    // read-only enumeration of the StaticPropOpaque
    // MaterialGpu table — one row per s_materialGpuTable entry. Sourced from
    // batcher_getStaticPropMaterialInventory*; never mutates the live table.
    // Designed for Track V planning: surfaces every distinct static-prop
    // material (albedo layer, source nodeIdx, texture name, pixel dims,
    // usage count, placeholder/missing-texture flag).
    drawMaterialInventory();

    // Material — shown for any kind; GPU fields only when MC2_MATERIAL_GPU active.
    // For static props: handle bits come from RenderWorld lookup (currently unpopulated →
    // handle=0). Use Render Spine above for authoritative materialIdx + albedoTex.
    drawMaterialRwLookup(lk);

    // Env Gates — live read of all MC2_* flags (default-ON: absent = enabled).
    drawEnvGates();

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
