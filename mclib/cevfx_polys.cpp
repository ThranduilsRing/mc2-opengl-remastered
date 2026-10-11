// VFX elements: TextureElement and the PolygonQuad / PolygonTri / TexturedPolygonQuad / TexturedPolygonTri elements.
// Declared in the class headers; shared names in cevfx_internal.h.
#include "cevfx_internal.h"


//---------------------------------------------------------------------------
void TextureElement::draw (void)
{
	//-------------------------------------
	// Nothing else to do with this bUt
	// -gosDraw!

	float textureFactor = tWidth * Camera::globalScaleFactor;
	
	xHS *= Camera::globalScaleFactor;
	yHS *= Camera::globalScaleFactor;

	//----------------------------
	// Clip and draw top triangle
	gos_VERTEX gVertex[3];

	gVertex[0].x		= x - xHS;
	gVertex[0].y		= y - yHS;
	gVertex[0].z		= topZ;
	gVertex[0].rhw		= 0.5;
	gVertex[0].u		= 0.0;
	gVertex[0].v		= 0.0;
	gVertex[0].argb		= lightRGB;
	gVertex[0].frgb		= fogRGB;

	gVertex[1].x		= gVertex[0].x+textureFactor;
	gVertex[1].y		= gVertex[0].y;
	gVertex[1].z		= topZ;
	gVertex[1].rhw		= 0.5;
	gVertex[1].u		= 1.0;
	gVertex[1].v		= 0.0;
	gVertex[1].argb		= lightRGB;
	gVertex[1].frgb		= fogRGB;

	gVertex[2].x		= gVertex[0].x+textureFactor;
	gVertex[2].y		= gVertex[0].y+textureFactor;
	gVertex[2].z		= z;
	gVertex[2].rhw		= 0.5;
	gVertex[2].u		= 1.0;
	gVertex[2].v		= 1.0;
	gVertex[2].argb		= lightRGB;
	gVertex[2].frgb		= fogRGB;

	//--------------------------------
	//Set States for Software Renderer
	gos_SetRenderState( gos_State_Texture, textureMemoryHandle );
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 0);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 2);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
	}

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}

	//---------------------------------------
	// Clip and draw Bottom Triangle
	gVertex[1].x		= gVertex[2].x;
	gVertex[1].y		= gVertex[2].y;
	gVertex[1].z		= gVertex[2].z;
	gVertex[1].rhw		= gVertex[2].rhw;
	gVertex[1].u		= gVertex[2].u;
	gVertex[1].v		= gVertex[2].v;
	gVertex[1].argb		= gVertex[2].argb;
	gVertex[1].frgb		= gVertex[2].frgb;

	gVertex[2].x		= gVertex[0].x;
	gVertex[2].y		= gVertex[0].y+textureFactor;
	gVertex[2].z		= z;
	gVertex[2].rhw		= 0.5;
	gVertex[2].u		= 0.0;
	gVertex[2].v		= 1.0;
	gVertex[2].argb		= lightRGB;
	gVertex[2].frgb		= fogRGB;

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}
}

//---------------------------------------------------------------------------
// class PolygonQuadElement
void PolygonQuadElement::init (gos_VERTEX *v)
{
	vertices[0] = v[0];
	vertices[1] = v[1];
	vertices[2] = v[2];
	vertices[3] = v[3];
	
	//------------------------------------------
	// Calc Average Depth here.
	depth = -(v[0].y + v[1].y + v[2].y + v[3].y) / 4.0;
}

//---------------------------------------------------------------------------
void PolygonQuadElement::draw (void)
{
	//-------------------------------------
	// nothing else to do with this bUt
	// -gosDraw!

	//------------------------
	// Draw em!
	//--------------------------------
	//Set States for Software Renderer
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 0);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
//		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_Texture, 0);
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 2);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
//		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_Texture, 0);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	//----------------------------------------
	// Clip and draw top triangle
	gos_VERTEX gVertex[3];
	memcpy(gVertex,vertices,sizeof(gos_VERTEX) * 3);

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}

	//---------------------------------------
	// Clip and draw Bottom Triangle
	gVertex[1].x		= gVertex[2].x;
	gVertex[1].y		= gVertex[2].y;
	gVertex[1].z		= gVertex[2].z;
	gVertex[1].rhw		= gVertex[2].rhw;
	gVertex[1].u		= gVertex[2].u;
	gVertex[1].v		= gVertex[2].v;
	gVertex[1].argb		= gVertex[2].argb;
	gVertex[1].frgb		= gVertex[2].frgb;

	gVertex[2].x		= vertices[3].x;
	gVertex[2].y		= vertices[3].y;
	gVertex[2].z		= vertices[3].z;
	gVertex[2].rhw		= vertices[3].rhw;
	gVertex[2].u		= vertices[3].u;
	gVertex[2].v		= vertices[3].v;
	gVertex[2].argb		= vertices[3].argb;
	gVertex[2].frgb		= vertices[3].frgb;

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}
}

