//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"gosfxheaders.hpp"

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline
// (replaces C7 EffectAdapter-at-MakeEffect, which only caught top-level
// MakeEffect returns and missed composite-children spawned by EffectCloud).
#include"particles/batcher.h"
#include <algorithm>
#include<cstdio>   // VFX-ORACLE diagnostics (VFX-CARD-ORACLE-HARVEST-1)
#include "mc2fmt.h"

//############################################################################
//########################  gosFX::Card__Specification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::Card__Specification::Card__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	Singleton__Specification(gosFX::CardClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Verify(m_class == CardClassID);
	Verify(gos_GetCurrentHeap() == Heap);

	m_halfHeight.Load(stream, gfx_version);
	m_aspectRatio.Load(stream, gfx_version);

	//
	//-------------------------------------------------------------------
	// If we are reading an old version of the card cloud, ignore all the
	// animation on the UV channels
	//-------------------------------------------------------------------
	//
	if (gfx_version < 10)
	{
		m_index.m_ageCurve.SetCurve(0.0f);
		m_index.m_seedCurve.SetCurve(1.0f);
		m_index.m_seeded = false;

		SeededCurveOf<ComplexCurve, LinearCurve,Curve::e_ComplexLinearType> temp;
		temp.Load(stream, gfx_version);
		Stuff::Scalar v = temp.ComputeValue(0.0f, 0.0f);
		m_UOffset.SetCurve(v);

		temp.Load(stream, gfx_version);
		v = temp.ComputeValue(0.0f, 0.0f);
		m_VOffset.SetCurve(v);

		m_USize.Load(stream, gfx_version);
		m_VSize.Load(stream, gfx_version);

		m_animated = false;
	}

	//
	//------------------------------
	// Otherwise, read in the curves
	//------------------------------
	//
	else
	{
		m_index.Load(stream, gfx_version);
		m_UOffset.Load(stream, gfx_version);
		m_VOffset.Load(stream, gfx_version);
		m_USize.Load(stream, gfx_version);
		m_VSize.Load(stream, gfx_version);
		*stream >> m_animated;
	}
	SetWidth();
}

//------------------------------------------------------------------------------
//
gosFX::Card__Specification::Card__Specification():
	Singleton__Specification(gosFX::CardClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_animated = false;
	m_width = 1;
}

//------------------------------------------------------------------------------
//
gosFX::Card__Specification*
	gosFX::Card__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	Card__Specification *spec =
		new gosFX::Card__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Card__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	Singleton__Specification::Save(stream);

	m_halfHeight.Save(stream);
	m_aspectRatio.Save(stream);
	m_index.Save(stream);
	m_UOffset.Save(stream);
	m_VOffset.Save(stream);
	m_USize.Save(stream);
	m_VSize.Save(stream);
	*stream << m_animated;
}

//------------------------------------------------------------------------------
//
void 
	gosFX::Card__Specification::BuildDefaults()
{

	Check_Object(this);
	Singleton__Specification::BuildDefaults();

	m_alignZUsingX = false;
	m_alignZUsingY = false;

	m_halfHeight.m_ageCurve.SetCurve(1.0f);
	m_halfHeight.m_seeded = false;
	m_halfHeight.m_seedCurve.SetCurve(1.0f);

	m_aspectRatio.m_ageCurve.SetCurve(1.0f);
	m_aspectRatio.m_seeded = false;
	m_aspectRatio.m_seedCurve.SetCurve(1.0f);

	m_index.m_ageCurve.SetCurve(0.0f);
	m_index.m_seeded = false;
	m_index.m_seedCurve.SetCurve(1.0f);

	m_UOffset.SetCurve(0.0f);
	m_VOffset.SetCurve(0.0f);
	m_USize.SetCurve(1.0f);
	m_VSize.SetCurve(1.0f);

	m_animated = false;
	m_width = 1;
}


