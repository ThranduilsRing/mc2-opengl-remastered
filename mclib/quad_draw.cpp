// TerrainQuad: TerrainQuad::draw: the SOLID / DRAWALPHA terrain and overlay submission, the M2d decal producer and its parity probe.
// Declared in the class headers; shared names in quad_internal.h.
#include "quad_internal.h"


// Step 1b-1 (cpu-pack-retirement plan §2 A1): the M2 thin-record emit scope is
// orphaned by Fix B (commit 005ebc7) — the GPU compute shader
// gpu_driven_terrain_solid.comp re-derives the thin record + the pzTri flag
// bits for every quad it admits and overwrites the CPU-written record. When
// gos_terrain_indirect::IsFrameSolidArmed() is true (default-armed under
// MC2_GPU_DRIVEN_TERRAIN_SOLID=1) the CPU thin-emit is dead work. Only the
// thin-emit is gated; pz/pzTri compute, the both-culled cull, and the M2d
// gos_PushTerrainOverlay decal producer stay UNCONDITIONAL (the live
// cement-transition/runway/road decal path — §2 WARNING, 9964d5a regression
// class). MC2_TERRAIN_INDIRECT_THINEMIT_TRACE=1 emits a one-shot lifecycle
// line so smoke artifacts capture the gate state.
static const bool s_thinEmitTrace = mc2::gate::get<"MC2_TERRAIN_INDIRECT_THINEMIT_TRACE">();

// MC2_M2D_PZ_PARITY probe (VPL Step 8b precursor validator). Re-authored per
// docs/superpowers/reviews/2026-05-15-overlay-pz-scoped-redesign-rereview.md
// Finding 5. Arming is a static file-scope gate read (NOT a Renderer!=3
// arm-guard — that guard sat in the dead legacy arm; the modern fast path is
// the perspective GL renderer, Environment.Renderer != 3). When armed, the
// M2d pzc/pzTri block bit-compares pzc_new (the corrected reprojection +
// clipInfo==0 sentinel guard) against pzc_old (the literal vertices[c]->pz
// formula, legal probe-only because terrain.cpp still writes cv->pz pre-Step-8b
// and the read is confined to a probe-local). First-N mismatches are emitted
// unconditionally (NOT frame-modulo); a per-mission summary line asserts
// checked > 0 (the v1 failure mode was checked == 0 = probe never fired).
// Demote-don't-delete after green per the Debug-instrumentation rule.
static const bool s_m2dPzParity = mc2::gate::get<"MC2_M2D_PZ_PARITY">();
static int  s_m2dPzParityChecked = 0;       // quads the probe actually compared
static int  s_m2dPzParityMismatch = 0;      // quads with any pzc/pzTri divergence
static int  s_m2dPzParityEmitted = 0;       // first-N detail lines emitted so far
static bool s_m2dPzParityArmLogged = false; // one-shot arm/scope-assertion line
static const int s_m2dPzParityEmitCap = 32; // first-N detail-line cap

// Fill terrain extra data for tessellation — routed to per-node storage via texture manager
static void fillTerrainExtra(DWORD texHandle, DWORD flags, VertexPtr v0, VertexPtr v1, VertexPtr v2) {
    gos_TERRAIN_EXTRA textra[3];
    textra[0].wx = v0->vx; textra[0].wy = v0->vy; textra[0].wz = v0->pVertex->elevation;
    textra[0].nx = v0->pVertex->vertexNormal.x; textra[0].ny = v0->pVertex->vertexNormal.y; textra[0].nz = v0->pVertex->vertexNormal.z;
    textra[1].wx = v1->vx; textra[1].wy = v1->vy; textra[1].wz = v1->pVertex->elevation;
    textra[1].nx = v1->pVertex->vertexNormal.x; textra[1].ny = v1->pVertex->vertexNormal.y; textra[1].nz = v1->pVertex->vertexNormal.z;
    textra[2].wx = v2->vx; textra[2].wy = v2->vy; textra[2].wz = v2->pVertex->elevation;
    textra[2].nx = v2->pVertex->vertexNormal.x; textra[2].ny = v2->pVertex->vertexNormal.y; textra[2].nz = v2->pVertex->vertexNormal.z;
    mcTextureManager->addTerrainExtra(texHandle, textra, flags);
}

// Builds the same 3-element gos_TERRAIN_EXTRA[3] that fillTerrainExtra
// would push to addTerrainExtra, but returns it by value so the modern
// patch stream can mirror-write the bytes without re-reading the verts.
// Keeping the math identical to fillTerrainExtra is the BR-byte-parity
// guarantee for shadow / grass behavior.
static inline void buildTerrainExtraTriple(VertexPtr v0, VertexPtr v1, VertexPtr v2,
                                           gos_TERRAIN_EXTRA out[3])
{
    VertexPtr vs[3] = { v0, v1, v2 };
    for (int k = 0; k < 3; ++k) {
        out[k].wx = vs[k]->vx;
        out[k].wy = vs[k]->vy;
        out[k].wz = vs[k]->pVertex->elevation;
        out[k].nx = vs[k]->pVertex->vertexNormal.x;
        out[k].ny = vs[k]->pVertex->vertexNormal.y;
        out[k].nz = vs[k]->pVertex->vertexNormal.z;
    }
}

// GPU projection: pack MC2 world coords into overlay vertex instead of screen-space
static inline void setOverlayWorldCoords(gos_VERTEX& v, const VertexPtr src) {
	v.x = src->vx;
	v.y = src->vy;
	v.z = src->pVertex->elevation + OVERLAY_ELEV_OFFSET;
	v.rhw = 1.0f;
}

