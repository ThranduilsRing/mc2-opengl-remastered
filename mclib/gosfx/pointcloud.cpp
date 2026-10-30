#include"gosfxheaders.hpp"
#include "particles/batcher.h"
#include "particles/spec_library.h"
#include "Gates/gate.h"

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline.

#include<cstdio>   // VFX-POINTCLOUD-ORACLE diagnostics
#include "mc2fmt.h"
//==========================================================================//
// File:	 gosFX_PointCloud.cpp											//
// Contents: Base gosFX::PointCloud Component								//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
//############################################################################
//########################  gosFX::PointCloud__Specification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::PointCloud__Specification::PointCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	ParticleCloud__Specification(gosFX::PointCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Verify(m_class == PointCloudClassID);
	Verify(gos_GetCurrentHeap() == Heap);
	m_totalParticleSize = gosFX::PointCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::PointCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud__Specification::PointCloud__Specification():
	ParticleCloud__Specification(gosFX::PointCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_totalParticleSize = gosFX::PointCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::PointCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud__Specification*
	gosFX::PointCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	PointCloud__Specification *spec =
		new gosFX::PointCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//############################################################################
//##############################  gosFX::PointCloud  ################################
//############################################################################

gosFX::PointCloud::ClassData*
	gosFX::PointCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			PointCloudClassID,
			"gosFX::PointCloud",
			ParticleCloud::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud::PointCloud(
	Specification *spec,
	unsigned flags
):
	ParticleCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);


	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_localTranslation = Cast_Pointer(glm::vec3*, &m_data[index]);
	index += spec->m_maxParticleCount*sizeof(glm::vec3);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);

}

//------------------------------------------------------------------------------
//
gosFX::PointCloud::~PointCloud() = default;

