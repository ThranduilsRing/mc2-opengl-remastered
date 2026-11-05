#include"gosfxheaders.hpp"

//==========================================================================//
// File:	 gosFX_EffectCloud.cpp											//
// Contents: Base gosFX::EffectCloud Component								//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//------------------------------------------------------------------------------
//
gosFX::EffectCloud__Specification::EffectCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	SpinningCloud__Specification(gosFX::EffectCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);
	Verify(m_class == EffectCloudClassID);
	m_totalParticleSize = gosFX::EffectCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::EffectCloud::Particle);

	*stream >> m_particleEffectID;
}

//------------------------------------------------------------------------------
//
gosFX::EffectCloud__Specification::EffectCloud__Specification():
	SpinningCloud__Specification(gosFX::EffectCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_totalParticleSize = gosFX::EffectCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::EffectCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::EffectCloud__Specification*
	gosFX::EffectCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	EffectCloud__Specification *spec =
		new gosFX::EffectCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	SpinningCloud__Specification::Save(stream);

	*stream << m_particleEffectID;
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectCloud__Specification::Copy(EffectCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	SpinningCloud__Specification::Copy(spec);

	m_particleEffectID = spec->m_particleEffectID;
}

//############################################################################
//##############################  gosFX::EffectCloud  ################################
//############################################################################

gosFX::EffectCloud::ClassData*
	gosFX::EffectCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::EffectCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			EffectCloudClassID,
			"gosFX::EffectCloud",
			SpinningCloud::DefaultData,
			effectFactoryThunk<&Make>,
			specificationFactoryThunk<&Specification::Make>
		);
	Check_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectCloud::TerminateClass()
{
	Check_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::EffectCloud::EffectCloud(
	Specification *spec,
	unsigned flags
):
	SpinningCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);
}

//------------------------------------------------------------------------------
//
gosFX::EffectCloud::~EffectCloud()
{
	if (m_activeParticleCount)
	{
		for (int i=0; i < m_activeParticleCount; i++)
		{
			Particle *particle = GetParticle(i);
			Check_Object(particle);
			if (particle->m_effect)
			{
				Check_Object(particle->m_effect);
				delete particle->m_effect;
			}
		}
	}
}

//------------------------------------------------------------------------------
//
gosFX::EffectCloud*
	gosFX::EffectCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	EffectCloud *cloud = new gosFX::EffectCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectCloud::CreateNewParticle(
		unsigned index,
		glm::vec3 *translation
	)
{
	Check_Object(this);

	//
	//---------------------------
	// Let our parent do creation
	//---------------------------
	//
	SpinningCloud::CreateNewParticle(index, translation);

	//
	//----------------------------------------
	// Now create a new effect under ourselves
	//----------------------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	// Stays raw by design: particles live in the cloud's zeroed byte block (no constructor or destructor runs on a slot), so the
	// slot is freed by hand in DestroyParticle and ~EffectCloud.
	particle->m_effect =
		EffectLibrary::Instance->MakeEffect(
			spec->m_particleEffectID,
			ExecuteFlag|DynamicWorldSpaceSimulationMode
		).release();
	Effect *effect = particle->m_effect;
	Check_Object(effect);
	particle->m_radius = 0.0f;

	//
	//-------------------------------------------------------------
	// Set the transform on the effect, then start the child effect
	//-------------------------------------------------------------
	//
	mc2::math::buildTranslation(effect->m_localToParent, particle->m_localTranslation);
	mc2::math::buildRotation(effect->m_localToParent, particle->m_localRotation);
	ExecuteInfo
		local_info(
			m_lastRan,
			&m_localToWorld,
			nullptr,
			particle->m_seed
		);
	local_info.m_age = particle->m_age;
	local_info.m_ageRate = particle->m_ageRate;
	effect->Start(&local_info);
}

//------------------------------------------------------------------------------
//
bool
	gosFX::EffectCloud::AnimateParticle(
		unsigned index,
		const glm::mat4 *world_to_new_local,
		Stuff::Time till
	)
{
	Check_Object(this);

	//
	//--------------------------------------------------------------------
	// Make sure that we don't blow the age counters out of the base cloud
	// effects
	//--------------------------------------------------------------------
	//
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	if (particle->m_age >= 1.0f)
		particle->m_age = 1.0f - Stuff::SMALL;
	SpinningCloud::AnimateParticle(index, world_to_new_local, till);

	//
	//---------------------------------
	// Update the location of the cloud
	//---------------------------------
	//
	Effect *effect = particle->m_effect;
	Check_Object(effect);
	mc2::math::buildTranslation(effect->m_localToParent, particle->m_localTranslation);
	mc2::math::buildRotation(effect->m_localToParent, particle->m_localRotation);

	//
	//-----------------------
	// Execute all the effect
	//-----------------------
	//
	mc2::math::Obb bounds;
	ExecuteInfo
		info(
			till,
			&m_localToWorld,
			&bounds
		);
	if (effect->Execute(&info))
	{
		glm::vec3 center = mc2::math::getTranslation(bounds.localToParent);
		particle->m_radius = glm::length(center) + bounds.sphereRadius;
		return true;
	}

	particle->m_radius = 0.0f;
	delete particle->m_effect;
	particle->m_effect = nullptr;
	return false;
}

//------------------------------------------------------------------------------
//
void gosFX::EffectCloud::DestroyParticle(unsigned index)
{
	Check_Object(this);

	Particle *particle = GetParticle(index);
	Check_Object(particle);
	if (particle->m_effect)
	{
		Check_Object(particle->m_effect);
		delete particle->m_effect;
		particle->m_effect = nullptr;
	}
	SpinningCloud::DestroyParticle(index);
}

//------------------------------------------------------------------------------
//
void gosFX::EffectCloud::Draw(DrawInfo *info)
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
		for (int i=0; i < m_activeParticleCount; i++)
		{
			Particle *particle = GetParticle(i);
			Check_Object(particle);

			//
			//-----------------------------------------------------------------
			// If the particle is still alive, concatenate into world space and
			// issue the draw command
			//-----------------------------------------------------------------
			//
			if (particle->m_age < 1.0f)
			{
				if (particle->m_effect)
				{
					Check_Object(particle->m_effect);
					particle->m_effect->Draw(info);
				}
			}
		}
	}

	SpinningCloud::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
