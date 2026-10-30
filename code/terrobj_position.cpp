// Terrain object position and rotation setters.
#include "mclib.h"
#include "terrobj.h"

void TerrainObject::setTerrainPosition(const glm::vec3& position, const Stuff::Vector2DOf<long>& numbers)
{
	setPosition(position);
	vertexNumber = numbers.x;
	blockNumber = numbers.y;
}

void TerrainObject::setRotation(float rot)
{
	rotation = rot;
	(static_cast<ObjectAppearance*>(appearance))->rotation = rot;
}
