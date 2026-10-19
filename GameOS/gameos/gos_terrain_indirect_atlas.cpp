// gos_terrain_indirect: dense recipe storage, recipe slot build/flush, colormap atlas and cement catalog atlas upload.
// Declared in the class headers; shared names in gos_terrain_indirect_internal.h.
#include "gos_terrain_indirect_internal.h"
#include "mc2fmt.h"
#include "terrain_field_store.h"


// ---------------------------------------------------------------------------
// Stage 2 — dense recipe SSBO storage and helpers
// ---------------------------------------------------------------------------

namespace gos_terrain_indirect_detail {

// Module-private: dense recipe array indexed by vertexNum (= mx + my * mapSide).
// Sized mapSide² when built; cleared on ResetDenseRecipe.
std::vector<TerrainQuadRecipe> g_denseRecipes;
std::vector<bool>              g_denseRecipeDirty;

// ---------------------------------------------------------------------------
// Camera-windowed solid dispatch (Approach A: GPU windowed-index buffer).
// Structural twin of WaterStream's narrow path (gos_terrain_water_stream.cpp
// BuildQuadWindowSSBO / g_quadWindowSsbo / g_quadWindowSsboCapacity).  std430
// plain uint[] (4 B stride): each entry is a recipe index (= top-left
// vertexNum).
//
// LIFECYCLE (LAG-FREE): terrain.cpp calls BeginFrameSolidWindow() then fills
// g_solidWindowStaging in the SLIM LOOP (Terrain::geometry slimReduce), which
// runs BEFORE gos_terrain_indirect::ComputeDispatch() consumes it later in
// the SAME geometry() call.  Frame N's dispatch consumes frame N's window —
// no 1-frame lag, so a moving camera cannot transiently vanish edge terrain.
// Declared here (not with the other Stage-3 GLuint state) so the per-mission
// ResetDenseRecipe() below can clear the staging + high-water mark (MAJOR-1).
std::vector<uint32_t> g_solidWindowStaging;
GLuint   g_solidQuadWindowSsbo     = 0;   // per-frame: recipe indices in camera window
uint32_t g_solidQuadWindowCapacity = 0;   // CPU-side mirror of allocated size (bytes); NO readback
uint32_t g_solidWindowMaxSeen      = 0;   // high-water mark for reserve sizing

bool                           g_denseRecipeAnyDirty = false;
GLuint                         g_recipeSSBO          = 0;
int32_t                        g_recipeMapSide       = 0;
bool                           g_recipeReady         = false;

// VPL parity-infra retirement (cpu-pack-retirement plan §7 OQ-2, full
// delete): s_packParityMask / kParityMaskWords removed. The last consumer
// (gos_terrain_mask_dispatch accessor read) was retired in Step 4 (2e11617);
// the txmmgr ComputeDispatchParity_Check caller is removed in this same
// commit. See memory/mc_texture_manager_dual_queue_legacy_retirement_debt.md.

// Mission-latch for trace reset
bool s_firstDrawPrintedThisMission = false;

// Cached trace flag (avoids repeated getenv calls in hot paths)
static bool s_indirectTrace = false;
static bool s_indirectTraceKnown = false;
bool traceOn() {
    if (!s_indirectTraceKnown) {
        s_indirectTrace = mc2::gate::get<"MC2_TERRAIN_INDIRECT_TRACE">();
        s_indirectTraceKnown = true;
    }
    return s_indirectTrace;
}

// Populate one slot in the dense recipe array from MapData.
// Called both from BuildDenseRecipe (full build at primeMissionTerrainCache)
// and from InvalidateRecipeForVertexNum (in-gameplay precise rebuild).
//
// Judgment call on UV defaults during in-gameplay invalidation:
//   setTerrain() calls invalidateTerrainFaceCache(), which frees the entire
//   Shape C cache. After that, getTerrainFaceCacheEntry returns nullptr until
//   the next primeMissionTerrainCache call (mission reload). For in-gameplay
//   mutations we therefore fall back to the default UV values from
//   quad.cpp:1734-1737 — the same fallback legacy uses once the cache is gone.
//   This is correct: default UVs are the pre-Shape-C half-texel padding values.
void buildRecipeSlot(int32_t vn, TerrainQuadRecipe& out) {
    const long mapSide = Terrain::realVerticesMapSide;
    if (mapSide <= 0) { memset(&out, 0, sizeof(TerrainQuadRecipe)); return; }

    const long mx = vn % mapSide;
    const long my = vn / mapSide;

    if (mx >= mapSide - 1 || my >= mapSide - 1) {
        memset(&out, 0, sizeof(TerrainQuadRecipe));
        return;
    }

    const PostcompVertexPtr blocks = Terrain::mapData ? Terrain::mapData->getBlocks() : nullptr;
    if (!blocks) { memset(&out, 0, sizeof(TerrainQuadRecipe)); return; }

    const long halfSide = Terrain::halfVerticesMapSide;
    const float wupv    = Terrain::worldUnitsPerVertex;

    // Corner layout (matches water stream and quad.cpp):
    //   v0 = (mx,   my)     top-left
    //   v1 = (mx+1, my)     top-right
    //   v2 = (mx+1, my+1)   bottom-right
    //   v3 = (mx,   my+1)   bottom-left
    const PostcompVertex& p0 = blocks[mx       + my       * mapSide];
    const PostcompVertex& p1 = blocks[(mx + 1) + my       * mapSide];
    const PostcompVertex& p2 = blocks[(mx + 1) + (my + 1) * mapSide];
    const PostcompVertex& p3 = blocks[mx       + (my + 1) * mapSide];

    // World X/Y from map indices (gos_terrain_water_stream.cpp:125-130)
    const float wx0 = float(mx     - halfSide) * wupv;
    const float wy0 = float(halfSide - my    ) * wupv;
    const float wx1 = float(mx + 1 - halfSide) * wupv;
    const float wy1 = wy0;
    const float wx2 = wx1;
    const float wy2 = float(halfSide - (my + 1)) * wupv;
    const float wx3 = wx0;
    const float wy3 = wy2;

    out.wx0 = wx0; out.wy0 = wy0; out.wz0 = p0.elevation; out._wp0 = 0.f;
    out.wx1 = wx1; out.wy1 = wy1; out.wz1 = p1.elevation; out._wp1 = 0.f;
    out.wx2 = wx2; out.wy2 = wy2; out.wz2 = p2.elevation; out._wp2 = 0.f;
    out.wx3 = wx3; out.wy3 = wy3; out.wz3 = p3.elevation; out._wp3 = 0.f;

    out.nx0 = p0.vertexNormal.x; out.ny0 = p0.vertexNormal.y; out.nz0 = p0.vertexNormal.z; out._np0 = 0.f;
    out.nx1 = p1.vertexNormal.x; out.ny1 = p1.vertexNormal.y; out.nz1 = p1.vertexNormal.z; out._np1 = 0.f;
    out.nx2 = p2.vertexNormal.x; out.ny2 = p2.vertexNormal.y; out.nz2 = p2.vertexNormal.z; out._np2 = 0.f;
    out.nx3 = p3.vertexNormal.x; out.ny3 = p3.vertexNormal.y; out.nz3 = p3.vertexNormal.z; out._np3 = 0.f;

    // UV extents — mirror quad.cpp:1734-1748 logic.
    // Default: half-texel padding. Override from Shape C cache when available.
    float minU = 0.5f / TERRAIN_TXM_SIZE;
    float maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
    float minV = minU;
    float maxV = maxU;

    if (Terrain::terrainTextures2 && Terrain::mapData) {
        const MapData::WorldQuadTerrainCacheEntry* entry =
            Terrain::mapData->getTerrainFaceCacheEntry(my, mx);  // (tileR=my, tileC=mx)
        if (entry && entry->isValid()) {
            // Mirror quad.cpp:1743: only use uvData when NOT (overlayHandle==0xffffffff && isCement)
            const bool isCement  = entry->isCement();
            const bool noOverlay = (entry->overlayHandle == 0xffffffffu);
            if (!(noOverlay && isCement)) {
                minU = entry->uvData.minU;
                minV = entry->uvData.minV;
                maxU = entry->uvData.maxU;
                maxV = entry->uvData.maxV;
            }
            // Bake terrain texture nodeId into _wp2 for GPU-direct dispatch.
            // _wp3 (cementWord) is baked in PopulateRecipeCementWords() after
            // BuildCementCatalogAtlas() because g_cementLayerIndexBySlot isn't
            // ready yet at buildRecipeSlot() time.
            const uint32_t nodeId = static_cast<uint32_t>(entry->terrainHandle);
            memcpy(&out._wp2, &nodeId, 4);
        }
        // If entry is NULL or !isValid(): _wp2 remains 0 → GPU skips quad.
    }

    out.minU = minU; out.minV = minV; out.maxU = maxU; out.maxV = maxV;

    // Pack 4 corner material types into _wp0 (bit-preserving; shader reads
    // via floatBitsToUint per gos_terrain_thin.vert:122).
    {
        const uint32_t m0 = terrainTypeToMaterialLocal(p0.terrainType);
        const uint32_t m1 = terrainTypeToMaterialLocal(p1.terrainType);
        const uint32_t m2 = terrainTypeToMaterialLocal(p2.terrainType);
        const uint32_t m3 = terrainTypeToMaterialLocal(p3.terrainType);
        const uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
        memcpy(&out._wp0, &tpacked, 4);
    }

    // NOTE: _wp3 (cement word) is NOT baked here.  Reasons:
    //   * First-init from BuildDenseRecipe: g_cementLayerIndexBySlot is empty
    //     (BuildCementCatalogAtlas hasn't run yet) — PopulateRecipeCementWords
    //     bakes it post-atlas-build.
    //   * Later callers (InvalidateRecipeForVertexNum / InvalidateAllRecipes):
    //     they MUST re-bake cement after calling buildRecipeSlot, otherwise
    //     out._wp3 = 0.f above silently wipes the cement layer for every
    //     subsequent FlushDirtyRecipeSlotsToGPU upload.
}


}  // namespace gos_terrain_indirect_detail

