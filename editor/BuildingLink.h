#ifndef BUILDINGLINK_H
#define BUILDINGLINK_H
//----------------------------------------------------------------------------
//
// Link.h - represents a link between buildings
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <vector>

class EditorObject;

class BuildingLink
{

public:

	BuildingLink( const EditorObject* pParent );
	BuildingLink( const BuildingLink& Link );
	BuildingLink& operator=( const BuildingLink& );
	BuildingLink( const glm::vec3& ParentPosition );

	bool	HasChild( const EditorObject* ) const;
	bool	HasParent( const EditorObject* ) const;
	int		GetLinkCount() const;
	bool	RemoveObject( const EditorObject* );
	const	glm::vec3& GetParentPosition() const;
	void	SetParentPosition( const glm::vec3& pos );
	int		GetChildrenPositions( glm::vec3*, int MaxNumberOfPoints ) const;
	bool	AddChild( const EditorObject* pObject );
	void	SetParentAlignment( int alignment );
	void	FixHeights();

	static	bool TypeCanBeParent( const EditorObject* ); // type gotten from building mgr
	static	bool CanLink( const EditorObject* pParent, const EditorObject* pChild );

	bool operator==( const BuildingLink& Src )const;

	void	render();


private:
	 struct Info
	 {
		 long m_ID;		 
		 glm::vec3 pos;
	 	 bool operator==( const Info& Src )const;
	 };

	 Info	parent;
	 std::vector<Info> children;


	 // Helper function
	 void CopyData( const BuildingLink& );

	 friend class EditorObjectMgr;
};



#endif// BUILDINGLINK_H
