#include"gosfxheaders.hpp"
#include"fx_trace/fx_trace.h"
#include"fx_trace/fx_cost_split.h"  // Q2-S0 FX cost-split (env MC2_FX_COST_SPLIT)
#include <tracy/Tracy.hpp>          // Q2-S0 coarse CPU zones

// [B1 C16] (diagnostic) GOSFX_HEAP + GOSFX_CHILD counters
// Env-gated on MC2_GPU_PARTICLES=1. No-op when disabled.
// Hypotheses being differentiated:
//   H1: gosFX heap exhaustion via process-wide allocation growth.
//   H2: Effect::m_children leak on looped parents (creates > destroys).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"
#include "mc2fmt.h"
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#endif

namespace {
    std::atomic<long long> g_effectCreates{0};
    std::atomic<long long> g_effectDestroys{0};
    std::atomic<long long> g_effectAlivePeak{0};
    std::atomic<long long> g_effectAliveNow{0};

    long long g_diagFrameCount = 0;
    bool      g_diagEnabledChecked = false;
    bool      g_diagEnabled = false;
    bool      g_diagAtexitRegistered = false;
    long long g_diagHeapPeakBytes = 0;

    void emit_diag_summary(const char* reason) {
        long long creates  = g_effectCreates.load();
        long long destroys = g_effectDestroys.load();
        long long peak     = g_effectAlivePeak.load();
        mc2::fmt::print_err("[GOSFX_CHILD v1] event=summary reason={} frames={:d} creates={:d} destroys={:d} delta={:d} max_alive={:d} peak_rss={:d}KB\n", mc2::fmt::safe(reason), mc2::fmt::as_i64(g_diagFrameCount), mc2::fmt::as_i64(creates), mc2::fmt::as_i64(destroys), mc2::fmt::as_i64(creates - destroys), mc2::fmt::as_i64(peak), mc2::fmt::as_i64(g_diagHeapPeakBytes / 1024));
    }

    void diag_init_if_needed() {
        if (g_diagEnabledChecked) return;
        g_diagEnabledChecked = true;
        g_diagEnabled = mc2::gate::get<"MC2_GPU_PARTICLES">();
        if (g_diagEnabled && !g_diagAtexitRegistered) {
            g_diagAtexitRegistered = true;
            std::atexit([](){ emit_diag_summary("atexit"); });
            mc2::fmt::print_err("[INSTR v1] enabled: GOSFX_HEAP + GOSFX_CHILD counters (C16)\n");
        }
    }
}

namespace gosFX {
    void DiagFrameTick() {
        diag_init_if_needed();
        if (!g_diagEnabled) return;
        ++g_diagFrameCount;
        if ((g_diagFrameCount % 500) != 0) return;

        long long workingSet = 0;
        long long pagefile   = 0;
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS pmc;
        ZeroMemory(&pmc, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            workingSet = static_cast<long long>(pmc.WorkingSetSize);
            pagefile   = static_cast<long long>(pmc.PagefileUsage);
        }
#endif
        if (workingSet > g_diagHeapPeakBytes) g_diagHeapPeakBytes = workingSet;

        long long creates  = g_effectCreates.load();
        long long destroys = g_effectDestroys.load();
        long long alive    = g_effectAliveNow.load();
        long long peak     = g_effectAlivePeak.load();
        mc2::fmt::print_err("[GOSFX_HEAP v1] frame={:d} rss={:d}KB pagefile={:d}KB peakRss={:d}KB [GOSFX_CHILD v1] creates={:d} destroys={:d} delta={:d} alive={:d} max_alive={:d}\n", mc2::fmt::as_i64(g_diagFrameCount), mc2::fmt::as_i64(workingSet / 1024), mc2::fmt::as_i64(pagefile / 1024), mc2::fmt::as_i64(g_diagHeapPeakBytes / 1024), mc2::fmt::as_i64(creates), mc2::fmt::as_i64(destroys), mc2::fmt::as_i64(creates - destroys), mc2::fmt::as_i64(alive), mc2::fmt::as_i64(peak));
    }
}

//==========================================================================//
// File:	 gosFX_Effect.cpp												//
// Contents: Base gosFX::Effect Component									//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//############################################################################
//########################  EffectSpecification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::Event::Event(const Event& event):
	Plug(DefaultData)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);

	m_time = event.m_time;
	m_flags = event.m_flags;
	m_effectID = event.m_effectID;
}

//------------------------------------------------------------------------------
//
gosFX::Event::Event(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	Plug(DefaultData)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	*stream >> m_time >> m_flags >> m_effectID >> m_localToParent;
}