namespace gos_terrain_indirect_detail {


// The CPU copy of the colormap atlas the Vulkan terrain island imports, kept in the terrain field store.
static void keepColormapAtlas(std::span<const uint8_t> bytes, int width, int height, uint32_t vkFormat) {
    if (mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore())
        store->setColormapAtlas(bytes, width, height, vkFormat);
    else
        mc2::fmt::print_err("[TERRAIN_FIELDS v1] colormap atlas built with no terrain field store: the Vulkan terrain stays olive\n");
}

void BuildColormapAtlas() {
    ZoneScopedN("Terrain::IndirectAtlasUpload");
    // Negative control (MC2_LOAD_RACE_DELAY_ATLAS): defer the real upload for
    // ~30s of scenario time so the atlas is genuinely absent. ComputePreflight()
    // re-invokes this each frame while g_atlasGLTex == 0. One-shot log on release.
    if (s_loadRaceDelayAtlas) {
        static bool s_delayReleased = false;
        if (!s_delayReleased) {
            if (scenarioTime < 30.0f) {
                static bool s_deferLogged = false;
                if (g_loadRaceTrace && !s_deferLogged) {
                    s_deferLogged = true;
                    mc2::fmt::print_err("[LOADRACE] NEG-CONTROL atlas upload DEFERRED (scenarioTime={:.2f} < 30) frame={:d}\n", scenarioTime, mc2::fmt::as_u32(g_mc2FrameCounter));
                    fflush(stderr);
                }
                return;   // leave g_atlasGLTex == 0
            }
            s_delayReleased = true;
            if (g_loadRaceTrace) {
                mc2::fmt::print_err("[LOADRACE] NEG-CONTROL atlas delay RELEASED (scenarioTime={:.2f}) frame={:d} -- uploading now\n", scenarioTime, mc2::fmt::as_u32(g_mc2FrameCounter));
                fflush(stderr);
            }
        }
    }
    if (!Terrain::terrainTextures2) {
        if (traceOn()) MC2_DIAGF("[TERRAIN_INDIRECT v1] event=atlas_skip reason=no_terrainTextures2\n");
        return;
    }
    auto* tcm = Terrain::terrainTextures2;

    int atlasSizeCapture = 0;
    bool atlasBuilt = false;

    // --- KTX2 / BC7 path (COLORMAP-BC7-KTX2-1) ---
    // Precedes the cpuColorMap early-return so the KTX2 path fires even when
    // cpuColorMap was never allocated (e.g. future "skip-cpu-alloc" optimization).
    // cpuColorMap is always populated as RGBA8 fallback (in case BPTC cap absent).
    if (tcm->ktx2ColormapPath[0] != '\0' && GLEW_ARB_texture_compression_bptc) {
        RenderCore::KtxImage img;
        if (RenderCore::ktxLoadRgba8(tcm->ktx2ColormapPath, img) &&
            img.isCompressed && (img.vkFormat == 145 || img.vkFormat == 146)) {

            GLenum glIF = (img.vkFormat == 146) ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                                                 : GL_COMPRESSED_RGBA_BPTC_UNORM;
            // Mip 0 only: colormap is sampled at a fixed world-space scale, no LOD.
            size_t mip0Bytes = (img.mipCount > 1 && img.mipByteOffsets.size() > 1)
                ? static_cast<size_t>(img.mipByteOffsets[1])
                : img.pixels.size();

            // TEX-CLASS: asset-pool -- terrain colormap atlas (content)
            if (g_atlasGLTex == 0) glGenTextures(1, &g_atlasGLTex);
            MC2_GL_BindTexture(GL_TEXTURE_2D, g_atlasGLTex);
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, glIF,
                                   img.width, img.height, 0,
                                   (GLsizei)mip0Bytes, img.pixels.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            MC2_GL_BindTexture(GL_TEXTURE_2D, 0);

            atlasSizeCapture = img.width;
            atlasBuilt = true;
            keepColormapAtlas(std::span<const uint8_t>(img.pixels.data(), mip0Bytes), img.width, img.height, img.vkFormat);

            mc2::fmt::print_out("[COLORMAP] COLORMAP-BC7-KTX2-1: uploaded {:d}x{:d} BC7 fmt=0x{:x} bytes={:d} gltex={:d}\n", mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height), mc2::fmt::as_u32(static_cast<unsigned>(glIF)), mc2::fmt::as_size(mip0Bytes), mc2::fmt::as_u32(static_cast<unsigned>(g_atlasGLTex)));

            // KTX2 path succeeded — retire CPU copies (same as RETIRE-1, but
            // also clear the path so we don't attempt a second upload).
            tcm->ktx2ColormapPath[0] = '\0';
            if (retireCpuColorMap()) {
                free(tcm->cpuColorMap);   tcm->cpuColorMap    = nullptr;
                tcm->cpuColorMapSize = 0;
                free(tcm->cpuDispAlpha);  tcm->cpuDispAlpha   = nullptr;
                tcm->cpuDispAlphaSize = 0;
            }
        } else {
            mc2::fmt::print_out("[COLORMAP] COLORMAP-BC7-KTX2-1: KTX2 load failed or wrong format ({}), falling back to RGBA8\n", mc2::fmt::safe(tcm->ktx2ColormapPath));
            tcm->ktx2ColormapPath[0] = '\0';
        }
    } else if (tcm->ktx2ColormapPath[0] != '\0' && !GLEW_ARB_texture_compression_bptc) {
        // BPTC cap absent — log once and fall through to RGBA8.
        mc2::fmt::print_out("[COLORMAP] COLORMAP-BC7-KTX2-1: BPTC unsupported, using RGBA8 fallback\n");
        tcm->ktx2ColormapPath[0] = '\0';
    }

