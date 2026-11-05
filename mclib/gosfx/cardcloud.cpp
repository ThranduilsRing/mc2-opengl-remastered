//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"gosfxheaders.hpp"

#include"particles/batcher.h"
#include <algorithm>
#include<cmath>    // VFX-ORIGINAL-RENDER-ANIM-FIELDS-1: std::atan2
#include<cstdio>   // VFX-ORACLE diagnostics
#include<vector>
#include "mc2fmt.h"
//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification::CardCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	SpinningCloud__Specification(gosFX::CardCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(m_class == CardCloudClassID);
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
		m_pIndex.m_ageCurve.SetCurve(0.0f);
		m_pIndex.m_seedCurve.SetCurve(1.0f);
		m_pIndex.m_seeded = false;

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
		m_pIndex.Load(stream, gfx_version);
		m_UOffset.Load(stream, gfx_version);
		m_VOffset.Load(stream, gfx_version);
		m_USize.Load(stream, gfx_version);
		m_VSize.Load(stream, gfx_version);
		*stream >> m_animated;
	}
	SetWidth();

	m_totalParticleSize = gosFX::CardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::CardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification::CardCloud__Specification():
	SpinningCloud__Specification(gosFX::CardCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_animated = false;
	m_width = 1;
	m_totalParticleSize = gosFX::CardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::CardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification*
	gosFX::CardCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	CardCloud__Specification *spec =
		new gosFX::CardCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	SpinningCloud__Specification::Save(stream);

	m_halfHeight.Save(stream);
	m_aspectRatio.Save(stream);
	m_pIndex.Save(stream);
	m_UOffset.Save(stream);
	m_VOffset.Save(stream);
	m_USize.Save(stream);
	m_VSize.Save(stream);
	*stream << m_animated;
}
//------------------------------------------------------------------------------
//

void 
	gosFX::CardCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	SpinningCloud__Specification::BuildDefaults();

	m_halfHeight.m_ageCurve.SetCurve(1.0f);
	m_halfHeight.m_seeded = false;
	m_halfHeight.m_seedCurve.SetCurve(1.0f);

	m_aspectRatio.m_ageCurve.SetCurve(1.0f);
	m_aspectRatio.m_seeded = false;
	m_aspectRatio.m_seedCurve.SetCurve(1.0f);

	m_pIndex.m_ageCurve.SetCurve(0.0f);
	m_pIndex.m_seeded = false;
	m_pIndex.m_seedCurve.SetCurve(1.0f);

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
	gosFX::CardCloud__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);

		Stuff::Scalar max_offset, min_offset;
	Stuff::Scalar max_scale, min_scale;
	m_USize.ExpensiveComputeRange(&min_scale, &max_scale);
	Stuff::Scalar lower = min_scale;
	lower = std::min(lower, 0.0f);
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
	lower = std::min(lower, 0.0f);
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

	
	return	SpinningCloud__Specification::IsDataValid(fix_data);

}


//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::Copy(CardCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	SpinningCloud__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_halfHeight = spec->m_halfHeight;
	m_aspectRatio = spec->m_aspectRatio;
	m_pIndex = spec->m_pIndex;
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
	gosFX::CardCloud__Specification::SetWidth()
{
	m_width =
		static_cast<BYTE>(1.0f / m_USize.ComputeValue(0.0f, 0.0f));
}

//############################################################################
//##############################  gosFX::CardCloud  ################################
//############################################################################

gosFX::CardCloud::ClassData*
	gosFX::CardCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			CardCloudClassID,
			"gosFX::CardCloud",
			SpinningCloud::DefaultData,
			effectFactoryThunk<&Make>,
			specificationFactoryThunk<&Specification::Make>
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud::CardCloud(
	Specification *spec,
	unsigned flags
):
	SpinningCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);


	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_vertices = Cast_Pointer(glm::vec3*, &m_data[index]);
	index += 4*spec->m_maxParticleCount*sizeof(glm::vec3);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);
	index += spec->m_maxParticleCount * sizeof(Stuff::RGBAColor);
	m_P_uvs = Cast_Pointer(Stuff::Vector2DOf<Stuff::Scalar>*, &m_data[index]);

}

