//==========================================================================//
// File:	 gosFX_EffectCloud.hpp											//
// Contents: EffectCloud Component											//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
#pragma once

#include"gosfx.hpp"
#include"spinningcloud.hpp"

namespace gosFX
{
	//############################################################################
	//########################  EffectCloud__Specification  #############################
	//############################################################################

	class EffectCloud__Specification:
		public SpinningCloud__Specification
	{
	//----------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		EffectCloud__Specification(
			Stuff::MemoryStream *stream,
			int gfx_version
		);

	public:
		EffectCloud__Specification();

		static EffectCloud__Specification*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);

		void
			Copy(EffectCloud__Specification *spec);

		void
			Save(Stuff::MemoryStream *stream);

		unsigned
			m_particleEffectID;
	};

	//############################################################################
	//########################  SpinningCloud__Particle  #############################
	//############################################################################

	class EffectCloud__Particle:
		public SpinningCloud__Particle
	{
	public:
		Effect
			*m_effect;
	};

	//############################################################################
	//#############################  EffectCloud  #################################
	//############################################################################

	class EffectCloud:
		public SpinningCloud
	{
	//----------------------------------------------------------------------------
	// Class Registration Support
	//
	public:
		static void InitializeClass();
		static void	TerminateClass();

		using Specification = EffectCloud__Specification;
		using Particle = EffectCloud__Particle;

		static constexpr int ParticleSize = sizeof(Particle);

	//----------------------------------------------------------------------------
	// Class Data Support
	//
	protected:
		EffectCloud(
			Specification *spec,
			unsigned flags
		);

	public:
		~EffectCloud();

		static EffectCloud*
			Make(
				Specification *spec,
				unsigned flags
			);

		Specification*
			GetSpecification()
				{
					Check_Object(this);
					return
						Cast_Object(Specification*, m_specification);
				}
		Particle*
			GetParticle(unsigned index)
				{
					Check_Object(this); Check_Object(GetSpecification());
					return
						Cast_Pointer(
							Particle*,
							&m_data[index*GetSpecification()->m_particleClassSize]
						);
				}

		const Particle*
			GetParticle(unsigned index) const
				{ return const_cast<EffectCloud*>(this)->GetParticle(index); }

		static ClassData
			*DefaultData;

	//----------------------------------------------------------------------------
	// Testing
	//
	public:
		void
			TestInstance() const;

	//----------------------------------------------------------------------------
	// API
	//
	protected:
		bool
			AnimateParticle(
				unsigned index,
				const glm::mat4 *world_to_new_local,
				Stuff::Time till
			);
		void
			CreateNewParticle(
				unsigned index,
				glm::vec3 *translation
			);
		void
			DestroyParticle(unsigned index);

	public:
		void
			Draw(DrawInfo *info) const;
	};
}
