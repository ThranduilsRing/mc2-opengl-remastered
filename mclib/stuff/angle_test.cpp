//===========================================================================//
// File:	angle.tst                                                        //
// Contents: Tests for angle classes                                         //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"stuffheaders.hpp"

//
//#############################################################################
//#############################################################################
//
bool
	Radian::TestClass()
{
	SPEW((GROUP_STUFF_TEST, "Starting Radian Test..."));

	const Radian a(1.25f);
	Radian
		b,c;

	Test_Assumption(a);

	c = 0.0f;
	Test_Assumption(!c);

	Test_Assumption(Normalize(3.1f) == 3.1f);
	Test_Assumption(Normalize(-3.1f) == -3.1f);
	Scalar f = Normalize(Pi+Pi_Over_2);
	Test_Assumption(Close_Enough(f,Pi_Over_2 - Pi));
	f = Normalize(-Pi-Pi_Over_2);
	Test_Assumption(Close_Enough(f,Pi - Pi_Over_2));

	c = a;
#if 0
	Test_Assumption(c			== a);
	Test_Assumption(c.angle	== a);
	Test_Assumption(c			== a.angle);
#endif

	b.Negate(c);
#if 0
	Test_Assumption(b			== -c.angle);
#endif

	return true;
}

//
//#############################################################################
//#############################################################################
//
bool
	Degree::TestClass()
{
	SPEW((GROUP_STUFF_TEST, "Starting Degree test..."));

	const Degree a(Degrees_Per_Radian);
	Degree
		b,c;

	Radian
		r(1.0f),s;

	s = a;
	Test_Assumption(r == s);
	b = r;
	s = b;
#if 0
	Test_Assumption(r == s);
#endif
	c = Degrees_Per_Radian;
	s = c;
#if 0
	Test_Assumption(r == s);
#endif
	b = c;
	s = b;
#if 0
	Test_Assumption(r == s);
#endif

	return true;
}

//
//#############################################################################
//#############################################################################
//
bool
	SinCosPair::TestClass()
{
	SPEW((GROUP_STUFF_TEST, "Starting SinCos test..."));
	Radian
		s,r(Pi_Over_2);

	SinCosPair a;
	a = r;

	Test_Assumption(Close_Enough(a.sine,1.0f));
	Test_Assumption(Small_Enough(a.cosine));

	s = a;
	Test_Assumption(s == r);

	return true;
}
