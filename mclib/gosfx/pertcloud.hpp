//==========================================================================//
// File:	 gosFX_PertCloud.hpp											//
// Contents: PertCloud Component											//
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
	//########################  PertCloud__Specification  #############################
	//############################################################################

	class PertCloud__Specification:
		public SpinningCloud__Specification
	{
	//----------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		PertCloud__Specification(
			Stuff::MemoryStream *stream,
			int gfx_version
		);

	public:
		PertCloud__Specification(unsigned sides);

		static PertCloud__Specification*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);

		void
			Copy(PertCloud__Specification *spec);

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
			m_perturbation;
		SeededCurveOf<ComplexCurve, LinearCurve,Curve::e_ComplexLinearType>
			m_pCenterRed,
			m_pCenterGreen,
			m_pCenterBlue,
			m_pCenterAlpha;

		unsigned
			m_vertices;
	};


	//############################################################################
	//########################  SpinningCloud__Particle  #############################
	//############################################################################

	class PertCloud__Particle:
		public SpinningCloud__Particle
	{
	public:
		glm::vec3
			m_vertices[MidLevelRenderer::Limits::Max_Number_Of_NGon_Vertices];
	};

//############################################################################
//#############################  PertCloud  ##################################
//############################################################################

	class PertCloud : public SpinningCloud
	{
	//----------------------------------------------------------------------------
	// Class Registration Support
	//
	public:
		static void InitializeClass();
		static void	TerminateClass();

		using Specification = PertCloud__Specification;
		using Particle = PertCloud__Particle;

	protected:
		glm::vec3		*m_P_vertices;
		Stuff::RGBAColor	*m_P_color;

	//----------------------------------------------------------------------------
	// Class Data Support
	//
	protected:
		PertCloud(
			Specification *spec,
			unsigned flags
		);

	public:
		~PertCloud();

		static PertCloud*
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
				{ return const_cast<PertCloud*>(this)->GetParticle(index); }
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
