//***************************************************************************
//
//	err.cpp -- Error Handling routines
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//--------------
// Include Files
#include<stdio.h>
#include<gameos.hpp>
#include "mc2fmt.h"

//***************************************************************************
// Error Handling routines
//***************************************************************************

void Fatal (long errCode, const char* errMessage) {

	char msg[512];
	mc2::fmt::to_buf(msg, " [FATAL {:d}] {} ", mc2::fmt::as_i32(errCode), mc2::fmt::safe(errMessage));
	STOP((msg));
}

//---------------------------------------------------------------------------

void Assert (bool test, long errCode, const char* errMessage) {

	if (!test) {
		char msg[512];
		mc2::fmt::to_buf(msg, " [ASSERT {:d}] {} ", mc2::fmt::as_i32(errCode), mc2::fmt::safe(errMessage));
		STOP((msg));
	}
}

//***************************************************************************
