// Pure line-of-sight kernel: see los_kernel.h. Every expression below mirrors the legacy code it replaces
// (MapData::terrainElevation, Terrain::worldToCell/getCellPos, Team::lineOfSight); do not "tidy" the float/double mix.
#include <cmath>  // before any other include: which floor/sqrt overload a call binds must not depend on include order
#include "los_kernel.h"
#include "job_system.h"

namespace mc2::los {

namespace {
constexpr float ACCURACY_ADJUST = 1.5f;
const float HALF_CELL_DIST = (128.0f / 6.0f);
constexpr int kBlockQueries = 64;   // fixed 64-record blocks (design 3.2)
constexpr int kCostNsPerQuery = 4000;

inline float vertexElevation(const Snapshot& s, long x, long y)
{
	const unsigned char* p = s.vertexBase + static_cast<size_t>(x + y * s.realVerticesMapSide) * s.vertexStride;
	return *reinterpret_cast<const float*>(p);
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
	return (row > -1) && (row < s.cellHeight) && (col > -1) && (col < s.cellWidth);
}

inline unsigned long cellLocalHeight(const Snapshot& s, long row, long col)
{
	if (!inBounds(s, row, col)) return 0;
	return (s.cells[row * s.cellWidth + col] & kCellHeightMask) >> kCellHeightShift;
}

inline bool cellForest(const Snapshot& s, long row, long col)
{
	if (!inBounds(s, row, col)) return false;
	return (s.cells[row * s.cellWidth + col] & kCellForestMask) ? true : false;
}

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
		startPos.z += startLocal;
		endPos.z += endLocal;

		glm::vec3 deltaCellVec;
		deltaCellVec.y = tCellRow - mCellRow;
		deltaCellVec.x = tCellCol - mCellCol;
		deltaCellVec.z = 0.0f;
		float startHeight = startPos.z;

		float length = glm::length(deltaCellVec);
		length *= ACCURACY_ADJUST;

		if (length > s.small)
		{
			float colLength = (endPos.x - startPos.x) / length;
			float rowLength = (endPos.y - startPos.y) / length;
			float heightLen = (endPos.z - startPos.z) / (length + ACCURACY_ADJUST);

			glm::vec3 currentPos = startPos;
			currentPos.z = terrainElevation(s, currentPos);
			long maxDistIter = (length - 0.5f);
			long maxTrees = 0;

			glm::vec3 dist;
			dist = endPos - currentPos;

			float remainingDist = glm::length(dist);
			bool checkExtent = (extRad > s.small);
			bool checkStart = (startExtRad > s.small);
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
				currentPos.z = terrainElevation(s, currentPos);
			}
		}
	}

	return true;
}
}  // namespace

float terrainElevation(const Snapshot& s, const glm::vec3& position)
{
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

	enum { BOTTOMRIGHT_ = 1, BOTTOMLEFT_ = 2 };
	long uvMode = 0;
	if (yby2)
		uvMode = xby2 ? BOTTOMRIGHT_ : BOTTOMLEFT_;
	else
		uvMode = xby2 ? BOTTOMLEFT_ : BOTTOMRIGHT_;

	float deltaX = 0.0f;
	float deltaY = 0.0f;
	const float wuv = s.worldUnitsPerVertex;

	if (uvMode == BOTTOMRIGHT_)
	{
		deltaX = fabs(position.x - upperLeft.x);
		deltaY = fabs(upperLeft.y - position.y);

		if (deltaX > deltaY)
		{
			triVert[0] = triPos;

			triVert[1].x = triVert[0].x + wuv;
			triVert[1].y = triVert[0].y;
			triVert[1].z = e2;

			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[0].y - wuv;
			triVert[2].z = e3;

			triangleVector[0] = triVert[1] - triVert[0];
			triangleVector[1] = triVert[2] - triVert[0];
		}
		else
		{
			triVert[0] = triPos;

			triVert[1].x = triVert[0].x + wuv;
			triVert[1].y = triVert[0].y - wuv;
			triVert[1].z = e3;

			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = e4;

			triangleVector[0] = triVert[2] - triVert[0];
			triangleVector[1] = triVert[1] - triVert[0];
		}
	}
	else if (uvMode == BOTTOMLEFT_)
	{
		deltaX = fabs((upperLeft.x + wuv) - position.x);
		deltaY = fabs(upperLeft.y - position.y);

		if (deltaX > deltaY)
		{
			triVert[1] = triPos;

			triVert[0].x = triPos.x + wuv;
			triVert[0].y = triPos.y;
			triVert[0].z = e2;

			triVert[2].x = triVert[1].x;
			triVert[2].y = triVert[1].y - wuv;
			triVert[2].z = e4;

			triangleVector[0] = triVert[1] - triVert[0];
			triangleVector[1] = triVert[2] - triVert[0];
		}
		else
		{
			triVert[0].x = triPos.x + wuv;
			triVert[0].y = triPos.y;
			triVert[0].z = e2;

			triVert[1].x = triPos.x;
			triVert[1].y = triVert[0].y - wuv;
			triVert[1].z = e4;

			triVert[2].x = triVert[0].x;
			triVert[2].y = triVert[1].y;
			triVert[2].z = e3;

			triangleVector[0] = triVert[2] - triVert[0];
			triangleVector[1] = triVert[1] - triVert[0];
		}

		deltaX = -deltaX;
	}

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

void batch(const Snapshot& s, const Query* q, uint8_t* out, int n)
{
	mc2::jobs::parallel_for(mc2::jobs::Domain::Sim, n, kBlockQueries, kCostNsPerQuery,
		[&s, q, out](int begin, int end) {
			for (int i = begin; i < end; ++i)
				out[i] = query(s, q[i]) ? 1 : 0;
		});
}

}  // namespace mc2::los
