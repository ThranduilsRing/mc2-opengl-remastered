#include"gosfxheaders.hpp"
//==========================================================================//
// File:	 gosFX_SpinningCloud.cpp										//
// Contents: Base gosFX::SpinningCloud Component							//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
//############################################################################
//########################  gosFX::SpinningCloud__Specification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::SpinningCloud__Specification::SpinningCloud__Specification(
	Stuff::RegisteredClass::ClassID class_id,
	Stuff::MemoryStream *stream,
	int gfx_version
):
	ParticleCloud__Specification(class_id, stream, gfx_version)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	//
	//-----------------
	// Load the fcurves
	//-----------------
	//
	m_pSpin.Load(stream, gfx_version);
	m_pScale.Load(stream, gfx_version);

	//
	//-----------------------
	// Load the rotation flag
	//-----------------------
	//
	*stream >> m_randomStartingRotation;
	*stream >> m_alignZUsingX >> m_alignZUsingY;
	if (gfx_version >= 10)
		*stream >> m_alignYUsingVelocity;
	else
		m_alignYUsingVelocity = false;
}

//------------------------------------------------------------------------------
//
gosFX::SpinningCloud__Specification::SpinningCloud__Specification(
	Stuff::RegisteredClass::ClassID class_id
):
	ParticleCloud__Specification(class_id)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_randomStartingRotation = false;
	m_alignZUsingX = false;
	m_alignZUsingY = false;
	m_alignYUsingVelocity = false;
}

//------------------------------------------------------------------------------
//
void
	gosFX::SpinningCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	ParticleCloud__Specification::Save(stream);

	m_pSpin.Save(stream);
	m_pScale.Save(stream);

	*stream << m_randomStartingRotation << m_alignZUsingX << m_alignZUsingY;
	*stream << m_alignYUsingVelocity;
}

//------------------------------------------------------------------------------
//
void 
	gosFX::SpinningCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	ParticleCloud__Specification::BuildDefaults();

	m_randomStartingRotation = false;
	m_alignZUsingX = false;
	m_alignZUsingY = false;
	m_alignYUsingVelocity = false;

	m_pSpin.m_ageCurve.SetCurve(0.0f);
	m_pSpin.m_seeded = false;
	m_pSpin.m_seedCurve.SetCurve(1.0f);

	m_pScale.m_ageCurve.SetCurve(1.0f);
	m_pScale.m_seeded = false;
	m_pScale.m_seedCurve.SetCurve(1.0f);

}


//------------------------------------------------------------------------------
//
bool gosFX::SpinningCloud__Specification::IsDataValid(bool fix_data)
{

    Check_Object(this);
    Stuff::Scalar min,max;
    m_pScale.ExpensiveComputeRange(&min,&max);
    if(min<0.0f) {
        if(fix_data)
        {
            m_pScale.m_ageCurve.SetCurve(1.0f);
            m_pScale.m_seeded = false;
            m_pScale.m_seedCurve.SetCurve(1.0f);
            PAUSE(("Warning: Curve \"pScale\" in Effect \"%s\" Is Out of Range and has been Reset",static_cast<char *>(m_name)));

        }
        else
            return false;
    }
    return ParticleCloud__Specification::IsDataValid(fix_data);
}

//------------------------------------------------------------------------------
//
void
	gosFX::SpinningCloud__Specification::Copy(SpinningCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	ParticleCloud__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_pSpin = spec->m_pSpin;
	m_pScale = spec->m_pScale;

	m_randomStartingRotation = spec->m_randomStartingRotation;
	m_alignZUsingX = spec->m_alignZUsingX;
	m_alignZUsingY = spec->m_alignZUsingY;
	m_alignYUsingVelocity = spec->m_alignYUsingVelocity;

	gos_PopCurrentHeap();
}

//############################################################################
//##############################  gosFX::SpinningCloud  ################################
//############################################################################

gosFX::SpinningCloud::ClassData*
	gosFX::SpinningCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::SpinningCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			SpinningCloudClassID,
			"gosFX::SpinningCloud",
			ParticleCloud::DefaultData,
			nullptr,
			nullptr
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::SpinningCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::SpinningCloud::SpinningCloud(
	ClassData *class_data,
	Specification *spec,
	unsigned flags
):
	ParticleCloud(class_data, spec, flags)
{
	Verify(gos_GetCurrentHeap() == Heap);
}

