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
long RandomNumber (long range)
{
    //sebi
	//gosASSERT(RAND_MAX==(1<<15)-1 );		// This is always TRUE in VC
	//return( (gos_rand()*range)>>15 );			// Used to used mod (%) - which costs 40+ cycles (AG)

    return gos_rand()%range;
}

//---------------------------------------------------------------------------
bool RollDice (long percent)
{
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







