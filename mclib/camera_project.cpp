// Camera: screen-to-world projection: the ground-plane and terrain approximations and inverseProject.
// Declared in the class headers; shared names in camera_internal.h.
#include <algorithm>

#include "camera_internal.h"
#include "mc2fmt.h"
//---------------------------------------------------------------------------

inline void mapTileCellToWorldPos (long tileR, long tileC, long cellR, long cellC, glm::vec3& worldPos) {

	worldPos.x = Terrain::tileColToWorldCoord[tileC] + Terrain::cellToWorldCoord[cellC] + Terrain::halfWorldUnitsPerCell;
	worldPos.y = Terrain::tileRowToWorldCoord[tileR] - Terrain::cellToWorldCoord[cellR] - Terrain::halfWorldUnitsPerCell;
	worldPos.z = static_cast<float>(0.0);
}

//---------------------------------------------------------------------------

inline void mapCellToWorldPos (long cellR, long cellC, glm::vec3& worldPos) {

	worldPos.x = Terrain::cellColToWorldCoord[cellC] + Terrain::halfWorldUnitsPerCell;
	worldPos.y = Terrain::cellRowToWorldCoord[cellR] - Terrain::halfWorldUnitsPerCell;
	worldPos.z = static_cast<float>(0.0);
}

//---------------------------------------------------------------------------
// Ported screen-triangle containment (the body of the deleted overThisTile),
// rewritten to operate on FRESHLY forward-projected screen corners instead of
// stale per-frame vertices[]->px/py. corners[4] are screen XY from
// projectForSelectionPicking; valid[4] flags whether each projected inside the
// viewport rect. A tile is "over" the cursor if it contains it in either of
// its two triangles (0,1,2) / (0,2,3) - same theorem as the original.
static inline bool s_pointInScreenTri (const glm::vec3& v0,
                                       const glm::vec3& v1,
                                       const glm::vec3& v2,
                                       long mouseX, long mouseY)
{
	// CAMERA-FRUSTUM-HARNESS-1: delegate to camera_frustum_math.h (same arithmetic).
	return camera_frustum_math::pointInScreenTri(v0.x, v0.y, v1.x, v1.y,
	                                             v2.x, v2.y, mouseX, mouseY);
}

static inline bool s_overThisTileProjected (const glm::vec3 corners[4],
                                             const bool valid[4],
                                             long mouseX, long mouseY)
{
	// Triangle one: corners 0,1,2.
	if (valid[0] && valid[1] && valid[2] &&
	    s_pointInScreenTri(corners[0], corners[1], corners[2], mouseX, mouseY))
		return true;

	// Triangle two: corners 0,2,3 (tiles are two polys).
	if (valid[0] && valid[2] && valid[3] &&
	    s_pointInScreenTri(corners[0], corners[2], corners[3], mouseX, mouseY))
		return true;

	return false;
}
		
//---------------------------------------------------------------------------
// PARITY DIAGNOSTIC helper (MC2_TERRAIN_PICK_PARITY). Logs the raycast vs
// legacy world results for the same screen coords, then yields the value the
// active flag-on mode (raycast-if-flag-on-else-legacy) would have returned:
//   - raycast hit  -> overwrite point with the raycast hit, return 0.
//   - raycast miss -> leave the legacy point/return unchanged.
static unsigned long parityReport (Stuff::Vector2DOf<long> &screenPos,
                                   bool raycastHit,
                                   const glm::vec3 &raycastPt,
                                   glm::vec3 &point,
                                   unsigned long legacyRet)
{
	const long verticesMapSideDivTwo = Terrain::realVerticesMapSide / 2;
	const long metersMapSideDivTwo =
		verticesMapSideDivTwo * float2long(Terrain::worldUnitsPerVertex);

	// Tile cell index for the legacy world point.
	long legacyTileC =
		float2long(point.x * Terrain::oneOverWorldUnitsPerVertex + verticesMapSideDivTwo);
	long legacyTileR =
		float2long((metersMapSideDivTwo - point.y) * Terrain::oneOverWorldUnitsPerVertex);

	long rcTileC = -1, rcTileR = -1;
	float dWorld = -1.0f;
	if (raycastHit)
	{
		rcTileC = float2long(raycastPt.x * Terrain::oneOverWorldUnitsPerVertex + verticesMapSideDivTwo);
		rcTileR = float2long((metersMapSideDivTwo - raycastPt.y) * Terrain::oneOverWorldUnitsPerVertex);
		const float ddx = raycastPt.x - point.x;
		const float ddy = raycastPt.y - point.y;
		const float ddz = raycastPt.z - point.z;
		const float d2 = ddx*ddx + ddy*ddy + ddz*ddz;
		dWorld = (d2 > 0.0f) ? sqrtf(d2) : 0.0f;
	}

	mc2::fmt::print_out("[PICK_PARITY] screen=({:d},{:d})  raycast=({:.3f},{:.3f},{:.3f}) cell=({:d},{:d})  legacy=({:.3f},{:.3f},{:.3f}) cell=({:d},{:d})  dWorld={:.4f}  raycastHit={:d}\n", mc2::fmt::as_i32(static_cast<long>(screenPos.x)), mc2::fmt::as_i32(static_cast<long>(screenPos.y)), raycastPt.x, raycastPt.y, raycastPt.z, mc2::fmt::as_i32(rcTileC), mc2::fmt::as_i32(rcTileR), point.x, point.y, point.z, mc2::fmt::as_i32(legacyTileC), mc2::fmt::as_i32(legacyTileR), dWorld, mc2::fmt::as_i32(raycastHit ? 1 : 0));
	std::fflush(stdout);

	// Preserve flag-on return behavior: raycast wins when it hit.
	if (raycastHit)
	{
		point = raycastPt;
		return 0;
	}
	return legacyRet;
}

