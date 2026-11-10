#include <algorithm>

#include"gosfxheaders.hpp"
//==========================================================================//
// File:	 gosFX_DebrisCloud.cpp											//
// Contents: Base gosFX::DebrisCloud Component								//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
//############################################################################
//########################  gosFX::DebrisCloud__Specification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::DebrisCloud__Specification::DebrisCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	Effect__Specification(gosFX::DebrisCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Verify(m_class == DebrisCloudClassID);
	Verify(gos_GetCurrentHeap() == Heap);

	m_minimumDeviation.Load(stream, gfx_version);	//	ConstantCurve
	m_maximumDeviation.Load(stream, gfx_version);	//	SplineCurve
	m_startingSpeed.Load(stream, gfx_version);		//	SeededCurveOf<ComplexCurve, ComplexCurve,Curve::e_ComplexComplexType>
	m_pLifeSpan.Load(stream, gfx_version);			//	SeededCurveOf<ComplexCurve, SplineCurve,Curve::e_ComplexSplineType>
	m_pEtherVelocityY.Load(stream, gfx_version);		//	SeededCurveOf<ConstantCurve, LinearCurve,Curve::e_ConstantLinearType>
	m_pAccelerationY.Load(stream, gfx_version);		//	SeededCurveOf<SplineCurve, LinearCurve,Curve::e_SplineLinearType>
	m_pDrag.Load(stream, gfx_version);				//	SeededCurveOf<ComplexCurve, ComplexCurve,Curve::e_ComplexComplexType>
	m_pAlpha.Load(stream, gfx_version);				//	SeededCurveOf<ComplexCurve, LinearCurve,Curve::e_ComplexLinearType>
	m_pSpin.Load(stream, gfx_version);				//	SeededCurveOf<ConstantCurve, LinearCurve,Curve::e_ConstantLinearType>

	*stream >> centerOfForce;			//	Stuff::Point3D

	LoadGeometry(stream);
}

//------------------------------------------------------------------------------
//
gosFX::DebrisCloud__Specification::DebrisCloud__Specification():
	Effect__Specification(gosFX::DebrisCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	debrisPieces.resize(0);
	debrisPositions.resize(0);
	debrisSpheres.resize(0);
	debrisSeed.resize(0);
}

//------------------------------------------------------------------------------
//
gosFX::DebrisCloud__Specification::~DebrisCloud__Specification()
{
	Check_Object(this);

	int i, nrOfParticles = static_cast<int>(debrisPieces.size());

	for(i=0;i<nrOfParticles;i++)
	{
		Check_Object(debrisPieces[i]);
		debrisPieces[i]->DetachReference();
	}

	debrisPieces.resize(0);
	debrisPositions.resize(0);
	debrisSpheres.resize(0);
	debrisSeed.resize(0);
}

