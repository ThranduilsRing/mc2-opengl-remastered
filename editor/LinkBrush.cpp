//----------------------------------------------------------------------------
//
// LinkBrush.cpp - the link brush links turrets to turret controls, gates to 
//						gate controls etc.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "stdafx.h"
#include "LinkBrush.h"
#include "BuildingLink.h"
#include "EditorObjectMgr.h"
#include "Camera.h"

// GPU-primary object pick (defined in EditorInterface.cpp),
// same picker the Select tool uses. Avoids pulling the heavy EditorInterface.h in here.
extern EditorObject* EditorPickObjectAtScreen(int screenX, int screenY);

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

LinkBrush::LinkBrush( bool Link )
{
	parentPos.x = parentPos.y = -1;
	bLink = Link;
	parent = nullptr;
}

LinkBrush::~LinkBrush()
{
}

bool LinkBrush::beginPaint()
{
	pAction = new LinkAction();

	return true;

}

Action* LinkBrush::endPaint()
{
	if ( pAction && !pAction->changedLinks.empty() )
	{
		return pAction;
	}
	else
		delete pAction;

	return nullptr;
}

bool LinkBrush::paint( glm::vec3& worldPos, int screenX, int screenY )
{
	if ( !bLink )
		return unPaint( worldPos, screenX, screenY );
	
	const EditorObject* pObject = EditorPickObjectAtScreen( screenX, screenY );

	if ( !pObject )
		return 0;

	if (BuildingLink::TypeCanBeParent( pObject ) && 
		( !parent || ( parent && !BuildingLink::CanLink( parent, pObject ) ) ) )
	{
		
		parent = pObject;
		parentPos = parent->getPosition();
			
		BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( pObject );

		if ( pLink )
		{
			pAction->AddToListOnce( LinkInfo(pLink, LinkInfo::EDIT) );
		}
		else
		{
			BuildingLink* pLink = new BuildingLink( pObject );

			EditorObjectMgr::instance()->addLink( pLink );

			pAction->AddToListOnce( LinkInfo(pLink, LinkInfo::ADD) );
		}

	}

	else // we already have a parnet, this is a child building
	{
		if ( !parent )
		{
			return false;
		}

		BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( parent );

		if ( !pLink )
		{
			pLink = new BuildingLink( parent );

			EditorObjectMgr::instance()->addLink( pLink );
			pAction->AddToListOnce( LinkInfo(pLink, LinkInfo::ADD) );

		}
		
		BuildingLink* pOldLink = EditorObjectMgr::instance()->getLinkWithBuilding( pObject );

		if ( pOldLink && pOldLink != pLink )
		{
			pAction->AddToListOnce( LinkInfo(pOldLink, LinkInfo::EDIT ) );
			pOldLink->RemoveObject( pObject );
			pAction->AddToListOnce( LinkInfo( pLink, LinkInfo::EDIT  ));
		}
		if ( !pLink->AddChild( pObject ) )
		{
			SPEW((0,"LinkBrush failed to add a link to a parent\n"));
		
		}

		if ( pObject->getSpecialType() != EditorObjectMgr::POWER_STATION )
			(const_cast<EditorObject*>(pObject))->setAlignment( parent->getAlignment() ); 

		return true;
	}


	return false;
	
	
}


bool LinkBrush::canPaint( glm::vec3& pos, int x, int y, int flags ) 
{ 
	if ( !bLink )
		return canUnPaint( pos, x, y, flags );
	
	const EditorObject* pObject = EditorPickObjectAtScreen( x, y );

	if ( !pObject )
		return false;

	if ( BuildingLink::TypeCanBeParent( pObject) ) // see if this can be a parent
	{
		return  true;
	}

	else if ( parent ) 
	{
		if ( BuildingLink::CanLink( parent, pObject) )
			return true;
	}


	return false;
}

bool LinkBrush::canUnPaint( glm::vec3& pos, int x, int y, int flags )
{ 

	const EditorObject* pBuilding = EditorPickObjectAtScreen( x, y );

	if ( !pBuilding )
		return false;

	if ( EditorObjectMgr::instance()->getLinkWithParent( pBuilding ) )
		return true;

	else if ( EditorObjectMgr::instance()->getLinkWithBuilding( pBuilding) )
		return true; 


	return false;
	 
} 

void LinkBrush::LinkAction::AddToListOnce( const LinkBrush::LinkInfo& Info )
{
	for ( const auto& changed : changedLinks )
	{
		if ( mc2::math::closeEnough(changed.m_LinkCopy.GetParentPosition(), Info.m_LinkCopy.GetParentPosition()) )
		{
			return;  // assume if parents are in the same place, we have the same link
		}
	}

	changedLinks.push_back( Info );
}

LinkBrush::LinkAction::LinkAction( )
{ 
	
}

