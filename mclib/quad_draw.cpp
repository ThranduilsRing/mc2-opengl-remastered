// TerrainQuad: TerrainQuad::draw: the SOLID / DRAWALPHA terrain and overlay submission, the M2d decal producer and its parity probe.
// Declared in the class headers; shared names in quad_internal.h.
#include <algorithm>

#include "quad_internal.h"
#include "texture_assets.h"
#include "mc2fmt.h"


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
static const bool s_m2dPzParity = false;
static int  s_m2dPzParityChecked = 0;       // quads the probe actually compared
static int  s_m2dPzParityMismatch = 0;      // quads with any pzc/pzTri divergence
static int  s_m2dPzParityEmitted = 0;       // first-N detail lines emitted so far
static bool s_m2dPzParityArmLogged = false; // one-shot arm/scope-assertion line
static const int s_m2dPzParityEmitCap = 32; // first-N detail-line cap

// Builds the gos_TERRAIN_EXTRA[3] (world position and normal per corner) of one
// triangle by value, so the patch stream can write the bytes without re-reading the verts.


// GPU projection: pack MC2 world coords into overlay vertex instead of screen-space
static inline void setOverlayWorldCoords(gos_VERTEX& v, const VertexPtr src) {
	v.x = src->vx;
	v.y = src->vy;
	v.z = src->pVertex->elevation + OVERLAY_ELEV_OFFSET;
	v.rhw = 1.0f;
}

//---------------------------------------------------------------------------
// Locals TerrainQuad::draw hands from one phase to the next: the shared vertex scratch and UV
// insets (QuadDrawFrame) and the per-triangle pz-validity flags of the fast path (QuadFastFrame).
struct QuadDrawFrame {
	gos_VERTEX gVertex[3];
	float minU;
	float maxU;
	float minV;
	float maxV;
	float oldminU;
	float oldmaxU;
	float oldminV;
	float oldmaxV;
	bool fastPathEligible;
};

struct QuadFastFrame {
	bool pzTri1;
	bool pzTri2;
};


