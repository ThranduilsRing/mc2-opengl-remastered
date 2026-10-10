// Terrain: terrain ray-triangle test and Terrain::raycastTerrain.
// Declared in the class headers; shared names in terrain_internal.h.
#include "terrain_internal.h"


//---------------------------------------------------------------------------
// Phase 7B: heightfield DDA raycast.
// Casts a ray in MC2 world space against the full-resolution PostcompVertex
// heightfield. Active only when MC2_TERRAIN_LOD_CHUNK=1 (caller-gated).
//
// Coordinate system (MC2 world):
//   x = east,  y = north,  z = up.
//   Vertex (vx, vy) in grid:
//     worldX = vx * 128.0 - halfMap
//     worldY = halfMap - vy * 128.0
//
// Heightfield is realVerticesMapSide x realVerticesMapSide vertices,
// laid out as blocks[vx + vy * realVerticesMapSide].elevation.
// Cell (cx,cy) has corners at vertices (cx,cy), (cx+1,cy), (cx,cy+1), (cx+1,cy+1).
// Diagonal split matches the terrain quad uvMode checkerboard:
//   uvMode = BOTTOMRIGHT when (cx & 1) == (cy & 1), else BOTTOMLEFT.
//   BOTTOMRIGHT: tri0 = (TL,TR,BR), tri1 = (TL,BR,BL)  [diagonal TL->BR]
//   BOTTOMLEFT:  tri0 = (TL,TR,BL), tri1 = (TR,BR,BL)  [diagonal TR->BL]
//
// Algorithm: 2D DDA to step from cell to cell along the ray's XY projection.
// For each visited cell, test the ray against both triangles using
// Moller-Trumbore. First positive-t hit wins.
//
static bool s_rayTriangle(
    float ox, float oy, float oz,
    float dx, float dy, float dz,
    float ax, float ay, float az,
    float bx, float by, float bz,
    float cx, float cy, float cz,
    float* tOut)
{
    float ex1 = bx - ax, ey1 = by - ay, ez1 = bz - az;
    float ex2 = cx - ax, ey2 = cy - ay, ez2 = cz - az;
    float hx = dy * ez2 - dz * ey2;
    float hy = dz * ex2 - dx * ez2;
    float hz = dx * ey2 - dy * ex2;
    float a = ex1 * hx + ey1 * hy + ez1 * hz;
    if (fabsf(a) < 1e-6f) return false;
    float f = 1.0f / a;
    float sx = ox - ax, sy = oy - ay, sz = oz - az;
    float u = f * (sx * hx + sy * hy + sz * hz);
    if (u < 0.0f || u > 1.0f) return false;
    float qx = sy * ez1 - sz * ey1;
    float qy = sz * ex1 - sx * ez1;
    float qz = sx * ey1 - sy * ex1;
    float v = f * (dx * qx + dy * qy + dz * qz);
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = f * (ex2 * qx + ey2 * qy + ez2 * qz);
    if (t < 1e-4f) return false;
    *tOut = t;
    return true;
}