//------------------------------------------------------------------------------
//
bool 
	gosFX::Card__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);

	Stuff::Scalar max_offset, min_offset;
	Stuff::Scalar max_scale, min_scale;
	m_USize.ExpensiveComputeRange(&min_scale, &max_scale);
	Stuff::Scalar lower = min_scale;
	if (lower > 0.0f)
		lower = 0.0f;
	Stuff::Scalar upper = max_scale;

	//
	//------------------------------------
	// Calculate the worst case UV offsets
	//------------------------------------
	//
	m_VOffset.ExpensiveComputeRange(&min_offset, &max_offset);
	lower += min_offset;
	upper += max_offset;

	if (upper > 99.0f || lower < -99.0f)
	{
		if(fix_data)
		{
		m_VOffset.SetCurve(0.0f);
		PAUSE(("Warning: Curve \"VOffset\" in Effect \"%s\" Is Out of Range and has been Reset",static_cast<char *>(m_name)));
		}
			else
		return false;
	}
	
	m_VSize.ExpensiveComputeRange(&min_scale, &max_scale);
	lower = min_scale;
	if (lower > 0.0f)
		lower = 0.0f;
	upper = max_scale;

	//
	//------------------------------------
	// Calculate the worst case UV offsets
	//------------------------------------
	//
	//max_offset, min_offset;
	m_UOffset.ExpensiveComputeRange(&min_offset, &max_offset);
	lower += min_offset;
	upper += max_offset;

	if (upper > 99.0f || lower < -99.0f)
	{
		if(fix_data)
		{
			m_UOffset.SetCurve(0.0f);
			PAUSE(("Warning: Curve \"UOffset\" in Effect \"%s\" Is Out of Range and has been Reset",static_cast<char *>(m_name)));

		}
		else
			return false;
	}

return		Singleton__Specification::IsDataValid(fix_data);
}


//------------------------------------------------------------------------------
//
void
	gosFX::Card__Specification::Copy(Card__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	Singleton__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_halfHeight = spec->m_halfHeight;
	m_aspectRatio = spec->m_aspectRatio;
	m_index = spec->m_index;
	m_UOffset = spec->m_UOffset;
	m_VOffset = spec->m_VOffset;
	m_USize = spec->m_USize;
	m_VSize = spec->m_VSize;
	m_animated = spec->m_animated;
	m_width = spec->m_width;
	gos_PopCurrentHeap();
}

//------------------------------------------------------------------------------
//
void
	gosFX::Card__Specification::SetWidth()
{
	m_width =
		static_cast<BYTE>(1.0f / m_USize.ComputeValue(0.0f, 0.0f));
}

//############################################################################
//##############################  gosFX::Card  ################################
//############################################################################

gosFX::Card::ClassData*
	gosFX::Card::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::Card::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			CardClassID,
			"gosFX::Card",
			Singleton::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Card::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::Card::Card(
	Specification *spec,
	unsigned flags
):
	Singleton(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

}

//------------------------------------------------------------------------------
//
gosFX::Card::~Card()
{
}

//------------------------------------------------------------------------------
//
gosFX::Card*
	gosFX::Card::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	Card *cloud = new gosFX::Card(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Card::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Object(info);
	Singleton::Start(info);

	Specification *spec = GetSpecification();
	Check_Object(spec);
	m_halfY = spec->m_halfHeight.ComputeValue(m_age, m_seed);
	m_halfX = m_halfY * spec->m_aspectRatio.ComputeValue(m_age, m_seed);
	m_radius = Stuff::Sqrt(m_halfX * m_halfX + m_halfY * m_halfY);

	// GPU spawn moved to Draw() so it fires every frame (not just once at Start).
}

