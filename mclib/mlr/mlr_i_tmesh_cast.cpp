// MLR_I_TMesh: CastRay and the CreateIndexedTri* test shapes.
// Declared in the class headers; shared names in mlr_i_tmesh_internal.h.
#include "mlr_i_tmesh_internal.h"


//---------------------------------------------------------------------------
//
bool
	MLR_I_TMesh::CastRay(
		Line3D *line,
		Normal3D *normal
	)
{
	Check_Object(this);
	Check_Object(line);
	Check_Pointer(normal);

	//
	//---------------------------------------------------------------------
	// We have to spin through each of the polygons stored in the shape and
	// collide the ray against each
	//---------------------------------------------------------------------
	//
	int poly_start = 0;
	bool hit = false;
	for (int polygon=0; polygon<numOfTriangles; poly_start += 3,++polygon)
	{
		//
		//---------------------------------
		// See if the line misses the plane
		//---------------------------------
		//
		Scalar product;
		const Plane *plane = &facePlanes[polygon];
		Check_Object(plane);
		Scalar distance = line->GetDistanceTo(*plane, &product);
		if (distance < 0.0f || distance > line->length)
		{
			continue;
		}
		bool negate = false;
		if (product > -SMALL)
		{
			if (GetCurrentState().GetBackFaceMode() == MLRState::BackFaceOnMode)
			{
				continue;
			}
			negate = true;
		}

		//
		//-------------------------------------------
		// Figure out where on the plane the line hit
		//-------------------------------------------
		//
		Point3D impact;
		line->Project(distance, &impact);

		//
		//-------------------------------------------------------------------
		// We now need to find out which cardinal plane we should project the
		// triangle onto
		//-------------------------------------------------------------------
		//
		int s,t;
		Scalar nx = Abs(plane->normal.x);
		Scalar ny = Abs(plane->normal.y);
		Scalar nz = Abs(plane->normal.z);
		if (nx > ny)
		{
			if (nx > nz)
			{
				s = Y_Axis;
				t = Z_Axis;
			}
			else
			{
				s = X_Axis;
				t = Y_Axis;
			}
		}
		else if (ny > nz)
		{
			s = Z_Axis;
			t = X_Axis;
		}
		else
		{
			s = X_Axis;
			t = Y_Axis;
		}

		//
		//----------------------------------------
		// Initialize the vertex and leg variables
		//----------------------------------------
		//
		Point3D *v1, *v2, *v3;
		v1 = &coords[index[poly_start]];
		v2 = &coords[index[poly_start+1]];
		v3 = &coords[index[poly_start+2]];

		//
		//---------------------------------------
		// Get the projection of the impact point
		//---------------------------------------
		//
		Scalar s0 = impact[s] - (*v1)[s];
		Scalar t0 = impact[t] - (*v1)[t];
		Scalar s1 = (*v2)[s] - (*v1)[s];
		Scalar t1 = (*v2)[t] - (*v1)[t];

		//
		//------------------------------------------------------------
		// For each triangle, figure out what the second leg should be
		//------------------------------------------------------------
		//
		bool local_hit = false;

		Check_Pointer(v3);
		Scalar s2 = (*v3)[s] - (*v1)[s];
		Scalar t2 = (*v3)[t] - (*v1)[t];

		//
		//--------------------------------
		// Now, see if we hit the triangle
		//--------------------------------
		//
		if (Small_Enough(s1))
		{
			Verify(!Small_Enough(s2));
			Scalar beta = s0 / s2;
			if (beta >= 0.0f && beta < 1.0f)
			{
				Verify(!Small_Enough(t1));
				Scalar alpha = (t0 - beta*t2) / t1;
				local_hit = (alpha >= 0.0f && alpha+beta <= 1.0f);
			}
		}
		else
		{
			Scalar beta = (t0*s1 - s0*t1);
			Scalar alpha = (t2*s1 - s2*t1);
			beta /= alpha;
			if (beta >= 0.0f && beta <= 1.0f)
			{
				alpha = (s0 - beta*s2) / s1;
				local_hit = (alpha >= 0.0f && alpha+beta <= 1.0f);
			}
		}

		//
		//
		//----------------------------------------------------
		// Handle the hit status, and move to the next polygon
		//----------------------------------------------------
		//
		if (local_hit)
		{
			hit = true;
			line->length = distance;
			if (negate)
				normal->Negate(plane->normal);
			else
				*normal = plane->normal;
			Verify(*normal * line->direction <= -SMALL);
		}
	}

	//
	//----------------------
	// Return the hit status
	//----------------------
	//
	return hit;
}

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
	long    nrTri = (long) ceil (icoInfo.all * pow (4.0f, icoInfo.depth));
	Point3D v[3];

	if(3*nrTri >= Limits::Max_Number_Vertices_Per_Mesh)
	{
		nrTri = Limits::Max_Number_Vertices_Per_Mesh/3;
	}

	Point3D *coords = new Point3D [nrTri*3];
	Register_Pointer(coords);
	
	Point3D *collapsedCoords = nullptr;
	if(icoInfo.indexed==true)
	{
		collapsedCoords = new Point3D [nrTri*3];
		Register_Pointer(collapsedCoords);
	}

	unsigned short	*index = new unsigned short [nrTri*3];
	Register_Pointer(index);
	Vector2DScalar *texCoords = new Vector2DScalar[nrTri*3];
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
					if(coords[i] == collapsedCoords[j])
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
	delete [] texCoords;

	Unregister_Pointer(index);
	delete [] index;

	if(icoInfo.indexed==true)
	{
		Unregister_Pointer(collapsedCoords);
		delete [] collapsedCoords;
	}
	
	Unregister_Pointer(coords);
	delete [] coords;

	gos_PopCurrentHeap();
	return ret;
}
