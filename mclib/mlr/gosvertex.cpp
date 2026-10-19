//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"

Scalar
	GOSVertex::farClipReciprocal;

Scalar
	ViewportScalars::MulX,
	ViewportScalars::MulY,
	ViewportScalars::AddX,
	ViewportScalars::AddY;

#if FOG_HACK
	BYTE GOSVertex::fogTable[Limits::Max_Number_Of_FogStates][1024];
#endif

//#############################################################################
//############################    GOSVertex   #################################
//#############################################################################

GOSVertex::GOSVertex()
{
	x = 0.0f;
	y = 0.0f;
	z = 0.0f;
	rhw = 1.0f;

	argb = 0xffffffff;

	u = 0.0f;
	v = 0.0f;

	frgb = 0xffffffff;
}

void
	GOSVertex::SetFogTableEntry(int entry, Scalar nearFog, Scalar farFog, Scalar fogDensity)
{
	float Fog;

	Verify(farFog > nearFog);

	entry--;

	GOSVertex::fogTable[entry][0] = 0;

	for( int t1=0; t1<1024; t1++ )
	{
		if( 0.0f == fogDensity )
		{
			Fog=(farFog-t1)/(farFog-nearFog);													// 0.0 = Linear fog table (from Start to End)
		}
		else
		{
			if( fogDensity<1.0f )
			{
				Fog=static_cast<float>(exp(-fogDensity*t1));											// 0.0->1.0 = FOG_EXP
			}
			else
			{
				Fog=static_cast<float>(exp(-((fogDensity-1.0f)*t1)*((fogDensity-1.0f)*t1)));		// 1.0->2.0 = FOG_EXP2
			}
		}
		
		if( Fog<0.0f )
			Fog=0.0f;
		if( Fog>1.0f )
			Fog=1.0f;

		GOSVertex::fogTable[entry][t1]=static_cast<BYTE>((255.9f*Fog));
	}
}
