#include"gosfxheaders.hpp"
#include "particles/batcher.h"
#include "Gates/gate.h"

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline.
#include<cstdio>   // VFX-ORACLE diagnostics
#include "mc2fmt.h"
#include "diagnostic_trace.h"
//==========================================================================//
// File:	 gosFX_ShardCloud.cpp											//
// Contents: Base gosFX::ShardCloud Component								//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//------------------------------------------------------------------------------
//
gosFX::ShardCloud__Specification::ShardCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	SpinningCloud__Specification(gosFX::ShardCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);
	Verify(m_class == ShardCloudClassID);

	m_size.Load(stream, gfx_version);
	m_angularity.Load(stream, gfx_version);

	m_totalParticleSize = gosFX::ShardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::ShardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud__Specification::ShardCloud__Specification():
	SpinningCloud__Specification(gosFX::ShardCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_totalParticleSize = gosFX::ShardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::ShardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud__Specification*
	gosFX::ShardCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	ShardCloud__Specification *spec =
		new gosFX::ShardCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	SpinningCloud__Specification::Save(stream);

	m_size.Save(stream);
	m_angularity.Save(stream);
}

//------------------------------------------------------------------------------
//
void 
	gosFX::ShardCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	SpinningCloud__Specification::BuildDefaults();

	m_size.m_ageCurve.SetCurve(1.0f);
	m_size.m_seeded = false;
	m_size.m_seedCurve.SetCurve(1.0f);

	m_angularity.m_ageCurve.SetCurve(1.0f);
	m_angularity.m_seeded = false;
	m_angularity.m_seedCurve.SetCurve(1.0f);

}

//------------------------------------------------------------------------------
//
bool 
	gosFX::ShardCloud__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);
return		SpinningCloud__Specification::IsDataValid(fix_data);
/*
	m_size.m_ageCurve.SetCurve(1.0f);
	m_size.m_seeded = false;
	m_size.m_seedCurve.SetCurve(1.0f);

	m_angularity.m_ageCurve.SetCurve(1.0f);
	m_angularity.m_seeded = false;
	m_angularity.m_seedCurve.SetCurve(1.0f);
*/
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud__Specification::Copy(ShardCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	SpinningCloud__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_size = spec->m_size;
	m_angularity = spec->m_angularity;
	gos_PopCurrentHeap();
}

//############################################################################
//##############################  gosFX::ShardCloud  ################################
//############################################################################

gosFX::ShardCloud::ClassData*
	gosFX::ShardCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			ShardCloudClassID,
			"gosFX::ShardCloud",
			SpinningCloud::DefaultData,
			effectFactoryThunk<&Make>,
			specificationFactoryThunk<&Specification::Make>
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud::ShardCloud(
	Specification *spec,
	unsigned flags
):
	SpinningCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);


	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_vertices = Cast_Pointer(glm::vec3*, &m_data[index]);
	index += 3*spec->m_maxParticleCount*sizeof(glm::vec3);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);

}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud::~ShardCloud() = default;