//------------------------------------------------------------------------------
//
gosFX::DebrisCloud__Specification*
	gosFX::DebrisCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	DebrisCloud__Specification *spec =
		new gosFX::DebrisCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	Effect__Specification::Save(stream);

	m_minimumDeviation.Save(stream);	//	ConstantCurve
	m_maximumDeviation.Save(stream);	//	SplineCurve
	m_startingSpeed.Save(stream);		//	SeededCurveOf<ComplexCurve, ComplexCurve,Curve::e_ComplexComplexType>
	m_pLifeSpan.Save(stream);			//	SeededCurveOf<ComplexCurve, SplineCurve,Curve::e_ComplexSplineType>
	m_pEtherVelocityY.Save(stream);		//	SeededCurveOf<ConstantCurve, LinearCurve,Curve::e_ConstantLinearType>
	m_pAccelerationY.Save(stream);		//	SeededCurveOf<SplineCurve, LinearCurve,Curve::e_SplineLinearType>
	m_pDrag.Save(stream);				//	SeededCurveOf<ComplexCurve, ComplexCurve,Curve::e_ComplexComplexType>
	m_pAlpha.Save(stream);				//	SeededCurveOf<ComplexCurve, LinearCurve,Curve::e_ComplexLinearType>
	m_pSpin.Save(stream);				//	SeededCurveOf<ConstantCurve, LinearCurve,Curve::e_ConstantLinearType>

	*stream << centerOfForce;			//	Stuff::Point3D

	MidLevelRenderer::WriteMLRVersion(stream);

	int i, nrOfParticles = static_cast<int>(debrisPieces.size());
	*stream << nrOfParticles;

	for(i=0;i<nrOfParticles;i++)
	{
		*stream << debrisPositions[i];

		*stream << debrisSpheres[i].center;
		*stream << debrisSpheres[i].radius;

		Check_Object(debrisPieces[i]);
		debrisPieces[i]->Save(stream);
	}
}

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud__Specification::Copy(DebrisCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	Effect__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);

	m_minimumDeviation = spec->m_minimumDeviation;	//	ConstantCurve
	m_maximumDeviation = spec->m_maximumDeviation;	//	SplineCurve
	m_startingSpeed = spec->m_startingSpeed;		//	SeededCurveOf<ComplexCurve, ComplexCurve,Curve::e_ComplexComplexType>
	m_pLifeSpan = spec->m_pLifeSpan;				//	SeededCurveOf<ComplexCurve, SplineCurve,Curve::e_ComplexSplineType>
	m_pEtherVelocityY = spec->m_pEtherVelocityY;	//	SeededCurveOf<ConstantCurve, LinearCurve,Curve::e_ConstantLinearType>
	m_pAccelerationY = spec->m_pAccelerationY;		//	SeededCurveOf<SplineCurve, LinearCurve,Curve::e_SplineLinearType>
	m_pDrag = spec->m_pDrag;						//	SeededCurveOf<ComplexCurve, ComplexCurve,Curve::e_ComplexComplexType>
	m_pAlpha = spec->m_pAlpha;						//	SeededCurveOf<ComplexCurve, LinearCurve,Curve::e_ComplexLinearType>
	m_pSpin = spec->m_pSpin;						//	SeededCurveOf<ConstantCurve, LinearCurve,Curve::e_ConstantLinearType>

	centerOfForce = spec->centerOfForce;			//	Stuff::Point3D

	int i, nrOfParticles = static_cast<int>(spec->debrisPieces.size());

	debrisPieces.resize(nrOfParticles);
	debrisPositions.resize(nrOfParticles);
	debrisSpheres.resize(nrOfParticles);
	debrisSeed.resize(nrOfParticles);

	for(i=0;i<nrOfParticles;i++)
	{
		debrisPositions[i] = spec->debrisPositions[i];
		debrisSpheres[i] = spec->debrisSpheres[i];

		debrisPieces[i] = spec->debrisPieces[i];

		Check_Object(debrisPieces[i]);
		debrisPieces[i]->AttachReference();

		debrisSeed[i] = spec->debrisSeed[i];
	}
	gos_PopCurrentHeap();
}


//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud__Specification::LoadGeometry(Stuff::MemoryStream *stream)
{
	int i, mlrVersion, nrOfParticles;

	mlrVersion = MidLevelRenderer::ReadMLRVersion(stream);
	*stream >> nrOfParticles;

	debrisPieces.resize(nrOfParticles);
	debrisPositions.resize(nrOfParticles);
	debrisSpheres.resize(nrOfParticles);
	debrisSeed.resize(nrOfParticles);

	Stuff::Scalar minRadius = 100000000000.0f, maxRadius = 0.0f;

	for(i=0;i<nrOfParticles;i++)
	{
		*stream >> debrisPositions[i];

		glm::vec3 center;
		*stream >> center;
		debrisSpheres[i].center = center;
		*stream >> debrisSpheres[i].radius;

		minRadius = std::min(minRadius, debrisSpheres[i].radius);
		maxRadius = std::max(maxRadius, debrisSpheres[i].radius);

		//
		//---------------
		// Load the shape
		//---------------
		//
		debrisPieces[i] =
			MidLevelRenderer::MLRShape::Make(
				stream,
				mlrVersion
			);
		Register_Object(debrisPieces[i]);
	}

	Verify(maxRadius > minRadius);
	for(i=0;i<nrOfParticles;i++)
	{
		debrisSeed[i] = (debrisSpheres[i].radius-minRadius)/(maxRadius-minRadius);
	}
}

