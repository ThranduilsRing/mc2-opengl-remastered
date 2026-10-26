// gos_terrain_lod_chunk: VK tier-1 terrain frame feed (mc2_terrain_tier1_get_frame_feed) and its version counters.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"
#include "terrain_field_store.h"

#ifdef MC2_VULKAN_ISLAND
// ---------------------------------------------------------------------------
// VK-P10 TERRAIN TIER-1 feed. When MC2_VK_TERRAIN_TIER1 (+ shared depth) is armed,
// SubmitDrawCommands captures the LOD0 (lodStep==1) patch list + shared params into
// this scratch, SKIPS the GL LOD0 main+skirt draws, and the VK terrain island
// co-writes those patches into the shared scene GBuffer instead. Default-OFF ->
// none of this runs -> the GL path draws every band exactly as before.
// The height / cement / visual-height bytes and their versions come from the terrain field store.
// ---------------------------------------------------------------------------
#endif
#ifdef MC2_VULKAN_ISLAND
// VK-TERRAIN-VISUALDISPLACE-FAR-1: file-scope reads of the SAME env knobs the draw
// path caches locally (MC2_TERRAIN_VISUAL_DISPLACE / _FAR at gos_terrain_lod_chunk.cpp
// draw site) so the feed getter can report them without touching the draw statics.
bool terrainVisualDisplaceGateOn()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
    return s_on;
}
float terrainVisualDisplaceFarParam()
{
    static const float s_far = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_FAR">();
    return s_far;
}
#endif
#ifdef MC2_VULKAN_ISLAND
std::vector<Mc2TerrainTier1Patch> s_vkTerrainPatches;  // this frame's LOD0 patches
float    s_vkTerrainMvp[16] = {0};
bool     s_vkTerrainFeedValid = false;
#endif
#ifdef MC2_VULKAN_ISLAND

namespace {
std::string activitySnapshot;
uint32_t activityFrame = UINT32_MAX;

std::string feedHex(const void* data, size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}

// FNV is a content-change receipt, not an asset authentication hash. Never hash
// pointers or struct padding: only resource bytes and the initialized GPU ABI.
std::string feedReceipt(const void* data, size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    if (data) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return R"json({"bytes":)json" + std::to_string(size) + R"json(,"fnv1a64":)json" +
        (data ? std::to_string(hash) : "null") + "}";
}
}

extern "C" int mc2_terrain_feed_observation_enabled()
{
    return mc2vkfeeddump::enabled() && !mc2vkfeeddump::latch("terrain");
}

extern "C" void mc2_terrain_feed_observe_activity(uint32_t frame, std::span<const uint8_t> blocks,
    std::span<const bool> vertices, std::span<const uint32_t> solid, bool solidEnabled)
{
    if (!mc2_terrain_feed_observation_enabled()) return;
    // Normalize bool representations; observe the final flags, including the
    // additive frustum fallback. Keep the solid candidate order and duplicates.
    std::vector<uint8_t> vertexFlags(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) vertexFlags[i] = vertices[i] ? 1 : 0;
    activitySnapshot = R"json({"blocks":")json" + feedHex(blocks.data(), blocks.size()) +
        R"json(","vertices":")json" + feedHex(vertexFlags.data(), vertexFlags.size()) +
        R"json(","solid_enabled":)json" + (solidEnabled ? "true" : "false") +
        R"json(,"solid_count":)json" + std::to_string(solid.size()) +
        R"json(,"solid_hex":")json" + feedHex(solid.data(), solid.size_bytes()) + R"json("})json";
    activityFrame = frame;
}