//------------------------------------------------------------------------------
//
gosFX::Event*
	gosFX::Event::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	Event *event = new Event(stream, gfx_version);
	gos_PopCurrentHeap();

	return event;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Event::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	*stream << m_time << m_flags << m_effectID << m_localToParent;
}

//############################################################################
//########################  EffectSpecification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::Effect__Specification::Effect__Specification(
	Stuff::RegisteredClass::ClassID class_id,
	Stuff::MemoryStream *stream,
	int gfx_version
):
	m_events(nullptr)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	if (gfx_version < 9)
	{
		STOP(("This version of gosFX is no longer supported"));
		return;
	}

	//
	//--------------
	// Read the name
	//--------------
	//
	m_class = class_id;
	*stream >> m_name;

	//
	//--------------------------------------------------------------------
	// Read the events.  If we are using an array, no events will be saved
	//--------------------------------------------------------------------
	//
	unsigned event_count;
	*stream >> event_count;
	while (event_count-- > 0)
	{
		Event *event = Event::Make(stream, gfx_version);
		Register_Object(event);
		m_events.Add(event);
	}

	//
	//--------------------------------------------------------
	// Load the curves, variances, and MLRState if appropriate
	//--------------------------------------------------------
	//
	m_lifeSpan.Load(stream, gfx_version);
	m_minimumChildSeed.Load(stream, gfx_version);
	m_maximumChildSeed.Load(stream, gfx_version);
	m_state.Load(stream, MidLevelRenderer::ReadMLRVersion(stream));
	if (gfx_version < 14)
	{
		m_state.SetRenderPermissionMask(
			m_state.GetRenderPermissionMask() | MidLevelRenderer::MLRState::TextureMask
		);
	}
	if (
		m_state.GetAlphaMode() != MidLevelRenderer::MLRState::OneZeroMode
		&& m_state.GetPriority() < MidLevelRenderer::MLRState::AlphaPriority
	)
		m_state.SetPriority(MidLevelRenderer::MLRState::AlphaPriority);
}

//------------------------------------------------------------------------------
//
gosFX::Effect__Specification::Effect__Specification(
	Stuff::RegisteredClass::ClassID class_id
):
	m_events(nullptr)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_class = class_id;
}

//------------------------------------------------------------------------------
//
gosFX::Effect__Specification::~Effect__Specification()
{
	Check_Pointer(this);

	Stuff::ChainIteratorOf<Event*> events(&m_events);
	events.DeletePlugs();
}