    // --- RGBA8 fallback (pre-existing path) ---
    if (!atlasBuilt) {
        if (!tcm->cpuColorMap || tcm->cpuColorMapSize <= 0) {
            if (traceOn()) MC2_DIAGF("[TERRAIN_INDIRECT v1] event=atlas_skip reason=no_cpuColorMap\n");
            return;
        }

        // TEX-CLASS: asset-pool -- terrain colormap atlas (content, alt path)
        if (g_atlasGLTex == 0) glGenTextures(1, &g_atlasGLTex);
        MC2_GL_BindTexture(GL_TEXTURE_2D, g_atlasGLTex);
        // cpuColorMap is BGRA-in-memory (mc2_argb_packing memory note: MC2's
        // textures are BGRA). Upload format param = GL_BGRA so the driver swizzles
        // to RGBA8 storage at upload time.
        MC2_GL_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     tcm->cpuColorMapSize, tcm->cpuColorMapSize,
                     0, GL_BGRA, GL_UNSIGNED_BYTE, tcm->cpuColorMap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        MC2_GL_BindTexture(GL_TEXTURE_2D, 0);

        atlasSizeCapture = tcm->cpuColorMapSize;
        {
            // RGBA order: the GL upload above swizzles the same BGRA bytes.
            const size_t texels = static_cast<size_t>(tcm->cpuColorMapSize) * static_cast<size_t>(tcm->cpuColorMapSize);
            std::vector<uint8_t> rgba(texels * 4u);
            for (size_t i = 0; i < texels; ++i) {
                rgba[i * 4 + 0] = tcm->cpuColorMap[i * 4 + 2];
                rgba[i * 4 + 1] = tcm->cpuColorMap[i * 4 + 1];
                rgba[i * 4 + 2] = tcm->cpuColorMap[i * 4 + 0];
                rgba[i * 4 + 3] = tcm->cpuColorMap[i * 4 + 3];
            }
            keepColormapAtlas(rgba, tcm->cpuColorMapSize, tcm->cpuColorMapSize,
                              mc2terrain::ColormapAtlas::kRgba8);
        }

        if (retireCpuColorMap()) {
            // Atlas is on GPU — release the CPU copies (COLORMAP-CPU-RETIRE-1).
            free(tcm->cpuColorMap);   tcm->cpuColorMap    = nullptr;
            tcm->cpuColorMapSize = 0;
            free(tcm->cpuDispAlpha);  tcm->cpuDispAlpha   = nullptr;
            tcm->cpuDispAlphaSize = 0;
            if (traceOn())
                mc2::fmt::print_out("[COLORMAP] COLORMAP-CPU-RETIRE-1: released cpuColorMap+cpuDispAlpha ({:d}x{:d} BGRA) after RGBA8 atlas upload\n", mc2::fmt::as_i32(atlasSizeCapture), mc2::fmt::as_i32(atlasSizeCapture));
        }
    }

    // MC2_LOAD_RACE_TRACE: latch the frame the colormap atlas first exists.
    if (g_loadRaceTrace && g_atlasGLTex != 0 && g_loadRaceAtlasFirstFrame < 0) {
        g_loadRaceAtlasFirstFrame = static_cast<long>(g_mc2FrameCounter);
        mc2::fmt::print_err("[LOADRACE] atlas first-resident frame={:d} gltex={:d} scenarioTime={:.2f}\n", mc2::fmt::as_i32(g_loadRaceAtlasFirstFrame), mc2::fmt::as_u32(static_cast<unsigned>(g_atlasGLTex)), scenarioTime);
        fflush(stderr);
    }

    g_atlasSize              = atlasSizeCapture;
    g_atlasNumTexturesAcross = tcm->getNumTexturesAcross();
    g_atlasMapTopLeftX       = Terrain::mapTopLeft3d.x;
    g_atlasMapTopLeftY       = Terrain::mapTopLeft3d.y;
    g_atlasOneOverWorldUnits = Terrain::oneOverWorldUnitsMapSide;

    if (traceOn()) {
        MC2_DIAGF("[TERRAIN_INDIRECT v1] event=atlas_built size=%d numTilesAcross=%.4f "
               "mapTopLeftX=%.3f mapTopLeftY=%.3f oneOverWorldUnits=%.9f gltex=%u "
               "bc7=%s\n",
               g_atlasSize, g_atlasNumTexturesAcross,
               g_atlasMapTopLeftX, g_atlasMapTopLeftY, g_atlasOneOverWorldUnits,
               static_cast<unsigned>(g_atlasGLTex), atlasBuilt ? "yes" : "no");
        fflush(stdout);
    }
}