extern "C" void mc2_terrain_feed_dump_consumed(const Mc2TerrainTier1Feed* f, uint32_t frame, int w, int h,
    std::span<const uint8_t> ubos, int poolCount, int gridDim, std::span<const uint64_t, 8> importedVersions)
{
    if (!mc2_terrain_feed_observation_enabled() || !mc2vkfeeddump::readyToDump()) return;
    mc2vkfeeddump::resetOnce();
    FILE* file = mc2vkfeeddump::openAppend(false);
    if (!file) return;  // Retry; an unwritten receipt must never look complete.
    std::string json = R"json({"island":"terrain","schema":1,"render_frame":)json" + std::to_string(frame) +
        R"json(,"activity_frame":)json" + std::to_string(activityFrame) +
        R"json(,"frozen":)json" + (mc2CaptureGaitFreezeActive() ? "true" : "false") +
        R"json(,"resolution":[)json" + std::to_string(w) + "," + std::to_string(h) +
        R"json(],"mapSide":)json" + std::to_string(f->mapSide) +
        R"json(,"patchCount":)json" + std::to_string(f->patchCount) + R"json(,"patches":[)json";
    for (uint32_t i = 0; i < f->patchCount; ++i) {
        const auto& p = f->patches[i];
        uint32_t skirtBits = 0;
        std::memcpy(&skirtBits, &p.skirtDepth, sizeof(skirtBits));
        if (i) json += ',';
        json += "[" + std::to_string(p.blockOriginX) + "," + std::to_string(p.blockOriginY) +
            "," + std::to_string(p.quadCountX) + "," + std::to_string(p.quadCountY) +
            "," + std::to_string(p.lodStep) + "," + std::to_string(skirtBits) +
            "," + std::to_string(p.skirtEdgeMask) + "," + std::to_string(p.visualMode) +
            "," + std::to_string(p.edgeStitch) + "]";
    }
    json += R"json(],"ubo_hex":")json" + feedHex(ubos.data(), ubos.size()) + R"json(","fields":{)json";
    bool first = true;
    auto field = [&](std::string_view name, const void* data, size_t bytes) {
        if (!first) json += ',';
        first = false;
        json += R"json(")json" + std::string(name) + R"json(":)json" + feedReceipt(data, bytes);
    };
    field("height", f->heightCpu, static_cast<size_t>(f->heightBytes));
    field("cement", f->cementCpu, static_cast<size_t>(f->cementBytes));
    field("visual_height", f->visualHeightCpu, static_cast<size_t>(f->visualHeightBytes));
    field("colormap", f->colormapBytes, f->colormapByteCount);
    field("cement_atlas", f->cementAtlasRgba, static_cast<size_t>(f->cementAtlasSide) * f->cementAtlasSide * 4);
    field("shore_mask", f->shorelineMaskRgba, static_cast<size_t>(f->shorelineMaskW) * f->shorelineMaskH * 4);
    field("shore_field", f->shoreFieldTexels, static_cast<size_t>(f->shoreFieldW) * f->shoreFieldH * sizeof(float));
    field("light_pool", poolCount > 0 ? f->poolLights : nullptr, static_cast<size_t>(poolCount) * 48);
    field("light_header", gridDim > 0 ? f->lightGridHeader : nullptr, gridDim > 0 ? size_t(f->lightGridHeaderWords) * 4 : 0);
    field("light_indices", gridDim > 0 ? f->lightGridIndices : nullptr, gridDim > 0 ? size_t(f->lightGridIndexWords) * 4 : 0);
    json += R"json(},"versions":[)json";
    const uint64_t versions[] = {f->heightVersion, f->cementVersion, f->visualHeightVersion, f->colormapVersion,
        f->cementAtlasVersion, f->shorelineMaskVersion, f->shoreFieldVersion, f->matNormalVersion, f->staticShadowGeneration};
    for (size_t i = 0; i < std::size(versions); ++i) {
        if (i) json += ',';
        json += std::to_string(versions[i]);
    }
    json += R"json(],"imported_versions":[)json";
    for (size_t i = 0; i < 8; ++i) {
        if (i) json += ',';
        json += std::to_string(importedVersions[i]);
    }
    json += R"json(],"resource_shapes":[)json" + std::to_string(f->colormapWidth) + "," + std::to_string(f->colormapHeight) +
        "," + std::to_string(f->colormapVkFormat) + "," + std::to_string(f->cementAtlasSide) +
        "," + std::to_string(f->shorelineMaskW) + "," + std::to_string(f->shorelineMaskH) +
        "," + std::to_string(f->shoreFieldW) + "," + std::to_string(f->shoreFieldH) +
        "," + std::to_string(f->dynamicShadowSize) + "," + std::to_string(f->csmArraySize) + "," + std::to_string(f->csmFullMapSize) +
        R"json(],"material_handles":[)json";
    for (size_t i = 0; i < std::size(f->matNormalHandles); ++i) {
        if (i) json += ',';
        json += std::to_string(f->matNormalHandles[i]);
    }
    // External GPU images are identified by generation/matrices/readiness, not
    // read back. Feed equality alone does not prove shadow or material pixels.
    json += R"json(],"external_image_contents":"not_read_back","activity":)json" +
        (activitySnapshot.empty() ? "null" : activitySnapshot) + "}\n";
    const bool written = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    const bool closed = std::fclose(file) == 0;
    if (written && closed) mc2vkfeeddump::latch("terrain") = true;
}

