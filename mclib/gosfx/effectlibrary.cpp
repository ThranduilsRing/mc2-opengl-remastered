#include"gosfxheaders.hpp"
#include"particles/spec_library.h"
// Effect creation retains CPU state; GPU rendering harvests it in Draw().
//==========================================================================//
// File:	 gosFX_Effect.cpp												//
// Contents: Base gosFX::Effect Component									//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
// B1 Stage 2' C1: parser/lookup body extracted to
// mc2::particles::SpecLibrary (mclib/particles/spec_library.{h,cpp}).
// This file is now the gosFX-namespace forwarding adapter. m_effects
// ownership moved to SpecLibrary; FX_TRACE_SPAWN emit site moved with
// Find() to spec_library.cpp.
//

gosFX::EffectLibrary*
	gosFX::EffectLibrary::Instance = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::InitializeClass()
{
	Verify(!Instance);
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::TerminateClass()
{
	if (Instance)
	{
		Unregister_Object(Instance);
		delete Instance;
		Instance=nullptr;
	}
	// Tear down the underlying SpecLibrary so a subsequent mission load
	// reparses cleanly. Safe to call repeatedly.
	mc2::particles::SpecLibrary::Shutdown();
}

//------------------------------------------------------------------------------
//
gosFX::EffectLibrary::EffectLibrary()
{
	Verify(gos_GetCurrentHeap() == Heap);
	// Lazy-init the underlying spec library so producer-side singleton-init
	// at the existing call sites (code/mechcmd2.cpp:1657, Viewer/View.cpp:550,
	// mclib/txmmgr.cpp:522) wires SpecLibrary without touching those sites.
	mc2::particles::SpecLibrary::Instance();
}

//------------------------------------------------------------------------------
//
gosFX::EffectLibrary::~EffectLibrary()
{
	// Storage lives on SpecLibrary and the texture manager's flush deletes this object without TerminateClass(): the specs go with the
	// library, or every mission reload parses a second set on top of the first and drops the old one.
	// Live effects must already be dead before this runs: they point into the specs freed here (the flush kills them before it deletes us).
	mc2::particles::SpecLibrary::Shutdown();
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::Load(Stuff::MemoryStream* stream)
{
	mc2::particles::SpecLibrary::Instance()->Load(stream);
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::Save(Stuff::MemoryStream* stream)
{
	mc2::particles::SpecLibrary::Instance()->Save(stream);
}

//------------------------------------------------------------------------------
//
gosFX::Effect::Specification*
	gosFX::EffectLibrary::Find(mc2::czstring name)
{
	return mc2::particles::SpecLibrary::Instance()->Find(name);
}

//------------------------------------------------------------------------------
//
gosFX::EffectPtr
	gosFX::EffectLibrary::MakeEffect(
		unsigned index,
		unsigned flags
	)
{
	gosFX::Effect::Specification *spec =
		mc2::particles::SpecLibrary::Instance()->At(index);
	Check_Object(spec);

	// B1 Stage 2' C8: unconditional legacy construction. The GPU pipeline
	// is now wired by per-subclass Start overrides (see card.cpp /
	// pointcloud.cpp / shardcloud.cpp / tube.cpp), which catches both
	// top-level MakeEffect returns AND composite-children via the
	// EffectCloud parent that iterates and calls child Start. The C7
	// EffectAdapter-at-this-boundary approach is retired.
	gosFX::Effect::ClassData *data =
		Cast_Pointer(
			gosFX::Effect::ClassData*,
			Stuff::RegisteredClass::FindClassData(spec->GetClassID())
		);
	Check_Object(data);
	Check_Pointer(data->effectFactory);
	// The class registry factories return the new object; this is the one place it gets its owner.
	return EffectPtr{(*data->effectFactory)(spec, flags)};
}