//------------------------------------------------------------------------------
//
gosFX::ShardCloud*
	gosFX::ShardCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	ShardCloud *cloud = new gosFX::ShardCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::ShardCloud::AnimateParticle(
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
	Set_Statistic(Shard_Count, Shard_Count+1);
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
	index *= 3;
	m_P_color[index].red = spec->m_pRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pAlpha.ComputeValue(age, seed);
	m_P_color[index+2] = m_P_color[index+1] = m_P_color[index];
	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud::CreateNewParticle(
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
	particle->m_radius = spec->m_size.ComputeValue(m_age, particle->m_seed);
	particle->m_angle =
		mc2::math::sinD(spec->m_angularity.ComputeValue(m_age, particle->m_seed));
}

//------------------------------------------------------------------------------
//
void gosFX::ShardCloud::Draw(DrawInfo *info) const
{
	Check_Object(this);
	Check_Object(info);

	// Accepted primitives use the native stream; the kill-switch suppresses them.

	//
	//---------------------------------------------------------
	// If we have active particles, set up the draw information
	//---------------------------------------------------------
	//
	if (m_activeParticleCount)
	{
		const Specification *spec = GetSpecification();
		Check_Object(spec);
        if (mc2::gate::get<"MC2_GPU_PARTICLES_LOG">() && mc2_diag::tagRequested("VFX_PRIMITIVE")) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                mc2::fmt::print_err("[VFX_PRIMITIVE] class=ShardCloud live={:d} route={}\n",
                    mc2::fmt::as_u32(m_activeParticleCount),
                    mc2::particles::Batcher::is_enabled() ? "native" : "off");
            }
        }
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
					const Particle *particle = GetParticle(i);
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
						glm::mat4 shard_to_cloud;
						mc2::math::buildRotation(shard_to_cloud, particle->m_localRotation);
						mc2::math::alignLocalAxisToWorldVector(shard_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::Y_Axis, Stuff::X_Axis);
						mc2::math::buildTranslation(shard_to_cloud, particle->m_localTranslation);

						//
						//--------------------------------------------------
						// Figure out the scale, then build the three points
						//--------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								0.0f,
								-0.5f*scale*particle->m_radius,
								0.0f
							), shard_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							), shard_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								-scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							), shard_to_cloud);
					}
					else
						vert += 3;
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
					const Particle *particle = GetParticle(i);
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
						glm::mat4 shard_to_cloud;
						mc2::math::buildRotation(shard_to_cloud, particle->m_localRotation);
						mc2::math::alignLocalAxisToWorldVector(shard_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::X_Axis, -1);
						mc2::math::buildTranslation(shard_to_cloud, particle->m_localTranslation);

						//
						//--------------------------------------------------
						// Figure out the scale, then build the three points
						//--------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								0.0f,
								-0.5f*scale*particle->m_radius,
								0.0f
							), shard_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							), shard_to_cloud);
						m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
								-scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							), shard_to_cloud);
					}
					else
						vert += 3;
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
				const Particle *particle = GetParticle(i);
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
					glm::mat4 shard_to_cloud;
					mc2::math::buildRotation(shard_to_cloud, particle->m_localRotation);
					mc2::math::alignLocalAxisToWorldVector(shard_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::Y_Axis, -1);
					mc2::math::buildTranslation(shard_to_cloud, particle->m_localTranslation);

					//
					//--------------------------------------------------
					// Figure out the scale, then build the three points
					//--------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							0.0f,
							-0.5f*scale*particle->m_radius,
							0.0f
						), shard_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						), shard_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							-scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						), shard_to_cloud);
				}
				else
					vert += 3;
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
				const Particle *particle = GetParticle(i);
				Check_Object(particle);
				if (particle->m_age < 1.0f)
				{

					//
					//--------------------------------
					// Build the local to cloud matrix
					//--------------------------------
					//
					glm::mat4 shard_to_cloud;
					mc2::math::buildRotation(shard_to_cloud, particle->m_localRotation);
					mc2::math::buildTranslation(shard_to_cloud, particle->m_localTranslation);

					//
					//--------------------------------------------------
					// Figure out the scale, then build the three points
					//--------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							0.0f,
							-0.5f*scale*particle->m_radius,
							0.0f
						), shard_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						), shard_to_cloud);
					m_P_vertices[vert++] = mc2::math::transformPoint(glm::vec3(
							-scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						), shard_to_cloud);
				}
				else
					vert += 3;
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
            const auto am = state.GetAlphaMode();
            const bool additive = am == MidLevelRenderer::MLRState::OneOneMode ||
                                  am == MidLevelRenderer::MLRState::AlphaOneMode;
            auto& batcher = mc2::particles::Batcher::Instance();
            batcher.BeginGroup(0, 0, 0, 1, 1, am == MidLevelRenderer::MLRState::OneOneMode ? 2 : (additive ? 1 : 0), 0xffffffffu);
            for (unsigned i = 0; i < m_activeParticleCount; ++i) {
                if (GetParticle(i)->m_age >= 1.0f) continue;
                for (unsigned corner = 0; corner < 3; ++corner) {
                    const unsigned index = i * 3 + corner;
                    const auto world = mc2::math::transformPoint(m_P_vertices[index], local_to_world);
                    mc2::particles::GpuParticle vertex{};
                    vertex.position[0] = world.x;
                    vertex.position[1] = world.y;
                    vertex.position[2] = world.z;
                    const auto& color = m_P_color[index];
                    vertex.color[0] = color.red;
                    vertex.color[1] = color.green;
                    vertex.color[2] = color.blue;
                    vertex.color[3] = am == MidLevelRenderer::MLRState::OneZeroMode ? 1.0f : color.alpha;
                    vertex.kind_flags = 8u | 32u | (additive ? 2u : 0u);
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
	gosFX::ShardCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