extern "C" int mc2_terrain_tier1_get_load_feed(Mc2TerrainTier1Feed* out)
{
    if (!out) return 0;
    const mc2terrain::TerrainFieldStore* fields = gos_TerrainFieldStore();
    if (!fields || s_mapSide <= 0 || fields->mapSide() != s_mapSide) return 0;
    out->patches    = s_vkTerrainPatches.data();
    out->patchCount = static_cast<uint32_t>(s_vkTerrainPatches.size());
    std::memcpy(out->worldToClipGL, s_vkTerrainMvp, sizeof(out->worldToClipGL));
    out->mapSide       = s_mapSide;
    out->halfMap       = s_halfMap;
    out->heightCpu     = fields->height().data();
    out->heightBytes   = static_cast<uint64_t>(fields->height().size()) * sizeof(float);
    out->heightVersion = fields->heightVersion();
    // COLORMAP-BASE sub-slice: hand the GL colormap atlas tex + UV params to the
    // island so it can copy-once import the colormap and sample the REAL base color.
    {
        const mc2terrain::ColormapAtlas& cm = fields->colormapAtlas();
        const bool have = !cm.bytes.empty();
        out->colormapBytes     = have ? cm.bytes.data() : nullptr;
        out->colormapByteCount = have ? cm.bytes.size() : 0;
        out->colormapWidth     = have ? cm.width : 0;
        out->colormapHeight    = have ? cm.height : 0;
        out->colormapVkFormat  = have ? cm.vkFormat : 0;
        out->colormapVersion   = have ? cm.version : 0;
    }
    out->atlasTopLeftX          = gos_terrain_indirect_getAtlasMapTopLeftX();
    out->atlasTopLeftY          = gos_terrain_indirect_getAtlasMapTopLeftY();
    out->atlasOneOverWorldUnits = gos_terrain_indirect_getAtlasOneOverWorldUnits();

    // CEMENT/ROAD atlas sub-slice: hand the GL cement atlas + per-vertex cement-word
    // SSBO + the cement uniforms the GL LOD-chunk frag reads. useCement mirrors the GL
    // gate (gos_terrain_lod_chunk.cpp:1122-1124): atlas ready AND cement SSBO live.
    const bool cementReady = gos_terrain_indirect_isCementAtlasReady();
    const bool cementLive  = cementReady && !fields->cementWords().empty();
    out->cementGridSide     = gos_terrain_indirect_getCementAtlasGridSide();
    {
        const auto& atlas = fields->cementAtlas();
        const bool have = !atlas.texels.empty() && atlas.width == atlas.height;
        out->cementAtlasRgba    = have ? atlas.texels.data() : nullptr;
        out->cementAtlasSide    = have ? atlas.width : 0;
        out->cementAtlasVersion = have ? atlas.version : 0;
    }
    out->cementWUPT         = 128.0f;  // Terrain::worldUnitsPerVertex (GL uploads 128.0)
    out->useCement          = cementLive ? 1 : 0;
    out->useTransitionMask  = gos_terrain_indirect_isTransitionMaskReady() ? 1 : 0;
    {
        out->cementDiagConnect = mc2::gate::get<"MC2_TERRAIN_CEMENT_DIAG_CONNECT">() ? 1 : 0;
    }
    // Null until the words of this map were uploaded: the island then keeps an all-zero (no cement) field of the same size.
    const bool cementMatchesMap = fields->cementWords().size() == static_cast<size_t>(s_mapSide) * static_cast<size_t>(s_mapSide);
    out->cementCpu          = cementMatchesMap ? fields->cementWords().data() : nullptr;
    out->cementBytes        = static_cast<uint64_t>(s_mapSide) * static_cast<uint64_t>(s_mapSide) * sizeof(unsigned int);
    out->cementVersion      = fields->cementVersion();

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): hand the 4x visual-height bake SSBO +
    // mode-2 params to the island. Armed ONLY when the sub-gate is on AND the GL displace
    // gate is on AND the bake is loaded -- else all-zero (island honors visualMode=0 only,
    // and the capture predicate below never emits mode-2 patches in that case anyway).
    out->visualHeightCpu     = nullptr;
    out->visualHeightBytes   = 0;
    out->visualHeightVersion = 0;
    out->visualSide          = 0;
    out->visualDisplaceFar   = 0.0f;
    if (mc2_vk_terrain_visualdisplace_far_enabled() && terrainVisualDisplaceGateOn()
        && s_visualSide > 0 && fields->visualSide() == s_visualSide) {
        // The fine bake only (the mip maxes that follow it are a GL-path geomorph input).
        out->visualHeightCpu     = fields->visualHeight().data();
        out->visualHeightBytes   = static_cast<uint64_t>(s_visualSide) * static_cast<uint64_t>(s_visualSide) * sizeof(float);
        out->visualHeightVersion = fields->visualHeightVersion();
        out->visualSide          = s_visualSide;
        out->visualDisplaceFar   = terrainVisualDisplaceFarParam();
    }

    // VK-TERRAIN-LIGHTING sub-slice: hand the sun dir + the STATIC sun shadow map + the
    // static light-space matrix to the island so the VK terrain frag can reproduce the GL
    // NdotL relief band + baked static PCF shadow (terrain_lod_chunk.frag:889-928). Gate
    // MC2_VK_TERRAIN_LIGHTING (default-off -> lightingEnabled=0 -> VK stays flat, byte-safe).
    {
        out->lightingEnabled = 1;
        float lx = 0.f, ly = 0.f, lz = 1.f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        out->terrainLightDir[0] = lx; out->terrainLightDir[1] = ly; out->terrainLightDir[2] = lz;
        const auto* pp=getGosPostProcess();
        out->shadowsEnabled=pp && pp->shadowsEnabled_;
        out->dynShadowsEnabled=out->shadowsEnabled;
    }

    // VK-TERRAIN-DETAIL sub-slice: hand the material detail-normal ARRAY tex + the GL
    // detail tuning scalars to the island so the VK terrain frag can perturb the surface
    // normal (macro slope from height + material detail normal) EXACTLY like GL
    // (terrain_lod_chunk.frag:740-808) -> grass reads with NdotL relief instead of flat.
    // Gate MC2_VK_TERRAIN_DETAIL (requires MC2_VK_TERRAIN_LIGHTING; default-off ->
    // detailEnabled=0 -> VK N stays flat-up, byte-identical to the lighting-only frame).
    {
        out->detailEnabled       = 1;
        out->matNormalVersion   = gos_GetTerrainMaterialNormalVersion();
        for (int slot = 0; slot < 9; ++slot)
            out->matNormalHandles[slot] = gos_GetTerrainMaterialNormalHandle(slot);
        // GL uniform values (gameos_graphics.cpp member defaults; all 1.0). The VK frag
        // bakes per-material tiling/boost/class thresholds as const, so only these three
        // scalars flow through the UBO. See terrain_tier1.frag MATV_* consts.
        out->nfhStrength    = 1.0f;   // terrainNormalsFromHeightStrength (terrain_nfh_strength_)
        out->detailTiling   = 1.0f;   // detailNormalTiling.x (terrain_detail_tiling_)
        out->detailStrength = 1.0f;   // detailNormalStrength.x (terrain_detail_strength_)
    }

    // Retired ambient-floor slots stay neutral to preserve the shared UBO layout.
    out->ambientFloor   = 0.0f;
    out->ambientDayGate = 0.0f;
    // VK-PARITY-F3: legacy material tint producers (same values as the GL upload below).
    {
        float tr[3]={0.36f,0.37f,0.40f}; gos_GetTerrainTintRock(&tr[0],&tr[1],&tr[2]);
        float tg[3]={0.35f,0.42f,0.25f}; gos_GetTerrainTintGrass(&tg[0],&tg[1],&tg[2]);
        float td[3]={0.48f,0.42f,0.33f}; gos_GetTerrainTintDirt(&td[0],&td[1],&td[2]);
        float tc[3]={0.55f,0.53f,0.50f}; gos_GetTerrainTintConcrete(&tc[0],&tc[1],&tc[2]);
        float tsn[3]={0.75f,0.78f,0.84f}; gos_GetTerrainTintSnow(&tsn[0],&tsn[1],&tsn[2]);
        for (int k = 0; k < 3; ++k) {
            out->tintRock[k] = tr[k]; out->tintGrass[k] = tg[k]; out->tintDirt[k] = td[k];
            out->tintConcrete[k] = tc[k]; out->tintSnow[k] = tsn[k];
        }
        out->tintStrengthScale = gos_GetTerrainTintStrengthScale();
        out->snowBrightnessDampen = gos_TerrainSnowBrightnessDampen();
    }
    out->edgeFeatherStrength = kTglcEdgeFeather ? kTglcEdgeFeatherStr : 0.0f;   // the GL u_edgeFeather / u_edgeFeatherStrength pair
    out->cementAtlasSeamFix  = kTglcCementAtlasSeamFix;                           // the GL u_cementAtlasSeamFix
    {
        const TerrainShorelineKnobs k = gos_TerrainShorelineKnobs();
        out->shorelineOn = k.on;
        out->shorelineWaterElev = k.waterElevation;   out->shorelineTime = k.timeSeconds;
        out->shorelineStrength = k.strength;          out->shorelineFoamStrength = k.foamStrength;
        out->shorelineWetRun = k.wetRun;              out->shorelineFoamRun = k.foamRun;
        out->shorelineEdgeJitter = k.edgeJitter;
        const auto& mask = fields->shorelineMask();
        const bool have = !mask.texels.empty() && mask.width > 0 && mask.height > 0;
        out->shorelineMaskRgba    = have ? mask.texels.data() : nullptr;
        out->shorelineMaskW       = have ? mask.width : 0;
        out->shorelineMaskH       = have ? mask.height : 0;
        out->shorelineMaskVersion = have ? mask.version : 0;
        const float* b = gos_TerrainLodChunk_ShorelineMaskBounds();
        for (int i = 0; i < 4; ++i) out->shorelineBounds[i] = b ? b[i] : 0.0f;
        // SHORE-FIELD-1: the water's smooth shore field, so the coastal colormap follows the drawn coast.
        const auto& sf = fields->waterShoreHeight();
        const bool haveSf = gos_WaterShoreMode() == 3 && !sf.texels.empty() && sf.width > 1 && sf.height > 1;
        out->shoreFieldTexels  = haveSf ? sf.texels.data() : nullptr;
        out->shoreFieldW       = haveSf ? sf.width : 0;
        out->shoreFieldH       = haveSf ? sf.height : 0;
        out->shoreFieldVersion = haveSf ? sf.version : 0;
        const bool isSdf = haveSf &&
                           fields->waterShoreHeightKind() == mc2terrain::WaterShoreHeightKind::ShoreSdf;
        out->shoreFieldIsSdf = isSdf ? 1 : 0;
        const float* sb = gos_TerrainLodChunk_WaterShoreHeightBounds();
        for (int i = 0; i < 4; ++i) out->shoreFieldBounds[i] = sb ? sb[i] : 0.0f;
    }
    {
        float fr = 1.f, fg = 1.f, fb = 1.f;
        gos_GetTerrainMissionFlatLight(&fr, &fg, &fb);
        out->missionFlatLight[0] = fr; out->missionFlatLight[1] = fg; out->missionFlatLight[2] = fb;
    }

    // VK-PARITY-TERRAIN-2: night light pools + light grid + pad-area glow, from the
    // SAME producers the GL upload uses. The pool/grid ARRAYS are the CPU copies of
    // what PackAndDispatch uploaded into GL bindings 28/29/30 this frame (cone-spill /
    // LIGHT-REALISM reshape, vis gate and mover flag already applied there).
    {
        out->numPoolLights = gos_TerrainLightPoolCount();
        out->poolLights    = nullptr;
        out->lightGridDim  = 0;
        out->lightGridMin[0] = out->lightGridMin[1] = 0.0f;
        out->lightGridInvCell[0] = out->lightGridInvCell[1] = 0.0f;
        out->lightGridHeader = nullptr; out->lightGridHeaderWords = 0;
        out->lightGridIndices = nullptr; out->lightGridIndexWords = 0;
        if (out->numPoolLights > 0) {
            uint32_t n = 0;
            const gos_terrain_lighting::GpuTerrainLight* pl = gos_terrain_lighting::GetLightPoolCpu(&n);
            if (!pl || n == 0) {
                out->numPoolLights = 0;
            } else {
                out->poolLights = pl;   // numPoolLights (<= n, capped 256) entries are read
                gos_terrain_lighting::LightGridCpu lg{};
                if (gos_terrain_lighting::GetLightGridCpu(lg)) {
                    out->lightGridDim        = static_cast<int32_t>(lg.dim);
                    out->lightGridMin[0]     = lg.minXY[0];
                    out->lightGridMin[1]     = lg.minXY[1];
                    out->lightGridInvCell[0] = lg.invCell[0];
                    out->lightGridInvCell[1] = lg.invCell[1];
                    out->lightGridHeader      = lg.header;
                    out->lightGridHeaderWords = lg.headerWords;
                    out->lightGridIndices     = lg.indices;
                    out->lightGridIndexWords  = lg.indexWords;
                }
            }
        }
        gos_TerrainLightPoolKnobs(&out->lightPoolSoft, &out->lightPoolMoverDim,
                                  &out->lightPoolMoverScale, &out->lightPoolMoverMax,
                                  &out->lightPoolCompress, &out->moverContactShadow);
        gos_TerrainPadGlowRGB(out->padGlow);
        out->padGlowSpill = gos_TerrainPadGlowSpillWu();

    }

    // Reserved ABI slots; all terrain bands retain full detail.
    out->lodFadeEnabled = 0;

    return 1;
}

extern "C" int mc2_terrain_tier1_get_frame_feed(Mc2TerrainTier1Feed* out)
{
    if (!s_vkTerrainFeedValid || s_vkTerrainPatches.empty()) return 0;
    return mc2_terrain_tier1_get_load_feed(out);
}
#endif