//------------------------------------------------------------------------------
//
void 
	gosFX::DebrisCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	Effect__Specification::BuildDefaults();

	centerOfForce = glm::vec3(0.0f, 00.0f, 0.0f);

	m_lifeSpan.SetCurve(20.0f);

	m_minimumDeviation.SetCurve(0.0f);
	m_maximumDeviation.SetCurve(Stuff::Pi_Over_6);

	m_startingSpeed.m_ageCurve.SetCurve(1.0f);
	m_startingSpeed.m_seeded = true;
	m_startingSpeed.m_seedCurve.SetCurve(1.0f, 3.0f);

	m_pEtherVelocityY.m_ageCurve.SetCurve(0.0f);
	m_pEtherVelocityY.m_seeded = false;
	m_pEtherVelocityY.m_seedCurve.SetCurve(1.0f);

	m_pAccelerationY.m_ageCurve.SetCurve(-9.8f);
	m_pAccelerationY.m_seeded = false;
	m_pAccelerationY.m_seedCurve.SetCurve(1.0f);

	m_pDrag.m_ageCurve.SetCurve(0.3f);
	m_pDrag.m_seeded = false;
	m_pDrag.m_seedCurve.SetCurve(1.0f);

	m_pAlpha.m_ageCurve.SetCurve(1.0f);
	m_pAlpha.m_seeded = false;
	m_pAlpha.m_seedCurve.SetCurve(1.0f);

	m_pSpin.m_ageCurve.SetCurve(Stuff::Pi_Over_2);
	m_pSpin.m_seeded = true;
	m_pSpin.m_seedCurve.SetCurve(-1.0f, 1.0f);

	m_pLifeSpan.m_ageCurve.SetCurve(20.0f);
	m_pLifeSpan.m_seeded = false;
	m_pLifeSpan.m_seedCurve.SetCurve(1.0f);
}


//------------------------------------------------------------------------------
//
bool 
	gosFX::DebrisCloud__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);
	return	Effect__Specification::IsDataValid(fix_data);
/*
	centerOfForce = glm::vec3(0.0f, 00.0f, 0.0f);

	m_lifeSpan.SetCurve(20.0f);

	m_minimumDeviation.SetCurve(0.0f);
	m_maximumDeviation.SetCurve(Stuff::Pi_Over_6);

	m_startingSpeed.m_ageCurve.SetCurve(1.0f);
	m_startingSpeed.m_seeded = true;
	m_startingSpeed.m_seedCurve.SetCurve(1.0f, 3.0f);

	m_pEtherVelocityY.m_ageCurve.SetCurve(0.0f);
	m_pEtherVelocityY.m_seeded = false;
	m_pEtherVelocityY.m_seedCurve.SetCurve(1.0f);

	m_pAccelerationY.m_ageCurve.SetCurve(-9.8f);
	m_pAccelerationY.m_seeded = false;
	m_pAccelerationY.m_seedCurve.SetCurve(1.0f);

	m_pDrag.m_ageCurve.SetCurve(0.3f);
	m_pDrag.m_seeded = false;
	m_pDrag.m_seedCurve.SetCurve(1.0f);

	m_pAlpha.m_ageCurve.SetCurve(1.0f);
	m_pAlpha.m_seeded = false;
	m_pAlpha.m_seedCurve.SetCurve(1.0f);

	m_pSpin.m_ageCurve.SetCurve(Stuff::Pi_Over_2);
	m_pSpin.m_seeded = true;
	m_pSpin.m_seedCurve.SetCurve(-1.0f, 1.0f);

	m_pLifeSpan.m_ageCurve.SetCurve(20.0f);
	m_pLifeSpan.m_seeded = false;
	m_pLifeSpan.m_seedCurve.SetCurve(1.0f);
	*/

}



//############################################################################
//##########################  gosFX::DebrisCloud  ############################
//############################################################################

