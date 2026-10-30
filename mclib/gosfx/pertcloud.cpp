#include"gosfxheaders.hpp"
#include "particles/batcher.h"
#include <mdspan>
//==========================================================================//
// File:	 gosFX_gosFX::PertCloud.cpp										//
// Contents: Base gosFX::PertCloud Component								//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//------------------------------------------------------------------------------
//
gosFX::PertCloud__Specification::PertCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	SpinningCloud__Specification(gosFX::PertCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Verify(m_class == gosFX::PertCloudClassID);
	Verify(gos_GetCurrentHeap() == Heap);

	m_size.Load(stream, gfx_version);
	m_perturbation.Load(stream, gfx_version);
	m_pCenterRed.Load(stream, gfx_version);
	m_pCenterGreen.Load(stream, gfx_version);
	m_pCenterBlue.Load(stream, gfx_version);
	m_pCenterAlpha.Load(stream, gfx_version);

	m_particleClassSize = sizeof(gosFX::PertCloud::Particle);
	Verify(gfx_version > 5);
	*stream >> m_vertices;
	m_totalParticleSize =
		sizeof(gosFX::PertCloud::Particle)
		 + m_vertices * sizeof(glm::vec3)
		 + 2 * sizeof(Stuff::RGBAColor);
}

//------------------------------------------------------------------------------
//
gosFX::PertCloud__Specification::PertCloud__Specification(unsigned sides):
	SpinningCloud__Specification(gosFX::PertCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_particleClassSize = sizeof(gosFX::PertCloud::Particle);
	m_vertices = sides+2;
	m_totalParticleSize =
		sizeof(gosFX::PertCloud::Particle)
		 + m_vertices * sizeof(glm::vec3)
		 + 2 * sizeof(Stuff::RGBAColor);
}

//------------------------------------------------------------------------------
//
gosFX::PertCloud__Specification*
	gosFX::PertCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	PertCloud__Specification *spec =
		new gosFX::PertCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::PertCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	SpinningCloud__Specification::Save(stream);

	m_size.Save(stream);
	m_perturbation.Save(stream);
	m_pCenterRed.Save(stream);
	m_pCenterGreen.Save(stream);
	m_pCenterBlue.Save(stream);
	m_pCenterAlpha.Save(stream);

	*stream << m_vertices;
}

//------------------------------------------------------------------------------
//
void 
	gosFX::PertCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	SpinningCloud__Specification::BuildDefaults();

	m_pCenterRed.m_ageCurve.SetCurve(1.0f);
	m_pCenterRed.m_seeded = false;
	m_pCenterRed.m_seedCurve.SetCurve(1.0f);

	m_pCenterGreen.m_ageCurve.SetCurve(1.0f);
	m_pCenterGreen.m_seeded = false;
	m_pCenterGreen.m_seedCurve.SetCurve(1.0f);

	m_pCenterBlue.m_ageCurve.SetCurve(1.0f);
	m_pCenterBlue.m_seeded = false;
	m_pCenterBlue.m_seedCurve.SetCurve(1.0f);

	m_pCenterAlpha.m_ageCurve.SetCurve(1.0f);
	m_pCenterAlpha.m_seeded = false;
	m_pCenterAlpha.m_seedCurve.SetCurve(1.0f);

	m_size.m_ageCurve.SetCurve(1.0f);
	m_size.m_seeded = false;
	m_size.m_seedCurve.SetCurve(1.0f);

	m_perturbation.m_ageCurve.SetCurve(0.25f);
	m_perturbation.m_seeded = false;
	m_perturbation.m_seedCurve.SetCurve(1.0f);

}


//------------------------------------------------------------------------------
//
bool 
	gosFX::PertCloud__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);
	return	SpinningCloud__Specification::IsDataValid(fix_data);
/*
	m_pCenterRed.m_ageCurve.SetCurve(1.0f);
	m_pCenterRed.m_seeded = false;
	m_pCenterRed.m_seedCurve.SetCurve(1.0f);

	m_pCenterGreen.m_ageCurve.SetCurve(1.0f);
	m_pCenterGreen.m_seeded = false;
	m_pCenterGreen.m_seedCurve.SetCurve(1.0f);

	m_pCenterBlue.m_ageCurve.SetCurve(1.0f);
	m_pCenterBlue.m_seeded = false;
	m_pCenterBlue.m_seedCurve.SetCurve(1.0f);

	m_pCenterAlpha.m_ageCurve.SetCurve(1.0f);
	m_pCenterAlpha.m_seeded = false;
	m_pCenterAlpha.m_seedCurve.SetCurve(1.0f);

	m_size.m_ageCurve.SetCurve(1.0f);
	m_size.m_seeded = false;
	m_size.m_seedCurve.SetCurve(1.0f);

	m_perturbation.m_ageCurve.SetCurve(0.25f);
	m_perturbation.m_seeded = false;
	m_perturbation.m_seedCurve.SetCurve(1.0f);
*/

}