// Bridge accessor: gosHandle → GLuint texture name (declared in
// gos_terrain_bridge.h, included above).  Implemented at
// GameOS/gameos/gameos_graphics.cpp:1775-1781.

// BuildCementCatalogAtlas — GPU readback path (B1 of plan v2.1).
// Runs at BuildDenseRecipe() time.  Walks textures[0..nextAvailable-1],
// filters by isCement(slot), reads each cement tile via glGetTexImage,
// blits into a packed grid atlas, uploads as a single GL_TEXTURE_2D.
//
// quickLoad-safe: does not depend on tileRAMHeap textureData[0] (which
// is NULL in stock gameplay per terrtxm.cpp:561 enclosing gate).
//
// NO MIPMAPS: cement atlas cells are packed without inter-cell gutters,
// so glGenerateMipmap would bleed neighboring cells.  Sampler is GL_LINEAR
// min/mag; potential shimmer at distance/oblique angles is accepted —
// Gate A includes a distance/oblique screenshot to surface this.  Per-cell
// mip generation with gutters is a follow-up slice.
void BuildCementCatalogAtlas() {
    ZoneScopedN("Terrain::IndirectCementAtlasUpload");

    if (!Terrain::terrainTextures) {
        if (traceOn()) MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_terrainTextures\n");
        return;
    }
    auto* tt = Terrain::terrainTextures;

    const int txmSize = TERRAIN_TXM_SIZE;  // extern int, typically 64 (terrtxm.cpp:51)
    const long lastSlot = tt->getNextAvailableSlot();
    if (lastSlot <= 0) {
        if (traceOn()) MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_slots\n");
        return;
    }

    // Pass 1: enumerate cement slots, resolve each to (nodeIdx, GLuint).
    std::vector<int>    cementSlots;
    // CEMENT-UNIFORM-2 fix: parallel "this cell came from the ALPHA pass" flag.
    // cementSlots feeds BOTH the atlas cell list AND the SOLID slot->layer map
    // g_cementLayerIndexBySlot. Alpha cells must appear in the former (they need
    // atlas storage) but NEVER in the latter, or PopulateRecipeCementWords sees
    // a valid solid layer for a transition slot and bakes an opaque slab.
    std::vector<uint8_t> cementSlotIsAlpha;
    std::vector<DWORD>  cementNodeIndices;
    std::vector<GLuint> cementGLTextures;
    cementSlots.reserve(64);
    cementNodeIndices.reserve(64);
    cementGLTextures.reserve(64);
    bool truncated = false;

    // CEMENT_DIAG (temporary, gated by MC2_TERRAIN_INDIRECT_TRACE).
    // Walk the FULL slot range without the 255-cap break, classify each
    // cement-flagged slot as base (< firstTransition) or transition,
    // and emit a per-slot line + summary.  See MEMORY: cement_diag breakdown.
    const long firstTransition = tt->getFirstTransition();
    long diagTotal = 0, diagBase = 0, diagTransition = 0;

    for (long slot = 0; slot < lastSlot; ++slot) {
        if (!tt->isCement(static_cast<DWORD>(slot))) continue;
        if (tt->isAlpha(static_cast<DWORD>(slot))) continue;  // pure-cement only (brainstorm Q1)
        const DWORD nodeIdx = tt->peekTextureHandle(static_cast<DWORD>(slot));

        const int isBase = (firstTransition < 0 || slot < firstTransition) ? 1 : 0;
        ++diagTotal;
        if (isBase) ++diagBase; else ++diagTransition;
        if (traceOn()) {
            mc2::fmt::print_out("[CEMENT_DIAG] slot={:d} nodeIdx={:d} isBase={:d} (firstTransition={:d} nextAvailable={:d})\n", mc2::fmt::as_i32(slot), mc2::fmt::as_u32(static_cast<unsigned>(nodeIdx)), mc2::fmt::as_i32(isBase), mc2::fmt::as_i32(firstTransition), mc2::fmt::as_i32(lastSlot));
        }

        if (nodeIdx == 0xffffffffu) continue;
        if (nodeIdx >= static_cast<DWORD>(MC_MAXTEXTURES)) {
            if (traceOn()) {
                MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_atlas_nodeidx_oob "
                       "slot=%ld nodeIdx=%u cap=%d\n",
                       slot, static_cast<unsigned>(nodeIdx), static_cast<int>(MC_MAXTEXTURES));
                fflush(stdout);
            }
            continue;
        }
        const DWORD gosHandle = tex_resolve(nodeIdx);
        if (gosHandle == 0u) continue;
        const GLuint glTex = gos_terrain_bridge_glTextureForGosHandle(static_cast<unsigned>(gosHandle));
        if (glTex == 0) continue;
        // Record-but-don't-break past atlas budget cap: we still want the
        // diag totals to reflect TRUE counts for tier1.  Atlas allocation
        // gated on size below to avoid memory blowup.
        // Cap = 1024 = atlas budget cap (gridSide=32, 2048x2048 = 16 MB).
        // NOT the _pad0 encoding cap (16-bit field, max 65535).  If anyone
        // ever exceeds 1024, the atlas budget needs revisiting.
        if (cementNodeIndices.size() < 1024) {
            cementSlots.push_back(static_cast<int>(slot));
            cementSlotIsAlpha.push_back(0u);
            cementNodeIndices.push_back(nodeIdx);
            cementGLTextures.push_back(glTex);
        } else {
            truncated = true;
        }
    }

    // CEMENT-ALPHA-LAND-1: second enumeration pass — APPEND alpha (transition)
    // cement slots as their own atlas cells. Appended after every solid cell so
    // solid layer indices are byte-stable vs gate-OFF. Same resolve/validation
    // chain and the same 1024-cell budget cap as the solid pass above.
    const int nSolidCells = static_cast<int>(cementNodeIndices.size());
    long alphaAdmitted = 0, alphaSkipped = 0;
    memset(g_cementAlphaLayerIndexBySlot, 0xFF, sizeof(g_cementAlphaLayerIndexBySlot));
    g_cementAlphaLayerMapReady = false;
    std::vector<int> alphaCementSlots;
    {
        for (long slot = 0; slot < lastSlot; ++slot) {
            if (!tt->isCement(static_cast<DWORD>(slot))) continue;
            if (!tt->isAlpha(static_cast<DWORD>(slot))) continue;   // alpha/transition slots only
            const DWORD nodeIdx = tt->peekTextureHandle(static_cast<DWORD>(slot));
            if (nodeIdx == 0xffffffffu || nodeIdx >= static_cast<DWORD>(MC_MAXTEXTURES)) { ++alphaSkipped; continue; }
            const DWORD gosHandle = tex_resolve(nodeIdx);
            if (gosHandle == 0u) { ++alphaSkipped; continue; }
            const GLuint glTex = gos_terrain_bridge_glTextureForGosHandle(static_cast<unsigned>(gosHandle));
            if (glTex == 0) { ++alphaSkipped; continue; }
            if (cementNodeIndices.size() >= 1024) { truncated = true; ++alphaSkipped; continue; }
            if (slot >= 0 && slot < MC_MAX_TERRAIN_TXMS) {
                g_cementAlphaLayerIndexBySlot[slot] = static_cast<uint16_t>(cementNodeIndices.size());
            }
            cementSlots.push_back(static_cast<int>(slot));
            cementSlotIsAlpha.push_back(1u);   // CEMENT-UNIFORM-2: keep out of the SOLID map
            cementNodeIndices.push_back(nodeIdx);
            cementGLTextures.push_back(glTex);
            alphaCementSlots.push_back(static_cast<int>(slot));
            ++alphaAdmitted;
        }
        g_cementAlphaLayerMapReady = true;
    }
    MC2_DIAGF("[CEMENT_ALPHA_ATLAS v1] event=admit mission=%s solid_cells=%d "
           "alpha_admitted=%ld alpha_skipped=%ld\n",
           (::missionName[0] ? ::missionName : "unknown"),
           nSolidCells, alphaAdmitted, alphaSkipped);
    fflush(stdout);

    if (traceOn()) {
        mc2::fmt::print_out("[CEMENT_DIAG] summary mission={} total_cement={:d} base={:d} transitions={:d} nextAvailable={:d} firstTransition={:d}\n", mc2::fmt::safe((::missionName[0] ? ::missionName : "unknown")), mc2::fmt::as_i32(diagTotal), mc2::fmt::as_i32(diagBase), mc2::fmt::as_i32(diagTransition), mc2::fmt::as_i32(lastSlot), mc2::fmt::as_i32(firstTransition));
        fflush(stdout);
    }

    const int N = static_cast<int>(cementNodeIndices.size());
    // Always-on summary — not trace-gated so the count is visible without MC2_TERRAIN_INDIRECT_TRACE.
    MC2_DIAGF("[CEMENT_ATLAS v1] event=build_result N=%d diagTotal=%ld mission=%s\n",
           N, diagTotal, (::missionName[0] ? ::missionName : "unknown"));
    fflush(stdout);
    if (N == 0) {
        if (traceOn()) MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_cement_tiles count=0\n");
        return;
    }

    memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));
    for (int k = 0; k < N; ++k) {
        g_cementLayerIndexByNodeIdx[cementNodeIndices[k]] = static_cast<uint16_t>(k);
    }

    // Build slot → layer-index map.  Slot is the STABLE key (nodeIdx mutates
    // per-frame per memory/mc2_texture_handle_is_live.md).  This is the lookup
    // the per-frame packer uses; the nodeIdx map is kept for one-commit dead-code.
    memset(g_cementLayerIndexBySlot, 0xFF, sizeof(g_cementLayerIndexBySlot));
    for (int k = 0; k < N; ++k) {
        // CEMENT-UNIFORM-2: alpha/transition cells own atlas storage but must NOT
        // register as SOLID cement, or every transition tile takes the solid
        // branch in PopulateRecipeCementWords and bakes an opaque slab (observed:
        // gate ON collapsed transition_baked/dropped/alpha_rescued all to 0).
        if (k < static_cast<int>(cementSlotIsAlpha.size()) && cementSlotIsAlpha[k]) continue;
        if (cementSlots[k] >= 0 && cementSlots[k] < MC_MAX_TERRAIN_TXMS) {
            g_cementLayerIndexBySlot[cementSlots[k]] = static_cast<uint16_t>(k);
        }
    }

    // Grid: smallest power-of-2 side fitting N cells in a square.
    int gridSide = 1;
    while (gridSide * gridSide < N) gridSide <<= 1;
    const int atlasPixelSide = gridSide * txmSize;

    std::vector<uint32_t> atlasBuf(static_cast<size_t>(atlasPixelSide) * atlasPixelSide, 0u);
    std::vector<uint32_t> tileBuf(static_cast<size_t>(txmSize) * txmSize, 0u);

    GLint savedActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0Binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    GLint savedPackAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &savedPackAlign);
    GLint savedUnpackAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    for (int k = 0; k < N; ++k) {
        MC2_GL_BindTexture(GL_TEXTURE_2D, cementGLTextures[k]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, tileBuf.data());

        const int col  = k % gridSide;
        const int row  = k / gridSide;
        const int dstX = col * txmSize;
        const int dstY = row * txmSize;
        for (int py = 0; py < txmSize; ++py) {
            const uint32_t* srcRow = &tileBuf[static_cast<size_t>(py) * txmSize];
            uint32_t*       dstRow = &atlasBuf[static_cast<size_t>((dstY + py)) * atlasPixelSide + dstX];
            memcpy(dstRow, srcRow, static_cast<size_t>(txmSize) * sizeof(uint32_t));
        }
    }

    // ---- CEMENT-ATLAS-MAGENTA-KEY-1: census (always) + gated zeroing --------
    {
        long magentaTexels = 0;
        long magentaCells  = 0;
        for (int k = 0; k < N; ++k) {
            const int dstX = (k % gridSide) * txmSize;
            const int dstY = (k / gridSide) * txmSize;
            long cellHits = 0;
            for (int py = 0; py < txmSize; ++py) {
                uint32_t* row = &atlasBuf[static_cast<size_t>((dstY + py)) * atlasPixelSide + dstX];
                for (int px = 0; px < txmSize; ++px) {
                    const uint32_t c = row[px];
                    const int b = static_cast<int>(( c        & 0xFFu));
                    const int g = static_cast<int>(((c >>  8) & 0xFFu));
                    const int r = static_cast<int>(((c >> 16) & 0xFFu));
                    // Same chroma test as terrtxm.cpp textureFromMemoryAlpha: red AND
                    // blue both clearly dominate green. Grey/asphalt/cement never match.
                    if (r > 150 && b > 150 && r > g + 40 && b > g + 40) {
                        ++cellHits;
                    }
                }
            }
            if (cellHits > 0) { ++magentaCells; magentaTexels += cellHits; }
        }
        MC2_DIAGF("[CEMENT_ATLAS_MAGENTA v1] event=census mission=%s cells=%d "
               "magenta_cells=%ld magenta_texels=%ld\n",
               (::missionName[0] ? ::missionName : "unknown"), N,
               magentaCells, magentaTexels);
        fflush(stdout);
    }

    const int NT = nSolidCells;

    // ---- CEMENT-ATLAS-CELL-2: concrete-family classification + the two arms.
    // Runs AFTER the tone-match/runway-tone passes above (so it sees whatever
    // atlasBuf those produced) and BEFORE the GL upload below. Solid cells only:
    // [0,nSolidCells). Alpha/transition cells -- which is where authored runway
    // and road MARKING art lives -- are appended past nSolidCells and are never
    // touched by either arm, by construction.
    if (NT > 0) {
        std::vector<CementCellStats> st(static_cast<size_t>(NT));
        for (int k = 0; k < NT; ++k)
            CementCellComputeStats(atlasBuf, atlasPixelSide, gridSide, txmSize, k, st[static_cast<size_t>(k)]);

        float refR = 0.f, refG = 0.f, refB = 0.f;
        const int canon = ClassifyConcreteFamily(st, NT, refR, refG, refB);

        // Census prints regardless of either gate, so a map can be judged --
        // and the "do markings live in solid concrete cells?" question answered
        // per-cell by NAME -- without enabling anything.
        long famCells = 0, markCells = 0, roadCells = 0;
        float famLmin = 1e30f, famLmax = -1e30f;
        // CEMENT-INTRACELL-1: the BACKGROUND-only span is the number that decides
        // whether a cross-cell concrete TONE problem exists at all. Measured on
        // all six maps it is 2-5 luma, i.e. there is none -- what varies inside a
        // concrete cell is the baked road art (~65 luma below the background).
        float bgLmin = 1e30f, bgLmax = -1e30f;
        for (int k = 0; k < NT; ++k) {
            const CementCellStats& c = st[static_cast<size_t>(k)];
            if (!c.family) continue;
            ++famCells;
            if (c.marks) ++markCells;
            if (c.road)  ++roadCells;
            famLmin = std::min(famLmin, c.lum);
            famLmax = std::max(famLmax, c.lum);
            bgLmin  = std::min(bgLmin, c.bgLum);
            bgLmax  = std::max(bgLmax, c.bgLum);
        }
        for (int k = 0; k < NT; ++k) {
            const CementCellStats& c = st[static_cast<size_t>(k)];
            if (!(c.marks || c.road || traceOn())) continue;
            const int slot = (k < static_cast<int>(cementSlots.size())) ? cementSlots[k] : -1;
            const long nTypes = Terrain::terrainTextures
                              ? Terrain::terrainTextures->getNumTypes() : 0;
            const long nameId = (Terrain::terrainTextures && slot >= 0 && slot < static_cast<int>(nTypes))
                              ? Terrain::terrainTextures->getTextureNameID(slot) : -1;
            MC2_DIAGF("[CEMENT_CONCRETE v1] mission=%s cell=%d slot=%d nameId=%ld "
                   "band_lum=%.1f bg_lum=%.1f chroma=%.3f mark_frac=%.4f "
                   "road_frac=%.4f family=%d marks=%d road=%d\n",
                   (::missionName[0] ? ::missionName : "unknown"), k, slot, nameId,
                   c.lum, c.bgLum, c.chroma, c.markFrac, c.roadFrac,
                   c.family ? 1 : 0, c.marks ? 1 : 0, c.road ? 1 : 0);
        }
        MC2_DIAGF("[CEMENT_CONCRETE v1] event=census mission=%s solid_cells=%d "
               "family=%ld family_marked=%ld family_road=%ld family_lum=%.1f..%.1f "
               "family_bg_lum=%.1f..%.1f bg_span=%.1f "
               "ref_rgb=%.1f,%.1f,%.1f canon_cell=%d\n",
               (::missionName[0] ? ::missionName : "unknown"), NT, famCells, markCells,
               roadCells,
               (famCells ? famLmin : 0.f), (famCells ? famLmax : 0.f),
               (famCells ? bgLmin : 0.f), (famCells ? bgLmax : 0.f),
               (famCells ? bgLmax - bgLmin : 0.f),
               refR, refG, refB, canon);
        fflush(stdout);

    }

    // ---- CEMENT-ATLAS-CELL-2 diagnostic: dump the assembled atlas so cell art
    // can be inspected directly (which cells actually carry painted markings is
    // an empirical question about the ART, not one to answer from thresholds).
    // MC2_CEMENT_ATLAS_DUMP=<path-prefix>. Writes <prefix>.rgba (raw BGRA8,
    // atlasPixelSide^2) + <prefix>.csv (per-cell stats). Diagnostic-only.
    {
        const std::string dumpPfxStr = mc2::gate::str<"MC2_CEMENT_ATLAS_DUMP">();   // "" when unset or empty
        const char* dumpPfx = dumpPfxStr.c_str();
        if (dumpPfx[0]) {
            char pathBuf[1024];
            mc2::fmt::to_buf(pathBuf, "{}.rgba", mc2::fmt::safe(dumpPfx));
            if (FILE* f = fopen(pathBuf, "wb")) {
                fwrite(atlasBuf.data(), 4, atlasBuf.size(), f);
                fclose(f);
            }
            mc2::fmt::to_buf(pathBuf, "{}.csv", mc2::fmt::safe(dumpPfx));
            if (FILE* f = fopen(pathBuf, "w")) {
                mc2::fmt::print(f, "cell,slot,is_alpha,grid_side,tile_px,atlas_px\n");
                for (int k = 0; k < N; ++k) {
                    mc2::fmt::print(f, "{:d},{:d},{:d},{:d},{:d},{:d}\n", mc2::fmt::as_i32(k), mc2::fmt::as_i32((k < static_cast<int>(cementSlots.size())) ? cementSlots[k] : -1), mc2::fmt::as_i32((k < static_cast<int>(cementSlotIsAlpha.size())) ? static_cast<int>(cementSlotIsAlpha[k]) : -1), mc2::fmt::as_i32(gridSide), mc2::fmt::as_i32(txmSize), mc2::fmt::as_i32(atlasPixelSide));
                }
                fclose(f);
            }
            MC2_DIAGF("[CEMENT_ATLAS_DUMP v1] event=written prefix=%s atlas_px=%d "
                   "grid=%d tile=%d cells=%d solid=%d\n",
                   dumpPfx, atlasPixelSide, gridSide, txmSize, N, nSolidCells);
            fflush(stdout);
        }
    }

    // TEX-CLASS: asset-pool -- cement/road overlay atlas (content)
    if (g_cementAtlasGLTex == 0) glGenTextures(1, &g_cementAtlasGLTex);
    MC2_GL_BindTexture(GL_TEXTURE_2D, g_cementAtlasGLTex);
    MC2_GL_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 atlasPixelSide, atlasPixelSide, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, atlasBuf.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // no mips — see header comment
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
    MC2_GL_BindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glActiveTexture((GLenum)savedActive);

    // The CPU copy the Vulkan terrain island imports (RGBA order; the GL upload above swizzles the same BGRA words).
    if (mc2terrain::TerrainFieldStore* store = gos_TerrainFieldStore()) {
        std::vector<uint8_t> rgba(atlasBuf.size() * 4u);
        for (size_t i = 0; i < atlasBuf.size(); ++i) {
            const uint32_t w = atlasBuf[i];   // BGRA in memory: B lowest byte
            rgba[i * 4 + 0] = static_cast<uint8_t>(w >> 16);
            rgba[i * 4 + 1] = static_cast<uint8_t>(w >> 8);
            rgba[i * 4 + 2] = static_cast<uint8_t>(w);
            rgba[i * 4 + 3] = static_cast<uint8_t>(w >> 24);
        }
        store->setCementAtlas(rgba.data(), atlasPixelSide, atlasPixelSide);
    } else {
        mc2::fmt::print_err("[TERRAIN_FIELDS v1] cement atlas built with no terrain field store: the Vulkan terrain keeps the colormap base\n");
    }
    g_cementAtlasGridSide    = gridSide;
    g_cementAtlasTileCount   = N;
    g_cementCatalogTruncated = truncated ? 1 : 0;
    g_cementLayerMapReady    = true;

    // REGISTRY-TERRAIN-SSBO-1: register the cement catalog atlas (observe-only).
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::CementAtlas;
        d.kind      = RenderCore::RenderResourceKind::Texture2D;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::RGBA8;
        d.debugName = "CementAtlas";
        d.width     = static_cast<uint32_t>(atlasPixelSide);
        d.height    = static_cast<uint32_t>(atlasPixelSide);
        d.glName    = static_cast<uint32_t>(g_cementAtlasGLTex);
        d.sizeBytes = static_cast<uint64_t>(atlasPixelSide) * static_cast<uint64_t>(atlasPixelSide) * 4ull;
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    if (traceOn()) {
        MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_catalog_built tile_count=%d "
               "atlas_size=%dx%d grid_side=%d gltex=%u truncated=%d "
               "unmapped_pack_count=%u\n",
               N, atlasPixelSide, atlasPixelSide, gridSide,
               static_cast<unsigned>(g_cementAtlasGLTex),
               g_cementCatalogTruncated,
               g_cementPackUnmappedCount);
        if (truncated) {
            MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_catalog_truncated count=1024\n");
        }
        fflush(stdout);
    }

    // Stage B: build procedural 14-layer transition mask array alongside the atlas.
    BuildTransitionMaskArray();
}

}  // namespace gos_terrain_indirect_detail