// Set up the vertex scratch and the base / overlay UV insets for this quad.
void TerrainQuad::drawSetup(QuadDrawFrame& f) {
	auto& minU = f.minU; auto& maxU = f.maxU; auto& minV = f.minV;
	auto& maxV = f.maxV; auto& oldminU = f.oldminU; auto& oldmaxU = f.oldmaxU;
	auto& oldminV = f.oldminV; auto& oldmaxV = f.oldmaxV;
	//---------------------------------------
	// GOS 3D draw Calls now!

	// M2b instrumentation: pre-branch setup zone. Wraps gVertex decl, minU/maxU
	// compute, optional uvData read, camPosition load, fastPathEligible 7-flag
	// computation (4 PatchStream method calls), and the path_mix counter. This
	// runs for every quad that didn't pure-water early-exit. Closes at end of
	// the outer-if scope, so it INCLUDES the fast-path and legacy bodies — but
	// FP.* and Quad.legacy zones are children, so Tracy will show preBranch self
	// time = (preBranch total) - (FP.* + Quad.legacy + Legacy.*).

        // sebi half pixel, hope this is correct :-)
	minU = 0.5f / TERRAIN_TXM_SIZE;
	maxU = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
	minV = 0.5f / TERRAIN_TXM_SIZE;
	maxV = 1.0f - 0.5f / TERRAIN_TXM_SIZE;
	oldminU = 0.0078125f;
	oldmaxU = 0.9921875f;
	oldminV = 0.0078125f;
	oldmaxV = 0.9921875f;

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
		if (mc2::textures::logicalSize(overlayHandle, ovLogicalW, ovLogicalH))
		{
			const DWORD ovEdge = ovLogicalW * mc2::textures::uvScale(overlayHandle);
			if (ovEdge > 0)
			{
				oldminU = oldminV = 0.5f / static_cast<float>(ovEdge);
				oldmaxU = oldmaxV = 1.0f - 0.5f / static_cast<float>(ovEdge);
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

}

// Decide whether the M2 fast path takes this quad and run the path-mix diagnostic.
void TerrainQuad::drawPathSelect(QuadDrawFrame& f) {
    // Keep the authored overlay route and its existing kill switches. Base
    // terrain readiness belongs to the native indirect/LOD producer.
    static const bool fast = true
        && false
        && false
        && false;
    f.fastPathEligible = fast && gos_terrain_indirect::IsFrameSolidArmed()
        && (terrainHandle != 0 || (useOverlayTexture && overlayHandle != 0xffffffff)
            || (useWaterInterestTexture && terrainDetailHandle != 0xffffffff));
}

// Fast path: compute the pz-validity of each triangle and run the pz parity probe.
void TerrainQuad::fastPzGate(QuadFastFrame& fp) {
	auto& pzTri1 = fp.pzTri1; auto& pzTri2 = fp.pzTri2;
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
	            glm::vec3 ov3D(vertices[c]->vx, vertices[c]->vy,
	                                 vertices[c]->pVertex->elevation);
	            Stuff::Vector4D osp;
	            eye->projectForTerrainAdmission(ov3D, osp);
	            float pz_adj = osp.z + TERRAIN_DEPTH_FUDGE;
	            pzc[c] = (pz_adj >= 0.0f) && (pz_adj < 1.0f);
	        }
	    }

	    
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
	            mc2::fmt::print_err("[M2D_PZ_PARITY v1] event=armed mechanism=reproject+clipInfo_guard scope_assert=Renderer!=3 Environment.Renderer={:d} emit_cap={:d} note=probe_only_cv_pz_read_pre_step8b\n", mc2::fmt::as_i32(static_cast<int>(Environment.Renderer)), mc2::fmt::as_i32(s_m2dPzParityEmitCap));
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
	                mc2::fmt::print_err("[M2D_PZ_PARITY v1] event=mismatch n={:d} pzc_new={:d}{:d}{:d}{:d} pzc_old={:d}{:d}{:d}{:d} pzTri_new={:d}{:d} pzTri_old={:d}{:d} pzNeeded={:d} uvMode={:d}\n", mc2::fmt::as_i32(s_m2dPzParityEmitted), mc2::fmt::as_i32(pzc[0]), mc2::fmt::as_i32(pzc[1]), mc2::fmt::as_i32(pzc[2]), mc2::fmt::as_i32(pzc[3]), mc2::fmt::as_i32(pzc_old[0]), mc2::fmt::as_i32(pzc_old[1]), mc2::fmt::as_i32(pzc_old[2]), mc2::fmt::as_i32(pzc_old[3]), mc2::fmt::as_i32(pzTri1), mc2::fmt::as_i32(pzTri2), mc2::fmt::as_i32(pzTri1_old), mc2::fmt::as_i32(pzTri2_old), mc2::fmt::as_i32(pzNeeded ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(uvMode)));
	                fflush(stderr);
	            }
	        }
	        if ((s_m2dPzParityChecked % 14000) == 0) {
	            mc2::fmt::print_err("[M2D_PZ_PARITY v1] event=summary checked={:d} mismatches={:d}\n", mc2::fmt::as_i32(s_m2dPzParityChecked), mc2::fmt::as_i32(s_m2dPzParityMismatch));
	            fflush(stderr);
	        }
	    }
}

// Fast path: emit the thin record (falls through to the detail label when the recipe SSBO is full).


// Fast path: emit the overlay decal triangles.
void TerrainQuad::fastOverlayEmit(QuadDrawFrame& f, QuadFastFrame& fp) {
	auto& oldminU = f.oldminU; auto& oldmaxU = f.oldmaxU; auto& oldminV = f.oldminV;
	auto& oldmaxV = f.oldmaxV; auto& pzTri1 = fp.pzTri1; auto& pzTri2 = fp.pzTri2;
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
	                wov_corner[c].fog  = static_cast<float>(((vertices[c]->fogRGB >> 24) & 0xFF)) / 255.0f;
	                wov_corner[c].argb = static_cast<unsigned int>(vertices[c]->lightRGB);
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
}