//------------------------------------------------------------------------------
//
void
	gosFX::PertCloud__Specification::Copy(PertCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	SpinningCloud__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_size = spec->m_size;
	m_perturbation = spec->m_perturbation;
	m_pCenterRed = spec->m_pCenterRed;
	m_pCenterGreen = spec->m_pCenterGreen;
	m_pCenterBlue = spec->m_pCenterBlue;
	m_pCenterAlpha = spec->m_pCenterAlpha;
	m_vertices = spec->m_vertices;
	gos_PopCurrentHeap();
}

//############################################################################
//##############################  gosFX::PertCloud  ################################
//############################################################################

gosFX::PertCloud::ClassData*
	gosFX::PertCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::PertCloud::InitializeClass()
{
	Verify(!DefaultData);
	DefaultData =
		new ClassData(
			gosFX::PertCloudClassID,
			"gosFX::PertCloud",
			SpinningCloud::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::PertCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::PertCloud::PertCloud(
	Specification *spec,
	unsigned flags
):
	SpinningCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_vertices = Cast_Pointer(glm::vec3*, &m_data[index]);
	index +=
		spec->m_vertices * spec->m_maxParticleCount * sizeof(glm::vec3);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);
	

}

//------------------------------------------------------------------------------
//
gosFX::PertCloud::~PertCloud()
{
}

