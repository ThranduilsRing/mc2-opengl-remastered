//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include "../fx_trace/fx_trace.h"    // fx_trace v1: gated MLR-leaf invocation counter
#include "mlr_gate.h"                // MC2_DISABLE_GOSFX env-gate (plan v6 §2)
extern DWORD gShowBirdView, gEnableDetailTexture, gEnableMultiTexture, gEnableLightMaps;

DrawShapeInformation::DrawShapeInformation()
{
	shape = nullptr;
    // sebi, MLRState has default constructor no need to do this crap here
	//state = NULL;
	shapeToWorld = nullptr;
	worldToShape = nullptr;

	activeLights = nullptr;
	nrOfActiveLights = 0;

	clippingFlags.SetClippingState(0);
};

DrawScalableShapeInformation::DrawScalableShapeInformation() : DrawShapeInformation()
{
	scaling = nullptr;

	paintMe = nullptr;
}

DrawEffectInformation::DrawEffectInformation()
{
	effect = nullptr;
    // sebi, MLRState has default constructor no need to do this crap here
	//state = NULL;
	effectToWorld = nullptr;

#if 0	
	activeLights = nullptr;
	nrOfActiveLights = 0;
#endif

	clippingFlags.SetClippingState(0);
};

//#############################################################################
//###########################    MLRClipper    ################################
//#############################################################################