//---------------------------------------------------------------------------
// [LOW-CAMERA] Guard-free general 4x4 inverse (Gauss-Jordan, partial pivot).
// Stuff::Matrix4D::Invert flags any det < SMALL(1e-4) singular -> ×1e30 garbage,
// which is EVERY reverse-Z projective matrix (the documented "Invert(
// worldToClipGL()) UNRELIABLE in the game" trap, camera.h). Used ONLY by the
// inverse-VP cursor->world helpers below (preview + mouse-anchored zoom); the
// global Matrix4D::Invert is left byte-identical, and terrain PICKING uses the
// forward-projection path, not this — so no working path is affected.
static bool lowCamInvert4x4 (const glm::mat4& M, glm::mat4& out)
{
	double a[4][8];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{ a[r][c] = static_cast<double>(M[c][r]); a[r][c+4] = (r==c)?1.0:0.0; }
	for (int col = 0; col < 4; ++col)
	{
		int piv = col; double best = fabs(a[col][col]);
		for (int r = col+1; r < 4; ++r)
			if (fabs(a[r][col]) > best) { best = fabs(a[r][col]); piv = r; }
		if (best < 1e-20) return false;
		if (piv != col) for (int k = 0; k < 8; ++k) { double t=a[col][k]; a[col][k]=a[piv][k]; a[piv][k]=t; }
		double inv = 1.0 / a[col][col];
		for (int k = 0; k < 8; ++k) a[col][k] *= inv;
		for (int r = 0; r < 4; ++r) if (r != col)
		{ double f=a[r][col]; for (int k=0;k<8;++k) a[r][k]-=f*a[col][k]; }
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out[c][r] = static_cast<float>(a[r][c+4]);
	return true;
}

//---------------------------------------------------------------------------
// O(1) screen -> z=0 ground-plane unproject. No quad scan, no terrain pick.
// See header. Mirrors the matrix-inverse unproject used by the LOD-chunk pick
// path, then intersects the camera ray with the world z=0 plane.
bool Camera::screenToGroundPlaneApprox (long screenX, long screenY, glm::vec3 &outWorld, float z_plane)
{
	outWorld.x = outWorld.y = outWorld.z = 0.0f;
	if (turn < 4)
		return false;

	glm::mat4 M = worldToClipGL();
	glm::mat4 Minv;
	if (!lowCamInvert4x4(M, Minv)) return false;

	const float w = screenResolution.x;
	const float h = screenResolution.y;
	if (w <= 0.0f || h <= 0.0f)
		return false;

	const float ndcX =  2.0f * (float(screenX) / w) - 1.0f;
	const float ndcY =  1.0f - 2.0f * (float(screenY) / h);

	// clip_row * Minv (row-vector convention), perspective divide.
	auto unprojectNDC = [&](float nz) -> glm::vec3 {
		float wx = ndcX*Minv[0][0] + ndcY*Minv[0][1] + nz*Minv[0][2] + Minv[0][3];
		float wy = ndcX*Minv[1][0] + ndcY*Minv[1][1] + nz*Minv[1][2] + Minv[1][3];
		float wz = ndcX*Minv[2][0] + ndcY*Minv[2][1] + nz*Minv[2][2] + Minv[2][3];
		float ww = ndcX*Minv[3][0] + ndcY*Minv[3][1] + nz*Minv[3][2] + Minv[3][3];
		const float invW = (fabsf(ww) > 1e-8f) ? (1.0f / ww) : 0.0f;
		glm::vec3 v; v.x = wx*invW; v.y = wy*invW; v.z = wz*invW;
		return v;
	};
	// GL-swapped (x'=-x, y'=z, z'=y) -> MC2 world (x=-glX, y=glZ, z=glY).
	auto renderToMc2 = [](const glm::vec3& g) -> glm::vec3 {
		glm::vec3 r; mc2ToRender(r, g); return r;
	};

	// REVERSE-Z, ZERO_TO_ONE: near plane is NDC z=1, far is z=0 (see inverseProject).
	glm::vec3 ro = renderToMc2(unprojectNDC(1.0f));    // near
	glm::vec3 rf = renderToMc2(unprojectNDC( 0.0f));   // far

	const float dz = rf.z - ro.z;
	if (fabsf(dz) < 1e-6f) { outWorld = ro; outWorld.z = 0.0f; return false; }

	const float t = (z_plane - ro.z) / dz;
	outWorld.x = ro.x + t * (rf.x - ro.x);
	outWorld.y = ro.y + t * (rf.y - ro.y);
	outWorld.z = z_plane;
	return true;
}

//---------------------------------------------------------------------------
// Double-precision Gauss-Jordan inverse used only by pickTerrainRay: the float
// inverse loses ~1e-3 relative on a reverse-Z projection, which is metres of ray
// drift at the far end of a 1K-map ray.
static bool pickInvert4x4d (const double (&m)[4][4], double (&out)[4][4])
{
	double a[4][8];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{ a[r][c] = m[r][c]; a[r][c+4] = (r==c)?1.0:0.0; }
	for (int col = 0; col < 4; ++col)
	{
		int piv = col; double best = fabs(a[col][col]);
		for (int r = col+1; r < 4; ++r)
			if (fabs(a[r][col]) > best) { best = fabs(a[r][col]); piv = r; }
		if (best < 1e-300) return false;
		if (piv != col) for (int k = 0; k < 8; ++k) { double t=a[col][k]; a[col][k]=a[piv][k]; a[piv][k]=t; }
		const double inv = 1.0 / a[col][col];
		for (int k = 0; k < 8; ++k) a[col][k] *= inv;
		for (int r = 0; r < 4; ++r) if (r != col)
		{ const double f=a[r][col]; for (int k=0;k<8;++k) a[r][k]-=f*a[col][k]; }
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out[r][c] = a[r][c+4];
	return true;
}

// MC2_TERRAIN_PICK_PARITY diagnostic: names which gate of pickTerrainRay refused (first few only).
static bool pickFail (int code, long sx, long sy)
{
	static const bool s_dbg = false;
	static int s_left = 12;
	if (s_dbg && s_left > 0)
	{
		--s_left;
		mc2::fmt::print_out("[PICK_RAY_PARITY v1] event=ray_refused code={:d} px=({:d},{:d})\n", mc2::fmt::as_i32(code), mc2::fmt::as_i32(sx), mc2::fmt::as_i32(sy));
	}
	return false;
}

bool Camera::pickTerrainRay (long screenX, long screenY, glm::vec3 &outWorld, glm::vec3 *rawHit)
{
	if (turn < 4 || !land || !Terrain::mapData)
		return pickFail(1, screenX, screenY);
	const double w = screenResolution.x;
	const double h = screenResolution.y;
	if (w <= 0.0 || h <= 0.0)
		return pickFail(2, screenX, screenY);

	// Row-vector convention (clip = p * G), G(r,c) = Mf[c][r] as in projectModernClipGL.
	// Inverse: p = clip * Gi, so p_i = sum_k clip_k * Gi[k][i].
	const glm::mat4 Mf = worldToClipGL();
	double G[4][4], Gi[4][4];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			G[r][c] = static_cast<double>(Mf[c][r]);
	if (!pickInvert4x4d(G, Gi))
		return pickFail(3, screenX, screenY);

	const double ndcX =  2.0 * (static_cast<double>(screenX) / w) - 1.0;
	const double ndcY =  1.0 - 2.0 * (static_cast<double>(screenY) / h);
	auto unproject = [&](double nz, double (&o)[3]) -> bool {
		const double clip[4] = { ndcX, ndcY, nz, 1.0 };
		double p[4];
		for (int i = 0; i < 4; ++i)
			p[i] = clip[0]*Gi[0][i] + clip[1]*Gi[1][i] + clip[2]*Gi[2][i] + clip[3]*Gi[3][i];
		if (fabs(p[3]) < 1e-12) return pickFail(4, screenX, screenY);
		// worldToClipGL() = kAxisSwapMC2toGL * view * proj in row-vector order, i.e. it takes a RAW MC2
		// world point (the swap is its first factor, exactly as projectModernClipGL feeds it), so the
		// inverse returns MC2 world directly. The older helpers swap AGAIN after the inverse; that second
		// swap is why their ray came out with a collapsed X response and a far point above the camera.
		o[0] = p[0] / p[3]; o[1] = p[1] / p[3]; o[2] = p[2] / p[3];
		return true;
	};

	// REVERSE-Z, ZERO_TO_ONE: near plane is NDC z=1; any other depth of the same pixel lies on the same
	// ray. Origin = the near-plane point, direction = toward NDC z=0.5 (z=0 is infinity: w -> 0). Both
	// come from the matrix, so the ray is the one the GPU rasterises (physicalPos is NOT used: it is
	// stored in render space and is not the MC2 eye).
	double ro[3], midPt[3];
	if (!unproject(1.0, ro))
		return pickFail(5, screenX, screenY);
	if (!unproject(0.5, midPt))
		return pickFail(6, screenX, screenY);
	double d[3] = { midPt[0]-ro[0], midPt[1]-ro[1], midPt[2]-ro[2] };
	const double dl = sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
	if (dl < 1e-9) return pickFail(7, screenX, screenY);
	for (double & i : d) i /= dl;
	if (d[2] > -1e-6)            // a ray that never descends cannot hit the surface
		return pickFail(8, screenX, screenY);

	// Advance the origin to where the ray enters the map's XY box so the DDA starts
	// on-map (raycastTerrain stops at the first off-map cell). Slab clip in XY.
	const double half = static_cast<double>(Terrain::worldUnitsMapSide) * 0.5 - 1.0;
	double t0 = 0.0, t1 = 1e30;
	for (int ax = 0; ax < 2; ++ax)
	{
		if (fabs(d[ax]) < 1e-12)
		{
			if (ro[ax] < -half || ro[ax] > half) return pickFail(9, screenX, screenY);
			continue;
		}
		double ta = (-half - ro[ax]) / d[ax];
		double tb = ( half - ro[ax]) / d[ax];
		if (ta > tb) { const double t = ta; ta = tb; tb = t; }
		t0 = std::max(t0, ta);
		t1 = std::min(t1, tb);
	}
	if (t0 > t1) return pickFail(10, screenX, screenY);
	const double ox = ro[0] + t0*d[0], oy = ro[1] + t0*d[1], oz = ro[2] + t0*d[2];

	float hx, hy, hz;
	if (!Terrain::raycastTerrain(static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz),
	                             static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]),
	                             &hx, &hy, &hz))
		return pickFail(11, screenX, screenY);

	// Verify against the forward projection the GPU and inverseProject use: the hit
	// must land back on the cursor pixel. Anything else (degenerate matrix, a ray the
	// DDA mis-walked) is a miss so the caller falls back.
	{
		glm::vec3 hp(hx, hy, hz);
		Stuff::Vector4D sc;
		projectForSelectionPicking(hp, sc);
		const float ex = sc.x - static_cast<float>(screenX);
		const float ey = sc.y - static_cast<float>(screenY);
		if (!(sc.z >= 0.0f && sc.z < 1.0f) || ex*ex + ey*ey > 9.0f)
			return pickFail(12, screenX, screenY);
	}
	if (rawHit) { rawHit->x = hx; rawHit->y = hy; rawHit->z = hz; }

	glm::vec3 hp(hx, hy, hz);
	if (land->getNumQuads() == 0)
	{
		// The production ground picker whenever the quadList is empty (LOD-chunk terrain path) is
		// inverseProject's "closest vertex in screen space" fallback: reproduce it from the four
		// corners of the quad the ray hit (the nearest projected vertex to a surface point is one of
		// them), so the answer is the same point the O(vertices) walk returns.
		int tileR = 0, tileC = 0;
		land->worldToTile(hp, tileR, tileC);
		const int side = static_cast<int>(Terrain::realVerticesMapSide);
		float bestD = 1.0e30f;
		for (int dr = 0; dr < 2; ++dr)
			for (int dc = 0; dc < 2; ++dc)
			{
				int r = tileR + dr, c = tileC + dc;
				if (r < 0) r = 0; else if (r >= side) r = side - 1;
				if (c < 0) c = 0; else if (c >= side) c = side - 1;
				glm::vec3 vw(Terrain::tileColToWorldCoord[c], Terrain::tileRowToWorldCoord[r], land->getTerrainElevation(r, c));
				Stuff::Vector4D sc;
				projectForSelectionPicking(vw, sc);
				const float dx = sc.x - static_cast<float>(screenX), dy = sc.y - static_cast<float>(screenY);
				const float d = dx*dx + dy*dy;
				if (d < bestD) { bestD = d; outWorld = vw; }
			}
		return true;
	}

	// Snap to the containing cell's centre (the point inverseProject returns for a populated quadList).
	int cellR = 0, cellC = 0;
	land->worldToCell(hp, cellR, cellC);
	const int maxCell = static_cast<int>(Terrain::realVerticesMapSide) * MAPCELL_DIM - 1;
	if (cellR < 0) cellR = 0; else if (cellR > maxCell) cellR = maxCell;
	if (cellC < 0) cellC = 0; else if (cellC > maxCell) cellC = maxCell;
	mapCellToWorldPos(cellR, cellC, outWorld);
	outWorld.z = land->getTerrainElevation(outWorld);
	return true;
}