//------------------------------------------------------------------------------
//
gosFX::Effect__Specification*
	gosFX::Effect__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	Effect__Specification *spec =
		new gosFX::Effect__Specification(EffectClassID, stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//

void 
	gosFX::Effect__Specification::BuildDefaults()
{

	Check_Object(this);
	
	m_lifeSpan.SetCurve(1.0f);
	m_minimumChildSeed.SetCurve(0.0f);
	m_maximumChildSeed.SetCurve(1.0f);
}


//------------------------------------------------------------------------------
//

bool gosFX::Effect__Specification::IsDataValid(bool fix_data)
{
    Check_Object(this);
    Stuff::Scalar minv,maxv;
    m_lifeSpan.ExpensiveComputeRange(&minv,&maxv);
    if(minv<0.0f)
    {
        if(fix_data)
        {
            m_lifeSpan.SetCurve(1.0f);
            PAUSE(("Warning: Curve \"lifespan\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

        }
        else 
            return false;
    }

    return true;
}

//------------------------------------------------------------------------------
//
gosFX::Effect__Specification*
	gosFX::Effect__Specification::Create(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);
	Stuff::RegisteredClass::ClassID class_id;
	*stream >> class_id;
	gosFX::Effect::ClassData* class_data =
		Cast_Pointer(
			gosFX::Effect::ClassData*,
			Stuff::RegisteredClass::FindClassData(class_id)
		);
	Check_Object(class_data);
	Check_Pointer(class_data->specificationFactory);
	gosFX::Effect__Specification *spec =
		(*class_data->specificationFactory)(stream, gfx_version);
	Register_Object(spec);
	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Effect__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	*stream << m_class << m_name;
	Stuff::ChainIteratorOf<Event*> events(&m_events);
	unsigned count = events.GetSize();
	*stream << count;
	Event *event;
	while ((event = events.ReadAndNext()) != nullptr)
	{
		Check_Object(event);
		event->Save(stream);
	}
	m_lifeSpan.Save(stream);
	m_minimumChildSeed.Save(stream);
	m_maximumChildSeed.Save(stream);
	MidLevelRenderer::WriteMLRVersion(stream);
	m_state.Save(stream);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Effect__Specification::Copy(Effect__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);
	gos_PushCurrentHeap(Heap);

	Verify(spec->m_class == m_class);
	m_name = spec->m_name;

	//
	//----------------------------------------------
	// Copy the events after delete our current ones
	//----------------------------------------------
	//
	Stuff::ChainIteratorOf<Event*> old_events(&m_events);
	old_events.DeletePlugs();
	Stuff::ChainIteratorOf<Event*> new_events(&spec->m_events);
	Event* event;
	while ((event = new_events.ReadAndNext()) != nullptr)
	{
		Check_Object(event);
		Event *new_event = new Event(*event);
		Register_Object(new_event);
		AdoptEvent(new_event);
	}

	//
	//---------------------------------------------------
	// Now copy the curves, variance modes, and MLR state
	//---------------------------------------------------
	//
	m_lifeSpan = spec->m_lifeSpan;
	m_minimumChildSeed = spec->m_minimumChildSeed;
	m_maximumChildSeed = spec->m_maximumChildSeed;
	m_state = spec->m_state;

	gos_PopCurrentHeap();
}

//------------------------------------------------------------------------------
//
void
	gosFX::Effect__Specification::AdoptEvent(Event *event)
{
	Check_Object(this);
	Check_Object(event);
	Verify(event->m_time >= 0.0f && event->m_time <= 1.0f);
	Verify(gos_GetCurrentHeap() == Heap);

	//
	//-----------------------------------------------------------
	// The event must be inserted into the chain in order of time
	//-----------------------------------------------------------
	//
	Stuff::ChainIteratorOf<Event*> events(&m_events);
	Event *insert = nullptr;
	while ((insert = events.GetCurrent()) != nullptr)
	{
		Check_Object(insert);
		if (insert->m_time > event->m_time)
		{
			events.Insert(event);
			return;
		}
		events.Next();
	}
	m_events.Add(event);
}

//############################################################################
//##############################   gosFX::Effect  ###################################
//############################################################################

gosFX::Effect::ClassData*
	gosFX::Effect::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::Effect::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			EffectClassID,
			"gosFX::Effect",
			Node::DefaultData,
			&Make,
			&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Effect::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
gosFX::Effect::Effect(
	ClassData *class_data,
	Specification *spec,
	unsigned flags
):
	Node(class_data),
	m_event(&spec->m_events)
{
	Check_Pointer(this);
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);
	m_specification = spec;
	m_age = 0.0f;
	m_flags = flags;
	m_lastRan = -1.0f;
	m_localToParent = Stuff::LinearMatrix4D::Identity;

	// [B1 C16] (diagnostic) Effect create counter — increments for every
	// gosFX::Effect subclass via base ctor. Env-gated read inside DiagFrameTick.
	{
		long long now = ++g_effectAliveNow;
		++g_effectCreates;
		long long prevPeak = g_effectAlivePeak.load();
		while (now > prevPeak &&
		       !g_effectAlivePeak.compare_exchange_weak(prevPeak, now)) {}
	}
}


//------------------------------------------------------------------------------
//
gosFX::Effect::~Effect()
{
	Check_Object(this);
	m_children.clear();

	// [B1 C16] (diagnostic) Effect destroy counter: a finished child released in
	// Execute and the children cleared above both end in this dtor.
	--g_effectAliveNow;
	++g_effectDestroys;
}

//------------------------------------------------------------------------------
//
gosFX::Effect*
	gosFX::Effect::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	Effect *effect = new gosFX::Effect(DefaultData, spec, flags);
	gos_PopCurrentHeap();

	return effect;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Effect::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//------------------------------------------------------------------------------
//
void
	gosFX::Effect::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);
	gos_PushCurrentHeap(Heap);

	// C10: per-ClassID per-Start-event counter (env-gated, default-off).
	// Plan v6 §B1 C10: classify gosFX ClassID distribution in stock content
	// to determine whether C8/C9's 4 routed primitive types (Card/Point/
	// Shard/Tube) actually dominate, or whether the deferred B2 types
	// (Pert/Shape/ShapeCloud/Debris/EffectCloud) do.
	if (m_specification)
	{
		FX_TRACE_CLASS(m_specification->GetClassID(),
		               m_specification->m_name);
	}

	//
	//---------------------------------------------------------------------
	// Don't override m_lastran if we are issuing a Start command while the
	// effect is already running
	//---------------------------------------------------------------------
	//
	if (!IsExecuted() || m_lastRan == -1.0)
		m_lastRan = info->m_time;
	SetExecuteOn();

	//
	//-------------------------------------------
	// If no seed was provided, pick one randomly
	//-------------------------------------------
	//
	m_seed = (info->m_seed == -1.0f) ? Stuff::Random::GetFraction() : info->m_seed;
	Verify(m_seed >= 0.0f && m_seed <= 1.0f);

	//
	//--------------------------------------------------------------------
	// Figure out how long the emitter will live and its initial age based
	// upon the effect seed
	//--------------------------------------------------------------------
	//
	Check_Object(m_specification);
	if (info->m_age == -1.0f)
	{
		Stuff::Scalar lifetime =
			m_specification->m_lifeSpan.ComputeValue(m_seed, 0.0f);
		Min_Clamp(lifetime, 0.033333f);
		m_ageRate = 1.0f / lifetime;
		m_age = 0;
	}
	else
	{
		m_age = info->m_age;
		m_ageRate = info->m_ageRate;
		Verify(m_age >= 0.0f && m_age <= 1.0f);
	}

	//
	//--------------------
	// Set up the matrices
	//--------------------
	//
	Check_Object(info->m_parentToWorld);
	m_localToWorld.Multiply(m_localToParent, *info->m_parentToWorld);

	//
	//-------------------------
	// Set up the event pointer
	//-------------------------
	//
	m_event.First();
	if (mc2::gate::get<"MC2_GPU_PARTICLES_LOG">())
	{
		mc2::fmt::print_err("[VFX_BIRTH v1] spec={} time={:.9f} age={:.9f} seed={:.9f} stream={} algorithm={} version={} initial={} state={} draws={} legacy_draws={}\n",
			mc2::fmt::safe(static_cast<const char*>(m_specification->m_name)),
			static_cast<double>(info->m_time), static_cast<double>(m_age), static_cast<double>(m_seed),
			"legacy", "legacy-table", 0u, 0u, 0u, 0ull,
			Stuff::Random::GetDrawCount());
	}
	gos_PopCurrentHeap();
}

