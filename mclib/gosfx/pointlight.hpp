//==========================================================================//
// File:	 gosFX_PointLight.hpp									    	//
// Contents: Base PointLight Particle									    //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
#pragma once

#include"gosfx.hpp"
#include"effect.hpp"
#include"particles/light_manager.h"

namespace gosFX
{
	//############################################################################
	//############################  LightManager  ################################
	//############################################################################
	//
	// B1 Stage 2' C2: class body moved to mc2::particles::LightManager
	// (mclib/particles/light_manager.{h,cpp}). The using-alias preserves the
	// gosFX::LightManager surface so this file's .cpp internal references and
	// the seven external lifecycle sites compile unchanged. gosFX::Light is
	// still the value-typed handle that LightManager::MakePointLight returns.

	class Light;
	using LightManager = ::mc2::particles::LightManager;

	//############################################################################
	//####################  PointLight__Specification  #########################
	//############################################################################

	class PointLight__Specification:
		public Effect__Specification
	{
	//----------------------------------------------------------------------
	// Constructors/Destructors
	//
	protected:
		PointLight__Specification(
			Stuff::MemoryStream *stream,
			int gfx_version
		);

	public:
		PointLight__Specification();

		void
			Copy(PointLight__Specification *spec);

		void
			Save(Stuff::MemoryStream *stream);

		void 
			BuildDefaults();

		bool 
			IsDataValid(bool fix_data=false);

		static PointLight__Specification*
			Make(
				Stuff::MemoryStream *stream,
				int gfx_version
			);

	//-------------------------------------------------------------------------
	// FCurves
	//
	public:
		ComplexCurve
			m_red,
			m_green,
			m_blue,
			m_intensity;
		SplineCurve
			m_innerRadius,
			m_outerRadius;

		bool
			m_twoSided;
		Stuff::MString
			m_lightMap;
	};

	//############################################################################
	//##############################  PointLight  #############################
	//############################################################################

	class PointLight:
		public Effect
	{
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

		static ClassData
			*DefaultData;

		using Specification = PointLight__Specification;

		static PointLight*
			Make(
				Specification *spec,
				unsigned flags
			);

		~PointLight();

	protected:
		PointLight(
			Specification *spec,
			unsigned flags
		);

		Light
			*m_light;

	//----------------------------------------------------------------------------
	// Class Data Support
	//
	public:
		Specification*
			GetSpecification()
				{
					Check_Object(this);
					return
						Cast_Object(Specification*, m_specification);
				}

	//----------------------------------------------------------------------------
	// API
	//
	public:
		void
			Start(ExecuteInfo *info);
		bool
			Execute(ExecuteInfo *info);
		void
			Kill();

	//----------------------------------------------------------------------------
	// Testing
	//
	public:
		void
			TestInstance() const;
	};
}
