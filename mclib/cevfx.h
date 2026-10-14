//---------------------------------------------------------------------------
//
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef CEVFX_H
#define CEVFX_H
//---------------------------------------------------------------------------
// Include files

#ifndef CELEMENT_H
#include"celement.h"
#endif

#include<float.h>
#include<gameos.hpp>
//---------------------------------------------------------------------------
class PolygonQuadElement : public Element
{
	public:
		
		//--------------------------------
		// This draws any untextured face.
		// Useful for status bars, etc.
		gos_VERTEX		vertices[4];
		
	PolygonQuadElement (void)
	{
	}
	
	void init (gos_VERTEX *v);
	
	virtual void draw (void);

};

//---------------------------------------------------------------------------
class PolygonTriElement : public Element
{
	public:
		
		//--------------------------------
		// This draws any untextured face.
		// Useful for status bars, etc.
		gos_VERTEX		vertices[3];
		
	PolygonTriElement (void)
	{
	}
	
	void init (gos_VERTEX *v);
	
	virtual void draw (void);

};

//---------------------------------------------------------------------------
class TexturedPolygonQuadElement : public PolygonQuadElement
{
	public:
		
		//--------------------------------
		// This draws any untextured face.
		// Useful everywhere
		DWORD		textureHandle;
		bool		zWrite;
		bool		zComp;
		
	TexturedPolygonQuadElement (void)
	{
	}
	
	void init (gos_VERTEX *v, DWORD tHandle, bool writeZ = true, bool compZ = true);
	
	virtual void draw (void);

};

//---------------------------------------------------------------------------
class TexturedPolygonTriElement : public PolygonTriElement
{
	public:
		
		//--------------------------------
		// This draws any textured face.
		// Useful everywhere.
		DWORD		textureHandle;
		
	TexturedPolygonTriElement (void)
	{
	}
	
	void init (gos_VERTEX *v, DWORD tHandle);
	
	virtual void draw (void);

};

//---------------------------------------------------------------------------
#endif
