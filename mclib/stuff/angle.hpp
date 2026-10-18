//===========================================================================//
// File:	angle.hh                                                         //
// Contents: Interface specification for angle classes                       //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"scalar.hpp"

// Step 5 S5.10: Stuff::Radian is gone. Angles are Scalar radians; the two Radian::Normalize intervals are
// mc2::math::wrapAngleStatic / wrapAngleMember (mclib/math/mc2_math.h, corpus-asserted exact).