//---------------------------------------------------------------------------
// class PolygonTriElement
void PolygonTriElement::init (gos_VERTEX *v)
{
	vertices[0] = v[0];
	vertices[1] = v[1];
	vertices[2] = v[2];
	
	//------------------------------------------
	// Calc Average Depth here.
	depth = -(v[0].y + v[1].y + v[2].y) / 3.0;
}

//---------------------------------------------------------------------------
void PolygonTriElement::draw (void)
{
	//-------------------------------------
	// nothing else to do with this bUt
	// -gosDraw!

	//------------------------
	// Draw em!
	//--------------------------------
	//Set States for Software Renderer
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 0);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
//		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_Texture, 0);
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 2);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
//		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_Texture, 0);
	}

	//----------------------------------------
	// Clip and draw top triangle
	gos_VERTEX gVertex[3];
	memcpy(gVertex,vertices,sizeof(gos_VERTEX) * 3);

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}
}

//---------------------------------------------------------------------------
//class TexturedPolygonQuadElement : public PolygonQuadElement
void TexturedPolygonQuadElement::init (gos_VERTEX *v, DWORD tHandle, bool writez, bool compz)
{
	PolygonQuadElement::init(v);
	textureHandle = tHandle;

	zWrite = writez;
	zComp = compz;
}	
	
//---------------------------------------------------------------------------
void TexturedPolygonQuadElement::draw (void)
{
	//-------------------------------------
	// nothing else to do with this bUt
	// -gosDraw!

	//------------------------
	// Draw em!
	//--------------------------------
	//Set States for Software Renderer
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 0);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, zComp);
		gos_SetRenderState(	gos_State_ZWrite, zWrite);
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 2);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, zComp);
		gos_SetRenderState(	gos_State_ZWrite, zWrite);
	}

		
	if (textureHandle == 0xffffffff)		
	{
		gos_SetRenderState( gos_State_Texture, 0 );
		gos_SetRenderState( gos_State_Filter, gos_FilterNone );
	}
	else
		gos_SetRenderState( gos_State_Texture, textureHandle );

	//----------------------------------------
	// Clip and draw top triangle
	gos_VERTEX gVertex[3];
	memcpy(gVertex,vertices,sizeof(gos_VERTEX) * 3);

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}

	//---------------------------------------
	// Clip and draw Bottom Triangle
	gVertex[1].x		= gVertex[2].x;
	gVertex[1].y		= gVertex[2].y;
	gVertex[1].z		= gVertex[2].z;
	gVertex[1].rhw		= gVertex[2].rhw;
	gVertex[1].u		= gVertex[2].u;
	gVertex[1].v		= gVertex[2].v;
	gVertex[1].argb		= gVertex[2].argb;
	gVertex[1].frgb		= gVertex[2].frgb;

	gVertex[2].x		= vertices[3].x;
	gVertex[2].y		= vertices[3].y;
	gVertex[2].z		= vertices[3].z;
	gVertex[2].rhw		= vertices[3].rhw;
	gVertex[2].u		= vertices[3].u;
	gVertex[2].v		= vertices[3].v;
	gVertex[2].argb		= vertices[3].argb;
	gVertex[2].frgb		= vertices[3].frgb;

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}
}	

//---------------------------------------------------------------------------
//class TexturedPolygonTriElement : public PolygonTriElement
void TexturedPolygonTriElement::init (gos_VERTEX *v, DWORD tHandle)
{
	PolygonTriElement::init(v);
	textureHandle = tHandle;
}	
	
//---------------------------------------------------------------------------
void TexturedPolygonTriElement::draw (void)
{
	//-------------------------------------
	// nothing else to do with this bUt
	// -gosDraw!

	//------------------------
	// Draw em!
	//--------------------------------
	//Set States for Software Renderer
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 0);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 2);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
	}

	if (textureHandle == 0xffffffff)		
	{
		gos_SetRenderState( gos_State_Texture, 0 );
		gos_SetRenderState( gos_State_Filter, gos_FilterNone );
	}
	else
		gos_SetRenderState( gos_State_Texture, textureHandle );

	//----------------------------------------
	// Clip and draw top triangle
	gos_VERTEX gVertex[3];
	memcpy(gVertex,vertices,sizeof(gos_VERTEX) * 3);

 	if ((gVertex[0].z >= 0.0f) &&
 		(gVertex[0].z < 1.0f) &&
 		(gVertex[1].z >= 0.0f) &&  
 		(gVertex[1].z < 1.0f) && 
 		(gVertex[2].z >= 0.0f) &&  
 		(gVertex[2].z < 1.0f))
	{
		//-----------------------------------------------------------------------------
		// Reject Any triangle which has vertices off screeen in software for now.
		// Do real cliping in geometry layer for software and hardware that needs it!
		gos_DrawTriangles(gVertex, 3);
	}
}	