//------------------------------------------------------------------------------
//
gosFX::PointCloud*
	gosFX::PointCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	PointCloud *cloud = new gosFX::PointCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::PointCloud::Execute(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	//
	//----------------------------------------
	// If we aren't supposed to execute, don't
	//----------------------------------------
	//
	if (!IsExecuted())
		return false;

	//
	//------------------------------------------------------------------
	// Animate the particles.  If it is time for us to die, return false
	//------------------------------------------------------------------
	//
	if (!ParticleCloud::Execute(info))
		return false;

	//
	//-----------------------------------------------------------------------
	// If there are active particles to animate, get the current center point
	// of the bounds
	//-----------------------------------------------------------------------
	//
	if (m_activeParticleCount > 0)
	{
		mc2::math::Aabb box;
		glm::vec3 *vertex = &m_P_localTranslation[0];
		unsigned i=0;

		//
		//-------------------------------------------------------------------
		// If there is no bounds yet, we need to create our extent box around
		// the first legal point we find
		//-------------------------------------------------------------------
		//
		while (i<m_activeParticleCount)
		{
			Particle *particle = GetParticle(i);
			Check_Object(particle);
			if (particle->m_age < 1.0f)
			{
				Check_Object(vertex);
				box.max.x = vertex->x;
				box.min.x = vertex->x;
				box.max.y = vertex->y;
				box.min.y = vertex->y;
				box.max.z = vertex->z;
				box.min.z = vertex->z;
				++vertex;
				++i;
				break;
			}
			++vertex;
			++i;
		}

		//
		//-----------------------------
		// Look for the other particles
		//-----------------------------
		//
		while (i<m_activeParticleCount)
		{
			Particle *particle = GetParticle(i);
			Check_Object(particle);
			if (particle->m_age < 1.0f)
			{
				Check_Object(vertex);
				if (vertex->x > box.max.x)
					box.max.x = vertex->x;
				else if (vertex->x < box.min.x)
					box.min.x = vertex->x;

				if (vertex->y > box.max.y)
					box.max.y = vertex->y;
				else if (vertex->y < box.min.y)
					box.min.y = vertex->y;

				if (vertex->z > box.max.z)
					box.max.z = vertex->z;
				else if (vertex->z < box.min.z)
					box.min.z = vertex->z;
			}
			++vertex;
			++i;
		}

		//
		//------------------------------------
		// Now, build a info->m_bounds around this box
		//------------------------------------
		//
		Verify(box.max.x >= box.min.x);
		Verify(box.max.y >= box.min.y);
		Verify(box.max.z >= box.min.z);
		mc2::math::Obb local_bounds = mc2::math::Obb();
		local_bounds.axisExtents.x = 0.5f * (box.max.x - box.min.x);
		local_bounds.axisExtents.y = 0.5f * (box.max.y - box.min.y);
		local_bounds.axisExtents.z = 0.5f * (box.max.z - box.min.z);
		local_bounds.localToParent[0][3] = box.min.x + local_bounds.axisExtents.x;
		local_bounds.localToParent[1][3] = box.min.y + local_bounds.axisExtents.y;
		local_bounds.localToParent[2][3] = box.min.z + local_bounds.axisExtents.z;
		local_bounds.sphereRadius = glm::length(local_bounds.axisExtents);
		if (local_bounds.sphereRadius < Stuff::SMALL)
			local_bounds.sphereRadius = 0.01f;
		mc2::math::Obb parent_bounds;
		parent_bounds = mc2::math::multiply(local_bounds, m_localToParent);
		(*info->m_bounds) = mc2::math::unite(*info->m_bounds, parent_bounds);
	}

	//
	//----------------------------------------------
	// Tell our caller that we get to keep executing
	//----------------------------------------------
	//
	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::CreateNewParticle(
		unsigned index,
		glm::vec3 *translation
	)
{
	Check_Object(this);
	Check_Pointer(translation);

	//
	//-----------------------------------------------------------------------
	// Let our parent do creation, then turn on the particle in the cloud and
	// set its position
	//-----------------------------------------------------------------------
	//
	ParticleCloud::CreateNewParticle(index, translation);
	m_P_localTranslation[index] = *translation;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::PointCloud::AnimateParticle(
		unsigned index,
		const glm::mat4 *world_to_new_local,
		Stuff::Time till
	)
{
	Check_Object(this);

	//
	//-----------------------------------------------------------------------
	// If this cloud is unparented, we need to transform the point from local
	// space into world space and set the internal position/velocity pointers
	// to these temporary values
	//-----------------------------------------------------------------------
	//
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	Stuff::Scalar age = particle->m_age;
	if (age >= 1.0f)
		return false;
	Set_Statistic(Point_Count, Point_Count+1);
	glm::vec3 *velocity = &particle->m_localLinearVelocity;
	glm::vec3 *translation = &m_P_localTranslation[index];
	int sim_mode = GetSimulationMode();
	if (sim_mode == DynamicWorldSpaceSimulationMode)
	{
		Check_Object(translation);
		Check_Object(velocity);
		particle->m_worldLinearVelocity = mc2::math::transformVector(*velocity, m_localToWorld);
		particle->m_worldTranslation = mc2::math::transformPoint(*translation, m_localToWorld);
		translation = &particle->m_worldTranslation;
		velocity = &particle->m_worldLinearVelocity;
	}
	Check_Object(translation);
	Check_Object(velocity);

	//
	//------------------------------------------------------------------
	// First, calculate the drag on the particle.  Drag can never assist
	// velocity
	//------------------------------------------------------------------
	//
	Stuff::Scalar seed = particle->m_seed;
	Specification *spec = GetSpecification();
	Check_Object(spec);
	glm::vec3 ether;
	ether.x = spec->m_pEtherVelocityX.ComputeValue(age, seed);
	ether.y = spec->m_pEtherVelocityY.ComputeValue(age, seed);
	ether.z = spec->m_pEtherVelocityZ.ComputeValue(age, seed);
	glm::vec3 accel(glm::vec3(0.0f));

	//
	//-------------------------------------------------------------------
	// Deal with pseudo-world simulation.  In this mode, we interpret the
	// forces as if they are already in worldspace, and we transform them
	// back to local space
	//-------------------------------------------------------------------
	//
	Stuff::Scalar drag = -spec->m_pDrag.ComputeValue(age, seed);
	Max_Clamp(drag, 0.0f);
	if (sim_mode == StaticWorldSpaceSimulationMode)
	{
		glm::mat4 world_to_effect;
		world_to_effect = glm::inverse(m_localToWorld);
		glm::vec3 local_ether;
		local_ether = (mc2::math::transformVector(ether, glm::inverse(world_to_effect)));
		glm::vec3 rel_vel;
		rel_vel = *velocity - local_ether;
		accel = rel_vel * drag;

		//
		//-----------------------------------------
		// Now, add in acceleration of the particle
		//-----------------------------------------
		//
		glm::vec3 world_accel;
		world_accel.x = spec->m_pAccelerationX.ComputeValue(age, seed);
		world_accel.y = spec->m_pAccelerationY.ComputeValue(age, seed);
		world_accel.z = spec->m_pAccelerationZ.ComputeValue(age, seed);
		glm::vec3 local_accel;
		local_accel = mc2::math::transformVector(world_accel, world_to_effect);
		accel += local_accel;
	}

	//
	//----------------------------------------------------------------------
	// Otherwise, just add the forces in the same space the particles are in
	//----------------------------------------------------------------------
	//
	else
	{
		glm::vec3 rel_vel;
		rel_vel = *velocity - ether;
		accel = rel_vel * drag;

		//
		//-----------------------------------------
		// Now, add in acceleration of the particle
		//-----------------------------------------
		//
		accel.x += spec->m_pAccelerationX.ComputeValue(age, seed);
		accel.y += spec->m_pAccelerationY.ComputeValue(age, seed);
		accel.z += spec->m_pAccelerationZ.ComputeValue(age, seed);
	}

	//
	//-------------------------------------------------
	// Compute the particle's new velocity and position
	//-------------------------------------------------
	//
	Stuff::Scalar time_slice =
		static_cast<Stuff::Scalar>(till - m_lastRan);
	(*velocity) = *velocity + accel * time_slice;
	(*translation) = *translation + *velocity * time_slice;

	//
	//---------------------------------------------------------------------
	// If we are unparented, we need to transform the velocity and position
	// data back into the NEW local space
	//---------------------------------------------------------------------
	//
	if (sim_mode == DynamicWorldSpaceSimulationMode)
	{
		Check_Object(world_to_new_local);
		particle->m_localLinearVelocity = mc2::math::transformVector(particle->m_worldLinearVelocity, *world_to_new_local);
		m_P_localTranslation[index] = mc2::math::transformPoint(particle->m_worldTranslation, *world_to_new_local);
	}

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
	return true;
}

//------------------------------------------------------------------------------
//
void gosFX::PointCloud::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	// Accepted primitives use the native stream; the kill-switch suppresses them.

	if (m_activeParticleCount)
	{
		Specification *spec = GetSpecification();
		Check_Object(spec);
        if (mc2::gate::get<"MC2_GPU_PARTICLES_LOG">()) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                mc2::fmt::print_err("[VFX_PRIMITIVE] class=PointCloud live={:d} route={}\n",
                    mc2::fmt::as_u32(m_activeParticleCount),
                    mc2::particles::Batcher::is_enabled() ? "native" : "off");
            }
        }
		const auto local_to_world = m_localToParent * *info->m_parentToWorld;
	
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
                const auto world = mc2::math::transformPoint(m_P_localTranslation[i], local_to_world);
                for (unsigned corner = 0; corner < 3; ++corner) {
                    mc2::particles::GpuParticle vertex{};
                    vertex.position[0] = world.x;
                    vertex.position[1] = world.y;
                    vertex.position[2] = world.z;
                    const auto& color = m_P_color[i];
                    vertex.color[0] = color.red;
                    vertex.color[1] = color.green;
                    vertex.color[2] = color.blue;
                    vertex.color[3] = am == MidLevelRenderer::MLRState::OneZeroMode ? 1.0f : color.alpha;
                    vertex.velocity[0] = corner == 1 ? 2.4f : 0.0f;
                    vertex.velocity[1] = corner == 2 ? 2.4f : 0.0f;
                    vertex.kind_flags = 8u | 32u | 128u | (additive ? 2u : 0u);
                    batcher.Emit(vertex, GetParticle(i), corner);
                }
            }
        }
	}
	ParticleCloud::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

void gosFX::PointCloud::PrepareOracle() {
        auto* base = mc2::particles::SpecLibrary::Instance()->Find("Missile_Flare");
        if (base && base->GetClassID() == gosFX::PointCloudClassID) {
            auto* point = static_cast<gosFX::PointCloud::Specification*>(base);
            point->m_maxParticleCount = 32;
            point->m_startingPopulation.SetCurve(8.0f);
            point->m_particlesPerSecond.SetCurve(30.0f);
            point->m_emitterSizeX.SetCurve(20.0f);
            point->m_emitterSizeY.SetCurve(20.0f);
            point->m_emitterSizeZ.SetCurve(20.0f);
            point->m_pRed.m_ageCurve.SetCurve(0.0f);
            point->m_pGreen.m_ageCurve.SetCurve(1.0f);
            point->m_pBlue.m_ageCurve.SetCurve(1.0f);
            point->m_pAlpha.m_ageCurve.SetCurve(1.0f);
        }
}