//------------------------------------------------------------------------------
//
bool
	gosFX::SpinningCloud::Execute(ExecuteInfo *info)
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
		unsigned i=0;

		//
		//-------------------------------------------------------------------
		// If there is no bounds yet, we need to create our extent box around
		// the first legal point we find
		//-------------------------------------------------------------------
		//
		while (i<m_activeParticleCount)
		{
			Particle *particle = GetParticle(i++);
			Check_Object(particle);

			//
			//-----------------------------------------------------------
			// We have found our first particle, so put the box around it
			//-----------------------------------------------------------
			//
			if (particle->m_age < 1.0f)
			{
				box.max.x =
					particle->m_localTranslation.x
					 + particle->m_radius*particle->m_scale;
				box.min.x =
					particle->m_localTranslation.x
					 - particle->m_radius*particle->m_scale;
				box.max.y =
					particle->m_localTranslation.y
					 + particle->m_radius*particle->m_scale;
				box.min.y =
					particle->m_localTranslation.y
					 - particle->m_radius*particle->m_scale;
				box.max.z =
					particle->m_localTranslation.z
					 + particle->m_radius*particle->m_scale;
				box.min.z =
					particle->m_localTranslation.z
					 - particle->m_radius*particle->m_scale;
				break;
			}
		}

		//
		//-----------------------------
		// Look for the other particles
		//-----------------------------
		//
		while (i<m_activeParticleCount)
		{
			Particle *particle = GetParticle(i++);
			Check_Object(particle);
			if (particle->m_age < 1.0f)
			{
				mc2::math::Aabb local_box;
				local_box.min.x =
					particle->m_localTranslation.x
					 - particle->m_radius*particle->m_scale;
				local_box.max.x =
					particle->m_localTranslation.x
					 + particle->m_radius*particle->m_scale;
				local_box.min.y =
					particle->m_localTranslation.y
					 - particle->m_radius*particle->m_scale;
				local_box.max.y =
					particle->m_localTranslation.y
					 + particle->m_radius*particle->m_scale;
				local_box.min.z =
					particle->m_localTranslation.z
					 - particle->m_radius*particle->m_scale;
				local_box.max.z =
					particle->m_localTranslation.z
					 + particle->m_radius*particle->m_scale;
				box = mc2::math::unite(box, local_box);
			}
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
	gosFX::SpinningCloud::CreateNewParticle(
		unsigned index,
		glm::vec3 *translation
	)
{
	//
	//---------------------------
	// Let our parent do creation
	//---------------------------
	//
	ParticleCloud::CreateNewParticle(index, translation);

	//
	//--------------------------------------
	// Figure out where the particle is born
	//--------------------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	Stuff::Scalar seed = particle->m_seed;
	Stuff::Scalar age = m_age;
	particle->m_localTranslation = *translation;

	//
	//---------------------------------
	// Figure out the particle rotation
	//---------------------------------
	//
	particle->m_angularVelocity.x = spec->m_pSpin.ComputeValue(age, seed);
	particle->m_angularVelocity.y = spec->m_pSpin.ComputeValue(age, seed);
	particle->m_angularVelocity.z = spec->m_pSpin.ComputeValue(age, seed);

	//
	//-----------------------------------------------------
	// If we are aligning with velocity, deal with that now
	//-----------------------------------------------------
	//
	if (spec->m_alignYUsingVelocity)
	{
		glm::mat4 basis = glm::mat4(1.0f);
		mc2::math::alignLocalAxisToWorldVector(basis, particle->m_localLinearVelocity, Stuff::Y_Axis, Stuff::X_Axis, Stuff::Z_Axis);
		particle->m_localRotation = mc2::math::quatFromRotation(basis);
	}
	else if (spec->m_randomStartingRotation)
	{
		// Match the shipped MSVC draw order, independent of argument evaluation.
		const auto roll = Stuff::Two_Pi * Stuff::Random::GetFraction();
		const auto yaw = Stuff::Pi * Stuff::Random::GetFraction();
		const auto pitch = Stuff::Two_Pi * Stuff::Random::GetFraction();
		const mc2::math::Euler rotation(pitch, yaw, roll);
		particle->m_localRotation = mc2::math::quatFromEuler(rotation);
	}
	else
		particle->m_localRotation = mc2::math::QuatIdentity;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::SpinningCloud::AnimateParticle(
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

	// Q2-S0 instrumentation only. Per-particle rdtsc bucket (NO Tracy zone —
	// per-particle Tracy scopes are forbidden by the 100ns floor). Placed after
	// the dead-particle early-return so only live particles are timed.

	glm::vec3 *translation = &particle->m_localTranslation;
	glm::quat *rotation = &particle->m_localRotation;
	glm::vec3 *velocity = &particle->m_localLinearVelocity;
	int sim_mode = GetSimulationMode();
	if (sim_mode == DynamicWorldSpaceSimulationMode)
	{
		particle->m_worldTranslation = mc2::math::transformPoint(*translation, m_localToWorld);
		glm::mat4 local_rot = mc2::math::rotationMatrix(*rotation);
		glm::mat4 world_rot;
		world_rot = local_rot * m_localToWorld;
		particle->m_worldRotation = mc2::math::quatFromRotation(world_rot);
		particle->m_worldLinearVelocity = mc2::math::transformVector(*velocity, m_localToWorld);
		translation = &particle->m_worldTranslation;
		rotation = &particle->m_worldRotation;
		velocity = &particle->m_worldLinearVelocity;
	}
	Check_Object(translation);
	Check_Object(rotation);
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
	glm::vec3 accel(glm::vec3(0.0f));
	{
	// Q2-S0b: curve-heavy force calc (7 of 8 ComputeValue calls in this
	// function) — the region the curve-cache / GPU-offload decision hinges on.
	Stuff::Scalar drag = -spec->m_pDrag.ComputeValue(age, seed);
	Max_Clamp(drag, 0.0f);
	glm::vec3 ether;
	ether.x = spec->m_pEtherVelocityX.ComputeValue(age, seed);
	ether.y = spec->m_pEtherVelocityY.ComputeValue(age, seed);
	ether.z = spec->m_pEtherVelocityZ.ComputeValue(age, seed);

	//
	//-------------------------------------------------------------------
	// Deal with pseudo-world simulation.  In this mode, we interpret the
	// forces as if they are already in worldspace, and we transform them
	// back to local space
	//-------------------------------------------------------------------
	//
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
	}  // end B_FX_FORCES scope

	//
	//-------------------------------------------------
	// Compute the particle's new velocity and position
	//-------------------------------------------------
	//
	{
	// Q2-S0b: velocity/position/rotation integration (no curve eval).
	Stuff::Scalar time_slice =
		static_cast<Stuff::Scalar>(till - m_lastRan);
	(*velocity) = *velocity + accel * time_slice;
	(*translation) = *translation + *velocity * time_slice;

	//
	//-----------------------
	// Deal with the rotation
	//-----------------------
	//
	if (!spec->m_alignYUsingVelocity)
	{
		glm::vec3 omega(particle->m_angularVelocity);
		omega *= time_slice;
		glm::quat omega_q;
		omega_q = mc2::math::quatFromRotationVector(omega);
		(*rotation) = mc2::math::composeQuat(omega_q, *rotation);
		(*rotation) = glm::normalize((*rotation));
	}
	}  // end B_FX_INTEGRATE scope

	//
	//---------------------------------------------------------------------
	// If we are unparented, we need to transform the velocity and position
	// data back into the NEW local space
	//---------------------------------------------------------------------
	//
	if (sim_mode == DynamicWorldSpaceSimulationMode)
	{
		Check_Object(world_to_new_local);
		particle->m_localLinearVelocity = mc2::math::transformVector(*velocity, *world_to_new_local);
		particle->m_localTranslation = mc2::math::transformPoint(*translation, *world_to_new_local);
		glm::mat4 world_rot = mc2::math::rotationMatrix(*rotation);
		glm::mat4 local_rot;
		local_rot = world_rot * *world_to_new_local;
		particle->m_localRotation = mc2::math::quatFromRotation(local_rot);
	}

	//
	//------------------------------------------------------------
	// If we are aligning Y using velocity, do the calculation now
	//------------------------------------------------------------
	//
	if (spec->m_alignYUsingVelocity)
	{
		glm::mat4 basis = glm::mat4(1.0f);
		mc2::math::alignLocalAxisToWorldVector(basis, particle->m_localLinearVelocity, Stuff::Y_Axis, Stuff::X_Axis, Stuff::Z_Axis);
		particle->m_localRotation = mc2::math::quatFromRotation(basis);
	}

	//
	//------------------
	// Animate the scale
	//------------------
	//
	particle->m_scale = spec->m_pScale.ComputeValue(age, seed);
	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::SpinningCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
