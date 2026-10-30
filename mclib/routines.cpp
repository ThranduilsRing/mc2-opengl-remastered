//
//
// Faster versions of common routines
//
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include<gameos.hpp>
#include<stdlib.h>
//sebi
#include "platform_windows.h"
#include "Gates/gate.h"
#include "sim_trace.h"
#include "mc2fmt.h"
#include <intrin.h>

void memclear(void *Dest,int Length);
void memfill(void *Dest,int Length);







#ifndef LINUX_BUILD
extern enum { CPU_UNKNOWN, CPU_PENTIUM, CPU_MMX, CPU_KATMAI } Processor;
static int64_t fillnum=-1;
#endif


//
// Instead of using memset(x,0,x) - use this function
//
void memclear(void *Dest,int Len)
{
    memset(Dest, 0, Len);
};





//
// Instead of using memset(x,0xff,x) - use this function
//
void memfill(void *Dest,int Len)
{
    memset(Dest, 0xff, Len);
};

//---------------------------------------------------------------------------
// Random Number Functions
static unsigned long s_simDraws = 0;
void RestoreSimRandomDraws(unsigned long draws) { s_simDraws = draws; }

unsigned long SimRandomDraws (void)
{
	return s_simDraws;
}

void ResetSimRandomDraws (void)
{
	s_simDraws = 0;
}

// Seed the simulation stream is pinned to under MC2_DETERMINISTIC_RNG (startup seed, then mission seed). ABL seedRandom(-1)
// reseeds from it instead of the wall clock.
static unsigned long s_simSeed = 0;
static unsigned long s_seedRandomCalls = 0;

void SetSimMissionSeed (unsigned long seed)
{
	s_simSeed = seed;
	s_seedRandomCalls = 0;
}

unsigned long NextSimSeedRandomCall (void)
{
	return ++s_seedRandomCalls;
}

unsigned long SimMissionSeed (void)
{
	return s_simSeed;
}

long RandomNumber (long range)
{
	++s_simDraws;
	if (mc2::gate::get<"MC2_SIM_IDENTITY_TRACE_DETAIL">()) mc2::fmt::print_err("[LOADRNG] n={} range={} caller={:x}\n", s_simDraws, range, reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)));
	MC2_SIM_TRACE_STACK("RNGCALL");
    //sebi
	//gosASSERT(RAND_MAX==(1<<15)-1 );		// This is always TRUE in VC
	//return( (gos_rand()*range)>>15 );			// Used to used mod (%) - which costs 40+ cycles (AG)

    return gos_rand()%range;
}

//---------------------------------------------------------------------------
bool RollDice (long percent)
{
	++s_simDraws;
	if (mc2::gate::get<"MC2_SIM_IDENTITY_TRACE_DETAIL">()) mc2::fmt::print_err("[LOADRNG] n={} percent={} caller={:x}\n", s_simDraws, percent, reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)));
	MC2_SIM_TRACE_STACK("RNGCALL");
	// DETERMINISTIC-RNG-1 (RISK#2): stock RollDice calls CRT rand() DIRECTLY,
	// bypassing gos_rand. Under MC2_DETERMINISTIC_RNG it must share the single
	// LCG stream, so route through gos_rand() (same 15-bit [0,32767] range as
	// the legacy `rand()%(1<<15)`, identical shaping). Gate OFF = exact CRT path.
	static bool s_checked = false;
	static bool s_det     = false;
	if (!s_checked) {
		s_checked = true;
		s_det = mc2::gate::get<"MC2_DETERMINISTIC_RNG">();
	}
	if (s_det)
		return (((gos_rand()*100)>>15) < percent);
	return (((rand()*100)>>15) < percent);			// Optimized the % out
}







