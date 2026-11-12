//----------------------------------------------------------------------------
//
// Link.cpp - 
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "stdafx.h"
#include "mclib.h"
#include "BuildingLink.h"
#include "EditorObjectMgr.h"
#include <algorithm>
#include "celine.h"
#include "Camera.h"
#include "../RenderCore/axis_convert.h"

//-------------------------------------------------------------------------------------------------
// Modern GL forward projection (projectModernClipGL + viewport remap). Legacy
// projectZ diverges/rejects when zoomed in (X-collapse). False = behind near plane.
static bool projectPtGL_link( Camera* eye, const glm::vec3& wp, float& sx, float& sy )
{
	if ( !eye ) return false;
	ModernClipResult r = eye->projectModernClipGL( wp );
	if ( r.clip.w <= 1e-4f ) return false;
	float vmx = 0.f, vmy = 0.f, vax = 0.f, vay = 0.f;
	gos_GetViewport( &vmx, &vmy, &vax, &vay );
	const float ndcX = r.clip.x / r.clip.w;
	const float ndcY = r.clip.y / r.clip.w;
	sx = vax + ( ndcX * 0.5f + 0.5f ) * vmx;
	sy = vay + ( 1.0f - ( ndcY * 0.5f + 0.5f ) ) * vmy;
	if ( !( sx == sx ) || !( sy == sy ) ) return false;
	return true;
}


BuildingLink::BuildingLink( const EditorObject* pParent )
{
	gosASSERT( pParent );
	
	parent.m_ID = pParent->getID();
	parent.pos = pParent->getPosition();
}

BuildingLink::BuildingLink( const glm::vec3& pos )
{
	parent.pos = pos;
	parent.m_ID = -1;
}

BuildingLink::BuildingLink( const BuildingLink& Src )
{
	CopyData( Src );
}

BuildingLink& BuildingLink::operator=( const BuildingLink& Src )
{
	if ( &Src != this )
	{
		CopyData( Src );
	}

	return *this;
}

void BuildingLink::CopyData( const BuildingLink& Src )
{
	parent = Src.parent;

	children = Src.children;
}



bool BuildingLink::AddChild( const EditorObject* pObject )
{
	int ID = pObject->getID();
	
	for ( const auto& child : children )
	{
		if ( mc2::math::closeEnough(child.pos, pObject->getPosition()) &&
			 child.m_ID == ID )
		{
			SPEW( (0, "Link::AddChild, not adding an object because it is"
				"already there\n" ) );

			return false;
		}
	}

	Info tmp;
	tmp.pos = pObject->getPosition();
	tmp.m_ID = ID;

	children.push_back( tmp );

	return true;

}

int BuildingLink::GetLinkCount( ) const
{
	return static_cast<int>(children.size());
}


const glm::vec3& BuildingLink::GetParentPosition() const
{
	return parent.pos;
}

bool BuildingLink::HasChild( const EditorObject* pObject ) const
{
	int ID = pObject->getID();
	pObject->getPosition();

	
	for ( const auto& child : children )
	{
		if ( mc2::math::closeEnough(child.pos, pObject->getPosition()) )
		{
			if ( child.m_ID == ID )
				return true;
		}
	}

	return false;
}

bool BuildingLink::HasParent( const EditorObject* pObject ) const
{
	int ID = pObject->getID();

	// ignore damage bit and rotation
	if ( mc2::math::closeEnough(parent.pos, pObject->getPosition()) && parent.m_ID == ID )
		return true;

	return false;
}

bool BuildingLink::TypeCanBeParent( const EditorObject* pObject )
{
	int Type = pObject->getSpecialType();
	switch (Type)
	{
		case EditorObjectMgr::TURRET_CONTROL:
		case EditorObjectMgr::GATE_CONTROL:
		case EditorObjectMgr::POWER_STATION:
		case EditorObjectMgr::SENSOR_CONTROL:
		case EditorObjectMgr::TURRET_GENERATOR:
		case EditorObjectMgr::BRIDGE_CONTROL:
		case EditorObjectMgr::SPOTLIGHT_CONTROL:
			return true;
			break;

		default:
			break;
	};

	return false;

}
bool BuildingLink::CanLink( const EditorObject* pParent, const EditorObject* pChild  )
{
	int ParentType = pParent->getSpecialType();
	int ChildType = pChild->getSpecialType();
	
	switch (ParentType)
	{
		case EditorObjectMgr::TURRET_CONTROL:
			return (ChildType == EditorObjectMgr::EDITOR_TURRET
					|| ChildType == EditorObjectMgr::LOOKOUT ) ? true : false; 
			break;
		case EditorObjectMgr::GATE_CONTROL:
			return ChildType == EditorObjectMgr::EDITOR_GATE ? true : false;
			break;
		case EditorObjectMgr::POWER_STATION:
			return (ChildType == EditorObjectMgr::BRIDGE_CONTROL
				|| ChildType == EditorObjectMgr::GATE_CONTROL
				|| ChildType == EditorObjectMgr::SPOTLIGHT_CONTROL ) ? true : false;
			break;
		case EditorObjectMgr::SENSOR_CONTROL:
			return ChildType == EditorObjectMgr::SENSOR_TOWER ? true : false;
			break;
		case EditorObjectMgr::TURRET_GENERATOR:
			return ChildType == EditorObjectMgr::TURRET_CONTROL ? true : false;
			break;

		case EditorObjectMgr::SPOTLIGHT_CONTROL:
			return ChildType == EditorObjectMgr::SPOTLIGHT ? true : false;
			break;

		case EditorObjectMgr::BRIDGE_CONTROL:
			return ChildType == EditorObjectMgr::EDITOR_BRIDGE ? true : false;
			break;
		default:
			break;
	};

	return false;
}