bool Terrain::raycastTerrain(
    float ox, float oy, float oz,
    float dx, float dy, float dz,
    float* outX, float* outY, float* outZ)
{
    // Guard: heightfield must exist.
    if (!Terrain::mapData || !Terrain::mapData->getBlocks())
        return false;

    const int N = Terrain::realVerticesMapSide; // vertex grid dimension
    if (N < 2) return false;
    const int Nq = N - 1;                        // quad/cell grid dimension
    const float VS = Terrain::worldUnitsPerVertex; // 128.0f
    const float halfMap = VS * 0.5f * float(N - 1); // = worldUnitsMapSide * 0.5

    // Helper: get elevation at vertex grid coords (vx, vy), clamped.
    // Note: terrainElevation(indexY, indexX) — Y is row param order!
    auto getElev = [&](int vx, int vy) -> float {
        vx = (vx < 0) ? 0 : (vx >= N ? N - 1 : vx);
        vy = (vy < 0) ? 0 : (vy >= N ? N - 1 : vy);
        return Terrain::mapData->getBlocks()[vx + vy * N].elevation;
    };

    // Helper: world (x,y) -> vertex grid float coords.
    //   worldX = vx * VS - halfMap  =>  vx = (worldX + halfMap) / VS
    //   worldY = halfMap - vy * VS  =>  vy = (halfMap - worldY) / VS
    auto worldToVF = [&](float wx, float wy, float& vfx, float& vfy) {
        vfx = (wx + halfMap) / VS;
        vfy = (halfMap - wy) / VS;
    };

    // Normalize ray direction in XY for DDA.
    float lenXY = sqrtf(dx * dx + dy * dy);
    if (lenXY < 1e-6f)
    {
        // Ray is nearly vertical — test the single cell under origin.
        float vfx, vfy;
        worldToVF(ox, oy, vfx, vfy);
        int cx = (int)floorf(vfx);
        int cy = (int)floorf(vfy);
        cx = (cx < 0) ? 0 : (cx >= Nq ? Nq - 1 : cx);
        cy = (cy < 0) ? 0 : (cy >= Nq ? Nq - 1 : cy);
        // TL, TR, BR, BL world positions
        float tlx = float(cx)     * VS - halfMap, tly = halfMap - float(cy)     * VS;
        float trx = float(cx + 1) * VS - halfMap, tby = halfMap - float(cy + 1) * VS;
        float h00 = getElev(cx,     cy);
        float h10 = getElev(cx + 1, cy);
        float h01 = getElev(cx,     cy + 1);
        float h11 = getElev(cx + 1, cy + 1);
        float t;
        bool uvBR = ((cx & 1) == (cy & 1));
        bool hit = false;
        if (uvBR) {
            hit = s_rayTriangle(ox,oy,oz, dx,dy,dz, tlx,tly,h00, trx,tly,h10, trx,tby,h11, &t) ||
                  s_rayTriangle(ox,oy,oz, dx,dy,dz, tlx,tly,h00, trx,tby,h11, tlx,tby,h01, &t);
        } else {
            hit = s_rayTriangle(ox,oy,oz, dx,dy,dz, tlx,tly,h00, trx,tly,h10, tlx,tby,h01, &t) ||
                  s_rayTriangle(ox,oy,oz, dx,dy,dz, trx,tly,h10, trx,tby,h11, tlx,tby,h01, &t);
        }
        if (hit) {
            *outX = ox + t * dx;
            *outY = oy + t * dy;
            *outZ = oz + t * dz;
            return true;
        }
        return false;
    }

    // Compute start cell from ray origin. Clamp to map.
    float vfx0, vfy0;
    worldToVF(ox, oy, vfx0, vfy0);
    int cx = (int)floorf(vfx0);
    int cy = (int)floorf(vfy0);

    // DDA setup: step direction and per-axis tDelta.
    // stepX/Y: +1 or -1 depending on ray direction in vertex-grid space.
    // In vertex grid: vfx increases with worldX, vfy increases as worldY decreases.
    // So dVfx/dt = dx / VS  (unchanged sign)
    // And dVfy/dt = -dy / VS (Y flipped)
    float dVx = dx / VS;   // rate of change of vfx per unit t along ray
    float dVy = -dy / VS;  // rate of change of vfy per unit t along ray

    int stepX = (dVx >= 0.0f) ? 1 : -1;
    int stepY = (dVy >= 0.0f) ? 1 : -1;

    // tDelta: t distance to traverse one full cell in X or Y
    float tDeltaX = (fabsf(dVx) > 1e-9f) ? fabsf(1.0f / dVx) : 1e30f;
    float tDeltaY = (fabsf(dVy) > 1e-9f) ? fabsf(1.0f / dVy) : 1e30f;

    // tMaxX/Y: t at which the ray first crosses a cell boundary in X or Y
    float fracX = (stepX > 0) ? (1.0f - (vfx0 - floorf(vfx0))) : (vfx0 - floorf(vfx0));
    float fracY = (stepY > 0) ? (1.0f - (vfy0 - floorf(vfy0))) : (vfy0 - floorf(vfy0));
    float tMaxX = fracX * tDeltaX;
    float tMaxY = fracY * tDeltaY;

    // Maximum steps: traverse the full diagonal of the map with some margin.
    const int maxSteps = N * 3;
    float bestT = 1e30f;
    bool  found = false;

    for (int step = 0; step < maxSteps; ++step)
    {
        // Test current cell if in bounds.
        if (cx >= 0 && cx < Nq && cy >= 0 && cy < Nq)
        {
            // World-space corners of this cell.
            float tlx = float(cx)     * VS - halfMap;
            float tly = halfMap - float(cy)     * VS;   // top-left world Y (north)
            float trx = float(cx + 1) * VS - halfMap;
            float tby = halfMap - float(cy + 1) * VS;   // bottom-right world Y (south)

            float h00 = getElev(cx,     cy);     // TL elevation
            float h10 = getElev(cx + 1, cy);     // TR elevation
            float h01 = getElev(cx,     cy + 1); // BL elevation
            float h11 = getElev(cx + 1, cy + 1); // BR elevation

            // uvMode checkerboard: BOTTOMRIGHT when (cx & 1) == (cy & 1)
            bool uvBR = ((cx & 1) == (cy & 1));
            float t0, t1;
            bool h0, h1;
            if (uvBR)
            {
                // Diagonal TL->BR: tri0=TL,TR,BR  tri1=TL,BR,BL
                h0 = s_rayTriangle(ox,oy,oz, dx,dy,dz,
                                   tlx,tly,h00, trx,tly,h10, trx,tby,h11, &t0);
                h1 = s_rayTriangle(ox,oy,oz, dx,dy,dz,
                                   tlx,tly,h00, trx,tby,h11, tlx,tby,h01, &t1);
            }
            else
            {
                // Diagonal TR->BL: tri0=TL,TR,BL  tri1=TR,BR,BL
                h0 = s_rayTriangle(ox,oy,oz, dx,dy,dz,
                                   tlx,tly,h00, trx,tly,h10, tlx,tby,h01, &t0);
                h1 = s_rayTriangle(ox,oy,oz, dx,dy,dz,
                                   trx,tly,h10, trx,tby,h11, tlx,tby,h01, &t1);
            }

            float cellBestT = 1e30f;
            if (h0 && t0 < cellBestT) cellBestT = t0;
            if (h1 && t1 < cellBestT) cellBestT = t1;

            if (cellBestT < bestT)
            {
                bestT = cellBestT;
                found = true;
                // Don't break immediately: the DDA may visit cells
                // slightly out of order near diagonal crossings.
                // Accept the hit and stop — first hit in DDA order is nearest.
                break;
            }
        }
        else if (step > 0)
        {
            // Once we've left the valid map, stop.
            break;
        }

        // Advance to next cell.
        if (tMaxX < tMaxY)
        {
            tMaxX += tDeltaX;
            cx    += stepX;
        }
        else
        {
            tMaxY += tDeltaY;
            cy    += stepY;
        }
    }

    if (found)
    {
        *outX = ox + bestT * dx;
        *outY = oy + bestT * dy;
        *outZ = oz + bestT * dz;
        return true;
    }
    return false;
}

//---------------------------------------------------------------------------
bool Terrain::IsValidTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f);

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
bool Terrain::IsEditorSelectTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f) - Terrain::worldUnitsPerVertex;

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}

//---------------------------------------------------------------------------
bool Terrain::IsGameSelectTerrainPosition (const Stuff::Vector3D pos)
{
	float metersCheck = (Terrain::worldUnitsMapSide / 2.0f) - (Terrain::worldUnitsPerVertex * 2.0f);

	if ((pos.x > -metersCheck) &&
		(pos.x < metersCheck) &&
		(pos.y > -metersCheck) &&
		(pos.y < metersCheck))
	{
		return true;
	}

	return false;
}
