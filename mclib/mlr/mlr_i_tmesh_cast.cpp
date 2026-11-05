// MLR_I_TMesh: the CreateIndexedTri* test shapes (CastRay, which had no caller, went at step 5 S5.11).
// Declared in the class headers; shared names in mlr_i_tmesh_internal.h.
#include "mlr_i_tmesh_internal.h"
#include <array>
#include <memory>


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLR_I_TMesh*
	MidLevelRenderer::CreateIndexedTriCube_NoColor_NoLit(
		Scalar half, 
		MLRState *state
	)
{
	return nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRShape*
	MidLevelRenderer::CreateIndexedTriIcosahedron_NoColor_NoLit(
		IcoInfo& icoInfo,
		MLRState *state
	)
{
	gos_PushCurrentHeap(Heap);
	MLRShape *ret = new MLRShape(20);
	Register_Object(ret);

	int i, j, k;
	long    nrTri = static_cast<long>(ceil (icoInfo.all * pow (4.0f, icoInfo.depth)));
	std::array<glm::vec3, 3> v;

	if(3*nrTri >= Limits::Max_Number_Vertices_Per_Mesh)
	{
		nrTri = Limits::Max_Number_Vertices_Per_Mesh/3;
	}

	auto coordsOwner = std::make_unique_for_overwrite<glm::vec3[]>(nrTri*3);
	glm::vec3* coords = coordsOwner.get();
	Register_Pointer(coords);
	
	std::unique_ptr<glm::vec3[]> collapsedCoordsOwner;
	glm::vec3 *collapsedCoords = nullptr;
	if(icoInfo.indexed==true)
	{
		collapsedCoordsOwner = std::make_unique_for_overwrite<glm::vec3[]>(nrTri*3);
		collapsedCoords = collapsedCoordsOwner.get();
		Register_Pointer(collapsedCoords);
	}

	auto indexOwner = std::make_unique_for_overwrite<unsigned short[]>(nrTri*3);
	unsigned short* index = indexOwner.get();
	Register_Pointer(index);
	auto texCoordsOwner = std::make_unique_for_overwrite<Vector2DScalar[]>(nrTri*3);
	Vector2DScalar* texCoords = texCoordsOwner.get();
	Register_Pointer(texCoords);

	int uniquePoints = 0;
	for (k=0;k<20;k++)
	{
		triDrawn = 0;
		MLR_I_TMesh *mesh = new MLR_I_TMesh();
		Register_Object(mesh);

// setup vertex position information
	    for (j=0;j<3;j++)
		{
			v[j].x = vdata[tindices[k][j]][0];
			v[j].y = vdata[tindices[k][j]][1];
			v[j].z = vdata[tindices[k][j]][2];
		}
		subdivide (coords, v[0], v[1], v[2], icoInfo.depth, nrTri, icoInfo.radius);

		mesh->SetSubprimitiveLengths(nullptr, nrTri);

		if(icoInfo.indexed==true)
		{
			uniquePoints = 1;
			collapsedCoords[0] = coords[0];
			index[0] = 0;

			for(i=1;i<nrTri*3;i++)
			{
				for(j=0;j<uniquePoints;j++)
				{
					if(mc2::math::closeEnough(coords[i], collapsedCoords[j]))
					{
						break;
					}
				}
				if(j==uniquePoints)
				{
					collapsedCoords[uniquePoints++] = coords[i];
				}
				index[i] = static_cast<unsigned short>(j);
			}
			mesh->SetCoordData(collapsedCoords, uniquePoints);
		}
		else
		{
			uniquePoints = nrTri*3;
			for(i=0;i<nrTri*3;i++)
			{
				index[i] = static_cast<unsigned short>(i);
			}
			mesh->SetCoordData(coords, nrTri*3);
		}

		mesh->SetIndexData(index, nrTri*3);

		mesh->FindFacePlanes();

		if(state == nullptr)
		{
			for(i=0;i<uniquePoints;i++)
			{
				texCoords[i] = Vector2DScalar(0.0f, 0.0f);
			}
		}
		else
		{
			mesh->SetReferenceState(*state);
			if(state->GetTextureHandle() > 0)
			{
				if(icoInfo.indexed==true)
				{
					for(i=0;i<uniquePoints;i++)
					{
						texCoords[i] = 
							Vector2DScalar(
								(1.0f + collapsedCoords[i].x)/2.0f,
								(1.0f + collapsedCoords[i].y)/2.0f
							);
					}
				}
				else
				{
					for(i=0;i<nrTri;i++)
					{
						texCoords[3*i] = 
							Vector2DScalar(
								(1.0f + coords[3*i].x)/2.0f,
								(1.0f + coords[3*i].y)/2.0f
							);
						texCoords[3*i+1] = 
							Vector2DScalar(
								(1.0f + coords[3*i+1].x)/2.0f,
								(1.0f + coords[3*i+1].y)/2.0f
							);
						texCoords[3*i+2] = 
							Vector2DScalar(
								(1.0f + coords[3*i+2].x)/2.0f,
								(1.0f + coords[3*i+2].y)/2.0f
							);
					}
				}
			}
			else
			{
				for(i=0;i<uniquePoints;i++)
				{
					texCoords[i] = Vector2DScalar(0.0f, 0.0f);
				}
			}
		}
		mesh->SetTexCoordData(texCoords, uniquePoints);

		ret->Add(mesh);
		mesh->DetachReference();
	}

	Unregister_Pointer(texCoords);
	texCoordsOwner.reset();

	Unregister_Pointer(index);
	indexOwner.reset();

	if(icoInfo.indexed==true)
	{
		Unregister_Pointer(collapsedCoords);
		collapsedCoordsOwner.reset();
	}
	
	Unregister_Pointer(coords);
	coordsOwner.reset();

	gos_PopCurrentHeap();
	return ret;
}