//------------------------------------------------------------------------------
//
bool gosFX::Effect::Execute(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);
	Verify(IsExecuted());
	gos_PushCurrentHeap(Heap);

	// Q2-S0 instrumentation only (no behavior change). Coarse Tracy zone +
	// inclusive-cycle bucket for the per-effect base Execute.
	ZoneScopedN("gosFX.Effect.Execute");
	mc2::fx_cost_split::Scope _fxcs(mc2::fx_cost_split::B_FX_EXECUTE);

	//
	//-----------------------------------------------------
	// If a new seed is provided, override the current seed
	//-----------------------------------------------------
	//
	if (info->m_seed != -1.0f)
	{
		Verify(info->m_seed>=0.0f && info->m_seed<1.0f);
		m_seed = info->m_seed;
	}

	//
	//--------------------------------------------
	// Figure out the new age and clear the bounds
	//--------------------------------------------
	//
	Stuff::Scalar age =
		m_age + static_cast<Stuff::Scalar>(info->m_time - m_lastRan) * m_ageRate;
	Verify(age >= 0.0f && age >= m_age);
	*info->m_bounds = Stuff::OBB::Identity;

	//
	//--------------------------------
	// Update the effectToWorld matrix
	//--------------------------------
	//
	Check_Object(info->m_parentToWorld);
	m_localToWorld.Multiply(m_localToParent, *info->m_parentToWorld);

	//
	//--------------------------------------------------
	// Check to see if the top event needs to be handled
	//--------------------------------------------------
	//
	Check_Object(m_specification);
	Event *event;
	while ((event = m_event.GetCurrent()) != nullptr)
	{
		Check_Object(event);
		if (event->m_time > m_age)
			break;

		//
		//-------------------------------------------------------------
		// This event needs to go, so spawn and bump the effect pointer
		//-------------------------------------------------------------
		//
		unsigned flags = ExecuteFlag;
		if ((event->m_flags&SimulationModeMask) == ParentSimulationMode)
		{
			Verify((m_flags&SimulationModeMask) != ParentSimulationMode);
			flags |= m_flags&SimulationModeMask;
		}
		else
			flags |= event->m_flags&SimulationModeMask;

		EffectPtr owned{
			EffectLibrary::Instance->MakeEffect(
				event->m_effectID,
				flags
			)};
		Effect* effect = owned.get();
		Register_Object(effect);
		m_children.push_back(std::move(owned));
		m_event.Next();

		//
		//---------------------------------------------
		// Now set the info for starting the new effect
		//---------------------------------------------
		//
		effect->m_localToParent = event->m_localToParent;
		Stuff::Scalar min_seed =
			m_specification->m_minimumChildSeed.ComputeValue(m_age, m_seed);
		Stuff::Scalar seed_range =
			m_specification->m_maximumChildSeed.ComputeValue(m_age, m_seed) - min_seed;
		Stuff::Scalar seed =
			Stuff::Random::GetFraction()*seed_range + min_seed;
		Clamp(seed, 0.0f, 1.0f);
		ExecuteInfo
			local_info(
				info->m_time,
				&m_localToWorld,
				nullptr,
				seed
			);
		effect->Start(&local_info);
	}

	//
	//------------------------------------------------------------
	// Execute all the children.  If any of them finish, kill them
	//------------------------------------------------------------
	//
	Stuff::OBB child_obb = Stuff::OBB::Identity;
	ExecuteInfo
		child_info(
			info->m_time,
			&m_localToWorld,
			&child_obb
		);
	child_info.m_bounds = &child_obb;
	bool released = false;
	for (EffectPtr& child : m_children)
	{
		Check_Object(child.get());
		if (!child->Execute(&child_info))
		{
			Unregister_Object(child.get());
			child.reset();
			released = true;
		}

		//
		//--------------------------------------------------------------
		// Merge the bounding sphere of the child into the bounds of the
		// parent
		//--------------------------------------------------------------
		//
		Stuff::OBB parent_bounds;
		parent_bounds.Multiply(child_obb, m_localToParent);
		info->m_bounds->Union(*info->m_bounds, parent_bounds);
	}
	if (released)
		std::erase(m_children, nullptr);

	Check_Object(info->m_bounds);

	//
	//----------------------------------------------------------------------
	// Set the new time, then if we have run the course of the effect, start
	// over if we loop, otherwise wait for our children to finish before
	// killing ourselves
	//----------------------------------------------------------------------
	//
	m_lastRan = info->m_time;
	m_age = age;
	if (m_age >= 1.0f)
	{
		if (IsLooped())
			Start(info);
		else if (HasFinished())
			Kill();
	}

	//
	//----------------------------------
	// Tell our parent if we need to die
	//----------------------------------
	//
	gos_PopCurrentHeap();
	return IsExecuted();
}