//------------------------------------------------------------------------------
//
gosFX::PertCloud*
	gosFX::PertCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	PertCloud *cloud = new gosFX::PertCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::PertCloud::AnimateParticle(
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
	Set_Statistic(Pert_Count, Pert_Count+1);
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
	index *= 2;
	m_P_color[index].red = spec->m_pCenterRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pCenterGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pCenterBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pCenterAlpha.ComputeValue(age, seed);

	++index;
	m_P_color[index].red = spec->m_pRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pAlpha.ComputeValue(age, seed);
	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::PertCloud::CreateNewParticle(
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
	//--------------------
	// Set up the particle
	//--------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);

	//
	//----------------------------------------
	// Now we compute the geometry of the pert
	//----------------------------------------
	//
	Verify(spec->m_vertices > 4);
	Stuff::Scalar angle_between = Stuff::Two_Pi/(spec->m_vertices-2);
	Stuff::Scalar radius = spec->m_size.ComputeValue(m_age, particle->m_seed);
	int even = 1;
	particle->m_vertices[0] = glm::vec3(0.0f);
	Stuff::Scalar bound = 0.0f;
    int j=1;
	for (; j<spec->m_vertices-1; j++)
	{
		Stuff::Scalar perturbance =
			even * spec->m_perturbation.ComputeValue(m_age, particle->m_seed);
		Stuff::Scalar temp = perturbance + radius;
		particle->m_vertices[j] =
			glm::vec3(
				mc2::math::sinD(j*angle_between)*temp,
				mc2::math::cosD(j*angle_between)*temp,
				perturbance
			);
		perturbance = temp*temp + perturbance*perturbance;
		if (perturbance > bound)
			bound = perturbance;
		even = -even;
	}
	particle->m_radius = Stuff::Sqrt(bound);
	particle->m_vertices[j] = particle->m_vertices[1];
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
//
void gosFX::PertCloud::Draw(DrawInfo *info)
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
						glm::mat4 pert_to_cloud;
						mc2::math::buildRotation(pert_to_cloud, particle->m_localRotation);
						mc2::math::alignLocalAxisToWorldVector(pert_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::Y_Axis, Stuff::X_Axis);
						mc2::math::buildTranslation(pert_to_cloud, particle->m_localTranslation);

						//
						//----------------------------------------------------
						// Figure out the scale, then transform all the points
						//----------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						for (unsigned v=0; v<spec->m_vertices; ++v)
						{
							glm::vec3 scaled;
							scaled = particle->m_vertices[v] * scale;
							m_P_vertices[vert++] = mc2::math::transformPoint(scaled, pert_to_cloud);
						}
					}
					else
						vert += spec->m_vertices;
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
						glm::mat4 pert_to_cloud;
						mc2::math::buildRotation(pert_to_cloud, particle->m_localRotation);
						mc2::math::alignLocalAxisToWorldVector(pert_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::X_Axis, -1);
						mc2::math::buildTranslation(pert_to_cloud, particle->m_localTranslation);

						//
						//----------------------------------------------------
						// Figure out the scale, then transform all the points
						//----------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						for (unsigned v=0; v<spec->m_vertices; ++v)
						{
							glm::vec3 scaled;
							scaled = particle->m_vertices[v] * scale;
							m_P_vertices[vert++] = mc2::math::transformPoint(scaled, pert_to_cloud);
						}
					}
					else
						vert += spec->m_vertices;
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
					glm::mat4 pert_to_cloud;
					mc2::math::buildRotation(pert_to_cloud, particle->m_localRotation);
					mc2::math::alignLocalAxisToWorldVector(pert_to_cloud, direction_in_cloud, Stuff::Z_Axis, Stuff::Y_Axis, -1);
					mc2::math::buildTranslation(pert_to_cloud, particle->m_localTranslation);

					//
					//----------------------------------------------------
					// Figure out the scale, then transform all the points
					//----------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					for (unsigned v=0; v<spec->m_vertices; ++v)
					{
						glm::vec3 scaled;
						scaled = particle->m_vertices[v] * scale;
						m_P_vertices[vert++] = mc2::math::transformPoint(scaled, pert_to_cloud);
					}
				}
				else
					vert += spec->m_vertices;
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
					glm::mat4 pert_to_cloud;
					mc2::math::buildRotation(pert_to_cloud, particle->m_localRotation);
					mc2::math::buildTranslation(pert_to_cloud, particle->m_localTranslation);

					//
					//----------------------------------------------------
					// Figure out the scale, then transform all the points
					//----------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					for (unsigned v=0; v<spec->m_vertices; ++v)
					{
						glm::vec3 scaled;
						scaled = particle->m_vertices[v] * scale;
						m_P_vertices[vert++] = mc2::math::transformPoint(scaled, pert_to_cloud);
					}
				}
				else
					vert += spec->m_vertices;
			}
		}

		//
		//---------------------
		// Now just do the draw
		//---------------------
		//
        if (mc2::particles::Batcher::is_enabled() && mc2::particles::Batcher::is_oracle_shape_enabled()) {
            MidLevelRenderer::MLRState state;
            state.Combine(info->m_state, spec->m_state);
            using State = MidLevelRenderer::MLRState;
            const auto alpha = state.GetAlphaMode();
            const int blend = alpha == State::OneOneMode ? 2 : alpha == State::AlphaOneMode ? 1 :
                alpha == State::OneZeroMode ? 3 : alpha == State::OneAlphaMode ? 4 :
                alpha == State::OneInvAlphaMode ? 5 : 0;
            auto& batcher = mc2::particles::Batcher::Instance();
            batcher.BeginGroup(0, 0, 0, 1, 1, blend == 3 ? 0 : blend, 0xffffffffu);
            const std::mdspan<glm::vec3, std::dextents<size_t, 2>> vertices(
                m_P_vertices, m_activeParticleCount, spec->m_vertices);
            for (unsigned i = 0; i < m_activeParticleCount; ++i) {
                if (GetParticle(i)->m_age >= 1.0f) continue;
                for (unsigned v = 1; v + 1 < spec->m_vertices; ++v) {
                    for (unsigned corner : {0u, v + 1, v}) {
                        const auto world = mc2::math::transformPoint(vertices[i, corner], local_to_world);
                        const auto& color = m_P_color[i*2 + (corner ? 1 : 0)];
                        mc2::particles::GpuParticle vertex{};
                        vertex.position[0] = world.x; vertex.position[1] = world.y; vertex.position[2] = world.z;
                        vertex.color[0] = color.red; vertex.color[1] = color.green; vertex.color[2] = color.blue;
                        vertex.color[3] = blend == 3 ? 1.0f : color.alpha;
                        vertex.kind_flags = 8u | 32u | (blend == 2 ? 2u : 0u) |
                            (blend == 4 || blend == 5 ? 512u : 0u);
                        batcher.Emit(vertex);
                    }
                }
            }
        }
	}

	SpinningCloud::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::PertCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