gosFX::DebrisCloud::ClassData*
	gosFX::DebrisCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			DebrisCloudClassID,
			"gosFX::DebrisCloud",
			EffectCloud::DefaultData,
			effectFactoryThunk<&Make>,
			specificationFactoryThunk<&Specification::Make>
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::DebrisCloud::DebrisCloud(
	Specification *spec,
	unsigned flags
):
	Effect(DefaultData, spec, flags)
{
	Verify(gos_GetCurrentHeap() == Heap);

	debrisPieces.resize(spec->debrisPieces.size());
}

//------------------------------------------------------------------------------
//
gosFX::DebrisCloud*
	gosFX::DebrisCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	DebrisCloud *cloud = new gosFX::DebrisCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud::Start(struct gosFX::Effect::ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	//
	//--------------------------------------------------------------------------
	// Let effect initialize, then figure out how many particles we want to make
	//--------------------------------------------------------------------------
	//
	Effect::Start(info);

	Specification *spec = GetSpecification();
	Check_Object(spec);
	
	Verify(debrisPieces.size() == spec->debrisPieces.size());

	int i, numOfParticles = static_cast<int>(debrisPieces.size());
	
	for(i=0;i<numOfParticles;i++)
	{
		Particle *particle = &debrisPieces[i];

		particle->m_localToParent = glm::mat4(1.0f);
		mc2::math::buildTranslation(particle->m_localToParent, spec->debrisPositions[i]);

		particle->m_age = 0.0f;
		particle->m_seed = spec->debrisSeed[i];

		Stuff::Scalar lifetime =
			spec->m_pLifeSpan.ComputeValue(0.0f, particle->m_seed);

		Min_Clamp(lifetime, 0.0333333f);
		particle->m_ageRate = 1.0f / lifetime;

		glm::vec3 v;

		v = spec->debrisPositions[i] - spec->centerOfForce;
//		v.y = 0.0f;

		Stuff::Scalar lerpFactor =
			Stuff::Lerp(
				spec->m_minimumDeviation.ComputeValue(0.0f, particle->m_seed),
				spec->m_maximumDeviation.ComputeValue(0.0f, particle->m_seed),
				Stuff::Random::GetFraction()
			);

		particle->m_linearVelocity = (mc2::math::lerp((mc2::math::Up), v, lerpFactor));
		particle->m_linearVelocity = glm::normalize(particle->m_linearVelocity);

		particle->m_linearVelocity *= spec->m_startingSpeed.ComputeValue(0.0f, particle->m_seed);

		Stuff::Scalar temp = spec->m_pSpin.ComputeValue(0.0f, particle->m_seed);

		particle->m_angularVelocity.x = temp*(2*Stuff::Random::GetFraction() - 1.0f);
		particle->m_angularVelocity.y = temp*(2*Stuff::Random::GetFraction() - 1.0f);
		particle->m_angularVelocity.z = temp*(2*Stuff::Random::GetFraction() - 1.0f);
	}
}

