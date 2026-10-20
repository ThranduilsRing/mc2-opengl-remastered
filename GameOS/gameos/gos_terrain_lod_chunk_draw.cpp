// Native terrain patch capture and submission; GL scene draws have no present consumer.
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"
#include "terrain_field_store.h"

bool terrainLodCheckerDiagEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_LOD_CHECKER_DIAG">();
    return s_on;
}

void gos_TerrainLodChunk_EnsureSceneClear() {
#ifdef MC2_VULKAN_ISLAND
    if (gosPostProcess* pp = getGosPostProcess())
        (void)mc2_vk_scene_clear(pp->getWidth(), pp->getHeight());
#endif
}

void gos_TerrainLodChunk_SubmitDrawCommands(
    const TerrainDrawCommand* cmds, const float* skirtDepths,
    const unsigned char* skirtEdgeMasks, const unsigned int* edgeStitch,
    const int* shadowTiers, const float* morphFactors, int count)
{
#ifdef MC2_VULKAN_ISLAND
    if (!cmds || count <= 0 || s_mapSide <= 0) return;
    s_vkTerrainPatches.clear();
    s_vkTerrainFeedValid = false;
    if (!mc2_vk_terrain_island_enabled()) return;
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !mc2_vk_terrain_island_prepare(pp->getWidth(), pp->getHeight())) return;

    // The water and terrain feeds must use the same dispatch snapshot.
    const float* mvp = gos_terrain_indirect::IsFrameSolidArmed()
        ? gos_terrain_indirect_getDispatchMvp16() : gos_GetTerrainMVPMat4();
    if (!mvp) mvp = gos_GetTerrainMVPMat4();
    if (!mvp) return;
    std::memcpy(s_vkTerrainMvp, mvp, sizeof(s_vkTerrainMvp));

    const bool visualDisplaceActive = terrainVisualDisplaceGateOn()
        && gos_TerrainFieldStore() && !gos_TerrainFieldStore()->visualHeight().empty() && s_visualSide > 0;
    const bool allLods = mc2_vk_terrain_all_lods_enabled() != 0;
    const bool farDisplace = allLods && mc2_vk_terrain_visualdisplace_far_enabled() != 0;
    const bool skirts = mc2_vk_terrain_skirts_enabled() != 0;
    for (int i = 0; i < count; ++i) {
        const TerrainDrawCommand& cmd = cmds[i];
        const int qcX = cmd.quadCountsPacked & 0xFF;
        const int qcY = (cmd.quadCountsPacked >> 8) & 0xFF;
        if (qcX <= 0 || qcY <= 0) continue;
        if (cmd.lodStep != 1 && !allLods) continue;
        if (visualDisplaceActive && !(farDisplace && cmd.lodStep != 1)) continue;
        Mc2TerrainTier1Patch p{};
        p.blockOriginX = cmd.blockOriginX;
        p.blockOriginY = cmd.blockOriginY;
        p.quadCountX = cmd.lodStep > 0 ? (qcX / cmd.lodStep) * cmd.lodStep : qcX;
        p.quadCountY = cmd.lodStep > 0 ? (qcY / cmd.lodStep) * cmd.lodStep : qcY;
        p.lodStep = cmd.lodStep;
        p.skirtDepth = skirts && skirtDepths ? skirtDepths[i] : 0.0f;
        p.skirtEdgeMask = skirts && skirtEdgeMasks ? skirtEdgeMasks[i] : 0u;
        p.visualMode = visualDisplaceActive && cmd.lodStep != 1 ? 2 : 0;
        p.edgeStitch = edgeStitch ? edgeStitch[i] : 0u;
        s_vkTerrainPatches.push_back(p);
    }
    s_vkTerrainFeedValid = !s_vkTerrainPatches.empty();
    pp->markTerrainDrawn();
    pp->setShorelineSuppressedByTerrainMask(gos_TerrainShorelineLegacySuppressOn());
    RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LODChunk);
    const int ran = mc2_vk_terrain_island_run(pp->getWidth(), pp->getHeight());
    if (ran) (void)mc2_vk_sky_island_run(pp->getWidth(), pp->getHeight());
#endif
}
