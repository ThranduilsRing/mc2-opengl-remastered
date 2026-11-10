//==========================================================================//
// File:	 gosFX_ShardCloud.hpp											//
// Contents: ShardCloud Component											//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
#pragma once

#include"gosfx.hpp"
#include"spinningcloud.hpp"
#include<mlr/mlr.hpp>


namespace gosFX
{
	//############################################################################
	//########################  ShardCloud__Specification  #############################
	//############################################################################

	class ShardCloud__Specification:
		public SpinningCloud__Specification
	{
	//----------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		ShardCloud__Specification(
			Stuff::MemoryStream *stream,
			int gfx_version
		);

	public:
		ShardCloud__Specification();

		static ShardCloud__Specification*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);
		void
			Copy(ShardCloud__Specification *spec);

		void
			Save(Stuff::MemoryStream *stream);

		void 
			BuildDefaults();

		bool 
			IsDataValid(bool fix_data=false);

	//-------------------------------------------------------------------------
	// FCurves
	//
	public:
		SeededCurveOf<ComplexCurve, SplineCurve,Curve::e_ComplexSplineType>
			m_size;
		SeededCurveOf<ConstantCurve, SplineCurve,Curve::e_ConstantSplineType>
			m_angularity;
	};

	//############################################################################
	//########################  SpinningCloud__Particle  #############################
	//############################################################################

	class ShardCloud__Particle:
		public SpinningCloud__Particle
	{
	public:
		Stuff::Scalar
			m_angle;
	};

//############################################################################
//#############################  ShardCloud  #################################
//############################################################################

	class ShardCloud : public SpinningCloud
	{
	//----------------------------------------------------------------------------
	// Class Registration Support
	//
	public:
		static void InitializeClass();
		static void	TerminateClass();

		using Specification = ShardCloud__Specification;
		using Particle = ShardCloud__Particle;

		static constexpr int
			ParticleSize =
				sizeof(Particle)
				 + 3*sizeof(glm::vec3)
				 + 3*sizeof(Stuff::RGBAColor);

	protected:
		glm::vec3			*m_P_vertices;
		Stuff::RGBAColor		*m_P_color;

	//----------------------------------------------------------------------------
	// Class Data Support
	//
	protected:
		ShardCloud(
			Specification *spec,
			unsigned flags
		);

	public:
		~ShardCloud();

		static ShardCloud*
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

		const Specification*
			GetSpecification() const
				{ return Cast_Object(const Specification*, m_specification); }
		const Particle*
			GetParticle(unsigned index) const
				{ return const_cast<ShardCloud*>(this)->GetParticle(index); }
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

	public:
		void
			Draw(DrawInfo *info) const;
	};
}