//------------------------------------------------------------------------------
//
bool
	gosFX::Card::Execute(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	if (!IsExecuted())
		return false;

	//
	//------------------------
	// Do the effect animation
	//------------------------
	//
	if (!Singleton::Execute(info))
		return false;

	//
	//-----------------------------------------
	// Animate the parent then get our pointers
	//-----------------------------------------
	//
	Set_Statistic(Card_Count, Card_Count+1);
	Specification *spec = GetSpecification();
	Check_Object(spec);

	//
	//----------------
	// Animate the uvs
	//----------------
	//
	Stuff::Scalar u = spec->m_UOffset.ComputeValue(m_age, m_seed);
	Stuff::Scalar v = spec->m_VOffset.ComputeValue(m_age, m_seed);
	Stuff::Scalar u2 = spec->m_USize.ComputeValue(m_age, m_seed);
	Stuff::Scalar v2 = spec->m_VSize.ComputeValue(m_age, m_seed);

	//
	//--------------------------------------------------------------
	// If we are animated, figure out the row/column to be displayed
	//--------------------------------------------------------------
	//
	if (spec->m_animated)
	{
		BYTE columns =
			Stuff::Truncate_Float_To_Byte(
				spec->m_index.ComputeValue(m_age, m_seed)
			);
		BYTE rows = static_cast<BYTE>(columns / spec->m_width);
		columns = static_cast<BYTE>(columns - rows*spec->m_width);

		//
		//---------------------------
		// Now compute the end points
		//---------------------------
		//
		u += u2*columns;
		v += v2*rows;
	}
	u2 += u;
	v2 += v;

	m_uvs[0].x = u;
	m_uvs[0].y = v2;
	m_uvs[1].x = u2;
	m_uvs[1].y = v2;
	m_uvs[2].x = u2;
	m_uvs[2].y = v;
	m_uvs[3].x = u;
	m_uvs[3].y = v;

	//
	//------------------
	// Fill in the color
	//------------------
	//

	//
	//---------------------
	// Fill in the position
	//---------------------
	//
	m_vertices[0].x = m_scale*m_halfX;
	m_vertices[0].y = -m_scale*m_halfY;
	m_vertices[0].z = 0.0f;

	m_vertices[1].x = -m_scale*m_halfX;
	m_vertices[1].y = -m_scale*m_halfY;
	m_vertices[1].z = 0.0f;

	m_vertices[2].x = -m_scale*m_halfX;
	m_vertices[2].y = m_scale*m_halfY;
	m_vertices[2].z = 0.0f;

	m_vertices[3].x = m_scale*m_halfX;
	m_vertices[3].y = m_scale*m_halfY;
	m_vertices[3].z = 0.0f;

	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Card::Kill()
{
	Check_Object(this);

	Singleton::Kill();
}

//------------------------------------------------------------------------------
//
void gosFX::Card::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	//
	//----------------------------
	// Set up the common draw info
	//----------------------------
	//
	MidLevelRenderer::MLRState state;
	Specification *spec = GetSpecification();
	Check_Object(spec);
	state.Combine(info->m_state, spec->m_state);
	glm::mat4 local_to_world;
	local_to_world = m_localToParent * *info->m_parentToWorld;

	//
	//--------------------------------------------------------------
	// Check the orientation mode.  The first case is XY orientation
	//--------------------------------------------------------------
	//
	if (spec->m_alignZUsingX)
	{
		glm::vec3
			camera_in_world = mc2::math::getTranslation(info->m_clipper->GetCameraToWorldMatrix());
		glm::vec3 card_in_world = mc2::math::getTranslation(local_to_world);
		glm::vec3 look_at;
		look_at = camera_in_world - card_in_world;
		if (spec->m_alignZUsingY)
			mc2::math::alignLocalAxisToWorldVector(local_to_world, look_at, Stuff::Z_Axis, Stuff::Y_Axis, Stuff::X_Axis);
		else
			mc2::math::alignLocalAxisToWorldVector(local_to_world, look_at, Stuff::Z_Axis, Stuff::X_Axis, -1);
	}

	//
	//-------------------------------------------------------
	// Each matrix needs to be aligned to the camera around Y
	//-------------------------------------------------------
	//
	else if (spec->m_alignZUsingY)
	{
		glm::vec3
			camera_in_world = mc2::math::getTranslation(info->m_clipper->GetCameraToWorldMatrix());
		glm::vec3 card_in_world = mc2::math::getTranslation(local_to_world);
		glm::vec3 look_at;
		look_at = camera_in_world - card_in_world;
		mc2::math::alignLocalAxisToWorldVector(local_to_world, look_at, Stuff::Z_Axis, Stuff::Y_Axis, -1);
	}

	//
	//---------------------
	// Now just do the draw
	//---------------------
	//
	if (mc2::particles::Batcher::is_enabled()) {
		// Use the authored world corners, not a camera-facing proxy. The native
		// geometry ABI shares GpuParticle's stride and bypasses billboard tuning.
		auto& batcher = mc2::particles::Batcher::Instance();
		const auto alpha = state.GetAlphaMode();
		const bool additive = alpha == MidLevelRenderer::MLRState::OneOneMode;
		const int blend = additive ? 2 : alpha == MidLevelRenderer::MLRState::AlphaOneMode ? 1 : 0;
		batcher.BeginGroup(state.GetTextureHandle(), 0, 0, 1, 1,
		                   blend, 0xffffffffu);
		for (unsigned corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
			mc2::particles::GpuParticle vertex{};
			const auto world = mc2::math::transformPoint(m_vertices[corner], local_to_world);
			vertex.position[0] = world.x;
			vertex.position[1] = world.y;
			vertex.position[2] = world.z;
			vertex.color[0] = m_color.red;
			vertex.color[1] = m_color.green;
			vertex.color[2] = m_color.blue;
			vertex.color[3] = m_color.alpha;
			vertex.velocity[0] = m_uvs[corner].x;
			vertex.velocity[1] = m_uvs[corner].y;
			vertex.size = m_scale * std::max(m_halfX, m_halfY);
			vertex.kind_flags = 8u | 64u | (additive ? 2u : 0u) |
				(state.GetTextureWrapMode() == MidLevelRenderer::MLRState::TextureClamp ? 16u : 0u) |
				(state.GetTextureHandle() == 0 ? 32u : 0u);
			batcher.Emit(vertex);
		}
	}
	Singleton::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Card::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
