//==========================================================================//
// File:	 gosFX_PointCloud.hpp											//
// Contents: PointCloud Component											//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
#pragma once

#include"gosfx.hpp"
#include"particlecloud.hpp"
#include<mlr/mlr.hpp>

namespace MidLevelRenderer {class MLRPointCloud;}

namespace gosFX
{
	//############################################################################
	//########################  PointCloud__Specification  #############################
	//############################################################################

	class PointCloud__Specification:
		public ParticleCloud__Specification
	{
	//----------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		PointCloud__Specification(
			Stuff::MemoryStream *stream,
			int gfx_version
		);

	public:
		PointCloud__Specification();

		static PointCloud__Specification*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);
	};

	//############################################################################
	//########################  ParticleCloud__Particle  #############################
	//############################################################################

	class PointCloud__Particle:
		public ParticleCloud__Particle
	{
	public:
		glm::vec3
			m_worldTranslation;
	};

//############################################################################
//#############################  PointCloud  #################################
//############################################################################

	class PointCloud : public ParticleCloud
	{
	//----------------------------------------------------------------------------
	// Class Registration Support
	//
	public:
		static void InitializeClass();
		static void	TerminateClass();

		static ClassData
			*DefaultData;

		typedef PointCloud__Specification Specification;
		typedef PointCloud__Particle Particle;

		enum {
			ParticleSize = sizeof(Particle) + sizeof(glm::vec3) + sizeof(Stuff::RGBAColor)
		};

	protected:
		std::unique_ptr<MidLevelRenderer::MLRPointCloud>
			m_cloudImplementation;
		glm::vec3
			*m_P_localTranslation;
		Stuff::RGBAColor
			*m_P_color;

	//----------------------------------------------------------------------------
	// Constructor/Destructor
	//
	protected:
		PointCloud(
			Specification *spec,
			unsigned flags
		);

	public:
		~PointCloud();

		static PointCloud*
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
		// Both render arms use the same CPU particle births and lifetimes.
		void
			Start(ExecuteInfo *info);
		bool
			Execute(ExecuteInfo *info);
		void
			Draw(DrawInfo *info);
	};
}