// Legacy path for a BOTTOMRIGHT quad: base, overlay, detail and patch-stream emit for both triangles.
void TerrainQuad::drawLegacyBottomRight(QuadDrawFrame& f) {
	auto& gVertex = f.gVertex; auto& minU = f.minU; auto& maxU = f.maxU;
	auto& minV = f.minV; auto& maxV = f.maxV; auto& oldminU = f.oldminU;
	auto& oldmaxU = f.oldmaxU; auto& oldminV = f.oldminV; auto& oldmaxV = f.oldmaxV;
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
			static const bool s_dbc = false;
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
					s_cwMin = std::min(s_cwMin, cw);
					s_cwMax = std::max(s_cwMax, cw);
					s_cwSum += cw;
					if (s_first < 12) {
						++s_first;
						mc2::fmt::print_err("[DEPTHBIAS_CALIB v2] event=sample pz={:.6f} clipw={:.3f}\n", pz, cw);
					}
					if ((++s_w % 4000) == 0) {
						mc2::fmt::print_err("[DEPTHBIAS_CALIB v2] event=window validN={:d} clipwMin={:.2f} clipwAvg={:.2f} clipwMax={:.2f}\n", mc2::fmt::as_i32(s_w), s_cwMin, s_cwSum / static_cast<float>(s_w), s_cwMax);
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

		if (pzTri1)
		{
			{
				// sebi: beware this will be drawn with alpha blending, so need to make sure that alpha is not zero, because this is a base terrain layer!
				if(terrainHandle!=0) {
					mc2::renderSubmissions().addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
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
							wov[_k].fog  = static_cast<float>(((oVertex[_k].frgb >> 24) & 0xFF)) / 255.0f;
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
					mc2::renderSubmissions().addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
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
				if(terrainHandle!=0) {
					mc2::renderSubmissions().addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
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
							wov[_k].fog  = static_cast<float>(((oVertex[_k].frgb >> 24) & 0xFF)) / 255.0f;
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
					mc2::renderSubmissions().addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
				}
			}
		}

		// PatchStream: one bucket lookup for both triangles of this quad.

}

// Legacy path for a BOTTOMLEFT quad: base, overlay, detail and patch-stream emit for both triangles.
void TerrainQuad::drawLegacyBottomLeft(QuadDrawFrame& f) {
	auto& gVertex = f.gVertex; auto& minU = f.minU; auto& maxU = f.maxU;
	auto& minV = f.minV; auto& maxV = f.maxV; auto& oldminU = f.oldminU;
	auto& oldmaxU = f.oldmaxU; auto& oldminV = f.oldminV; auto& oldmaxV = f.oldmaxV;
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

		if (pzTri1)
		{
			{
				if(terrainHandle!=0) {
					mc2::renderSubmissions().addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
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
					mc2::renderSubmissions().addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
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
							wov[_k].fog  = static_cast<float>(((oVertex[_k].frgb >> 24) & 0xFF)) / 255.0f;
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
				if(terrainHandle!=0) {
					mc2::renderSubmissions().addVertices(terrainHandle,gVertex,MC2_ISTERRAIN | MC2_DRAWSOLID);
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
					mc2::renderSubmissions().addVertices(terrainDetailHandle,sVertex,MC2_ISTERRAIN | MC2_DRAWALPHA);
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
							wov[_k].fog  = static_cast<float>(((oVertex[_k].frgb >> 24) & 0xFF)) / 255.0f;
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

}

// Emits this quad: the M2 fast path (thin record, overlay decals) when the patch stream is armed,
// otherwise the legacy per-triangle gos_VERTEX path for the BOTTOMRIGHT / BOTTOMLEFT split.
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

		QuadDrawFrame f;
		drawSetup(f);
		drawPathSelect(f);

		if (f.fastPathEligible)
		{
		    QuadFastFrame fp;
		    fastPzGate(fp);

		    if (!fp.pzTri1 && !fp.pzTri2) return;  // both culled — skip entirely

		    fastOverlayEmit(f, fp);

		    return;
		}
		// === end M2 branch — legacy path continues below ===

		if (uvMode == BOTTOMRIGHT)
		{
			drawLegacyBottomRight(f);
		}
		else if (uvMode == BOTTOMLEFT)
		{
			drawLegacyBottomLeft(f);
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
