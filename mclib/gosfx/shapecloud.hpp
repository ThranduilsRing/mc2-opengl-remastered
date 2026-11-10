//==========================================================================//
// File:	 gosFX_ShapeCloud.hpp											//
// Contents: ShapeCloud Component											//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
#pragma once

#include"gosfx.hpp"
#include"spinningcloud.hpp"
#include<mlr/mlr.hpp>

namespace MidLevelRenderer {class MLRShape;}

namespace gosFX
{
	//############################################################################
	//########################  ShapeCloud__Specification  #############################
	//############################################################################

	class ShapeCloud;

	class ShapeCloud__Specification:
		public SpinningCloud__Specification
	{
		friend class ShapeCloud;

	//----------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		ShapeCloud__Specification(
			Stuff::MemoryStream *stream,
			int gfx_version
		);

	public:
		ShapeCloud__Specification(MidLevelRenderer::MLRShape *shape);
		~ShapeCloud__Specification();

		void
			Save(Stuff::MemoryStream *stream);

		static ShapeCloud__Specification*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);

		void
			Copy(ShapeCloud__Specification *spec);

		void
			SetShape(MidLevelRenderer::MLRShape *shape);

	protected:
		MidLevelRenderer::MLRShape
			*m_shape;
		Stuff::Scalar
			m_radius;
	};

	//############################################################################
	//########################  SpinningCloud__Particle  #############################
	//############################################################################

	class ShapeCloud__Particle:
		public SpinningCloud__Particle
	{
	public:
		Stuff::RGBAColor
			m_color;
	};

//############################################################################
//#############################  ShapeCloud  #################################
//############################################################################

	class ShapeCloud : public SpinningCloud
	{
	//----------------------------------------------------------------------------
	// Class Registration Support
	//
	public:
		static void InitializeClass();
		static void	TerminateClass();

		using Specification = ShapeCloud__Specification;
		using Particle = ShapeCloud__Particle;

	//----------------------------------------------------------------------------
	// Class Data Support
	//
	protected:
		ShapeCloud(
			Specification *spec,
			unsigned flags
		);

	public:
		static ShapeCloud*
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
				{ return const_cast<ShapeCloud*>(this)->GetParticle(index); }
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