bool BuildingLink::RemoveObject( const EditorObject* pObject )
{
	if ( HasParent( pObject ) )
	{
		SPEW(( 0, "BuildingLink::RemoveObject could not remove a parent object\n") );
		return false;
	}

	glm::vec3 pos = pObject->getPosition( );
	long ID = pObject->getID();
	
	for ( auto iter = children.begin(); iter != children.end(); ++iter )
	{
		if ( ID == (*iter).m_ID && mc2::math::closeEnough(pos, (*iter).pos)  )
		{
			children.erase( iter );
			return true;
		}
	}

	SPEW(( 0, "BuildingLInk::RemoveObject failed because it didn't have the object\n") );
	return false;
}

int BuildingLink::GetChildrenPositions( glm::vec3* pos, int Count ) const
{
	if ( children.size() > static_cast<unsigned long>(Count) )
		return static_cast<int>(children.size());

	int i = 0;
	for ( const auto& child : children )
	{
		pos[i++] = child.pos;
	}

	return i; 
}

static bool isInView(const glm::vec3 &position)
{
	bool inView = false;

	if (eye)
	{
 		//--------------------------------------------------
		// First, if we are using perspective, figure out
		// if object too far from camera.  Far Clip Plane.
		if (eye->usePerspective)
		{
			glm::vec3 Distance;
			glm::vec3 eyePosition(eye->getPosition());
			glm::vec3 objPosition(position);
	
			Distance = objPosition - eyePosition;
			float eyeDistance = glm::length(Distance);
			if (eyeDistance > Camera::MaxClipDistance)
			{
				inView = false;
			}
			else if (eyeDistance > Camera::MinHazeDistance)
			{
				Camera::HazeFactor = Camera::fogHaze(eyeDistance);
				inView = true;
			}
			else
			{
				Camera::HazeFactor = 0.0f;
				inView = true;
			}
			
			//-----------------------------------------------------------------
			// If inside farClip plane, check if behind camera.
			// Find angle between lookVector of Camera and vector from camPos
			// to Target.  If angle is less then halfFOV, object is visible.
			if (inView)
			{
				glm::vec3 Distance;
				glm::vec3 objPosition;
				glm::vec3 eyePosition(eye->getCameraOrigin());
				mc2ToRender(objPosition, position);
		
				Distance = objPosition - eyePosition;
				Distance = glm::normalize(Distance);
				
				float cosine = glm::dot(Distance, eye->getLookVector());
 				if (cosine > eye->cosHalfFOV)
					inView = true;
				else
					inView = false;
			}
		}
		else
		{
			Camera::HazeFactor = 0.0f;
			inView = true;
		}
	}

	
	return(inView);
}

void BuildingLink::render()
{
	for ( const auto& child : children )
	{
		// Short segments keep long links visible under coarse object culling.
		glm::vec3 diffVect;
		diffVect = child.pos - parent.pos;
		float lineLength = glm::length(diffVect);
		const float lengthOfATypicalLargeObject = 150.0/*arbitrary*/;
		int numSegments = lineLength / lengthOfATypicalLargeObject;
		if (1 > numSegments)
		{
			numSegments = 1;
		}
		float segmentLength = lineLength / numSegments;
		glm::vec3 segVect = diffVect;
		segVect = glm::normalize(segVect);
		segVect *= segmentLength;

		glm::vec3 p1(parent.pos.x, parent.pos.y, parent.pos.z);
		bool p1IsInView = isInView(p1);
		int i;
		for (i = 0 ; i < numSegments; i += 1)
		{
			glm::vec3 p2 = p1;
			p2 += segVect;
			bool p2IsInView = isInView(p2);

			if (p1IsInView || p2IsInView)
			{
				// Modern zoom-correct projection (legacy projectZ X-collapses on zoom).
				float s1x, s1y, s2x, s2y;
				if ( projectPtGL_link( eye, p1, s1x, s1y ) &&
				     projectPtGL_link( eye, p2, s2x, s2y ) )
				{
					Stuff::Vector4D vertices[2];
					vertices[0].x = s1x;
					vertices[0].y = s1y;
					vertices[1].x = s2x;
					vertices[1].y = s2y;

					vertices[0].z = vertices[1].z = 0.1f;
					vertices[0].w = vertices[1].w = 0.9999f;

					LineElement elem( vertices[0], vertices[1], 0xffff0000, 0, -1 );
					elem.draw();
				}
			}

			p1 = p2;
			p1IsInView = p2IsInView;
		}

	}
	
}

void	BuildingLink::SetParentAlignment( int alignment )
{
	EditorObject* pTmp = EditorObjectMgr::instance()->getObjectAtLocation( parent.pos.x, parent.pos.y );

	if ( pTmp )
		pTmp->setAlignment( alignment ) ;
	else
	{
		gosASSERT(false);
	}

	for ( const auto& child : children )
	{
		pTmp = EditorObjectMgr::instance()->getObjectAtLocation( child.pos.x, child.pos.y ) ;
		if ( pTmp )
			pTmp->setAlignment( alignment );
		else
		{
			gosASSERT(false);
		}
	}
}

void	BuildingLink::FixHeights()
{
	parent.pos.z = land->getTerrainElevation( parent.pos );

	for ( auto& child : children )
	{
		child.pos.z = land->getTerrainElevation(child.pos );
	}
}
void	BuildingLink::SetParentPosition( const glm::vec3& pos )
{
	parent.pos = pos;
}




