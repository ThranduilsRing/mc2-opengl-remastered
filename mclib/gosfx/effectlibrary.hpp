//==========================================================================//
// File:	 EffectLibrary.hpp												//
// Contents: Base Effect Component											//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"gosfx.hpp"
#include"effect.hpp"

namespace gosFX
{
	class EffectLibrary
		#if defined(_ARMOR)
			: public Stuff::Signature
		#endif
	{
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Initialization
	//
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Constructors/Destructors
	//
	public:
		EffectLibrary();
		~EffectLibrary();

		void
			Load(Stuff::MemoryStream* stream);
		void
			Save(Stuff::MemoryStream* stream);

		enum MergeMode {
			OnlyAddNewEffects,
			ReplaceMatchingEffects,
			ReplaceNamedEffects
		};

		void
			Merge(
				EffectLibrary &source,
				MergeMode merge_mode=(MergeMode)OnlyAddNewEffects
			);

		static EffectLibrary*
			Instance;

	protected:
		EffectLibrary(EffectLibrary &source)
			{STOP(("Shouldn't be called"));}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Effect management
	//
	// Per B1 Stage 2' C1: the m_effects storage now lives in
	// mc2::particles::SpecLibrary. EffectLibrary is a forwarding adapter
	// during the migration window.
	public:
		Effect::Specification*
			Find(const char* name);
		// The caller owns the new effect.
		EffectPtr
			MakeEffect(
				unsigned index,
				unsigned flags
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Testing
	//
	public:
		void
			TestInstance() const
				{}
	};
}