namespace gos_terrain_indirect {

}  // namespace gos_terrain_indirect

// Bridge accessors — declared extern in gameos_graphics.cpp.
GLuint gos_terrain_indirect_getAtlasGLTex()            { return g_atlasGLTex; }
float  gos_terrain_indirect_getNumTexturesAcross()     { return g_atlasNumTexturesAcross; }
float  gos_terrain_indirect_getAtlasMapTopLeftX()      { return g_atlasMapTopLeftX; }
float  gos_terrain_indirect_getAtlasMapTopLeftY()      { return g_atlasMapTopLeftY; }
float  gos_terrain_indirect_getAtlasOneOverWorldUnits(){ return g_atlasOneOverWorldUnits; }

GLuint gos_terrain_indirect_getCementAtlasGLTex()    { return g_cementAtlasGLTex; }
int    gos_terrain_indirect_getCementAtlasGridSide() { return g_cementAtlasGridSide; }
bool   gos_terrain_indirect_isCementAtlasReady()     { return g_cementLayerMapReady && g_cementAtlasGLTex != 0; }
float  gos_terrain_indirect_getWorldUnitsPerVertex() { return Terrain::worldUnitsPerVertex; }

GLuint gos_terrain_indirect_getTransitionMaskArrayGL() { return g_transitionMaskArrayGL; }
bool   gos_terrain_indirect_isTransitionMaskReady()    { return g_transitionMaskReady && g_transitionMaskArrayGL != 0; }