//------------------------------------------------------------------------------
//
bool
	gosFX::DebrisCloud::Execute(struct gosFX::Effect::ExecuteInfo *info)
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
	//--------------------------------------------------------
	// Figure out the birth rate and request the new particles
	//--------------------------------------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Stuff::Scalar dT =
		static_cast<Stuff::Scalar>(info->m_time - m_lastRan);
	Verify(dT >= 0.0f);
	Stuff::Scalar prev_age = m_age;
	
	m_age += dT * m_ageRate;

	//
	//-----------------------------------
	// Deal with all the active particles
	//-----------------------------------
	//
	int i;
	glm::mat4 local_to_world;

	local_to_world = m_localToParent * *info->m_parentToWorld;

	for (i = 0; i < debrisPieces.size(); i++)
	{
		//
		//--------------------------------------------------------------------
		// If the particle is active, age it and if it is not yet time to die,
		// go to the next particle, otherwise kill it
		//--------------------------------------------------------------------
		//
		Particle *particle = GetParticle(i);
		Check_Object(particle);
		if (particle->m_age < 1.0f)
		{
			particle->m_age += dT*particle->m_ageRate;
			
			if (AnimateParticle(i, &local_to_world, info->m_time))
			{
				continue;
			}
			DestroyParticle(i);
		}
	}

	//
	//----------------------------
	// Now let effect do its thing
	//----------------------------
	//
	m_age = prev_age;

	if(!Effect::Execute(info))
		return false;

	//
	//-------------------------------------------------------------------
	// If there is no bounds yet, we need to create our extent box around
	// the first legal point we find
	//-------------------------------------------------------------------
	//
	mc2::math::Aabb box;
	for(i=0;i<debrisPieces.size();i++)
	{
		Particle *particle = GetParticle(i);
		Check_Object(particle);

		//
		//-----------------------------------------------------------
		// We have found our first particle, so put the box around it
		//-----------------------------------------------------------
		//
		if (particle->m_age < 1.0f)
		{
			glm::vec3 point;
			Stuff::Scalar radius = spec->debrisSpheres[i].radius;

			point = mc2::math::transformPoint(spec->debrisSpheres[i].center, particle->m_localToParent);

			box.max.x = point.x + radius;
			box.min.x = point.x - radius;

			box.max.y = point.y + radius;
			box.min.y = point.y - radius;

			box.max.z = point.z + radius;
			box.min.z = point.z - radius;

			break;
		}
		i++;
	}

	//
	//-----------------------------
	// Look for the other particles
	//-----------------------------
	//
	while (i<debrisPieces.size())
	{
		Particle *particle = GetParticle(i);
		Check_Object(particle);

		if (particle->m_age < 1.0f)
		{
			mc2::math::Aabb local_box;
			glm::vec3 point;
			Stuff::Scalar radius = spec->debrisSpheres[i].radius;

			point = mc2::math::transformPoint(spec->debrisSpheres[i].center, particle->m_localToParent);

			local_box.max.x = point.x + radius;
			local_box.min.x = point.x - radius;

			local_box.max.y = point.y + radius;
			local_box.min.y = point.y - radius;

			local_box.max.z = point.z + radius;
			local_box.min.z = point.z - radius;

			box = mc2::math::unite(box, local_box);
		}
		i++;
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

	//
	//----------------------------------------------
	// Tell our caller that we get to keep executing
	//----------------------------------------------
	//
	return true;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::DebrisCloud::HasFinished(void)
{
	Check_Object(this);
	return Effect::HasFinished();
}

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud::Kill(void)
{
	Check_Object(this);

	//
	//-------------------------------------------------------------
	// Destroy all the particles and set up an empty particle cloud
	//-------------------------------------------------------------
	//
	for(int i=0; i < debrisPieces.size(); i++)
	{
		DestroyParticle(i);
	}

	//
	//----------------------------------------
	// Now let the base effect handle stopping
	//----------------------------------------
	//
	Effect::Kill();
}

//------------------------------------------------------------------------------
//
bool
	gosFX::DebrisCloud::AnimateParticle(
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
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	Stuff::Scalar seed = particle->m_seed;
	Stuff::Scalar age = particle->m_age;
	if (age >= 1.0f)
		return false;

	//
	//------------------
	// Animate the color
	//------------------
	//
	Set_Statistic(Shape_Count, Shape_Count+1);
	particle->m_alpha = spec->m_pAlpha.ComputeValue(age, seed);

	//
	//-----------------------------------------------------------------------
	// If this cloud is unparented, we need to transform the point from local
	// space into world space and set the internal position/velocity pointers
	// to these temporary values
	//-----------------------------------------------------------------------
	//

	glm::vec3 translation;
	translation = mc2::math::getTranslation(particle->m_localToParent);

	glm::quat rotation;
	rotation = mc2::math::quatFromRotation(particle->m_localToParent);

	//
	//------------------------------------------------------------------
	// First, calculate the drag on the particle.  Drag can never assist
	// velocity
	//------------------------------------------------------------------
	//
	Stuff::Scalar drag = -spec->m_pDrag.ComputeValue(age, seed);
	Max_Clamp(drag, 0.0f);
	glm::vec3 ether;
	ether.x = 0.0f;
	ether.y = spec->m_pEtherVelocityY.ComputeValue(age, seed);
	ether.z = 0.0f;
	glm::vec3 accel(glm::vec3(0.0f));

	//
	//-------------------------------------------------------------------
	// Deal with pseudo-world simulation.  In this mode, we interpret the
	// forces as if they are already in worldspace, and we transform them
	// back to local space
	//-------------------------------------------------------------------
	//
	glm::mat4 world_to_effect;
	world_to_effect = glm::inverse(m_localToWorld);
	glm::vec3 local_ether;
	local_ether = (mc2::math::transformVector(ether, glm::inverse(world_to_effect)));
	glm::vec3 rel_vel;
	rel_vel = particle->m_linearVelocity - local_ether;
	accel = rel_vel * drag;

	//
	//-----------------------------------------
	// Now, add in acceleration of the particle
	//-----------------------------------------
	//
	glm::vec3 world_accel;
	world_accel.x = 0.0f;
	world_accel.y = spec->m_pAccelerationY.ComputeValue(age, seed);
	world_accel.z = 0.0f;
	glm::vec3 local_accel;
	local_accel = mc2::math::transformVector(world_accel, world_to_effect);
	accel += local_accel;

	//
	//-------------------------------------------------
	// Compute the particle's new velocity and position
	//-------------------------------------------------
	//
	Stuff::Scalar time_slice =
		static_cast<Stuff::Scalar>(till - m_lastRan);
	
	particle->m_linearVelocity = particle->m_linearVelocity + accel * time_slice;
	
	translation = translation + particle->m_linearVelocity * time_slice;

	//
	//-----------------------
	// Deal with the rotation
	//-----------------------
	//
	glm::vec3 omega(particle->m_angularVelocity);
	omega *= time_slice;
	glm::quat omega_q;
	omega_q = mc2::math::quatFromRotationVector(omega);

	rotation = mc2::math::composeQuat(omega_q, rotation);
	rotation = glm::normalize(rotation);

	mc2::math::buildRotation(particle->m_localToParent, rotation);
	mc2::math::buildTranslation(particle->m_localToParent, translation);

	Check_Object(&particle->m_localToParent);

	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud::DestroyParticle(unsigned int index)
{
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	particle->m_age = 1.0f;
}

//------------------------------------------------------------------------------
//
void gosFX::DebrisCloud::Draw(DrawInfo *info) const
{
	Check_Object(this);
	Check_Object(info);
	Check_Object(info->m_parentToWorld);

	//
	//----------------------------
	// Set up the common draw info
	//----------------------------
	//
	const Specification *spec = GetSpecification();
	Check_Object(spec);

	int i, nrOfParticle = static_cast<int>(debrisPieces.size());

	for(i=0;i<nrOfParticle;i++)
	{
		//
		//-----------------------------------------------------------------
		// If the particle is still alive, concatenate into world space and
		// issue the draw command
		//-----------------------------------------------------------------
		//
		const Particle *particle = GetParticle(i);
		Check_Object(particle);

		if (particle->m_age < 1.0f)
		{
			MidLevelRenderer::DrawScalableShapeInformation dinfo;
			MidLevelRenderer::MLRShape *shape = spec->debrisPieces[i];

			dinfo.clippingFlags.SetClippingState(0x3f);
			dinfo.worldToShape = nullptr;
			dinfo.state.Combine(info->m_state, spec->m_state);
			dinfo.activeLights = nullptr;
			dinfo.nrOfActiveLights = 0;
			dinfo.shape = shape;
			glm::mat4 local_to_world;
			local_to_world = m_localToParent * *info->m_parentToWorld;

			//
			//---------------------------------------------------------------
			// No alignment is necessary, so just multiply out all the active
			// particles
			//---------------------------------------------------------------
			//

			glm::mat4 shape_to_world;
			shape_to_world = particle->m_localToParent * local_to_world;
			dinfo.shapeToWorld = &shape_to_world;
			dinfo.scaling = nullptr;
			Stuff::RGBAColor color(1.0f, 1.0f, 1.0f, particle->m_alpha);
			dinfo.paintMe = &color;
			DrawNativeShape(dinfo);
		}
	}

	//
	//----------------------------
	// Let our parent do its thing
	//----------------------------
	//
	Effect::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::DebrisCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