//------------------------------------------------------------------------------
//
void gosFX::Effect::Stop()
{
	Check_Object(this);

	//
	//------------------------------------
	// Stop the children then stop ourself
	//------------------------------------
	//
	m_age = 1.0f;
	m_event.Last();
	m_event.Next();
}

//------------------------------------------------------------------------------
//
void gosFX::Effect::Kill()
{
	Check_Object(this);

	//
	//------------------------------------
	// Kill the children then kill ourself
	//------------------------------------
	//
	for (const EffectPtr& child : m_children)
	{
		Check_Object(child.get());
		child->Kill();
	}
	SetExecuteOff();
}

//------------------------------------------------------------------------------
//
void gosFX::Effect::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	// fx_trace v1: per-spec-name draw-invocation counter (env-gated, default-off).
	// Per plan v6 §1: nonzero under A2 default-on (Effect::Draw runs to
	// completion; only the 4 MLR work-leaves no-op downstream).
	FX_TRACE_DRAW(m_specification ? static_cast<const char*>(m_specification->m_name) : "(no_spec)");

	//
	//-------------------------------------
	// Make sure all the children get drawn
	//-------------------------------------
	//
	DrawInfo new_info;
	Check_Object(m_specification);
	new_info.m_state.Combine(info->m_state, m_specification->m_state);
	Stuff::LinearMatrix4D local_to_world;
	local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
	new_info.m_parentToWorld = &local_to_world;
	new_info.m_clipper = info->m_clipper;
	new_info.m_clippingFlags = info->m_clippingFlags;
	for (const EffectPtr& child : m_children)
	{
		Check_Object(child.get());
		child->Draw(&new_info);
	}
}

//------------------------------------------------------------------------------
//
bool gosFX::Effect::HasFinished()
{
	Check_Object(this);

	//
	//-------------------------------------------------------------------------
	// An effect is not finished if it is executing and its life hasn't expired
	//-------------------------------------------------------------------------
	//
	if (IsExecuted() && m_age < 1.0f)
		return false;

	//
	//-----------------------------------------------
	// It is also not finished if it has any children
	//-----------------------------------------------
	//
	return m_children.empty();
}

//#############################################################################
//##########################    gosFX::Effect__ClassData    ##########################
//#############################################################################

void
	gosFX::Effect__ClassData::TestInstance()
{
	Verify(IsDerivedFrom(gosFX::Effect::DefaultData));
}