namespace gos_terrain_indirect {

void BuildDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeBuild");
    if (!Terrain::mapData) return;

    g_recipeMapSide = Terrain::realVerticesMapSide;
    const size_t N  = static_cast<size_t>(g_recipeMapSide) * static_cast<size_t>(g_recipeMapSide);

    g_denseRecipes.assign(N, TerrainQuadRecipe{});
    g_denseRecipeDirty.assign(N, false);
    g_denseRecipeAnyDirty        = false;
    g_recipeReady                = false;
    s_firstDrawPrintedThisMission = false;

    // BuildDenseRecipe is called AFTER mapData->buildTerrainFaceCache (terrain.cpp:585),
    // so the Shape C cache is populated and getTerrainFaceCacheEntry returns valid entries.
    for (int32_t vn = 0; vn < static_cast<int32_t>(N); ++vn) {
        buildRecipeSlot(vn, g_denseRecipes[vn]);
    }

    // Upload the merged colormap atlas for the indirect draw bridge.
    // Must run after recipe build so terrainTextures2 is ready.
    BuildColormapAtlas();

    // Build cement catalog atlas via GPU readback — populates g_cementLayerIndexBySlot.
    BuildCementCatalogAtlas();

    // Bake cementWord into _wp3 now that g_cementLayerIndexBySlot is ready.
    // Must run BEFORE glBufferData so the cement words are included in the initial
    // GPU upload and don't require a FlushDirtyRecipeSlotsToGPU on frame 1.
    PopulateRecipeCementWords();