//------------------------------------------------------------------------------
//
gosFX::CardCloud::~CardCloud()
{
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud*
	gosFX::CardCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	CardCloud *cloud = new gosFX::CardCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::CreateNewParticle(
		unsigned index,
		glm::vec3 *translation
	)
{
	Check_Object(this);

	//
	//-------------------------------------------------------------------
	// Let our parent do creation, then turn on the particle in the cloud
	//-------------------------------------------------------------------
	//
	SpinningCloud::CreateNewParticle(index, translation);

	//
	//-----------------------------
	// Figure out the particle size
	//-----------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	particle->m_halfY =
		spec->m_halfHeight.ComputeValue(m_age, particle->m_seed);
	particle->m_halfX =
		particle->m_halfY * spec->m_aspectRatio.ComputeValue(m_age, particle->m_seed);
	particle->m_radius =
		Stuff::Sqrt(
			particle->m_halfX * particle->m_halfX
			 + particle->m_halfY * particle->m_halfY
		);
}

//------------------------------------------------------------------------------
//
bool
	gosFX::CardCloud::AnimateParticle(
		unsigned index,
		const glm::mat4 *world_to_new_local,
		Stuff::Time till
	)
{
	Check_Object(this);

	//
	//-----------------------------------------
	// Animate the parent then get our pointers
	//-----------------------------------------
	//
	if (!SpinningCloud::AnimateParticle(index, world_to_new_local, till))
		return false;
	Set_Statistic(Card_Count, Card_Count+1);
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	Stuff::Scalar seed = particle->m_seed;
	Stuff::Scalar age = particle->m_age;

	//
	//------------------
	// Animate the color
	//------------------
	//
	Check_Pointer(m_P_color);
	m_P_color[index].red = spec->m_pRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pAlpha.ComputeValue(age, seed);

	//
	//----------------
	// Animate the uvs
	//----------------
	//
	Stuff::Scalar u = spec->m_UOffset.ComputeValue(age, seed);
	Stuff::Scalar v = spec->m_VOffset.ComputeValue(age, seed);
	Stuff::Scalar u2 = spec->m_USize.ComputeValue(age, seed);
	Stuff::Scalar v2 = spec->m_VSize.ComputeValue(age, seed);

	//
	//--------------------------------------------------------------
	// If we are animated, figure out the row/column to be displayed
	//--------------------------------------------------------------
	//
	if (spec->m_animated)
	{
		BYTE columns =
			Stuff::Truncate_Float_To_Byte(
				spec->m_pIndex.ComputeValue(age, seed)
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

	index *= 4;
	m_P_uvs[index].x = u;
	m_P_uvs[index].y = v2;
	m_P_uvs[++index].x = u2;
	m_P_uvs[index].y = v2;
	m_P_uvs[++index].x = u2;
	m_P_uvs[index].y = v;
	m_P_uvs[++index].x = u;
	m_P_uvs[index].y = v;
	return true;
}

//------------------------------------------------------------------------------
//
void gosFX::CardCloud::DestroyParticle(unsigned index)
{
	Check_Object(this);

	SpinningCloud::DestroyParticle(index);
}

//------------------------------------------------------------------------------
//
void gosFX::CardCloud::Draw(DrawInfo *info)
{
	if (!mc2::particles::Batcher::is_log_enabled())
	{
		DrawState(info);
		return;
	}
	const auto legacyBefore = Stuff::Random::GetState();
	DrawState(info);
	const auto legacyAfter = Stuff::Random::GetState();
	mc2::fmt::print_err("[VFX_DRAW_RNG v1] class=CardCloud spec={} time={:.9f} age={:.9f} seed={:.9f} stream={} state_before={} state_after={} draws_before={} draws_after={} legacy_before={} legacy_after={} unchanged={}\n",
		mc2::fmt::safe(static_cast<const char*>(m_specification->m_name)),
		static_cast<double>(m_lastRan), static_cast<double>(m_age), static_cast<double>(m_seed),
		"legacy", 0u, 0u, legacyBefore.draws, legacyAfter.draws,
		legacyBefore.draws, legacyAfter.draws, (legacyBefore == legacyAfter) ? 1 : 0);
}

void gosFX::CardCloud::DrawState(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	//
	//---------------------------------------------------------
	// If we have active particles, set up the draw information
	//---------------------------------------------------------
	//
	if (m_activeParticleCount)
	{
		Specification *spec = GetSpecification();
		Check_Object(spec);
		const auto local_to_world = m_localToParent * *info->m_parentToWorld;

		//
		//--------------------------------------------------------------
		// Check the orientation mode.  The first case is XY orientation
		//--------------------------------------------------------------
		//
		unsigned i;
		unsigned vert=0;
		if (spec->m_alignZUsingX)
		{
			if (spec->m_alignZUsingY)
			{
				//
				//-----------------------------------------
				// Get the camera location into local space
				//-----------------------------------------
				//
				glm::vec3
					camera_in_world = mc2::math::getTranslation(info->m_clipper->GetCameraToWorldMatrix());
				glm::vec3 camera_in_cloud;
				camera_in_cloud = (mc2::math::transformPoint(camera_in_world, glm::inverse(local_to_world)));

				//
				//--------------------------------------
				// Spin through all the active particles
				//--------------------------------------
				//
				for (i = 0; i < m_activeParticleCount; i++)
				{
					Particle *particle = GetParticle(i);
					Check_Object(particle);
					if (particle->m_age < 1.0f)
					{

						//
						//--------------------------------
						// Build the local to cloud matrix
						//--------------------------------
						//
						glm::vec3 direction_in_cloud;
						direction_in_cloud = camera_in_cloud - particle->m_localTranslation;
						glm::mat4 card_to_cloud;
						mc2::math::buildRotation(card_to_cloud, particle->m_localRotation);
						mc2::math::alignLocalAxisToWorldVector(card_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::Y_Axis, Stuff::X_Axis);
						mc2::math::buildTranslation(card_to_cloud, particle->m_localTranslation);

						//
						//-------------------------------------------------
						// Figure out the scale, then build the four points
						//-------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								-scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								-scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
					}
					else
						vert += 4;
				}
			}

			//
			//-----------------------
			// Handle X-only rotation
			//-----------------------
			//
			else
			{
				//
				//-----------------------------------------
				// Get the camera location into local space
				//-----------------------------------------
				//
				glm::vec3
					camera_in_world = mc2::math::getTranslation(info->m_clipper->GetCameraToWorldMatrix());
				glm::vec3 camera_in_cloud;
				camera_in_cloud = (mc2::math::transformPoint(camera_in_world, glm::inverse(local_to_world)));

				//
				//--------------------------------------
				// Spin through all the active particles
				//--------------------------------------
				//
				for (i = 0; i < m_activeParticleCount; i++)
				{
					Particle *particle = GetParticle(i);
					Check_Object(particle);
					if (particle->m_age < 1.0f)
					{

						//
						//--------------------------------
						// Build the local to cloud matrix
						//--------------------------------
						//
						glm::vec3 direction_in_cloud;
						direction_in_cloud = camera_in_cloud - particle->m_localTranslation;
						glm::mat4 card_to_cloud;
						mc2::math::buildRotation(card_to_cloud, particle->m_localRotation);
						mc2::math::alignLocalAxisToWorldVector(card_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::X_Axis, -1);
						mc2::math::buildTranslation(card_to_cloud, particle->m_localTranslation);

						//
						//-------------------------------------------------
						// Figure out the scale, then build the four points
						//-------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								-scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								-scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							), card_to_cloud);
					}
					else
						vert += 4;
				}
			}
		}

		//
		//-------------------------------------------------------
		// Each matrix needs to be aligned to the camera around Y
		//-------------------------------------------------------
		//
		else if (spec->m_alignZUsingY)
		{
			//
			//-----------------------------------------
			// Get the camera location into local space
			//-----------------------------------------
			//
			glm::vec3
				camera_in_world = mc2::math::getTranslation(info->m_clipper->GetCameraToWorldMatrix());
			glm::vec3 camera_in_cloud;
			camera_in_cloud = (mc2::math::transformPoint(camera_in_world, glm::inverse(local_to_world)));

			//
			//--------------------------------------
			// Spin through all the active particles
			//--------------------------------------
			//
			for (i = 0; i < m_activeParticleCount; i++)
			{
				Particle *particle = GetParticle(i);
				Check_Object(particle);
				if (particle->m_age < 1.0f)
				{

					//
					//--------------------------------
					// Build the local to cloud matrix
					//--------------------------------
					//
					glm::vec3 direction_in_cloud;
					direction_in_cloud = camera_in_cloud - particle->m_localTranslation;
					glm::mat4 card_to_cloud;
					mc2::math::buildRotation(card_to_cloud, particle->m_localRotation);
					mc2::math::alignLocalAxisToWorldVector(card_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::Y_Axis, -1);
					mc2::math::buildTranslation(card_to_cloud, particle->m_localTranslation);

					//
					//-------------------------------------------------
					// Figure out the scale, then build the four points
					//-------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							-scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							-scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
				}
				else
					vert += 4;
			}
		}

		//
		//---------------------------------------------------------------
		// No alignment is necessary, so just multiply out all the active
		// particles
		//---------------------------------------------------------------
		//
		else
		{
			for (i = 0; i < m_activeParticleCount; i++)
			{
				Particle *particle = GetParticle(i);
				Check_Object(particle);
				if (particle->m_age < 1.0f)
				{

					//
					//--------------------------------
					// Build the local to cloud matrix
					//--------------------------------
					//
					glm::mat4 card_to_cloud;
					mc2::math::buildRotation(card_to_cloud, particle->m_localRotation);
					mc2::math::buildTranslation(card_to_cloud, particle->m_localTranslation);

					//
					//-------------------------------------------------
					// Figure out the scale, then build the four points
					//-------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							-scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							-scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						), card_to_cloud);
				}
				else
					vert += 4;
			}
		}

		//
		//---------------------
		// Now just do the draw
		//---------------------
		//
		if (mc2::particles::Batcher::is_enabled()) {
			MidLevelRenderer::MLRState state;
			state.Combine(info->m_state, spec->m_state);
			const auto alpha = state.GetAlphaMode();
			const bool additive = alpha == MidLevelRenderer::MLRState::OneOneMode;
			const int blend = additive ? 2 : alpha == MidLevelRenderer::MLRState::AlphaOneMode ? 1 : 0;
			auto& batcher = mc2::particles::Batcher::Instance();
			batcher.BeginGroup(state.GetTextureHandle(), 0, 0, 1, 1,
			                   blend, 0xffffffffu);
			for (unsigned i = 0; i < m_activeParticleCount; ++i) {
				if (GetParticle(i)->m_age >= 1.0f) continue;
				for (unsigned corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
					const unsigned index = i * 4 + corner;
					const auto world = mc2::math::transformPoint(m_P_vertices[index], local_to_world);
					mc2::particles::GpuParticle vertex{};
					vertex.position[0] = world.x;
					vertex.position[1] = world.y;
					vertex.position[2] = world.z;
					const auto& color = m_P_color[i];
					vertex.color[0] = color.red;
					vertex.color[1] = color.green;
					vertex.color[2] = color.blue;
					vertex.color[3] = color.alpha;
					vertex.velocity[0] = m_P_uvs[index].x;
					vertex.velocity[1] = m_P_uvs[index].y;
					const auto* particle = GetParticle(i);
					vertex.size = particle->m_scale * std::max(particle->m_halfX, particle->m_halfY);
					vertex.kind_flags = 8u | 64u | (additive ? 2u : 0u) |
						(state.GetTextureWrapMode() == MidLevelRenderer::MLRState::TextureClamp ? 16u : 0u) |
						(state.GetTextureHandle() == 0 ? 32u : 0u);
					batcher.Emit(vertex);
				}
			}
		}
	}

	SpinningCloud::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//------------------------------------------------------------------------------
// Both render arms require the inherited particle initialization.
//
void
	gosFX::CardCloud::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	SpinningCloud::Start(info);
}
