//===========================================================================//
// File:	rotation.tst                                                     //
// Contents: Test_Assumption code for rotation classes                       //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"stuffheaders.hpp"

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ EulerAngles ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//
//#############################################################################
//#############################################################################
//
bool
	EulerAngles::TestClass()
{
	SPEW((GROUP_STUFF_TEST, "Starting EulerAngle test..."));

	const EulerAngles
		a(Identity);
	EulerAngles
		b;
	const EulerAngles
		c(Pi_Over_4,Pi_Over_6,Pi_Over_3);

	Test_Assumption(!a.pitch && !a.yaw && !a.roll);
	Test_Assumption(c.pitch == Pi_Over_4 && c.yaw == Pi_Over_6 && c.roll == Pi_Over_3);

	Test_Assumption(!a);
	b = c;
	Test_Assumption(b == c);
	Test_Assumption(b != a);

	Test_Assumption(b[Y_Axis] == b.yaw);
	Test_Assumption(c[Z_Axis] == c.roll);

	b.Lerp(a,c,0.5f);
	Test_Assumption(b == EulerAngles(Stuff::Lerp(a.pitch,c.pitch,0.5f),Stuff::Lerp(a.yaw,c.yaw,0.5f),Stuff::Lerp(a.roll,c.roll,0.5f)));

	LinearMatrix4D m;
	m.BuildRotation(c);
	b = m;
	Test_Assumption(b == c);

	return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ UnitQuaternion ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

//
//#############################################################################
//#############################################################################
//
#define NUMBER_OF_MULTIPLICATIONS 30
#include"random.hpp"

class fstream;
bool
	UnitQuaternion::TestClass()
{

	SPEW((GROUP_STUFF_TEST, "Starting UnitQuaternion Test..."));

	return false;
}