    // Full GPU upload on mission load — includes _wp3 cement words from above.
    // TIER2-EXCLUDED: dead-path
    if (g_recipeSSBO == 0) glGenBuffers(1, &g_recipeSSBO);
    // Pre-upload audit: dumps how many recipes have a non-zero _wp3 (i.e.,
    // valid cement word) in the CPU memory glBufferData is about to upload.
    // Gated on MC2_TERRAIN_INDIRECT_TRACE; left in-tree as a tripwire because
    // a mismatch between this count and the subsequent `cement_words_baked`
    // count (also gated) is the canary for cement-bake regressions like the
    // InvalidateAllRecipes bug that motivated this audit.
    if (traceOn()) {
        long preUploadNonzero = 0;
        uint32_t firstNonzeroWp3 = 0;
        size_t firstNonzeroVn = static_cast<size_t>(-1);
        for (size_t vn = 0; vn < N; ++vn) {
            uint32_t w;
            memcpy(&w, &g_denseRecipes[vn]._wp3, 4);
            if (w != 0u) {
                ++preUploadNonzero;
                if (firstNonzeroVn == static_cast<size_t>(-1)) {
                    firstNonzeroVn = vn;
                    firstNonzeroWp3 = w;
                }
            }
        }
        mc2::fmt::print_out("[CEMENT_DIAG] event=pre_upload_wp3_audit nonzero={:d} firstVn={:d} firstWp3=0x{:08x} sizeof_recipe={:d} wp3_offset={:d}\n", mc2::fmt::as_i32(preUploadNonzero), mc2::fmt::as_i64(static_cast<long long>(firstNonzeroVn)), mc2::fmt::as_u32(firstNonzeroWp3), mc2::fmt::as_size(sizeof(TerrainQuadRecipe)), mc2::fmt::as_size(static_cast<size_t>((reinterpret_cast<const char*>(&g_denseRecipes[0]._wp3) - reinterpret_cast<const char*>(&g_denseRecipes[0])))));
        fflush(stdout);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)(N * sizeof(TerrainQuadRecipe)),
                 g_denseRecipes.data(),
                 GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // REGISTRY-TERRAIN-SSBO-1: register the dense recipe SSBO (observe-only).
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TerrainRecipeBuffer;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = "TerrainRecipeBuffer";
        d.glName    = static_cast<uint32_t>(g_recipeSSBO);
        d.sizeBytes = static_cast<uint64_t>(N) * sizeof(TerrainQuadRecipe);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // Clear dirty flags — data already in GPU from glBufferData above.
    g_denseRecipeDirty.assign(N, false);
    g_denseRecipeAnyDirty = false;