//---------------------------------------------------------------------------
void TerrainQuad::draw (void)
{
	if (terrainHandle != 0xffffffff)
	{
		numTerrainFaces++;

		// M2b: pure-water early-exit. Quads with no base terrain texture, no overlay,
		// and no water-interest detail have nothing to emit here — addVertices, appendQuad,
		// the M2 fast-path emit, and overlay/detail draws are all gated on these
		// conditions. Water surface is rendered separately by Terrain::renderWater().
		// On water-heavy maps (mc2_01 ~75% water) this skips ~28K wasted iterations/frame.
		// Most pure-water quads are loop-hoisted in Terrain::render (terrain.cpp); this
		// is the in-function fallback for when the global useOverlayTexture /
		// useWaterInterestTexture flags get toggled and the loop check no longer matches.
		if (terrainHandle == 0
		    && !(useOverlayTexture && overlayHandle != 0xffffffff)
		    && !(useWaterInterestTexture && terrainDetailHandle != 0xffffffff))
		{
		    return;
		}

		//---------------------------------------
		// GOS 3D draw Calls now!

		// M2b instrumentation: pre-branch setup zone. Wraps gVertex decl, minU/maxU
		// compute, optional uvData read, camPosition load, fastPathEligible 7-flag
		// computation (4 PatchStream method calls), and the path_mix counter. This
		// runs for every quad that didn't pure-water early-exit. Closes at end of
		// the outer-if scope, so it INCLUDES the fast-path and legacy bodies — but
		// FP.* and Quad.legacy zones are children, so Tracy will show preBranch self
		// time = (preBranch total) - (FP.* + Quad.legacy + Legacy.*).
		gos_VERTEX gVertex[3];

        // sebi half pixel, hope this is correct :-)
		float minU = 0.5f / TERRAIN_TXM_SIZE;
		float maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
		float minV = 0.5f / TERRAIN_TXM_SIZE;
		float maxV = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
		float oldminU = 0.0078125f;
		float oldmaxU = 0.9921875f;
		float oldminV = 0.0078125f;
		float oldmaxV = 0.9921875f;

		// OVERLAY-TILE-HIRES-1 (gated, default OFF => constants above unchanged).
		// oldmin/oldmax feed ONLY the overlay tile emits in this function and are
		// the half-texel inset for a 64px tile (0.5/64). A resynced hi-res tile
		// sampled with the 64px inset crops its outer texel ring (2 texels at
		// 256) and breaks road continuity across tile seams, so derive the inset
		// from the actual overlay texture edge (physical = logical * uvScale;
		// disk-loaded 4x-upscale TGAs store logical = physical/4).
		if (MC2_OverlayTileHiresSize() > 0 && overlayHandle != 0xffffffff)
		{
			DWORD ovLogicalW = 0, ovLogicalH = 0;
			if (mcTextureManager->tryGetTextureLogicalSize(overlayHandle, ovLogicalW, ovLogicalH))
			{
				const DWORD ovEdge = ovLogicalW * mcTextureManager->getUVScale(overlayHandle);
				if (ovEdge > 0)
				{
					oldminU = oldminV = 0.5f / (float)ovEdge;
					oldmaxU = oldmaxV = 1.0f - 0.5f / (float)ovEdge;
				}
			}
		}

		if (Terrain::terrainTextures2 && !(overlayHandle == 0xffffffff && isCement))
		{
			minU = uvData.minU;
			minV = uvData.minV;
			maxU = uvData.maxU;
			maxV = uvData.maxV;
		}

		Stuff::Point3D camPosition;
		camPosition = *TG_Shape::s_cameraOrigin;

		// === M2 fast-path eligibility ===
		// Quads enter the fast path when:
		//   (a) terrainHandle != 0           → emit thin record + optional detail + optional overlay
		//   (b) terrainHandle == 0 + detail  → emit detail only (+ optional overlay), no thin record
		//   (c) terrainHandle == 0 + overlay → emit overlay only, no thin record, no detail
		// M2d landed: overlay-only quads are now fast-path eligible; the inline overlay
		// emit below mirrors the legacy gos_PushTerrainOverlay sites (base/detail blocks
		// in legacy continue to exist as the fallback when FASTPATH env is off).
		// Per-gate flags retained so the path_mix diagnostic counter below can
		// attribute fastPathEligible failures to the specific gate (s_failOverlay
		// should read 0 post-M2d — leaving it in serves as the validation signal).
		const bool g_active   = TerrainPatchStream::isFastPathActive();
		const bool g_thin     = TerrainPatchStream::isThinRecordsActive();
		const bool g_ready    = TerrainPatchStream::isReady();
		const bool g_notOver  = !TerrainPatchStream::isOverflowed();
		const bool g_handle   = (terrainHandle != 0);
		const bool g_noOverlay= !(useOverlayTexture && overlayHandle != 0xffffffff);
		const bool g_noWater  = !(useWaterInterestTexture && terrainDetailHandle != 0xffffffff);
		const bool fastPathEligible =
		    g_active && g_thin && g_ready && g_notOver
		    && (g_handle || !g_noWater || !g_noOverlay);

		// MC2_THIN_DEBUG=1: per-gate fail counts to pinpoint which condition is culling
		// most quads from the fast path. Demoted to silent-by-default per CLAUDE.md
		// debug-instrumentation rule. Holds off until ~frame 1200 to skip warmup load
		// and capture steady-state. Prints 5 frames then stops; per-quad cost is one
		// branch on s_framesPrinted after that (effectively zero).
		{
		    static const bool s_pathMixOn = mc2::gate::get<"MC2_THIN_DEBUG">();
		    static uint32_t s_fast = 0, s_legacy = 0;
		    static uint32_t s_failActive = 0, s_failThin = 0, s_failReady = 0, s_failOver = 0;
		    static uint32_t s_failHandle = 0, s_failOverlay = 0, s_failWater = 0;
		    static uint32_t s_framesPrinted = 0;
		    static uint32_t s_quadsThisFrame = 0;
		    static uint32_t s_frameCounter = 0;
		    constexpr uint32_t kWarmupHoldoffFrames = 1200;  // ~15s @ 80fps
		    if (s_pathMixOn && s_framesPrinted < 5) {
		        const bool warmedUp = (s_frameCounter >= kWarmupHoldoffFrames);
		        if (warmedUp) {
		            if (fastPathEligible) ++s_fast;
		            else {
		                ++s_legacy;
		                if      (!g_active)    ++s_failActive;
		                else if (!g_thin)      ++s_failThin;
		                else if (!g_ready)     ++s_failReady;
		                else if (!g_notOver)   ++s_failOver;
		                else if (!g_handle)    ++s_failHandle;
		                else if (!g_noOverlay) ++s_failOverlay;
		                else if (!g_noWater)   ++s_failWater;
		            }
		        }
		        if (++s_quadsThisFrame >= 14000) {
		            ++s_frameCounter;
		            if (warmedUp) {
		                fprintf(stderr,
		                    "[THIN_DEBUG v1] event=path_mix frame=%u (post-warmup) fast=%u legacy=%u "
		                    "fail_active=%u fail_thin=%u fail_ready=%u fail_over=%u "
		                    "fail_handle0=%u fail_overlay=%u fail_water=%u\n",
		                    s_framesPrinted, s_fast, s_legacy,
		                    s_failActive, s_failThin, s_failReady, s_failOver,
		                    s_failHandle, s_failOverlay, s_failWater);
		                fflush(stderr);
		                s_fast = s_legacy = 0;
		                s_failActive = s_failThin = s_failReady = s_failOver = 0;
		                s_failHandle = s_failOverlay = s_failWater = 0;
		                ++s_framesPrinted;
		            }
		            s_quadsThisFrame = 0;
		        }
		    }
		}

		if (fastPathEligible)
		{
		    // pz-validity gate. v2-scoping: only the M2d overlay (live-when-armed)
		    // and the not-armed thin-emit consume pzc/pzTri; gate the (re-)compute
		    // behind the union of their producer predicates so the cost is bounded
		    // to the sparse overlay-quad set (NOT every terrain quad — the v1
		    // 146->42 regression was unconditional re-projection here).
		    // v1-mechanism: re-project on-site from the SAME (vx,vy,elevation)
		    // triple VPL used, and reproduce VPL's pz=-0.5 off-screen sentinel via
		    // the clipInfo==0 short-circuit (v1 Finding 1, proven bit-exact). This
		    // makes the gate INDEPENDENT of vertices[c]->pz so Step 8b can delete
		    // the VPL cv->pz write (terrain.cpp:1601 / :1738).
		    // Source of truth: docs/superpowers/reviews/
		    //   2026-05-15-overlay-pz-scoped-redesign-rereview.md Finding 1
		    //   §Corrected edit.
		    bool pzc[4] = { false, false, false, false };
		    const bool pzNeeded =
		        (useOverlayTexture && overlayHandle != 0xffffffff)
		        || (terrainHandle != 0 && !gos_terrain_indirect::IsFrameSolidArmed());
		    if (pzNeeded)
		    {
		        for (int c = 0; c < 4; c++) {
		            // VPL writes pz=-0.5 (sentinel) iff clipInfo==0 (onScreen==false)
		            // for the perspective renderer (terrain.cpp:1577/1601/1603,
		            // :1749/1759). Reproduce that co-decision; do NOT re-project the
		            // sentinel corners (re-projection of an off-cone corner whose true
		            // projectZ z is still in [0,1) would flip pzc false->true — the
		            // divergence v1 Finding 1 closes).
		            if (vertices[c]->clipInfo == 0) { pzc[c] = false; continue; }
		            Stuff::Vector3D ov3D(vertices[c]->vx, vertices[c]->vy,
		                                 vertices[c]->pVertex->elevation);
		            Stuff::Vector4D osp;
		            eye->projectForTerrainAdmission(ov3D, osp);
		            float pz_adj = osp.z + TERRAIN_DEPTH_FUDGE;
		            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
		        }
		    }

		    bool pzTri1, pzTri2;
		    if (uvMode == BOTTOMLEFT) {
		        pzTri1 = pzc[0] && pzc[1] && pzc[3];
		        pzTri2 = pzc[1] && pzc[2] && pzc[3];
		    } else {
		        pzTri1 = pzc[0] && pzc[1] && pzc[2];
		        pzTri2 = pzc[0] && pzc[2] && pzc[3];
		    }

		    // MC2_M2D_PZ_PARITY probe (Step 8b precursor validator, demote-don't-
		    // delete). Bit-compares the corrected reprojection+sentinel result
		    // above against the literal vertices[c]->pz formula in a PROBE-ONLY
		    // local (legal because terrain.cpp still writes cv->pz pre-Step-8b).
		    // Per 2026-05-15-overlay-pz-scoped-redesign-rereview.md Finding 5:
		    // static gate arm (NO Renderer!=3 arm-guard), one-shot Renderer!=3
		    // scope-assertion log, first-N unconditional mismatch emit, running
		    // summary with checked>0 (vacuous-probe guard). pzc_old does NOT feed
		    // the production path; it exists solely for this comparison.
		    if (s_m2dPzParity)
		    {
		        if (!s_m2dPzParityArmLogged) {
		            s_m2dPzParityArmLogged = true;
		            fprintf(stderr,
		                "[M2D_PZ_PARITY v1] event=armed mechanism=reproject+clipInfo_guard "
		                "scope_assert=Renderer!=3 Environment.Renderer=%d emit_cap=%d "
		                "note=probe_only_cv_pz_read_pre_step8b\n",
		                (int)Environment.Renderer, s_m2dPzParityEmitCap);
		            fflush(stderr);
		        }
		        bool pzc_old[4];
		        for (int c = 0; c < 4; c++) {
		            float pz_adj_old = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
		            pzc_old[c] = (pz_adj_old >= 0.0f) && (pz_adj_old < 1.0f);
		        }
		        bool pzTri1_old, pzTri2_old;
		        if (uvMode == BOTTOMLEFT) {
		            pzTri1_old = pzc_old[0] && pzc_old[1] && pzc_old[3];
		            pzTri2_old = pzc_old[1] && pzc_old[2] && pzc_old[3];
		        } else {
		            pzTri1_old = pzc_old[0] && pzc_old[1] && pzc_old[2];
		            pzTri2_old = pzc_old[0] && pzc_old[2] && pzc_old[3];
		        }
		        ++s_m2dPzParityChecked;
		        const bool mism =
		            (pzc[0] != pzc_old[0]) || (pzc[1] != pzc_old[1]) ||
		            (pzc[2] != pzc_old[2]) || (pzc[3] != pzc_old[3]) ||
		            (pzTri1 != pzTri1_old) || (pzTri2 != pzTri2_old);
		        if (mism) {
		            ++s_m2dPzParityMismatch;
		            if (s_m2dPzParityEmitted < s_m2dPzParityEmitCap) {
		                ++s_m2dPzParityEmitted;
		                fprintf(stderr,
		                    "[M2D_PZ_PARITY v1] event=mismatch n=%d "
		                    "pzc_new=%d%d%d%d pzc_old=%d%d%d%d "
		                    "pzTri_new=%d%d pzTri_old=%d%d pzNeeded=%d uvMode=%d\n",
		                    s_m2dPzParityEmitted,
		                    pzc[0], pzc[1], pzc[2], pzc[3],
		                    pzc_old[0], pzc_old[1], pzc_old[2], pzc_old[3],
		                    pzTri1, pzTri2, pzTri1_old, pzTri2_old,
		                    pzNeeded ? 1 : 0, (int)uvMode);
		                fflush(stderr);
		            }
		        }
		        if ((s_m2dPzParityChecked % 14000) == 0) {
		            fprintf(stderr,
		                "[M2D_PZ_PARITY v1] event=summary checked=%d mismatches=%d\n",
		                s_m2dPzParityChecked, s_m2dPzParityMismatch);
		            fflush(stderr);
		        }
		    }

		    if (!pzTri1 && !pzTri2) return;  // both culled — skip entirely

		    // === Thin-record emit path (terrainHandle != 0 only) ===
		    // Detail-only quads (terrainHandle == 0 + has detail) skip this entire block
		    // and go straight to the M2c detail emit below. The thin record is a base-
		    // texture submission; quads without a base texture have nothing to put in it.
		    //
		    // Step 1b-1 gate-split: skip the entire thin-emit scope (recipe
		    // peek/build via ensureRecipeForQuad + the TerrainQuadThinRecord
		    // build + appendThinRecordDirect) when GPU SOLID is armed. Post
		    // Fix-B the compute shader is the sole projection authority and
		    // re-derives this record (incl. the pzTri1/pzTri2 flag bits) for
		    // every admitted quad, so the CPU emit is dead work whose output
		    // the GPU overwrites. NOTE: pz/pzTri compute, the
		    // `if (!pzTri1 && !pzTri2) return;` cull, and the M2d
		    // gos_PushTerrainOverlay block below stay UNCONDITIONAL — they
		    // feed the live decal producer (§2 WARNING, 9964d5a
		    // conflated-overlay regression class). The `goto fp_detail_only`
		    // inside this block jumps to a label AFTER it, so gating the
		    // block leaves the SSBO-full detail fallthrough intact.
		    const bool thinEmitArmed = gos_terrain_indirect::IsFrameSolidArmed();
		    if (s_thinEmitTrace) {
		        static bool s_loggedThinEmit = false;
		        if (!s_loggedThinEmit) {
		            s_loggedThinEmit = true;
		            printf("[TERRAIN_INDIRECT v1] event=cpu_thinemit_demoted "
		                   "path=quad_m2 armed=%d gate=IsFrameSolidArmed "
		                   "note=overlay_producer_unconditional\n",
		                   thinEmitArmed ? 1 : 0);
		            fflush(stdout);
		        }
		    }
		    if (terrainHandle != 0 && !thinEmitArmed)
		    {
		        const bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
		        const bool isAlpha  = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);
		        const bool alphaOverride = Terrain::terrainTextures2 && (!isCement || isAlpha);

		        // effectiveLightRGB: alphaOverride first, then selected (matching legacy priority)
		        auto lightRGBc = [&](int c) -> uint32_t {
		            DWORD lc = vertices[c]->lightRGB;
		            if (alphaOverride) lc = 0xffffffffu;
		            if (vertices[c]->pVertex->selected) lc = static_cast<DWORD>(SELECTION_COLOR);
		            return static_cast<uint32_t>(lc);
		        };

		        // Lazy recipe build: peek the cache first using only corner-0 (wx0, wy0).
		        // ~99% of quads hit the cache after warmup; building the full recipe (~20
		        // cold reads through vertices[c]->pVertex->{elevation, vertexNormal,
		        // terrainType}, ~30 stack stores, 4× terrainTypeToMaterial, bit-pack +
		        // memcpy) on a hit is pure waste — ensureRecipeForQuad discards `recipe`
		        // on hit. Build it only on miss.
		        const float wx0 = vertices[0]->vx;
		        const float wy0 = vertices[0]->vy;
		        const uint64_t recipeKey = TerrainPatchStream::makeRecipeKey(wx0, wy0);
		        uint32_t recipeIdx = TerrainPatchStream::tryGetRecipeIdx(recipeKey);
		        if (recipeIdx == UINT32_MAX)
		        {
		            TerrainQuadRecipe recipe;
		            recipe.wx0=wx0;             recipe.wy0=wy0;             recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
		            recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
		            recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
		            recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
		            recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
		            recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
		            recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
		            recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
		            recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;

		            // Pack 4 corner material types into _wp0 — bit-preserving write; shader reads via floatBitsToUint
		            {
		                uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
		                uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
		                uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
		                uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
		                uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
		                memcpy(&recipe._wp0, &tpacked, 4);
		            }

		            recipeIdx = TerrainPatchStream::ensureRecipeForQuad(recipeKey, recipe);
		            if (recipeIdx == UINT32_MAX) goto fp_detail_only;  // SSBO full — try detail still
		        }

		        {
		            TerrainQuadThinRecord tr;
		            tr.recipeIdx     = recipeIdx;
		            tr.terrainHandle = static_cast<uint32_t>(terrainHandle);
		            tr.flags         = (uvMode == BOTTOMLEFT ? 1u : 0u)
		                             | (pzTri1 ? 2u : 0u)
		                             | (pzTri2 ? 4u : 0u);
		            tr.cementWord    = 0u;  // Fix B rename (was _pad0).  Patch-stream
		                                    // path doesn't carry cement bits.
		            tr.lightRGB0     = lightRGBc(0);
		            tr.lightRGB1     = lightRGBc(1);
		            tr.lightRGB2     = lightRGBc(2);
		            tr.lightRGB3     = lightRGBc(3);
		            // Fix B 2026-05-14: clipPos zeroed (patch-stream uses its own VS,
		            // not the indirect thin VS that consumes clipPos).
		            for (int k = 0; k < 16; ++k) tr.clipPos[k] = 0.0f;
		            TerrainPatchStream::appendThinRecordDirect(tr);
		        }
		    }
		    fp_detail_only:;

		    // PR2a Stage 1a — M2c detail emit DELETED 2026-05-08.
		    //
		    // Was: an `if (useWaterInterestTexture && terrainDetailHandle != 0xffffffff)`
		    // block (~64 lines) that built per-corner gos_VERTEX arrays + per-tri
		    // clampUVs lambda + per-tri addVertices calls into the masterVertexNodes
		    // queue with flags `MC2_ISTERRAIN | MC2_DRAWALPHA`.
		    //
		    // Two reasons it was retired:
		    //   1. ZERO PIXEL OUTPUT since 521d83a (2026-04-16, "Fix terrain underlayer
		    //      artifact"). Render.Overlays at txmmgr.cpp:1818-1822 unconditionally
		    //      resets-and-skips any non-water alpha+terrain node — detail nodes
		    //      have flags MC2_ISTERRAIN|MC2_DRAWALPHA (no water flag) so they hit
		    //      the suppress predicate. The flat-plane detail layer was suppressed
		    //      because tessellated terrain elevation made it visible as a "dark
		    //      striped under-pattern."
		    //   2. ZERO CPU EXECUTION across tier1 (verified 2026-05-08 Stage 0a,
		    //      m2c_detail_emit_quads=0 on mc2_01/03/10/17). Code drift since the
		    //      M2 thin-record landing eliminated the runtime path even though
		    //      the brainstorm cited m2_thin_record_cpu_reduction_results.md's
		    //      "~5,800 quads/frame on mc2_01" — that perf number is stale.
		    //
		    // Future "tessellation-aware detail layer" feature is independent scope
		    // (see recon-6 "Open follow-ups" #4); brainstorm separately if/when it
		    // ships. Don't resurrect this block.

		    // M2d: per-quad base-texture overlay emit. Replaces the four legacy
		    // gos_PushTerrainOverlay sites further down in this function (TOPRIGHT tri1+tri2,
		    // BOTTOMLEFT tri1+tri2) — same output, but builds WorldOverlayVert straight from
		    // vertices[c] instead of memcpy'ing gVertex through oVertex and calling
		    // setOverlayWorldCoords. wz includes OVERLAY_ELEV_OFFSET (0.15f) to
		    // prevent z-fighting against the base terrain — matches setOverlayWorldCoords.
		    // ARGB and fog use raw vertices[c]->lightRGB/fogRGB — NO `selected` override and
		    // NO `terrainTextures2` whiteout (those overrides apply to base/detail textures
		    // only; legacy overlay block also uses raw lightRGB).
		    if (useOverlayTexture && overlayHandle != 0xffffffff)
		    {
		        // PR2b Stage 0b — bracket the M2d fast-path block + counter.
		        // Counter increments once per quad that enters M2d (whether or
		        // not the overlay actually emits — overlayTexId==0 short-circuits
		        // below, but the per-quad scaffolding cost is real). Drops to
		        // zero post Stage 3b legacy gate-off.
		        CostSplitOverlayScope _csOverlay;
		        gos_terrain_indirect::Counters_AddM2dOverlayEmitQuad();

		        // Slice A — substitutive gate-off, mirrors the PR2c mine gate
		        // (quad.cpp `if (!IsFrameMineArmed()) enqueueTerrainMineState`).
		        // When armed, the mission-static cement-overlay bake
		        // (gos_terrain_indirect::DrawDecalStatic, Render.TerrainOverlays
		        // Static) reproduces this exact emit from a persistent buffer,
		        // so the per-quad gos_PushTerrainOverlay producer is skipped.
		        // With the overlay gate off (MC2_TERRAIN_INDIRECT_OVERLAY=0) this is
		        // false => the legacy per-quad emit runs unchanged: ZERO
		        // behavior change. The legacy :2599+ sites are the FASTPATH-off
		        // fallback (post-`return` body) and are deliberately untouched.
		        const DWORD overlayTexId = tex_resolve(overlayHandle);
		        if (overlayTexId != 0 &&
		            !gos_terrain_indirect::IsFrameOverlayArmed())
		        {
		            WorldOverlayVert wov_corner[4];
		            for (int c = 0; c < 4; c++) {
		                wov_corner[c].wx   = vertices[c]->vx;
		                wov_corner[c].wy   = vertices[c]->vy;
		                wov_corner[c].wz   = vertices[c]->pVertex->elevation + OVERLAY_ELEV_OFFSET;
		                wov_corner[c].fog  = (float)((vertices[c]->fogRGB >> 24) & 0xFF) / 255.0f;
		                wov_corner[c].argb = (unsigned int)vertices[c]->lightRGB;
		                // u/v assigned per-tri below.
		            }

		            if (uvMode == BOTTOMLEFT) {
		                if (pzTri1) {
		                    WorldOverlayVert w[3] = { wov_corner[0], wov_corner[1], wov_corner[3] };
		                    w[0].u=oldminU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldminV;
		                    w[2].u=oldminU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		                if (pzTri2) {
		                    WorldOverlayVert w[3] = { wov_corner[1], wov_corner[2], wov_corner[3] };
		                    w[0].u=oldmaxU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldmaxV;
		                    w[2].u=oldminU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		            } else {
		                // BOTTOMRIGHT (= TOPRIGHT diagonal)
		                if (pzTri1) {
		                    WorldOverlayVert w[3] = { wov_corner[0], wov_corner[1], wov_corner[2] };
		                    w[0].u=oldminU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldminV;
		                    w[2].u=oldmaxU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		                if (pzTri2) {
		                    WorldOverlayVert w[3] = { wov_corner[0], wov_corner[2], wov_corner[3] };
		                    w[0].u=oldminU; w[0].v=oldminV;
		                    w[1].u=oldmaxU; w[1].v=oldmaxV;
		                    w[2].u=oldminU; w[2].v=oldmaxV;
		                    gos_PushTerrainOverlay(w, overlayTexId);
		                }
		            }
		        }
		    }

		    return;
		}
		// === end M2 branch — legacy path continues below ===

		if (uvMode == BOTTOMRIGHT)
		{
			//--------------------------
			// Top Triangle
			DWORD lightRGB0 = vertices[0]->lightRGB; 
			DWORD lightRGB1 = vertices[1]->lightRGB; 
			DWORD lightRGB2 = vertices[2]->lightRGB; 
			bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
			bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);

			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB0 = lightRGB1 = lightRGB2 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			// [DEPTHBIAS_CALIB] VPL-#10 distance-proportional grounding
			// probe (env MC2_DEPTHBIAS_CALIB, INERT log-only). pw == |1/clip.w|;
			// clip.w == 1/pw. One mc2_17 zoom-sweep (in/default/out) gives
			// W0 (default-zoom clip.w) + the real min/max clip.w to ground
			// the 6 CRITICAL assumptions in the distance-proportional spec.
			{
				static const bool s_dbc = mc2::gate::get<"MC2_DEPTHBIAS_CALIB">();
				if (s_dbc) {
					// Filter to VALID on-screen post-admission terrain verts
					// ONLY: pz in [0,1) (the [0,1] glClipControl NDC-z the
					// depth buffer actually sees -- excludes off-screen /
					// pre-gate / pz=-0.5 sentinel), pw finite & positive in a
					// sane camera range (excludes the pw=0.5 sentinel and the
					// pw=512 / pw=5.4e-5 behind-camera/degenerate outliers
					// that wrecked the first capture). clip.w == 1/pw.
					const float pz = vertices[0]->pz;
					const float pw = vertices[0]->pw;
					const bool  valid =
						(pz >= 0.0f && pz < 1.0f) &&
						(pw > 1.0e-5f && pw < 0.2f);   // clip.w in (5, 100000)
					if (valid) {
						const float cw = 1.0f / pw;
						// WINDOWED stats, RESET each summary so the user's
						// distinct zoom pauses (full-in / default / full-out)
						// read as distinct consecutive summary lines.
						static int   s_w = 0;
						static float s_cwMin = 1e30f, s_cwMax = -1e30f, s_cwSum = 0.0f;
						static int   s_first = 0;
						if (cw < s_cwMin) s_cwMin = cw;
						if (cw > s_cwMax) s_cwMax = cw;
						s_cwSum += cw;
						if (s_first < 12) {
							++s_first;
							fprintf(stderr,
								"[DEPTHBIAS_CALIB v2] event=sample pz=%.6f clipw=%.3f\n",
								pz, cw);
						}
						if ((++s_w % 4000) == 0) {
							fprintf(stderr,
								"[DEPTHBIAS_CALIB v2] event=window validN=%d "
								"clipwMin=%.2f clipwAvg=%.2f clipwMax=%.2f\n",
								s_w, s_cwMin, s_cwSum / (float)s_w, s_cwMax);
							s_cwMin = 1e30f; s_cwMax = -1e30f; s_cwSum = 0.0f; s_w = 0;
						}
					}
				}
			}
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[2]->px;
			gVertex[2].y		= vertices[2]->py;
			gVertex[2].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[2]->pw;
			gVertex[2].u		= maxU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB2;
			gVertex[2].frgb		= vertices[2]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			// Capture tri1 pz result and vertex data before the destructive
			// gVertex shuffle for tri2. appendQuad (below) uses both.
			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
			if (pzTri1)
			{
				{
					// sebi: beware this will be drawn with alpha blending, so need to make sure that alpha is not zero, because this is a base terrain layer!
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[2]);
						// PatchStream append moved to appendQuad after both pz gates.
					}

					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldmaxU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[2]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[1]);
						setOverlayWorldCoords(oVertex[2], vertices[2]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);

						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();

						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
						
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);	
					}
				}
			}

			//--------------------------
			//Bottom Triangle
			//
			// gVertex[0] same as above gVertex[0].
			// gVertex[1] is same as above gVertex[2].
			// gVertex[2] is calced from vertex[3].
			DWORD lightRGB3 = vertices[3]->lightRGB;
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB3 = 0xffffffff;

			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[1].x		= gVertex[2].x;	
			gVertex[1].y		= gVertex[2].y;	
			gVertex[1].z		= gVertex[2].z;	
			gVertex[1].rhw		= gVertex[2].rhw;
			gVertex[1].u		= gVertex[2].u;
			gVertex[1].v		= gVertex[2].v;	
			gVertex[1].argb		= gVertex[2].argb;
			gVertex[1].frgb		= gVertex[2].frgb;

			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			if (pzTri2)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}

					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[2]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);

						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();

 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}

						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}

						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
				}
			}

			// PatchStream: one bucket lookup for both triangles of this quad.
			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
				if (!TerrainPatchStream::isThinRecordsActive()) {
					if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[2], tx1);
					if (pzTri2) buildTerrainExtraTriple(vertices[0], vertices[2], vertices[3], tx2);
				}
				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
				// M1 compact record — TOPRIGHT diagonal.
				// gvTri1[0..2] = corners 0,1,2 (saved before shuffle).
				// gVertex[2]   = corner 3 (constructed by shuffle from vertices[3]).
				TerrainQuadRecord rec;
				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gvTri1[2].argb; rec.lightRGB3=gVertex[2].argb;
				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gvTri1[2].frgb; rec.fogRGB3  =gVertex[2].frgb;
				rec.terrainHandle = (uint32_t)terrainHandle;
				rec.flags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
				TerrainPatchStream::appendQuadRecord(rec);
				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				{
					TerrainQuadRecipe recipe;
					recipe.wx0=vertices[0]->vx; recipe.wy0=vertices[0]->vy; recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
					recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
					recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
					recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
					recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
					recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
					recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
					recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
					recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;
					const uint32_t tFlags = 0u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=0 → TOPRIGHT
					{
						uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
						uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
						uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
						memcpy(&recipe._wp0, &tpacked, 4);
					}
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gvTri1[2].argb, gVertex[2].argb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
			}
		}
		else if (uvMode == BOTTOMLEFT)
		{
			//------------------------------
			// Top Triangle.
			DWORD lightRGB0 = vertices[0]->lightRGB;
			DWORD lightRGB1 = vertices[1]->lightRGB;
			DWORD lightRGB3 = vertices[3]->lightRGB;
			bool isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
			bool isAlpha = Terrain::terrainTextures->isAlpha(vertices[0]->pVertex->textureData & 0x0000ffff);
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB0 = lightRGB1 = lightRGB3 = 0xffffffff;

			lightRGB0 = vertices[0]->pVertex->selected ? SELECTION_COLOR : lightRGB0;
			lightRGB1 = vertices[1]->pVertex->selected ? SELECTION_COLOR : lightRGB1;
			lightRGB3 = vertices[3]->pVertex->selected ? SELECTION_COLOR : lightRGB3;

			gVertex[0].x		= vertices[0]->px;
			gVertex[0].y		= vertices[0]->py;
			gVertex[0].z		= vertices[0]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[0].rhw		= vertices[0]->pw;
			gVertex[0].u		= minU;
			gVertex[0].v		= minV;
			gVertex[0].argb		= lightRGB0;
			gVertex[0].frgb		= vertices[0]->fogRGB;
			gVertex[0].frgb		= (gVertex[0].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[0]->pVertex->terrainType);

			gVertex[1].x		= vertices[1]->px;
			gVertex[1].y		= vertices[1]->py;
			gVertex[1].z		= vertices[1]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[1]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= minV;
			gVertex[1].argb		= lightRGB1;
			gVertex[1].frgb		= vertices[1]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[1]->pVertex->terrainType);

			gVertex[2].x		= vertices[3]->px;
			gVertex[2].y		= vertices[3]->py;
			gVertex[2].z		= vertices[3]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[2].rhw		= vertices[3]->pw;
			gVertex[2].u		= minU;
			gVertex[2].v		= maxV;
			gVertex[2].argb		= lightRGB3;
			gVertex[2].frgb		= vertices[3]->fogRGB;
			gVertex[2].frgb		= (gVertex[2].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[3]->pVertex->terrainType);

			// Capture tri1 pz result and vertex data before the destructive
			// gVertex shuffle for tri2. appendQuad (below) uses both.
			const bool pzTri1 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			const gos_VERTEX gvTri1[3] = {gVertex[0], gVertex[1], gVertex[2]};
			if (pzTri1)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[0], vertices[1], vertices[3]);
						// PatchStream append moved to appendQuad after both pz gates.
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTF		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[0]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[0]->vy) * oneOverTF;
		
						sVertex[1].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTF;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTF;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTF;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldminU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldminV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[0]->lightRGB;
						oVertex[1].argb		= vertices[1]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[0]);
						setOverlayWorldCoords(oVertex[1], vertices[1]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}
				}
			}

			//---------------------------------------
			// Bottom Triangle.
			// gVertex[0] is same as above gVertex[1]
			// gVertex[1] is new and calced from vertex[2].
			// gVertex[2] is same as above.
			DWORD lightRGB2 = vertices[2]->lightRGB;
			if (Terrain::terrainTextures2 && (!isCement || isAlpha))
				lightRGB2 = 0xffffffff;

			lightRGB2 = vertices[2]->pVertex->selected ? SELECTION_COLOR : lightRGB2;

			gVertex[0].x		= gVertex[1].x;	
			gVertex[0].y		= gVertex[1].y;	
			gVertex[0].z		= gVertex[1].z;	
			gVertex[0].rhw		= gVertex[1].rhw;	
			gVertex[0].u		= gVertex[1].u;	
			gVertex[0].v		= gVertex[1].v;	
			gVertex[0].argb		= gVertex[1].argb;
			gVertex[0].frgb		= gVertex[1].frgb;

			gVertex[1].x		= vertices[2]->px;
			gVertex[1].y		= vertices[2]->py;
			gVertex[1].z		= vertices[2]->pz + TERRAIN_DEPTH_FUDGE;
			gVertex[1].rhw		= vertices[2]->pw;
			gVertex[1].u		= maxU;
			gVertex[1].v		= maxV;
			gVertex[1].argb		= lightRGB2;
			gVertex[1].frgb		= vertices[2]->fogRGB;
			gVertex[1].frgb		= (gVertex[1].frgb & 0xFFFFFF00) | terrainTypeToMaterial(vertices[2]->pVertex->terrainType);

			const bool pzTri2 = ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
			                     (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
			                     (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
			if (pzTri2)
			{
				{
					if(terrainHandle!=0 && !TerrainPatchStream::isFastPathActive()) {
						mcTextureManager->addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
						fillTerrainExtra(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID, vertices[1], vertices[2], vertices[3]);
						// PatchStream append moved to appendQuad below.
					}

					//----------------------------------------------------
					// Draw the detail Texture
					if (useWaterInterestTexture && (terrainDetailHandle != 0xffffffff))
					{
						gos_VERTEX sVertex[3];
						memcpy(sVertex,gVertex,sizeof(gos_VERTEX)*3);
		
						float tilingFactor = Terrain::terrainTextures->getDetailTilingFactor(1);
						if (Terrain::terrainTextures2)
							tilingFactor = Terrain::terrainTextures2->getDetailTilingFactor();
							
 						float oneOverTf		= tilingFactor / Terrain::worldUnitsMapSide;
						
						sVertex[0].u		= (vertices[1]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[0].v		= (Terrain::mapTopLeft3d.y - vertices[1]->vy) * oneOverTf;
		
						sVertex[1].u		= (vertices[2]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
						sVertex[1].v		= (Terrain::mapTopLeft3d.y - vertices[2]->vy) * oneOverTf;
		
						sVertex[2].u		= (vertices[3]->vx - Terrain::mapTopLeft3d.x) * oneOverTf ;
						sVertex[2].v		= (Terrain::mapTopLeft3d.y - vertices[3]->vy) * oneOverTf ;
		
						if ((sVertex[0].u > MaxMinUV) || 
							(sVertex[0].v > MaxMinUV) ||
							(sVertex[1].u > MaxMinUV) || 
							(sVertex[1].v > MaxMinUV) ||
							(sVertex[2].u > MaxMinUV) || 
							(sVertex[2].v > MaxMinUV))
						{
							//If any are out range, move 'em back in range by adjustfactor.
							float maxU = fmax(sVertex[0].u,fmax(sVertex[1].u,sVertex[2].u));
							maxU = floor(maxU - (MaxMinUV-1.0f));
							
							float maxV = fmax(sVertex[0].v,fmax(sVertex[1].v,sVertex[2].v));
							maxV = floor(maxV - (MaxMinUV-1.0f));
							
							sVertex[0].u -= maxU;
							sVertex[1].u -= maxU;
							sVertex[2].u -= maxU;
							
							sVertex[0].v -= maxV;
							sVertex[1].v -= maxV;
							sVertex[2].v -= maxV;
						}
						
						//Light the Detail Texture
						if (Terrain::terrainTextures2)
						{
							sVertex[0].argb		= 
							sVertex[1].argb		= 
							sVertex[2].argb		= 0xffffffff;
						}
						// Slice A (A2) - DRAWALPHA detail dead-confirmation counter.
						// Counter-only; behaviour unchanged. Post-return legacy body,
						// unreachable on armed frames; expected ==0 (A2 gate).
						gos_terrain_indirect::Counters_AddLegacyDrawAlphaDetailQuad();
						mcTextureManager->addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
					}
					
					//--------------------------------------------------------------
					// Draw the Overlay Texture if it exists.
					if (useOverlayTexture && (overlayHandle != 0xffffffff))
					{
						//Uses EXACT same coords as the above normal texture.
						// Just replace the UVs and the texture handle!!
						gos_VERTEX oVertex[3];
						memcpy(oVertex,gVertex,sizeof(gos_VERTEX)*3);
						oVertex[0].u		= oldmaxU;
						oVertex[0].v		= oldminV;
						oVertex[1].u		= oldmaxU;
						oVertex[1].v		= oldmaxV;
						oVertex[2].u		= oldminU;
						oVertex[2].v		= oldmaxV;

						//Light the overlays!!
						oVertex[0].argb		= vertices[1]->lightRGB;
						oVertex[1].argb		= vertices[2]->lightRGB;
						oVertex[2].argb		= vertices[3]->lightRGB;

						// GPU projection: MC2 world coords into new typed batch
						setOverlayWorldCoords(oVertex[0], vertices[1]);
						setOverlayWorldCoords(oVertex[1], vertices[2]);
						setOverlayWorldCoords(oVertex[2], vertices[3]);

						{
							WorldOverlayVert wov[3];
							for (int _k = 0; _k < 3; ++_k) {
								wov[_k].wx   = oVertex[_k].x;
								wov[_k].wy   = oVertex[_k].y;
								wov[_k].wz   = oVertex[_k].z;
								wov[_k].u    = oVertex[_k].u;
								wov[_k].v    = oVertex[_k].v;
								wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
								wov[_k].argb = oVertex[_k].argb;
							}
							const DWORD overlayTexId = tex_resolve(overlayHandle);
							if (overlayTexId != 0)
								gos_PushTerrainOverlay(wov, overlayTexId);
						}
					}
				}
			}

			// PatchStream: one bucket lookup for both triangles of this quad.
			if (terrainHandle != 0 && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
				gos_TERRAIN_EXTRA tx1[3] = {}, tx2[3] = {};
				if (!TerrainPatchStream::isThinRecordsActive()) {
					if (pzTri1) buildTerrainExtraTriple(vertices[0], vertices[1], vertices[3], tx1);
					if (pzTri2) buildTerrainExtraTriple(vertices[1], vertices[2], vertices[3], tx2);
				}
				TerrainPatchStream::appendQuad(terrainHandle, gvTri1, tx1, pzTri1, gVertex, tx2, pzTri2);
				// M1 compact record — BOTTOMLEFT diagonal.
				// gvTri1 = {corner0, corner1, corner3} (saved before shuffle).
				// After shuffle: gVertex[0]=corner1, gVertex[1]=corner2, gVertex[2]=corner3.
				// corner2 (vertices[2]) is only in gVertex[1] after the shuffle.
				TerrainQuadRecord rec;
				rec.wx0=vertices[0]->vx; rec.wy0=vertices[0]->vy; rec.wz0=vertices[0]->pVertex->elevation; rec._wp0=0.f;
				rec.wx1=vertices[1]->vx; rec.wy1=vertices[1]->vy; rec.wz1=vertices[1]->pVertex->elevation; rec._wp1=0.f;
				rec.wx2=vertices[2]->vx; rec.wy2=vertices[2]->vy; rec.wz2=vertices[2]->pVertex->elevation; rec._wp2=0.f;
				rec.wx3=vertices[3]->vx; rec.wy3=vertices[3]->vy; rec.wz3=vertices[3]->pVertex->elevation; rec._wp3=0.f;
				rec.nx0=vertices[0]->pVertex->vertexNormal.x; rec.ny0=vertices[0]->pVertex->vertexNormal.y; rec.nz0=vertices[0]->pVertex->vertexNormal.z; rec._np0=0.f;
				rec.nx1=vertices[1]->pVertex->vertexNormal.x; rec.ny1=vertices[1]->pVertex->vertexNormal.y; rec.nz1=vertices[1]->pVertex->vertexNormal.z; rec._np1=0.f;
				rec.nx2=vertices[2]->pVertex->vertexNormal.x; rec.ny2=vertices[2]->pVertex->vertexNormal.y; rec.nz2=vertices[2]->pVertex->vertexNormal.z; rec._np2=0.f;
				rec.nx3=vertices[3]->pVertex->vertexNormal.x; rec.ny3=vertices[3]->pVertex->vertexNormal.y; rec.nz3=vertices[3]->pVertex->vertexNormal.z; rec._np3=0.f;
				rec.minU=minU; rec.minV=minV; rec.maxU=maxU; rec.maxV=maxV;
				// BOTTOMLEFT: gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3
				//             gVertex[1] after shuffle = corner2
				rec.lightRGB0=gvTri1[0].argb; rec.lightRGB1=gvTri1[1].argb; rec.lightRGB2=gVertex[1].argb; rec.lightRGB3=gvTri1[2].argb;
				rec.fogRGB0  =gvTri1[0].frgb; rec.fogRGB1  =gvTri1[1].frgb; rec.fogRGB2  =gVertex[1].frgb; rec.fogRGB3  =gvTri1[2].frgb;
				rec.terrainHandle = (uint32_t)terrainHandle;
				rec.flags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
				rec._ctrl2 = 0u; rec._ctrl3 = 0u;
				TerrainPatchStream::appendQuadRecord(rec);
				TerrainPatchStream::addRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				{
					TerrainQuadRecipe recipe;
					recipe.wx0=vertices[0]->vx; recipe.wy0=vertices[0]->vy; recipe.wz0=vertices[0]->pVertex->elevation; recipe._wp0=0.f;
					recipe.wx1=vertices[1]->vx; recipe.wy1=vertices[1]->vy; recipe.wz1=vertices[1]->pVertex->elevation; recipe._wp1=0.f;
					recipe.wx2=vertices[2]->vx; recipe.wy2=vertices[2]->vy; recipe.wz2=vertices[2]->pVertex->elevation; recipe._wp2=0.f;
					recipe.wx3=vertices[3]->vx; recipe.wy3=vertices[3]->vy; recipe.wz3=vertices[3]->pVertex->elevation; recipe._wp3=0.f;
					recipe.nx0=vertices[0]->pVertex->vertexNormal.x; recipe.ny0=vertices[0]->pVertex->vertexNormal.y; recipe.nz0=vertices[0]->pVertex->vertexNormal.z; recipe._np0=0.f;
					recipe.nx1=vertices[1]->pVertex->vertexNormal.x; recipe.ny1=vertices[1]->pVertex->vertexNormal.y; recipe.nz1=vertices[1]->pVertex->vertexNormal.z; recipe._np1=0.f;
					recipe.nx2=vertices[2]->pVertex->vertexNormal.x; recipe.ny2=vertices[2]->pVertex->vertexNormal.y; recipe.nz2=vertices[2]->pVertex->vertexNormal.z; recipe._np2=0.f;
					recipe.nx3=vertices[3]->pVertex->vertexNormal.x; recipe.ny3=vertices[3]->pVertex->vertexNormal.y; recipe.nz3=vertices[3]->pVertex->vertexNormal.z; recipe._np3=0.f;
					recipe.minU=minU; recipe.minV=minV; recipe.maxU=maxU; recipe.maxV=maxV;
					// BOTTOMLEFT: same corner order as fat record above.
					// gvTri1[0]=corner0, gvTri1[1]=corner1, gvTri1[2]=corner3; gVertex[1]=corner2
					const uint32_t tFlags = 1u | (pzTri1 ? 2u : 0u) | (pzTri2 ? 4u : 0u); // bit0=1 → BOTTOMLEFT
					{
						uint32_t m0 = terrainTypeToMaterial(vertices[0]->pVertex->terrainType);
						uint32_t m1 = terrainTypeToMaterial(vertices[1]->pVertex->terrainType);
						uint32_t m2 = terrainTypeToMaterial(vertices[2]->pVertex->terrainType);
						uint32_t m3 = terrainTypeToMaterial(vertices[3]->pVertex->terrainType);
						uint32_t tpacked = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
						memcpy(&recipe._wp0, &tpacked, 4);
					}
					TerrainPatchStream::appendThinRecord(terrainHandle, recipe, tFlags,
						gvTri1[0].argb, gvTri1[1].argb, gVertex[1].argb, gvTri1[2].argb);
					TerrainPatchStream::addThinRecordVertParity((pzTri1 ? 3u : 0u) + (pzTri2 ? 3u : 0u));
				}
			}
		}
	}

#ifdef _DEBUG
	if (selected )
	{
		drawLine();
		selected = FALSE;		
	}
#endif

}

extern float elevationAnimation;
