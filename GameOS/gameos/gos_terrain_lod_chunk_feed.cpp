// gos_terrain_lod_chunk: VK tier-1 terrain frame feed (mc2_terrain_tier1_get_frame_feed) and its version counters.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"

#ifdef MC2_VULKAN_ISLAND
// ---------------------------------------------------------------------------
// VK-P10 TERRAIN TIER-1 feed. When MC2_VK_TERRAIN_TIER1 (+ shared depth) is armed,
// SubmitDrawCommands captures the LOD0 (lodStep==1) patch list + shared params into
// this scratch, SKIPS the GL LOD0 main+skirt draws, and the VK terrain island
// co-writes those patches into the shared scene GBuffer instead. Default-OFF ->
// none of this runs -> the GL path draws every band exactly as before.
// ---------------------------------------------------------------------------
uint64_t g_terrainHeightVersion = 0;  // bumps on UploadHeightFull
uint64_t g_terrainCementVersion = 0;  // bumps on UploadCementWordsFull
uint64_t g_terrainVisualHeightVersion = 0;  // bumps on UploadVisualHeightFull (rung 5)
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
extern "C" int mc2_terrain_tier1_get_frame_feed(Mc2TerrainTier1Feed* out)
{
    if (!out) return 0;
    if (!s_vkTerrainFeedValid || s_vkTerrainPatches.empty()) return 0;
    if (!s_heightSsbo.created() || s_mapSide <= 0) return 0;
    out->patches    = s_vkTerrainPatches.data();
    out->patchCount = (uint32_t)s_vkTerrainPatches.size();
    std::memcpy(out->worldToClipGL, s_vkTerrainMvp, sizeof(out->worldToClipGL));
    out->mapSide       = s_mapSide;
    out->halfMap       = s_halfMap;
    out->glHeightSsbo  = s_heightSsbo.name();
    out->heightBytes   = (uint64_t)s_mapSide * (uint64_t)s_mapSide * sizeof(float);
    out->heightVersion = g_terrainHeightVersion;
    // COLORMAP-BASE sub-slice: hand the GL colormap atlas tex + UV params to the
    // island so it can copy-once import the colormap and sample the REAL base color.
    out->glColormapTex          = static_cast<uint32_t>(gos_terrain_indirect_getAtlasGLTex());
    out->atlasTopLeftX          = gos_terrain_indirect_getAtlasMapTopLeftX();
    out->atlasTopLeftY          = gos_terrain_indirect_getAtlasMapTopLeftY();
    out->atlasOneOverWorldUnits = gos_terrain_indirect_getAtlasOneOverWorldUnits();

    // CEMENT/ROAD atlas sub-slice: hand the GL cement atlas + per-vertex cement-word
    // SSBO + the cement uniforms the GL LOD-chunk frag reads. useCement mirrors the GL
    // gate (gos_terrain_lod_chunk.cpp:1122-1124): atlas ready AND cement SSBO live.
    const bool cementReady = gos_terrain_indirect_isCementAtlasReady();
    const bool cementLive  = cementReady && s_cementSsbo.created();
    out->glCementAtlasTex   = static_cast<uint32_t>(gos_terrain_indirect_getCementAtlasGLTex());
    out->cementGridSide     = gos_terrain_indirect_getCementAtlasGridSide();
    out->cementWUPT         = 128.0f;  // Terrain::worldUnitsPerVertex (GL uploads 128.0)
    out->useCement          = cementLive ? 1 : 0;
    out->useTransitionMask  = gos_terrain_indirect_isTransitionMaskReady() ? 1 : 0;
    {
        out->cementDiagConnect = mc2::gate::get<"MC2_TERRAIN_CEMENT_DIAG_CONNECT">() ? 1 : 0;
    }
    out->glCementSsbo       = s_cementSsbo.name();
    out->cementBytes        = (uint64_t)s_mapSide * (uint64_t)s_mapSide * sizeof(unsigned int);
    out->cementVersion      = g_terrainCementVersion;

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): hand the 4x visual-height bake SSBO +
    // mode-2 params to the island. Armed ONLY when the sub-gate is on AND the GL displace
    // gate is on AND the bake is loaded -- else all-zero (island honors visualMode=0 only,
    // and the capture predicate below never emits mode-2 patches in that case anyway).
    out->glVisualHeightSsbo  = 0;
    out->visualHeightBytes   = 0;
    out->visualHeightVersion = 0;
    out->visualSide          = 0;
    out->visualDisplaceFar   = 0.0f;
    if (mc2_vk_terrain_visualdisplace_far_enabled() && terrainVisualDisplaceGateOn()
        && s_visualHeightSsbo.created() && s_visualSide > 0) {
        out->glVisualHeightSsbo  = s_visualHeightSsbo.name();
        out->visualHeightBytes   = (uint64_t)s_visualSide * (uint64_t)s_visualSide * sizeof(float);
        out->visualHeightVersion = g_terrainVisualHeightVersion;
        out->visualSide          = s_visualSide;
        out->visualDisplaceFar   = terrainVisualDisplaceFarParam();
    }

    // VK-TERRAIN-LIGHTING sub-slice: hand the sun dir + the STATIC sun shadow map + the
    // static light-space matrix to the island so the VK terrain frag can reproduce the GL
    // NdotL relief band + baked static PCF shadow (terrain_lod_chunk.frag:889-928). Gate
    // MC2_VK_TERRAIN_LIGHTING (default-off -> lightingEnabled=0 -> VK stays flat, byte-safe).
    {
        // Umbrella family member: MC2_VK_SHARED_GBUFFER=1 arms it.
        int lightingOn = mc2vkgate::umbrellaOn() ? 1 : 0;
        out->lightingEnabled = lightingOn;
        float lx = 0.f, ly = 0.f, lz = 1.f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        out->terrainLightDir[0] = lx; out->terrainLightDir[1] = ly; out->terrainLightDir[2] = lz;
        // Default: no shadow (lit-only) until the GL static shadow map is live.
        out->shadowsEnabled     = 0;
        out->glStaticShadowTex  = 0;
        out->staticShadowSize   = 0;
        out->staticShadowGeneration = 0;
        out->shadowSoftness     = 2.5f;   // shadow.hglsl default
        std::memset(out->staticLightSpaceMatrix, 0, sizeof(out->staticLightSpaceMatrix));
        gosPostProcess* pp = getGosPostProcess();
        if (lightingOn && pp && pp->shadowsEnabled_) {
            const GLuint sTex = pp->getShadowTexture();
            if (sTex != 0) {
                out->shadowsEnabled    = 1;
                out->glStaticShadowTex = static_cast<uint32_t>(sTex);
                out->staticShadowSize  = 4096;   // gos_postprocess.cpp:5113 shadowMapSize_
                out->staticShadowGeneration = gos_GetStaticShadowGeneration();
                std::memcpy(out->staticLightSpaceMatrix, pp->getLightSpaceMatrix(),
                            sizeof(out->staticLightSpaceMatrix));
            }
        }

        // VK-TERRAIN-DYNSHADOW: the DYNAMIC (building/mech) shadow map. Mirrors the GL
        // LOD-chunk enable gate (gos_terrain_lod_chunk.cpp:1120,1131): shadowsEnabled_
        // && getDynamicShadowFBO(). Non-CSM legacy single-map path only (CSM array path
        // deferred — matches the mechanism-proven shadow-compare spike). Copy source is
        // the RAW engine GL texture so we never toggle compare-mode on a VK interop tex.
        out->dynShadowsEnabled = 0;
        out->dynamicShadowSize = 0;
        std::memset(out->dynamicLightSpaceMatrix, 0, sizeof(out->dynamicLightSpaceMatrix));
        // Umbrella member; requires lighting (its own precondition).
        int dynShadowGate = 0;
        if (out->lightingEnabled)
            dynShadowGate = mc2vkgate::umbrellaOn() ? 1 : 0;
        if (dynShadowGate && pp && pp->shadowsEnabled_ && pp->getDynamicShadowFBO()
            && !mc2ShadowCsmEnabled()) {
            const GLuint dTex = pp->getDynamicShadowTextureRaw();
            const int    dSize = pp->getDynamicShadowMapSize();
            if (dTex != 0 && dSize > 0) {
                out->dynShadowsEnabled = 1;
                out->dynamicShadowSize = dSize;
                std::memcpy(out->dynamicLightSpaceMatrix, pp->getDynamicLightSpaceMatrix(),
                            sizeof(out->dynamicLightSpaceMatrix));
            }
        }
        // VK-FLIP-FIELD-REPORT-1 CSM SUBSET: when CSM IS enabled (launcher default), the
        // single-map dyn path above is skipped (!mc2ShadowCsmEnabled), so VK terrain got
        // NO object shadow up close. GL LOD0 terrain samples calcDynamicShadow's CSM
        // cascades (near array + full-map catch-all). Full near-cascade-array interop is a
        // large lift (sampler2DArrayShadow import + N matrices) and is DEFERRED. The
        // BIGGEST-SAFE-SUBSET: feed the CSM FULL-MAP cascade (the last cascade — a single
        // GL_TEXTURE_2D DEPTH_COMPONENT texture, byte-shape-identical to the single dyn map
        // this island already imports) through the SAME binding-7 slot + computeBakedShadow.
        // Result: VK terrain gets whole-map object shadows (coarser than GL's near cascade
        // near the camera, but present) — closes the "no shadows at all" gap with the
        // proven single-map mechanism, zero new interop surface.
        //
        // VK-FLIP-FIELD-BUG1-1 (field oracle 2026-07-06, mc2_01 overview_center): the
        // full-map subset is NET-NEGATIVE at normal play distance. GL LOD0 terrain samples
        // the NEAR cascade up close; the full-map cascade projects tall building/mech
        // occluders as huge soft blobs that swim with the camera = the user's
        // "camera-dependent garbage shadow wash". CSM-OFF was PIXEL-CLEAN vs GL on the same
        // bookmark; CSM-ON washed the whole airport. The matrix/texture ARE consistent
        // (island renders full-map, UBO carries the full-map cascade matrix) — the wash is
        // inherent to the coarse subset, NOT a binding mismatch. The real cure is near-
        // cascade-array interop (sampler2DArrayShadow + N matrices), still DEFERRED.
        //
        // Therefore this subset is now DOUBLE-GATED: MC2_VK_TERRAIN_CSM=1 (presence) is no
        // longer sufficient — it additionally requires MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1.
        // The launcher/field bat sets MC2_VK_TERRAIN_CSM=1 as part of the umbrella family;
        // that alone must NOT re-introduce the wash. Off by default -> VK terrain draws with
        // NO dynamic object shadow under CSM (matches the clean CSM-OFF capture), which is
        // strictly better than the wash until the near-cascade port lands.
        auto csmSubsetOn = []() {
            const bool csmSub = mc2vkgate::umbrellaOn();
            return csmSub && mc2::gate::get<"MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL">();
        };
        if (dynShadowGate && !out->dynShadowsEnabled && pp && pp->shadowsEnabled_
            && mc2ShadowCsmEnabled()
            && csmSubsetOn()) {
            const GLuint fTex  = pp->getDynamicFullMapTexture();
            const int    fSize = pp->getDynamicFullMapSize();
            const int    ccount = pp->getDynamicShadowCascadeCount();
            const float* mats  = pp->getDynamicCascadeMatrices();
            if (fTex != 0 && fSize > 0 && ccount > 0 && mats) {
                out->dynShadowsEnabled = 1;
                out->dynamicShadowSize = fSize;
                // Full-map cascade = the LAST cascade; its matrix is row (ccount-1).
                std::memcpy(out->dynamicLightSpaceMatrix, mats + (ccount - 1) * 16,
                            sizeof(out->dynamicLightSpaceMatrix));
            }
        }

        // VK-TERRAIN-CSM-NEAR-1: the REAL cure. Feed the near-cascade array + separate
        // full-map cascade so the VK frag can port GL's calcDynamicShadow cascade-select
        // verbatim (near cascade up close, coarse full-map only past its range) instead
        // of the FULLMAP_EXPERIMENTAL subset's "always coarse" wash. Independent of the
        // two paths above: MC2_VK_TERRAIN_CSM alone (no FULLMAP_EXPERIMENTAL) now means
        // THIS implementation. GL must actually have CSM active+built this frame.
        //
        // FAR-BAND-BLACK-1 FIX: this block was gated on dynShadowGate alone, NOT on
        // the CSM sub-gate as the comment above claims -- so
        // setting MC2_VK_TERRAIN_DYNSHADOW=1 with GL CSM active (ShadowCSM on,
        // the default) fed CSM cascade data to the VK terrain frag even with
        // MC2_VK_TERRAIN_CSM=0 (or unset with the umbrella off). The field launcher bat
        // sets exactly this combination (DYNSHADOW+CSM both umbrella members, CSM only
        // gets explicitly forced off in isolation tests) -- the resulting csmEnabled=1
        // fragments made the VK terrain frag's CSM cascade-select path run in a
        // configuration nobody exercises intentionally, collapsing shading to
        // near-black over most of the screen (root cause of FAR-BAND-BLACK; isolated
        // via bisecting MC2_VK_TERRAIN_LIGHTING/DETAIL/DYNSHADOW/CSM one at a time --
        // DYNSHADOW alone reproduces, and disabling GL CSM instead of the VK gate also
        // clears it, confirming the CSM feed itself is the trigger, not depth/format).
        // Now requires the CSM sub-gate explicitly, matching the documented contract.
        const bool csmNearGate = mc2vkgate::umbrellaOn();
        out->csmEnabled = 0;
        out->csmArraySize = 0;
        out->csmCount = 0;
        std::memset(out->csmCascadeMatrices, 0, sizeof(out->csmCascadeMatrices));
        std::memset(out->csmCascadeTexelWorld, 0, sizeof(out->csmCascadeTexelWorld));
        out->csmDepthSpan = 1.0f;
        out->csmFullMapSize = 0;
        out->csmFullMapTexelWorld = 0.0f;
        if (csmNearGate && dynShadowGate && pp && pp->shadowsEnabled_ && mc2ShadowCsmEnabled()) {
            const GLuint arrTex  = pp->getDynamicShadowArrayTexture();
            const GLuint fmTex   = pp->getDynamicFullMapTexture();
            const int    ccount2 = pp->getDynamicShadowCascadeCount();
            const float* mats2   = pp->getDynamicCascadeMatrices();
            const float* texelW  = pp->getDynamicCascadeTexelWorld();
            if (arrTex != 0 && fmTex != 0 && ccount2 > 0 && ccount2 <= 3 && mats2 && texelW) {
                out->csmEnabled = 1;
                out->csmArraySize  = pp->getDynamicShadowMapSize();
                out->csmCount      = ccount2;
                std::memcpy(out->csmCascadeMatrices, mats2, sizeof(float) * 16 * ccount2);
                std::memcpy(out->csmCascadeTexelWorld, texelW, sizeof(float) * ccount2);
                out->csmDepthSpan  = pp->getCsmDepthSpan();
                out->csmFullMapSize  = pp->getDynamicFullMapSize();
                out->csmFullMapTexelWorld = pp->getDynamicFullMapTexelWorld();
            }
        }
    }

    // VK-TERRAIN-DETAIL sub-slice: hand the material detail-normal ARRAY tex + the GL
    // detail tuning scalars to the island so the VK terrain frag can perturb the surface
    // normal (macro slope from height + material detail normal) EXACTLY like GL
    // (terrain_lod_chunk.frag:740-808) -> grass reads with NdotL relief instead of flat.
    // Gate MC2_VK_TERRAIN_DETAIL (requires MC2_VK_TERRAIN_LIGHTING; default-off ->
    // detailEnabled=0 -> VK N stays flat-up, byte-identical to the lighting-only frame).
    {
        // Umbrella member; requires lighting (its own precondition) + subGate.
        int detailOn = 0;
        if (out->lightingEnabled)
            detailOn = mc2vkgate::umbrellaOn() ? 1 : 0;
        out->detailEnabled       = detailOn;
        out->glMatNormalArrayTex = detailOn ? (uint32_t)gos_GetTerrainNormalArrayTex() : 0u;
        // GL uniform values (gameos_graphics.cpp member defaults; all 1.0). The VK frag
        // bakes per-material tiling/boost/class thresholds as const, so only these three
        // scalars flow through the UBO. See terrain_tier1.frag MATV_* consts.
        out->nfhStrength    = 1.0f;   // terrainNormalsFromHeightStrength (terrain_nfh_strength_)
        out->detailTiling   = 1.0f;   // detailNormalTiling.x (terrain_detail_tiling_)
        out->detailStrength = 1.0f;   // detailNormalStrength.x (terrain_detail_strength_)
    }

    // VK-PARITY-TERRAIN-1: ambient floor + mission flat light from the SAME producers
    // the GL uniform upload uses (gos_TerrainAmbientFloor/DayGate, gos_GetTerrainMissionFlatLight).
    out->ambientFloor   = gos_TerrainAmbientFloor();
    out->ambientDayGate = gos_TerrainAmbientDayGate();
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
        // VK-PARITY-SHADOW-1: the SAME knobs GL bakes into the terrain program as defines.
        {
            const Mc2ShadowFadeKnobs sk = mc2ShadowFadeKnobs();
            out->shadowDistFadeWu  = sk.distFadeWu;
            out->shadowDistFadeMax = sk.distFadeMax;
            out->shadowMinSunSin   = sk.minSunSin;
            out->shadowKnobFlags   = sk.flags;
        }
    }

    // VK-TERRAIN-LODFADE sub-slice: camera-distance fade for detail-normal/POM, closing
    // the VK LOD0-vs-GL-band color seam (GL gos_terrain.frag lodNear, :322-328,616-736).
    // Gate MC2_VK_TERRAIN_LODFADE requires detailEnabled (fading a feature that's already
    // off is a no-op, so keep the precondition chain: LIGHTING -> DETAIL -> LODFADE).
    {
        int lodFadeOn = 0;
        if (out->detailEnabled)
            // VK-PARITY-F3: DEFAULT OFF even under the umbrella (explicit =1 still arms it).
            // The GL chunk terrain (the shipped renderer, terrain_lod_chunk.frag) has NO
            // camera-distance detail fade -- only the legacy gos_terrain.frag did -- so the
            // umbrella-armed fade removed ALL detail-normal relief beyond ~4000wu (every
            // high/RTS camera), leaving VK terrain flat-lit: measured N.x mean +0.07 vs GL
            // +0.53 (parF3 mc2_10 night), ~15% darker bare ground. All LOD bands are VK-drawn
            // by the same shader, so there is no LOD0-vs-band seam to close.
            lodFadeOn = mc2::gate::get<"MC2_VK_TERRAIN_LODFADE">() ? 1 : 0;
        out->lodFadeEnabled = lodFadeOn;
        // VK-FLIP oracle-visible gate-state proof: one-shot diagnostic line so a smoke
        // capture can confirm lodFadeEnabled's resolved value without a debugger attach.
        // Gated on presence of MC2_VK_TERRAIN_COPY_STATS (same opt-in convention as the
        // interop-copies stats line above) to stay silent by default.
        {
            static const bool s_diagOn = mc2::gate::get<"MC2_VK_TERRAIN_COPY_STATS">();
            static bool s_logged = false;
            if (s_diagOn && !s_logged) {
                s_logged = true;
                mc2::fmt::print_err("[VK_TERRAIN_LODFADE_DIAG] lodFadeEnabled={:d} detailEnabled={:d} lightingEnabled={:d}\n", mc2::fmt::as_i32(lodFadeOn), mc2::fmt::as_i32(out->detailEnabled), mc2::fmt::as_i32(out->lightingEnabled));
                fflush(stderr);
            }
        }
        extern void gos_GetTerrainCameraPos(float*, float*, float*);
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        gos_GetTerrainCameraPos(&cx, &cy, &cz);
        out->cameraPosMC2[0] = cx;
        out->cameraPosMC2[1] = cy;
        out->cameraPosMC2[2] = cz;
    }

    // VK-OFFLINE-PROOFS-1: golden feed dump. Fires once per process (paired
    // with the deterministic bookmark/sim-freeze capture so run-to-run this is
    // the SAME frame). No-op unless MC2_VK_FEED_DUMP is set.
    if (mc2vkfeeddump::enabled() && mc2vkfeeddump::readyToDump() && !mc2vkfeeddump::latch("terrain")) {
        mc2vkfeeddump::resetOnce();
        if (FILE* f = mc2vkfeeddump::openAppend(/*truncateFirst=*/false)) {
            unsigned displacedPatches = 0;
            for (uint32_t i = 0; i < out->patchCount; ++i)
                if (out->patches[i].visualMode == 2) ++displacedPatches;
            mc2::fmt::print(f, "{{\"island\":\"terrain\",\"patchCount\":{:d},\"mapSide\":{:d},\"halfMap\":{:.6f},\"heightVersion\":{:d},\"cementVersion\":{:d},\"useCement\":{:d},\"lightingEnabled\":{:d},\"shadowsEnabled\":{:d},\"dynShadowsEnabled\":{:d},\"detailEnabled\":{:d},\"lodFadeEnabled\":{:d},\"visualSide\":{:d},\"visualDisplaceFar\":{:.6f},\"displacedPatches\":{:d},\"patches\":[", mc2::fmt::as_u32(out->patchCount), mc2::fmt::as_i32(out->mapSide), out->halfMap, mc2::fmt::as_u64((unsigned long long)out->heightVersion), mc2::fmt::as_u64((unsigned long long)out->cementVersion), mc2::fmt::as_i32(out->useCement), mc2::fmt::as_i32(out->lightingEnabled), mc2::fmt::as_i32(out->shadowsEnabled), mc2::fmt::as_i32(out->dynShadowsEnabled), mc2::fmt::as_i32(out->detailEnabled), mc2::fmt::as_i32(out->lodFadeEnabled), mc2::fmt::as_i32(out->visualSide), out->visualDisplaceFar, mc2::fmt::as_u32(displacedPatches));
            for (uint32_t i = 0; i < out->patchCount; ++i) {
                const Mc2TerrainTier1Patch& p = out->patches[i];
                mc2::fmt::print(f, "{}{{\"qcX\":{:d},\"qcY\":{:d},\"blockOriginX\":{:d},\"blockOriginY\":{:d},\"lodStep\":{:d},\"skirtDepth\":{:.6f},\"skirtEdgeMask\":{:d}}}", mc2::fmt::safe(i ? "," : ""), mc2::fmt::as_i32(p.quadCountX), mc2::fmt::as_i32(p.quadCountY), mc2::fmt::as_i32(p.blockOriginX), mc2::fmt::as_i32(p.blockOriginY), mc2::fmt::as_i32(p.lodStep), p.skirtDepth, mc2::fmt::as_u32(p.skirtEdgeMask));
            }
            mc2::fmt::print(f, "]}}\n");
            std::fclose(f);
        }
        mc2vkfeeddump::latch("terrain") = true;
    }
    return 1;
}
#endif