    g_recipeReady = true;

    if (traceOn()) {
        MC2_DIAGF("[TERRAIN_INDIRECT v1] event=recipe_build mapSide=%d entries=%zu "
               "bytes=%zu ssbo=%u\n",
               g_recipeMapSide, N, N * sizeof(TerrainQuadRecipe),
               static_cast<unsigned>(g_recipeSSBO));
        fflush(stdout);
    }

    // Collect unique terrain texture nodeIds for per-frame LUT upload.
    CollectUniqueNodeIds();
}

void ResetDenseRecipe() {
    ZoneScopedN("Terrain::IndirectRecipeReset");

    if (traceOn()) {
        MC2_DIAGF("[TERRAIN_INDIRECT v1] event=recipe_reset ssbo=%u\n",
               static_cast<unsigned>(g_recipeSSBO));
        fflush(stdout);
    }

    g_denseRecipes.clear();
    g_denseRecipeDirty.clear();
    // MAJOR-1 (Approach A): the camera-window staging + its monotonic high-
    // water mark are per-mission state — clear them here alongside the recipe
    // reset.  Without this, g_solidWindowMaxSeen carries a previous (larger)
    // map's mark into a smaller map's reserve, and stale staging contents
    // could be uploaded on the first frame before the slim loop runs.
    g_solidWindowStaging.clear();
    g_solidWindowMaxSeen         = 0;
    g_denseRecipeAnyDirty        = false;
    g_recipeMapSide              = 0;
    g_recipeReady                = false;
    s_firstDrawPrintedThisMission = false;
    // g_recipeSSBO stays allocated — reused by next mission's BuildDenseRecipe.
    // Mirrors WaterStream::Reset() pattern.

    // Tear down the atlas GL texture (per-mission; rebuilt by BuildColormapAtlas).
    if (g_atlasGLTex != 0) {
        glDeleteTextures(1, &g_atlasGLTex);
        g_atlasGLTex = 0;
    }
    g_atlasSize              = 0;
    g_atlasNumTexturesAcross = 0.f;
    g_atlasMapTopLeftX       = 0.f;
    g_atlasMapTopLeftY       = 0.f;
    g_atlasOneOverWorldUnits = 0.f;
    // Clear stale nodeIds so the GPU path guard doesn't fire from a prior mission.
    g_uniqueTerrainNodeIds.clear();

    // Cement catalog atlas teardown — mirror g_atlasGLTex pattern.
    if (g_cementAtlasGLTex != 0) {
        glDeleteTextures(1, &g_cementAtlasGLTex);
        g_cementAtlasGLTex = 0;

        // REGISTRY-TERRAIN-SSBO-1: mark the cement atlas slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::CementAtlas;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    g_cementAtlasGridSide    = 0;
    g_cementAtlasTileCount   = 0;
    g_cementLayerMapReady    = false;
    g_cementCatalogTruncated = 0;
    g_cementPackUnmappedCount = 0;
    memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));
    memset(g_cementLayerIndexBySlot,    0xFF, sizeof(g_cementLayerIndexBySlot));

    if (traceOn()) {
        MC2_DIAGF("[TERRAIN_INDIRECT v1] event=cement_catalog_reset\n");
        fflush(stdout);
    }
}

bool IsDenseRecipeReady() {
    return g_recipeReady && g_recipeSSBO != 0;
}

const TerrainQuadRecipe* RecipeForVertexNum(int32_t vn) {
    if (vn < 0) return nullptr;
    if (static_cast<size_t>(vn) >= g_denseRecipes.size()) return nullptr;
    return &g_denseRecipes[vn];
}

void InvalidateRecipeForVertexNum(int32_t vn) {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (g_denseRecipes.empty()) return;
    if (vn < 0 || static_cast<size_t>(vn) >= g_denseRecipes.size()) return;
    buildRecipeSlot(vn, g_denseRecipes[vn]);
    // buildRecipeSlot zeroes _wp3.  Re-bake cement words from the layer map
    // so the next FlushDirtyRecipeSlotsToGPU doesn't upload a zeroed _wp3
    // and nuke this quad's cement render.  PopulateRecipeCementWords is
    // whole-grid; cheap (~10k simple iterations) and only fires on the rare
    // invalidate events (shadow recalc, light-dir change, normal recompute).
    PopulateRecipeCementWords();
    g_denseRecipeDirty[vn] = true;
    g_denseRecipeAnyDirty  = true;
    if (traceOn()) {
        MC2_DIAGF("[TERRAIN_INDIRECT v1] event=invalidate vn=%d\n", vn);
        fflush(stdout);
    }
}

void InvalidateAllRecipes() {
    if (!IsEnabled() && !IsParityCheckEnabled()) return;
    if (g_denseRecipes.empty()) return;
    const size_t N = g_denseRecipes.size();
    for (size_t vn = 0; vn < N; ++vn) {
        buildRecipeSlot(static_cast<int32_t>(vn), g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = true;
    }
    // Re-bake cement words: buildRecipeSlot zeroed _wp3 on every slot above.
    // Without this, FlushDirtyRecipeSlotsToGPU uploads zeroed _wp3 for all
    // cement quads and the GPU compute shader's read of r.pos3.w returns 0,
    // making cement pads render as bare colormap (the original cement bug).
    PopulateRecipeCementWords();
    g_denseRecipeAnyDirty = true;
    if (traceOn()) {
        MC2_DIAGF("[TERRAIN_INDIRECT v1] event=invalidate_all entries=%zu\n", N);
        fflush(stdout);
    }
}

void FlushDirtyRecipeSlotsToGPU() {
    if (!g_denseRecipeAnyDirty || g_recipeSSBO == 0) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_recipeSSBO);
    const size_t N = g_denseRecipes.size();
    for (size_t vn = 0; vn < N; ++vn) {
        if (!g_denseRecipeDirty[vn]) continue;
        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)(vn * sizeof(TerrainQuadRecipe)),
                        sizeof(TerrainQuadRecipe),
                        &g_denseRecipes[vn]);
        g_denseRecipeDirty[vn] = false;
    }
    g_denseRecipeAnyDirty = false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}
}  // namespace gos_terrain_indirect
