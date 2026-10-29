// Pure line-of-sight kernel: see los_kernel.h. Every expression below mirrors the legacy code it replaces
// (MapData::terrainElevation, Terrain::worldToCell/getCellPos, Team::lineOfSight); do not "tidy" the float/double mix.
#include <cmath>  // before any other include: which floor/sqrt overload a call binds must not depend on include order
#include "los_kernel.h"
#include "job_system.h"
#include <cstring>

namespace mc2::los {

namespace {
constexpr float ACCURACY_ADJUST = 1.5f;
const float HALF_CELL_DIST = (128.0f / 6.0f);
constexpr int kBlockQueries = 64;   // fixed 64-record blocks (design 3.2)
constexpr int kCostNsPerQuery = 4000;

inline float vertexElevation(const Snapshot& s, long x, long y)
{
	return s.vertices[y, x];
}

inline void worldToCell(const Snapshot& s, const glm::vec3& pos, int& cellR, int& cellC)
{
	cellC = static_cast<int>((pos.x - s.mapTopLeft3d.x) * (s.oneOverWorldUnitsPerVertex * 3.0f));
	cellR = static_cast<int>((s.mapTopLeft3d.y - pos.y) * (s.oneOverWorldUnitsPerVertex * 3.0f));
}

inline void getCellPos(const Snapshot& s, int cellR, int cellC, glm::vec3& cellPos)
{
	cellPos.x = (cellC * (s.worldUnitsPerVertex / 3.)) + (s.worldUnitsPerVertex / 6.);
	cellPos.y = (cellR * (s.worldUnitsPerVertex / 3.)) + (s.worldUnitsPerVertex / 6.);

	cellPos.x += s.mapTopLeft3d.x;
	cellPos.y = s.mapTopLeft3d.y - cellPos.y;

	cellPos.z = terrainElevation(s, cellPos);
}

inline bool inBounds(const Snapshot& s, long row, long col)
{
	return (row > -1) && (row < s.cells.extent(0)) && (col > -1) && (col < s.cells.extent(1));
}

inline unsigned long cellLocalHeight(const Snapshot& s, long row, long col)
{
	if (!inBounds(s, row, col)) return 0;
	return (s.cells[row, col] & kCellHeightMask) >> kCellHeightShift;
}

inline bool cellForest(const Snapshot& s, long row, long col)
{
	if (!inBounds(s, row, col)) return false;
	return (s.cells[row, col] & kCellForestMask) ? true : false;
}

}  // namespace

bool walk(const Snapshot& s, float startLocal, long mCellRow, long mCellCol, float endLocal, long tCellRow, long tCellCol,
          long teamId, float extRad, float startExtRad)
{
	if ((teamId < 0) || (teamId >= s.maxTeams))
		return true;

	if (s.useRealLOS)
	{
		glm::vec3 startPos, endPos;
		startPos = glm::vec3(0.0f);
		endPos = glm::vec3(0.0f);

		getCellPos(s, tCellRow, tCellCol, endPos);
		getCellPos(s, mCellRow, mCellCol, startPos);
		const float startGround = startPos.z;
		startPos.z += startLocal;
		endPos.z += endLocal;

		glm::vec3 deltaCellVec;
		deltaCellVec.y = tCellRow - mCellRow;
		deltaCellVec.x = tCellCol - mCellCol;
		deltaCellVec.z = 0.0f;
		float startHeight = startPos.z;

		float length = glm::length(deltaCellVec);
		length *= ACCURACY_ADJUST;

		if (length > s.smallEps)
		{
			float colLength = (endPos.x - startPos.x) / length;
			float rowLength = (endPos.y - startPos.y) / length;
			float heightLen = (endPos.z - startPos.z) / (length + ACCURACY_ADJUST);

			glm::vec3 currentPos = startPos;
			// The pure elevation path reads x/y only: getCellPos already sampled this exact point.
			currentPos.z = s.elevationOverride ? terrainElevation(s, currentPos) : startGround;
			long maxDistIter = (length - 0.5f);
			long maxTrees = 0;

			glm::vec3 dist;
			dist = endPos - currentPos;

			float remainingDist = glm::length(dist);
			bool checkExtent = (extRad > s.smallEps);
			bool checkStart = (startExtRad > s.smallEps);
			extRad += HALF_CELL_DIST;
			for (long distIter = 0; distIter < maxDistIter; distIter++)
			{
				bool outsideStartRadius = true;
				if (checkStart)
				{
					glm::vec3 distance;
					distance = currentPos - startPos;
					distance.z = 0.0f;
					float dist2 = glm::length(distance);
					if (dist2 <= startExtRad)
						outsideStartRadius = false;
				}

				startHeight += heightLen;

				int curCellRow, curCellCol;
				worldToCell(s, currentPos, curCellRow, curCellCol);

				float localElev = (s.worldUnitsPerMeter * 4.0f * static_cast<float>(cellLocalHeight(s, curCellRow, curCellCol)));
				float thisHeight = currentPos.z + localElev;

				if (checkExtent)
				{
					dist = endPos - currentPos;
					remainingDist = glm::length(dist);
					if (remainingDist <= extRad)
						break;
				}

				if (outsideStartRadius && (startHeight < thisHeight))
				{
					bool isTree = false;
					if (cellForest(s, curCellRow, curCellCol))
					{
						maxTrees++;
						isTree = true;
					}

					if (!isTree || (maxTrees >= s.maxTreeLOSCellBlock))
						return false;
				}

				currentPos.x += colLength;
				currentPos.y += rowLength;
				// The last sample is never read. Preserve override callback calls, whose inputs include z.
				if (s.elevationOverride || distIter + 1 < maxDistIter)
					currentPos.z = terrainElevation(s, currentPos);
			}
		}
	}

	return true;
}

float terrainElevation(const Snapshot& s, const glm::vec3& position)
{
	if (s.elevationOverride)
		return s.elevationOverride(position);

	glm::vec3 triVert[3];
	glm::vec2 upperLeft;
	glm::vec3 triPos;
	glm::vec3 triangleVector[2];
	glm::vec3 perpendicularVec;
	float result = 0.0;

	triVert[0] = glm::vec3(0.0f);
	triVert[1] = glm::vec3(0.0f);
	triVert[2] = glm::vec3(0.0f);

	{
		float metersCheck = (s.worldUnitsMapSide / 2.0f);
		if (!((position.x > -metersCheck) && (position.x < metersCheck) &&
		      (position.y > -metersCheck) && (position.y < metersCheck)))
			return (0.0f);
	}

	upperLeft.x = floor(position.x * s.oneOverWorldUnitsPerVertex);
	upperLeft.x *= s.worldUnitsPerVertex;

	upperLeft.y = floor(position.y * s.oneOverWorldUnitsPerVertex);
	if (float(position.y * s.oneOverWorldUnitsPerVertex) != static_cast<float>(upperLeft.y))
		upperLeft.y += 1.0;
	upperLeft.y *= s.worldUnitsPerVertex;

	glm::vec2 _upperLeft;
	_upperLeft.x = upperLeft.x * float(s.oneOverWorldUnitsPerVertex);
	_upperLeft.y = upperLeft.y * float(s.oneOverWorldUnitsPerVertex);

	long meshOffsetX = static_cast<long>(floor(_upperLeft.x));
	long meshOffsetY = static_cast<long>(floor(_upperLeft.y));

	long verticesMapSide = s.verticesMapSide;

	meshOffsetX += (verticesMapSide >> 1);
	meshOffsetY = (verticesMapSide >> 1) - meshOffsetY;

	if ((meshOffsetX < 0) || (meshOffsetX >= (verticesMapSide - 1)))
		return 0.0f;

	if ((meshOffsetY < 0) || (meshOffsetY >= (verticesMapSide - 1)))
		return 0.0f;

	const float e1 = vertexElevation(s, meshOffsetX,     meshOffsetY);
	const float e2 = vertexElevation(s, meshOffsetX + 1, meshOffsetY);
	const float e3 = vertexElevation(s, meshOffsetX + 1, meshOffsetY + 1);
	const float e4 = vertexElevation(s, meshOffsetX,     meshOffsetY + 1);

	triPos.x = s.worldUnitsPerVertex * floor(_upperLeft.x);
	triPos.y = s.worldUnitsPerVertex * floor(_upperLeft.y);
	triPos.z = e1;

	bool tlx = (long(s.topLeftVertexX) & 1);
	bool tly = (long(s.topLeftVertexY) & 1);

	long x = meshOffsetX - s.topLeftVertexX;
	long y = meshOffsetY - s.topLeftVertexY;

	bool yby2 = (y & 1) ^ (tly);
	bool xby2 = (x & 1) ^ (tlx);

	// The quad is split along one of its two diagonals (bottomRight: from the upper left; else from the upper right).
	const bool bottomRight = yby2 ? xby2 : !xby2;
	const float wuv = s.worldUnitsPerVertex;

	float deltaX = 0.0f;
	float deltaY = 0.0f;
	if (bottomRight)
		deltaX = fabs(position.x - upperLeft.x);
	else
		deltaX = fabs((upperLeft.x + wuv) - position.x);
	deltaY = fabs(upperLeft.y - position.y);
	const bool topTriangle = deltaX > deltaY;

	// Triangle corners as (right, down, vertex) in the legacy order; vertex 0..3 = upper-left, upper-right,
	// lower-right, lower-left. The bottom triangles swap the two edge vectors.
	struct Corner { bool right; bool down; int vertex; };
	const float elev[4] = { e1, e2, e3, e4 };
	Corner corner[3];
	if (bottomRight)
	{
		if (topTriangle)
			corner[0] = { false, false, 0 }, corner[1] = { true, false, 1 }, corner[2] = { true, true, 2 };
		else
			corner[0] = { false, false, 0 }, corner[1] = { true, true, 2 }, corner[2] = { false, true, 3 };
	}
	else
	{
		if (topTriangle)
			corner[0] = { true, false, 1 }, corner[1] = { false, false, 0 }, corner[2] = { false, true, 3 };
		else
			corner[0] = { true, false, 1 }, corner[1] = { false, true, 3 }, corner[2] = { true, true, 2 };
	}
	for (int k = 0; k < 3; ++k)
	{
		triVert[k].x = corner[k].right ? triPos.x + wuv : triPos.x;
		triVert[k].y = corner[k].down ? triPos.y - wuv : triPos.y;
		triVert[k].z = elev[corner[k].vertex];
	}
	if (topTriangle)
	{
		triangleVector[0] = triVert[1] - triVert[0];
		triangleVector[1] = triVert[2] - triVert[0];
	}
	else
	{
		triangleVector[0] = triVert[2] - triVert[0];
		triangleVector[1] = triVert[1] - triVert[0];
	}
	if (!bottomRight)
		deltaX = -deltaX;

	triangleVector[0] = glm::normalize(triangleVector[0]);
	triangleVector[1] = glm::normalize(triangleVector[1]);

	perpendicularVec = glm::cross(triangleVector[0], triangleVector[1]);

	if (perpendicularVec.z != 0L)
	{
		if (perpendicularVec.z < 0L)
			perpendicularVec = -perpendicularVec;

		float perpendX = perpendicularVec.x / perpendicularVec.z;
		float perpendY = perpendicularVec.y / perpendicularVec.z;

		result = (deltaX * perpendX);
		result += ((-deltaY) * perpendY);
		result = -result;
		result += triVert[0].z;
	}

	return result;
}

bool query(const Snapshot& s, const Query& q)
{
	int posCellR, posCellC;
	int tarCellR, tarCellC;
	worldToCell(s, q.position, posCellR, posCellC);
	worldToCell(s, q.target, tarCellR, tarCellC);

	float elev = terrainElevation(s, q.position);
	float localStart = q.position.z - elev;

	elev = terrainElevation(s, q.target);
	float localEnd = q.target.z - elev;

	return walk(s, localStart, posCellR, posCellC, localEnd, tarCellR, tarCellC, q.teamId, q.extRad, q.startExtRad);
}

void batch(const Snapshot& s, std::span<const Query> q, std::span<uint8_t> out)
{
	if (s.elevationOverride)  // reads engine globals: main thread only
	{
		for (size_t i = 0; i < q.size(); ++i)
			out[i] = query(s, q[i]) ? 1 : 0;
		return;
	}
	const auto runBlock = [&s, q, out](int begin, int end) {
			// Sensor rays share an origin. Its terrain inputs remain immutable until
			// this block joins; mixed-origin callers retain the exact query path.
			const glm::vec3& origin = q[begin].position;
			int posCellR, posCellC;
			worldToCell(s, origin, posCellR, posCellC);
			float elev = terrainElevation(s, origin);
			const float localStart = origin.z - elev;
			for (int i = begin; i < end; ++i) {
				// Compare representations so signed zeros cannot share cached math.
				if (std::memcmp(&q[i].position, &origin, sizeof(origin)) != 0) {
					out[i] = query(s, q[i]) ? 1 : 0;
					continue;
				}
				int tarCellR, tarCellC;
				worldToCell(s, q[i].target, tarCellR, tarCellC);
				elev = terrainElevation(s, q[i].target);
				float localEnd = q[i].target.z - elev;
				out[i] = walk(s, localStart, posCellR, posCellC, localEnd, tarCellR, tarCellC,
					q[i].teamId, q[i].extRad, q[i].startExtRad) ? 1 : 0;
			}
		};
	// One block cannot share work. Avoid waking and joining the pool for it.
	if (q.size() <= kBlockQueries) {
		if (!q.empty()) runBlock(0, static_cast<int>(q.size()));
	} else {
		mc2::jobs::parallel_for(mc2::jobs::Domain::Sim, static_cast<int>(q.size()),
			kBlockQueries, kCostNsPerQuery, runBlock);
	}
}

}  // namespace mc2::los
