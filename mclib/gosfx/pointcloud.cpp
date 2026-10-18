#include"gosfxheaders.hpp"
#include "mlr_cloud_draw.hpp"
#include<mlr/mlrpointcloud.hpp>

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline.
#include"particles/batcher.h"

#include<cstdio>   // VFX-POINTCLOUD-ORACLE diagnostics
#include<cmath>    // std::min / std::max fallback (also in gosfxheaders)
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

	gos_PushCurrentHeap(MidLevelRenderer::Heap);
	m_cloudImplementation =
		std::make_unique<MidLevelRenderer::MLRPointCloud>(spec->m_maxParticleCount);
	Register_Object(m_cloudImplementation.get());
	gos_PopCurrentHeap();

	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_localTranslation = Cast_Pointer(glm::vec3*, &m_data[index]);
	index += spec->m_maxParticleCount*sizeof(glm::vec3);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);

	m_cloudImplementation->SetData(
		Cast_Pointer(const int *, &m_activeParticleCount),
		m_P_localTranslation,
		m_P_color
	);
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud::~PointCloud()
{
	Unregister_Object(m_cloudImplementation.get());
}

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
	m_cloudImplementation->TurnOn(index);
	Verify(m_cloudImplementation->IsOn(index));
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
void gosFX::PointCloud::DestroyParticle(unsigned index)
{
	Check_Object(this);

	m_cloudImplementation->TurnOff(index);
	Verify(!m_cloudImplementation->IsOn(index));
	ParticleCloud::DestroyParticle(index);
}

//------------------------------------------------------------------------------
//
void gosFX::PointCloud::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	// Live particles retain birth randomness; rendering must not resample it.
	// An empty cloud emits nothing but still draws its child effects.
	if (mc2::particles::Batcher::is_enabled()) {
		if (m_activeParticleCount > 0) {
			Specification *spec = GetSpecification();
			Check_Object(spec);
			const uint32_t mlrTex =
				static_cast<uint32_t>(spec->m_state.GetTextureHandle());
			int blendMode = 0;
			{
				const MidLevelRenderer::MLRState::AlphaMode am =
					spec->m_state.GetAlphaMode();
				// MLR draws both as pure additive (ONE,ONE), alpha ignored: AlphaOneMode leaves the
				// renderer state it inherits, and the card data (alpha 0 halo cards) relies on that.
				if (am == MidLevelRenderer::MLRState::OneOneMode ||
				    am == MidLevelRenderer::MLRState::AlphaOneMode)
					blendMode = 2;
			}
			glm::mat4 local_to_world;
			local_to_world = m_localToParent * *info->m_parentToWorld;

			mc2::particles::Batcher &batcher = mc2::particles::Batcher::Instance();
			batcher.BeginGroup(mlrTex, 0.0f, 0.0f, 1.0f, 1.0f, blendMode, 0u);

			int harvested = 0;
			float minAlpha =  3.0e38f, maxAlpha = -3.0e38f;
			float minAge = 1.0f, maxAge = 0.0f;

			for (int i = 0; i < m_activeParticleCount; ++i) {
				Particle *p = GetParticle(i);
				Check_Object(p);
				if (p->m_age >= 1.0f) continue;   // skip dead slots

				glm::vec3 wc;
				wc = mc2::math::transformPoint(m_P_localTranslation[i], local_to_world);

				const Stuff::RGBAColor &c = m_P_color[i];

				mc2::particles::GpuParticle gp = {};
				gp.position[0] = wc.x;
				gp.position[1] = wc.y;
				gp.position[2] = wc.z;
				gp.color[0]    = c.red;
				gp.color[1]    = c.green;
				gp.color[2]    = c.blue;
				gp.color[3]    = c.alpha;
				// Preserve the harvest's existing billboard radius.
				gp.size        = 4.0f;
				gp.age         = static_cast<float>(p->m_age);  // normalized [0,1]
				gp.lifetime    = 1.0f;                           // normalized sentinel (real seconds = 1.0f/p->m_ageRate)

				batcher.Emit(gp);
				++harvested;
				if (c.alpha < minAlpha) minAlpha = c.alpha;
				if (c.alpha > maxAlpha) maxAlpha = c.alpha;
				if (gp.age < minAge) minAge = gp.age;
				if (gp.age > maxAge) maxAge = gp.age;
			}

			if (mc2::particles::Batcher::is_log_enabled()) {
				static bool s_first = false;
				if (!s_first && harvested > 0) {
					s_first = true;
					const double ageLo = static_cast<double>(minAge);
					const double ageHi = static_cast<double>(maxAge);
					mc2::fmt::print_err("[VFX_ORACLE v1] class=PointCloud FIRST_HARVEST spec=\"{}\" active={:d} harvested={:d} alpha=[{:.3f},{:.3f}] size=4.0 spin=none atlas=none ageRange=[{:.3f},{:.3f}]\n", mc2::fmt::safe(static_cast<const char*>(spec->m_name)), mc2::fmt::as_i32(m_activeParticleCount), mc2::fmt::as_i32(harvested), static_cast<double>(minAlpha), static_cast<double>(maxAlpha), ageLo, ageHi);
					std::fflush(stderr);
				}
				static unsigned long long s_calls = 0, s_harvTotal = 0;
				s_harvTotal += static_cast<unsigned>(harvested);
				if ((++s_calls % 240ull) == 0ull) {
					const double aLo = (harvested > 0) ? static_cast<double>(minAlpha) : 0.0;
					const double aHi = (harvested > 0) ? static_cast<double>(maxAlpha) : 0.0;
					mc2::fmt::print_err("[VFX_ORACLE v1] class=PointCloud calls={:d} active_this_call={:d} harvested_this_call={:d} emitted_this_call={:d} harvestedTotal={:d} fallback=0 alpha_this_call=[{:.3f},{:.3f}]\n", mc2::fmt::as_u64(s_calls), mc2::fmt::as_i32(m_activeParticleCount), mc2::fmt::as_i32(harvested), mc2::fmt::as_i32(harvested), mc2::fmt::as_u64(s_harvTotal), aLo, aHi);
					std::fflush(stderr);
				}
			}

		}

		Effect::Draw(info);
		return;
	}

	if (m_activeParticleCount)
	{
		Specification *spec = GetSpecification();
		Check_Object(spec);
		MlrCloudDraw submission(info, spec->m_state, m_cloudImplementation.get(), m_localToParent);
	
		submission.submit(info);
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

//------------------------------------------------------------------------------
// Both render arms retain the same particle birth and lifetime state.
void
	gosFX::PointCloud::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	ParticleCloud::Start(info);
}