//---------------------------------------------------------------------------
bool Camera::screenToTerrainApprox (long screenX, long screenY, glm::vec3 &outWorld)
{
	outWorld.x = outWorld.y = outWorld.z = 0.0f;
	if (turn < 4)
		return false;

	glm::mat4 M = worldToClipGL();
	glm::mat4 Minv;
	if (!lowCamInvert4x4(M, Minv)) return false;

	const float w = screenResolution.x;
	const float h = screenResolution.y;
	if (w <= 0.0f || h <= 0.0f)
		return false;

	const float ndcX =  2.0f * (float(screenX) / w) - 1.0f;
	const float ndcY =  1.0f - 2.0f * (float(screenY) / h);

	auto unprojectNDC = [&](float nz) -> glm::vec3 {
		float wx = ndcX*Minv[0][0] + ndcY*Minv[0][1] + nz*Minv[0][2] + Minv[0][3];
		float wy = ndcX*Minv[1][0] + ndcY*Minv[1][1] + nz*Minv[1][2] + Minv[1][3];
		float wz = ndcX*Minv[2][0] + ndcY*Minv[2][1] + nz*Minv[2][2] + Minv[2][3];
		float ww = ndcX*Minv[3][0] + ndcY*Minv[3][1] + nz*Minv[3][2] + Minv[3][3];
		const float invW = (fabsf(ww) > 1e-8f) ? (1.0f / ww) : 0.0f;
		glm::vec3 v; v.x = wx*invW; v.y = wy*invW; v.z = wz*invW;
		return v;
	};
	auto renderToMc2 = [](const glm::vec3& g) -> glm::vec3 {
		glm::vec3 r; mc2ToRender(r, g); return r;
	};

	// REVERSE-Z, ZERO_TO_ONE: near plane is NDC z=1, far is z=0.
	glm::vec3 ro = renderToMc2(unprojectNDC(1.0f));    // near
	glm::vec3 rf = renderToMc2(unprojectNDC( 0.0f));   // far

	const float dz = rf.z - ro.z;
	if (fabsf(dz) < 1e-6f) { outWorld = ro; return false; }

	// Fixed-point iteration: intersect the ray with the horizontal plane at
	// the terrain height of the previous hit. Converges in 2-3 steps on any
	// terrain a camera ray can see (slope bounded by the tilt).
	float planeZ = 0.0f;
	glm::vec3 hit;
	hit.x = hit.y = hit.z = 0.0f;
	for (int iter = 0; iter < 3; ++iter)
	{
		const float t = (planeZ - ro.z) / dz;
		hit.x = ro.x + t * (rf.x - ro.x);
		hit.y = ro.y + t * (rf.y - ro.y);
		hit.z = planeZ;
		if (!land)
			break;
		const float e = land->getTerrainElevation(hit);
		if (fabsf(e - planeZ) < 1.0f)
		{
			hit.z = e;
			break;
		}
		planeZ = e;
	}
	outWorld = hit;
	return true;
}