bool LinkBrush::LinkAction::undo( )
{
	for ( auto iter = changedLinks.rbegin(); iter != changedLinks.rend(); ++iter )
	{
		 if ( (*iter).type == LinkInfo::ADD )
		{
			(*iter).type = LinkInfo::REMOVE;

			glm::vec3 pos = (*iter).m_LinkCopy.GetParentPosition();
			
			const EditorObject* pBuilding = EditorObjectMgr::instance()->getObjectAtLocation( pos.x, pos.y );
			gosASSERT( pBuilding );

			if ( pBuilding )
			{
				BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithBuilding( pBuilding );

				if ( pLink )
				{

					(*iter).m_LinkCopy = *(pLink);
					EditorObjectMgr::instance()->deleteLink( pLink );
				}
			}  
		}
		else if ( (*iter).type == LinkInfo::REMOVE )
		{
			(*iter).type = LinkInfo::ADD;

			EditorObjectMgr::instance()->addLink( new BuildingLink((*iter).m_LinkCopy) );

			// make sure each of the buildings in the link has the right alignment
			int LinkCount = (*iter).m_LinkCopy.GetLinkCount();
			glm::vec3* pPoints = new glm::vec3[LinkCount];
			(*iter).m_LinkCopy.GetChildrenPositions( pPoints, LinkCount );

			glm::vec3 pos = (*iter).m_LinkCopy.GetParentPosition();

			const EditorObject* pBuilding = EditorObjectMgr::instance()->getObjectAtLocation( pos.x, pos.y );
			if ( !pBuilding )
				return false;

			int align = pBuilding->getAlignment();

			for ( int i = 0; i < LinkCount; ++i )
			{
				const EditorObject* pObject = EditorObjectMgr::instance()->getObjectAtLocation( pPoints[i].x, pPoints[i].y );
				if ( pObject )
				{
					(const_cast<EditorObject*>(pObject))->setAlignment( align );
				}
			}

		}
		else if ( (*iter).type == LinkInfo::EDIT )
		{
			glm::vec3 pos = (*iter).m_LinkCopy.GetParentPosition();
			const EditorObject* pBuilding = EditorObjectMgr::instance()->getObjectAtLocation( pos.x, pos.y );
			if ( pBuilding )
			{
				BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( pBuilding );

				if ( pLink )
				{
					BuildingLink tmp( *pLink );
					*pLink = (*iter).m_LinkCopy;
					(*iter).m_LinkCopy = tmp;
				}
			}
			else
			{

				SPEW(( 0,"LinkBursh::LinkAction::Undo failed because the map didn't have the link\n") );
				return false;
			}

		}

	}

	return true;

}

bool LinkBrush::unPaint( glm::vec3& pos, int XPos, int yPos )
{
 
	const EditorObject* pBuilding = EditorPickObjectAtScreen( XPos, yPos );

	if ( pBuilding )
	{

		BuildingLink* pLink = EditorObjectMgr::instance()->getLinkWithParent( pBuilding );

		if ( pLink ) // we are deleting a parent link
		{
			pAction->AddToListOnce( LinkInfo( pLink, LinkInfo::REMOVE ) );

			EditorObjectMgr::instance()->deleteLink( pLink );

			parent = nullptr;
			parentPos.x = parentPos.y = 0;

			return true;
		}
		
		else 
		{
			pLink = EditorObjectMgr::instance()->getLinkWithBuilding( pBuilding );

			if ( pLink )
			{
				pAction->AddToListOnce( LinkInfo( pLink, LinkInfo::EDIT ) );
				pLink->RemoveObject( pBuilding );
				
			}

			return true;
		}
	}
	
	return false;
}

bool LinkBrush::LinkAction::redo()
{
	return undo();
}

LinkBrush::LinkInfo::LinkInfo( BuildingLink* pOriginal, LinkBrush::LinkInfo::TYPE Type ) : 
m_LinkCopy( *pOriginal )
{
	type = Type;
}

void LinkBrush::render( int screenX, int screenY )
{

	if ( parent )
	{
		glm::vec3 parentPos = parent->getPosition();
		Stuff::Vector4D curScreen;
		curScreen.x = screenX;
		curScreen.y = screenY;
		curScreen.z = 0.1f;		//Gotta set Z or QNAN and crash!
		curScreen.w = 0.9999f;	//Gotta set W, too!

		// Modern zoom-correct projection (legacy projectZ X-collapses on zoom).
		float pX, pY;
		if ( projectPtGL_link( eye, parentPos, pX, pY ) )   // skip line if parent rejects
		{
			Stuff::Vector4D screenPos;
			screenPos.x = pX;
			screenPos.y = pY;
			screenPos.z = 0.1f;
			screenPos.w = 0.9999f;

			LineElement elem( curScreen, screenPos, 0xffff0000, 0, -1 );
			elem.draw();
		}

	}
}