MLRClipper::ClassData*
	MLRClipper::DefaultData = nullptr;

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRClipper::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == StaticHeap);
	DefaultData =
		new ClassData(
			MLRClipperClassID,
			"MidLevelRenderer::MLRClipper",
			RegisteredClass::DefaultData
		);
	Register_Object(DefaultData);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRClipper::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRClipper::MLRClipper(AndyDisplay *ad, MLRSorter *s):
	RegisteredClass(DefaultData), display(ad)
{
	Verify(gos_GetCurrentHeap() == Heap);
	frameRate = 0;
	usedTime = 0.0f;
	nowTime = 0.0f;

	sorter = s;
//	camMatrix;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
MLRClipper::~MLRClipper()
{
	if(sorter)
	{
		Unregister_Object(sorter);
		delete sorter;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRClipper::StartDraw (
		const glm::mat4 &camera_to_world,
		const glm::mat4 &cameraToClip,
		const RGBAColor &,
		const RGBAColor *background_color,
		const MLRState &default_state,
		const Scalar *z_value
	)
{
	Check_Object(this);

//
//	No detail under software rasterizer
//
	if(Environment.Renderer == 3)
	{
		gEnableDetailTexture = 0;
	}

	MLRState::SetAGPAvailable(TRUE==gos_GetMachineInformation(gos_Info_HasAGPAvailable));
	MLRState::SetMaxUV(static_cast<float>(gos_GetMachineInformation(gos_Info_GetMaximumUVSize, 256)));

  if(MLRState::GetHasMaxUVs() && MLRState::GetMaxUV() < 128.0f)
	{
		gEnableLightMaps = 0;
	}

	if(gEnableMultiTexture!=0)
	{
		MLRState::SetMultitextureLightMap(TRUE==gos_GetMachineInformation(gos_Info_CanMultitextureLightMap));
		MLRState::SetMultitextureSpecularMap(TRUE==gos_GetMachineInformation(gos_Info_CanMultitextureSpecularMap));
	}
	else
	{
		MLRState::SetMultitextureLightMap(false);
		MLRState::SetMultitextureSpecularMap(false);
	}

//
// Make viewport the whole screen
//
	gos_PushCurrentHeap(Heap);
	Scalar z = 1.0f;
	DWORD back_color = 0;
	bool
		fill = false,
		clear = false;
	if (z_value)
	{
		Check_Pointer(z_value);
		z = *z_value;
		fill = true;
	}
	if (background_color)
	{
		Check_Pointer(background_color);
		back_color = GOSCopyColor(background_color);
		clear = true;
	}

	MLRState::fogColor = back_color;

	/*	   Already done in MC2
	gos_SetupViewport(
		fill,
		z,
		clear,
		back_color,
		1.0,
		1.0,
		0.0,
		0.0
	);
	*/

	gos_SetRenderState( gos_State_Texture, 0 );
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero );
	gos_SetRenderState( gos_State_Clipping, 0 );
	gos_SetRenderState( gos_State_ZWrite, 1 );
	gos_SetRenderState( gos_State_Filter, gos_FilterTriLinear );
	gos_SetRenderState( gos_State_ZCompare, 1 );
	gos_SetRenderState( gos_State_WireframeMode, 0 );
	gos_SetRenderState( gos_State_Specular, 0 );
	gos_SetRenderState( gos_State_Dither, 0 );
	gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud );
	gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha );
	gos_SetRenderState( gos_State_AlphaTest, 0 );
	gos_SetRenderState( gos_State_Fog, 0 );


	sorter->SetFarClipReciprocal((1.0f-cameraToClip[2][2])/cameraToClip[2][3]);

	sorter->StartDraw(default_state);

	cameraToWorldMatrix = camera_to_world;

	worldToCameraMatrix = glm::inverse(cameraToWorldMatrix);

#ifdef LAB_ONLY

	if(gShowBirdView)
	{
		static mc2::math::YawPitchRange Camera_Direction(-Pi_Over_6, 0.0f, 15.0f);
		static Vector2DOf<Scalar> Camera_Shift(0.0f, 0.0f);
		static glm::mat4 birdsEye = glm::mat4(1.0f);

		//
		//---------------
		// Read the mouse
		//---------------
		//
		int
			x_delta,
			y_delta;
		DWORD
			buttons;
		gos_GetMouseInfo(nullptr, nullptr, &x_delta, &y_delta, nullptr, &buttons);
		Scalar
			x_speed = x_delta * 0.01f,
			y_speed = y_delta * 0.01f;

		//
		//------------------------------------------------------
		// If the left Ctrl button is held down, move the camera
		//------------------------------------------------------
		//
		gosEnum_KeyStatus status = gos_GetKeyStatus(KEY_LCONTROL);
		if (status != KEY_FREE && status != KEY_RELEASED)
		{
			Camera_Direction.pitch += y_speed;
			Camera_Direction.yaw -= x_speed;
		}

		//
		//------------------------------------------------------
		// If the left Shift button is held down, pan the camera
		//------------------------------------------------------
		//
		status = gos_GetKeyStatus(KEY_LSHIFT);
		if (status != KEY_FREE && status != KEY_RELEASED)
		{
			Camera_Shift.x += x_speed;
			Camera_Shift.y += y_speed;
		}

		//
		//----------------------------------------------------------------
		// If the left Alt button is held down, zoom the camera in and out
		//----------------------------------------------------------------
		//
		status = gos_GetKeyStatus(KEY_LMENU);
		if (status != KEY_FREE && status != KEY_RELEASED)
		{
			Camera_Direction.range *= 1.0f + 3.0f*y_speed;

			Camera_Direction.range = Camera_Direction.range > 0.0f ? Camera_Direction.range : 0.0f;
		}

		//
		//----------------------
		// Set the camera matrix
		//----------------------
		//
		birdsEye.BuildRotation(
			mc2::math::Euler(Camera_Direction.pitch, Camera_Direction.yaw, 0.0f)
		);
		glm::vec3
			world_left,
			world_up;
		birdsEye.GetLocalLeftInWorld(&world_left);
		birdsEye.GetLocalUpInWorld(&world_up);
		glm::vec3 translation = (mc2::math::polarToVector(Camera_Direction));
		translation.AddScaled(translation, world_left, Camera_Shift.x);
		translation.AddScaled(translation, world_up, Camera_Shift.y);
		birdsEye.BuildTranslation(translation);

		glm::mat4 worldToBird = glm::mat4(1.0f);

		worldToBird.Multiply(worldToCameraMatrix, birdsEye);
		worldToCameraMatrix = worldToBird;

//	push the far clip out

		Scalar near_clip, far_clip, left_clip, right_clip,	top_clip, bottom_clip;
		glm::mat4 birdToClip;

		cameraToClip.GetPerspective(&near_clip, &far_clip, &left_clip, &right_clip,	&top_clip, &bottom_clip);
		
		birdToClip.SetPerspective(
			near_clip,
			far_clip+2*Camera_Direction.range,
			left_clip,
			right_clip,
			top_clip,
			bottom_clip
		);

		worldToClipMatrix.Multiply(worldToCameraMatrix, birdToClip);
	}
	else
#endif

	worldToClipMatrix = worldToCameraMatrix * cameraToClip;

	cameraPosition = mc2::math::getTranslation(cameraToWorldMatrix);

//	Tell_Value(cameraPosition);

	sorter->Reset();

	allVerticesToDraw.Reset();

#ifdef CalDraw
	ToBeDrawnPrimitive::allVerticesToDraw = &allVerticesToDraw;
#endif

	MLRPrimitiveBase::InitializeDraw();


	gos_PopCurrentHeap();

//
// End timing function
//
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRClipper::DrawEffect (DrawEffectInformation *dInfo)
{
//
// Statistic timing function
//

	Check_Object(this);
	Check_Object(dInfo);
	Check_Object(dInfo->effect);
	// fx_trace v1 (plan v6 §1 CRITICAL-2): pre-gate enqueue counter.
	FX_TRACE_MLR_ENQUEUE("DrawEffect");
	MC2_GOSFX_GATE_EARLY_RETURN();

#ifdef LAB_ONLY
	if(gShowBirdView)
	{
		dInfo->clippingFlags = 0x3f;
	}
#endif

	dInfo->effect->SetEffectToClipMatrix(dInfo->effectToWorld, &worldToClipMatrix);
	gos_GetViewport( &ViewportScalars::MulX, &ViewportScalars::MulY, &ViewportScalars::AddX, &ViewportScalars::AddY );
	dInfo->effect->Draw(dInfo, &allVerticesToDraw, sorter);
//
// End timing function
//
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	MLRClipper::Clear (unsigned int flags)
{
	Check_Object(this);
}