//---------------------------------------------------------------------------
unsigned long Camera::inverseProject (Stuff::Vector2DOf<long> &screenPos, glm::vec3 &point)
{
	if (turn < 4)
	{
		point.x = point.y = point.z = 0.0f;
		return 0;
	}

	//-----------------------------------------------------------
	// Phase 7B: heightfield raycast picker — replaces the quadList AABB/
	// screen-triangle scan when MC2_TERRAIN_LOD_CHUNK=1.
	// The quadList may be empty or stale under the LOD chunk path, so we
	// build a world-space ray from the screen pixel via worldToClipGL()^-1
	// and fire it against the full-resolution PostcompVertex heightfield.
	// Object picking (findObjectByMouse) is UNAFFECTED — this replaces only
	// the terrain-surface portion of inverseProject.
	// DECOUPLED FROM CHUNK RENDER: the chunk heightfield-raycast picker's
	// screen->world ray is broken (getPosition() returns the camera GROUND-FOCUS
	// point not the eye, and the worldToClipGL inverse collapses the X response),
	// so every click hit a near-constant cell -> move orders teleported across the
	// map. The legacy forward-projection quadList picker below is correct
	// (projectForSelectionPicking uses the same matrix the GPU renders with), so
	// picking uses IT even when the chunk RENDER path is on (MC2_TERRAIN_LOD_CHUNK).
	// Opt back into the raycast picker with MC2_TERRAIN_RAYCAST_PICK to debug it.
	static const bool s_lodChunkPick = false;
	// PARITY DIAGNOSTIC: when MC2_TERRAIN_PICK_PARITY is set we compute BOTH
	// the heightfield raycast hit AND the legacy quadList result for the same
	// screen coords, log the delta, and still return exactly what the active
	// mode (raycast-if-flag-on-else-legacy) would have returned.
	static const bool s_pickParity = false;
	bool   parityRaycastHit = false;
	glm::vec3 parityRaycastPt;
	parityRaycastPt.x = parityRaycastPt.y = parityRaycastPt.z = 0.0f;
	if (s_lodChunkPick && land)
	{
		// Build the GL clip-to-world inverse matrix.
		glm::mat4 M = worldToClipGL();
		glm::mat4 Minv;
		Minv = glm::inverse(M);

		// Convert screen pixel to GL NDC.
		// screenPos: Y-down from top-left (same convention as screenResolution).
		// GL NDC: x in [-1,+1] left-to-right, y in [-1,+1] bottom-to-top.
		const float w = screenResolution.x;
		const float h = screenResolution.y;
		const float ndcX =  2.0f * (float(screenPos.x) / w) - 1.0f;
		const float ndcY =  1.0f - 2.0f * (float(screenPos.y) / h);

		// Unproject near and far NDC points to world space.
		// Row-vector convention: world_row = clip_row * Minv.
		// Clip near: (ndcX, ndcY, -1, 1); Clip far: (ndcX, ndcY, +1, 1).
		auto unprojectNDC = [&](float nx, float ny, float nz) -> glm::vec3 {
			// clip_row * Minv; w=1
			float wx = nx * Minv[0][0] + ny * Minv[0][1] + nz * Minv[0][2] + Minv[0][3];
			float wy = nx * Minv[1][0] + ny * Minv[1][1] + nz * Minv[1][2] + Minv[1][3];
			float wz = nx * Minv[2][0] + ny * Minv[2][1] + nz * Minv[2][2] + Minv[2][3];
			float ww = nx * Minv[3][0] + ny * Minv[3][1] + nz * Minv[3][2] + Minv[3][3];
			glm::vec3 v;
			const float invW = (fabsf(ww) > 1e-8f) ? (1.0f / ww) : 0.0f;
			v.x = wx * invW;
			v.y = wy * invW;
			v.z = wz * invW;
			return v;
		};

		// REVERSE-Z, ZERO_TO_ONE clip depth (glClipControl + glClearDepth(0)):
		// the NEAR plane is NDC z=1. The FAR plane (z=0) unproject is unreliable
		// here (it came out ABOVE the camera, inverting the ray), so we do NOT use
		// it. Instead build the ray from the known camera eye position (MC2 world)
		// toward the well-conditioned near-plane point.
		glm::vec3 nearPt = unprojectNDC(ndcX, ndcY, 1.0f);

		// worldToClipGL uses the GL axis swap (x'=-x, y'=z, z'=y); invert it:
		//   mc2X = -glX,  mc2Y = glZ,  mc2Z = glY
		auto renderToMc2 = [](const glm::vec3& g) -> glm::vec3 {
			glm::vec3 r;
			mc2ToRender(r, g);
			return r;
		};
		glm::vec3 nearMc = renderToMc2(nearPt);

		// Origin = camera eye (the matrix the GPU renders with is built from this);
		// direction = eye -> near-plane point (on the pixel ray, in front of eye).
		glm::vec3 ro = getPosition();
		float rdx = nearMc.x - ro.x;
		float rdy = nearMc.y - ro.y;
		float rdz = nearMc.z - ro.z;
		const float rlen = sqrtf(rdx*rdx + rdy*rdy + rdz*rdz);
		if (rlen > 1e-6f)
		{
			const float inv = 1.0f / rlen;
			rdx *= inv; rdy *= inv; rdz *= inv;
		}

		float hitX, hitY, hitZ;
		if (Terrain::raycastTerrain(ro.x, ro.y, ro.z, rdx, rdy, rdz,
		                            &hitX, &hitY, &hitZ))
		{
			if (s_pickParity)
			{
				// Record the raycast hit but DO NOT return yet: let the legacy
				// quadList path below run too so we can log both and the delta.
				parityRaycastHit = true;
				parityRaycastPt.x = hitX;
				parityRaycastPt.y = hitY;
				parityRaycastPt.z = hitZ;
			}
			else
			{
				point.x = hitX;
				point.y = hitY;
				point.z = hitZ;
				return 0;
			}
		}
		// Miss — fall through to the quadList path (which may also miss and
		// return 1). On an empty quadList the old path will iterate zero tiles
		// and hit the off-map fallback, which returns 1 (acceptable miss).
	}

	//-----------------------------------------------------------
	// VPL-retirement Step 3 (3a): recursion-free tile selection.
	// Replaces the old per-frame VPL tile walk (clipInfo admission +
	// overThisTile screen test reading stale vertices[]->px/py + closest-pz
	// selection). Two stages, both pure CPU, no GPU readback:
	//   (1) frustum-AABB admission over all quads via the shared helper
	//       (extractFrustumPlanes + quadAabbInFrustum on RAW world AABBs),
	//   (2) exact forward-projection screen-containment + nearest-screen
	//       refinement (ported overThisTile screen-triangle test on FRESHLY
	//       projected corners, NOT stale per-frame px/py).
	// This path never calls inverseProjectForPicking / inverseProjectZ, so
	// the !usePerspective recursion (inverseProjectZ) is structurally
	// unreachable from picking regardless of CTRL+ALT+P toggle state.
	static const bool s_vplPickTrace = false;
	{
		static bool s_announced = false;
		if (s_vplPickTrace && !s_announced)
		{
			s_announced = true;
			mc2::fmt::print_out("[VPL_PICK v1] event=repoint_active path=frustum_aabb+forward_proj_refine\n");
		}
	}

	TerrainQuadPtr currentTile = land->getQuadList();
	unsigned long numTiles = land->getNumQuads();
	TerrainQuadPtr closestTiles[100];
	memset(closestTiles,0,sizeof(TerrainQuadPtr)*100);

	TerrainQuadPtr closestTile = nullptr;
	long currentClosest = 0;
	VertexPtr closestVertex = nullptr;

	float planes[6][4];
	extractFrustumPlanes(planes);

	// Optional env-gated build-free self-test of the matrix/swizzle math:
	// for sampled quads, any RAW world point whose forward projection lands
	// strictly inside the viewport rect with screen.z in [0,1) MUST pass the
	// degenerate-AABB frustum test (helper may be true when projection is
	// just outside - acceptable; helper must NEVER be false when projection
	// is strictly inside).
	if (s_vplPickTrace && numTiles > 0)
	{
		const long sampleStride = (numTiles > 64) ? static_cast<long>((numTiles / 64)) : 1;
		TerrainQuadPtr st = land->getQuadList();
		long checked = 0;
		for (long i = 0; i < static_cast<long>(numTiles); i += sampleStride)
		{
			TerrainQuadPtr q = st + i;
			glm::vec3 wp;
			wp.x = q->vertices[0]->vx;
			wp.y = q->vertices[0]->vy;
			wp.z = q->vertices[0]->pVertex->elevation;

			Stuff::Vector4D sc;
			eye->projectForSelectionPicking(wp, sc);
			const bool strictlyInside =
				(sc.x > 0.0f) && (sc.y > 0.0f) &&
				(sc.x < screenResolution.x) && (sc.y < screenResolution.y) &&
				(sc.z >= 0.0f) && (sc.z < 1.0f);
			if (strictlyInside)
			{
				const bool inFr = quadAabbInFrustum(planes, wp, wp);
				gosASSERT(inFr); // conservative helper: never false when proj inside
			}
			if (++checked >= 64)
				break;
		}
	}

	//-----------------------------------------------------------
	// [LOW-CAMERA-PICK-RAY-1 v2] Two root causes make the legacy picker mis-land
	// at LOW/grazing pitch (greybeard + adversarial recon, both in the EXISTING
	// forward-projection picker — NOT a raycast):
	//   (A) the 100-tile cap (closestTiles[100]) fills in MAP ORDER, so at low
	//       pitch the frustum admits >>100 tiles and the true tile under the
	//       cursor is dropped before it is ever projected; and
	//   (B) the Stage-2 tiebreak is DEPTH-BLIND (corners[].z=0; picks the tile
	//       whose nearest projected corner is closest in SCREEN space), so among
	//       the many tiles a grazing ray pierces it readily picks a FAR one.
	// Fix = single pass (no cap) + pick the FRONTMOST tile that CONTAINS the
	// cursor (min eye-distance), reusing the same projection + containment test.
	// Gated MC2_LOWCAM_PICK (default ON; =0 -> exact legacy two-stage below).
	static const bool s_lowCamPick = true;
	static const bool s_capTrace = false;

	if (s_lowCamPick)
	{
		long admitted = 0, contained = 0;
		double bestEyeDistSq = 1.0e30;
		TerrainQuadPtr t = land->getQuadList();
		for (long i = 0; i < static_cast<long>(numTiles); i++, t++)
		{
			glm::vec3 mn, mx;
			mn.x = mx.x = t->vertices[0]->vx;
			mn.y = mx.y = t->vertices[0]->vy;
			mn.z = mx.z = t->vertices[0]->pVertex->elevation;
			for (int c = 1; c < 4; c++)
			{
				const float vx = t->vertices[c]->vx;
				const float vy = t->vertices[c]->vy;
				const float vz = t->vertices[c]->pVertex->elevation;
				mn.x = std::min(mn.x, vx); mx.x = std::max(mx.x, vx);
				mn.y = std::min(mn.y, vy); mx.y = std::max(mx.y, vy);
				mn.z = std::min(mn.z, vz); mx.z = std::max(mx.z, vz);
			}
			if (!quadAabbInFrustum(planes, mn, mx))
				continue;
			admitted++;

			glm::vec3 corners[4];
			bool valid[4];
			float cx = 0.0f, cy = 0.0f, cz = 0.0f;   // world tile center
			for (int c = 0; c < 4; c++)
			{
				glm::vec3 wp;
				wp.x = t->vertices[c]->vx;
				wp.y = t->vertices[c]->vy;
				wp.z = t->vertices[c]->pVertex->elevation;
				cx += wp.x; cy += wp.y; cz += wp.z;
				Stuff::Vector4D sc;
				eye->projectForSelectionPicking(wp, sc);
				corners[c].x = sc.x;
				corners[c].y = sc.y;
				corners[c].z = 0.0f;
				valid[c] = (sc.x >= 0.0f) && (sc.y >= 0.0f) &&
				           (sc.x <= screenResolution.x) &&
				           (sc.y <= screenResolution.y) &&
				           (sc.z >= 0.0f) && (sc.z < 1.0f);
			}
			if (!s_overThisTileProjected(corners, valid, screenPos.x, screenPos.y))
				continue;
			contained++;
			// Frontmost = smallest eye distance among containing tiles. physicalPos
			// is the true camera eye (MC2 world); getPosition() is the ground-focus.
			cx *= 0.25f; cy *= 0.25f; cz *= 0.25f;
			const double ex = static_cast<double>(cx) - static_cast<double>(physicalPos.x);
			const double ey = static_cast<double>(cy) - static_cast<double>(physicalPos.y);
			const double ez = static_cast<double>(cz) - static_cast<double>(physicalPos.z);
			const double eyeDistSq = ex*ex + ey*ey + ez*ez;
			if (eyeDistSq < bestEyeDistSq)
			{
				bestEyeDistSq = eyeDistSq;
				closestTile = t;
			}
		}
		if (s_capTrace)
			mc2::fmt::print_err("[PICK_CAP] mode=lowcam numTiles={:d} admitted={:d} contained={:d} cursor=({:d},{:d}) hit={:d}\n", mc2::fmt::as_u32(numTiles), mc2::fmt::as_i32(admitted), mc2::fmt::as_i32(contained), mc2::fmt::as_i32(static_cast<long>(screenPos.x)), mc2::fmt::as_i32(static_cast<long>(screenPos.y)), mc2::fmt::as_i32(closestTile ? 1 : 0));
	}
	else
	{
	//-----------------------------------------------------------
	// Stage 1: frustum-AABB admission over all quads. RAW world AABB built
	// from vertices[0..3]->vx/.vy/->pVertex->elevation (unrotated world
	// coords per vertex.h:75). Keep the existing cap-100 + guard.
	for (long i=0;i<static_cast<long>(numTiles);i++)
	{
		glm::vec3 mn, mx;
		mn.x = mx.x = currentTile->vertices[0]->vx;
		mn.y = mx.y = currentTile->vertices[0]->vy;
		mn.z = mx.z = currentTile->vertices[0]->pVertex->elevation;
		for (int c = 1; c < 4; c++)
		{
			const float vx = currentTile->vertices[c]->vx;
			const float vy = currentTile->vertices[c]->vy;
			const float vz = currentTile->vertices[c]->pVertex->elevation;
			mn.x = std::min(mn.x, vx); mx.x = std::max(mx.x, vx);
			mn.y = std::min(mn.y, vy); mx.y = std::max(mx.y, vy);
			mn.z = std::min(mn.z, vz); mx.z = std::max(mx.z, vz);
		}

		if (quadAabbInFrustum(planes, mn, mx) && (currentClosest < 100))
		{
			closestTiles[currentClosest] = currentTile;
			currentClosest++;
		}

		currentTile++;
	}

	//-----------------------------------------------------------
	// Stage 2: exact forward-projection screen-containment + nearest-screen
	// refinement. Forward-project each admitted tile's 4 corners FRESH via
	// projectForSelectionPicking, run the ported screen-triangle test, and
	// keep the tile whose nearest projected corner is closest to the cursor
	// in screen space (same nearest-screen-distance tiebreak semantics as
	// the old pz tiebreak, computed from live projection). On cap-100
	// saturation this ordering keeps the nearest-screen tile, not first-100.
	{
		double bestDistSq = 1.0e30;
		for (long i = 0; i < currentClosest; i++)
		{
			TerrainQuadPtr t = closestTiles[i];
			glm::vec3 corners[4];
			bool valid[4];
			double tileNearestSq = 1.0e30;
			for (int c = 0; c < 4; c++)
			{
				glm::vec3 wp;
				wp.x = t->vertices[c]->vx;
				wp.y = t->vertices[c]->vy;
				wp.z = t->vertices[c]->pVertex->elevation;

				Stuff::Vector4D sc;
				eye->projectForSelectionPicking(wp, sc);
				corners[c].x = sc.x;
				corners[c].y = sc.y;
				corners[c].z = 0.0f;
				valid[c] = (sc.x >= 0.0f) && (sc.y >= 0.0f) &&
				           (sc.x <= screenResolution.x) &&
				           (sc.y <= screenResolution.y) &&
				           (sc.z >= 0.0f) && (sc.z < 1.0f);

				const double dx = static_cast<double>(sc.x) - static_cast<double>(screenPos.x);
				const double dy = static_cast<double>(sc.y) - static_cast<double>(screenPos.y);
				const double dsq = dx * dx + dy * dy;
				tileNearestSq = std::min(tileNearestSq, dsq);
			}

			if (s_overThisTileProjected(corners, valid, screenPos.x, screenPos.y))
			{
				if (tileNearestSq < bestDistSq)
				{
					bestDistSq = tileNearestSq;
					closestTile = t;
				}
			}
		}
	}
	}

	if (closestTile)
	{
#ifdef _DEBUG
		closestTile->selected = TRUE;
		closestTile->vertices[0]->selected = TRUE;
#endif

		//-----------------------------------------------------------------------------
		// Once we have the tile, use the vertex[0] to find out which cell we are in.
		closestVertex = closestTile->vertices[0];

		//-------------------------------------------------------------------
		// Really new method.  Snap to center of closest Cell.  This is ideal
		// because movement should always be to this location and Heidi and Mike need
		// cell resolution in the Editor!
		Stuff::Vector4D cellCenter;

		long cellCenterC = -1;
		long cellCenterR = -1;
		float cellWidth = Terrain::worldUnitsPerCell;
		float halfCellWidth = cellWidth / 2.0f;
		long tileC = -1, tileR = -1;
		long VerticesMapSideDivTwo = Terrain::realVerticesMapSide / 2;
		long MetersMapSideDivTwo = VerticesMapSideDivTwo * float2long(Terrain::worldUnitsPerVertex);
		
		long dx,dy,cd=1<<30,dist,tvx=screenPos.x,tvy=screenPos.y;
		for (long cellC=0;cellC<3;cellC++)
		{
			for (long cellR=0;cellR<3;cellR++)
			{
				point.x = closestVertex->vx;
				point.y = closestVertex->vy;
				point.z = closestVertex->pVertex->elevation;

				point.x += (cellC) * cellWidth + halfCellWidth;
				point.y -= (cellR) * cellWidth + halfCellWidth;
				point.z = land->getTerrainElevation(point);
				// [PROJECTZ:SelectionPicking id=picking_closest_cell_center]
				PROJECTZ_SITE("picking_closest_cell_center", "SelectionPicking");
				eye->projectForSelectionPicking(point,cellCenter);

				dx = (tvx - float2long(cellCenter.x));
				dy = (tvy - float2long(cellCenter.y));
				dist = dx * dx + dy * dy;
				if (dist < cd)
				{
					cd = dist;
					cellCenterC = cellC;
					cellCenterR = cellR;

					tileC = float2long(point.x * Terrain::oneOverWorldUnitsPerVertex + VerticesMapSideDivTwo);
					tileR = float2long((MetersMapSideDivTwo - point.y) * Terrain::oneOverWorldUnitsPerVertex);
				}
			}
		}

		tileR = std::max<long>(tileR, 0);

		if (tileR >= Terrain::realVerticesMapSide)
			tileR = Terrain::realVerticesMapSide-1;

		tileC = std::max<long>(tileC, 0);

		if (tileC >= Terrain::realVerticesMapSide)
			tileC = Terrain::realVerticesMapSide-1;

		mapTileCellToWorldPos (tileR,tileC,cellCenterR,cellCenterC,point);
		point.z = land->getTerrainElevation(point);
	}
	else
	{
	
		// not in any tile, must be off map.  We're going to return the coord of the
		// closest vertex
		float dis = 999999999.f;
		int closeRow = 0, closeCol = 0;
		glm::vec3 tmpWorld;
		Stuff::Vector4D tmpScreen;
		
		/*for ( int column = 0; column < land->realVerticesMapSide; column += land->realVerticesMapSide - 1 )
		{
			for ( int row = 0; row < land->realVerticesMapSide; ++row )
			{
				tmpWorld.x = land->tileColToWorldCoord[column];
				tmpWorld.y = land->tileRowToWorldCoord[row];
				tmpWorld.z = land->getTerrainElevation( row, column );

				projectZ( tmpWorld, tmpScreen );
				
				float tmpDis = (tmpScreen.x - screenPos.x) * (tmpScreen.x - screenPos.x ) + (tmpScreen.y - screenPos.y) * (tmpScreen.y - screenPos.y );
				if ( tmpDis < dis )
				{
					closeRow = row;
					closeCol = column;
					dis = tmpDis;
				}
			}
		}*/

		// MISSION-INTERFACE-PERF-1: this "off map" fallback is the PRODUCTION
		// ground picker whenever the quadList is empty (LOD-chunk terrain path:
		// numTiles=0 -> closestTile never set -> every cache-miss cursor
		// unproject lands here). The legacy loop below brute-force-projects ALL
		// realVerticesMapSide^2 map vertices (getTerrainElevation +
		// projectForSelectionPicking each) = ~890us/frame on mc2_24, 85-95% of
		// the GameLogic.Mission.Interface zone ([IFACE_PERF v1] 2026-07-01).
		//
		const long side = land->realVerticesMapSide;
		auto sampleVertex = [&](long row, long col, float& bestDis,
		                        int& bestRow, int& bestCol) {
			tmpWorld.x = land->tileColToWorldCoord[col];
			tmpWorld.y = land->tileRowToWorldCoord[row];
			tmpWorld.z = land->getTerrainElevation( row, col );

			// [PROJECTZ:SelectionPicking id=picking_closest_vertex_fallback]
			PROJECTZ_SITE("picking_closest_vertex_fallback", "SelectionPicking");
			projectForSelectionPicking( tmpWorld, tmpScreen );

			float tmpDis = (tmpScreen.x - screenPos.x) * (tmpScreen.x - screenPos.x ) + (tmpScreen.y - screenPos.y) * (tmpScreen.y - screenPos.y );
			if ( tmpDis < bestDis )
			{
				bestRow = static_cast<int>(row);
				bestCol = static_cast<int>(col);
				bestDis = tmpDis;
			}
		};

		{
			for ( long column = 0; column < side; column ++ )
			{
				for ( long row = 0; row < side; row++/*row += land->realVerticesMapSide - 1*/ )
				{
					sampleVertex(row, column, dis, closeRow, closeCol);
				}
			}
		}

		point.x = land->tileColToWorldCoord[closeCol];
		point.y = land->tileRowToWorldCoord[closeRow];
		point.z = land->getTerrainElevation( closeRow, closeCol );

		if (s_pickParity && s_lodChunkPick)
			return parityReport(screenPos, parityRaycastHit, parityRaycastPt, point, 1);

		return 1; // don't return success for bogus value
	}

	if (s_pickParity && s_lodChunkPick)
		return parityReport(screenPos, parityRaycastHit, parityRaycastPt, point, 0);

	return(0);
}
