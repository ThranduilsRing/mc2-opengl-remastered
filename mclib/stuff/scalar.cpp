//===========================================================================//
// File:	scalar.cpp                                                       //
// Contents: Base information used by all MUNGA source files                 //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"stuffheaders.hpp"

int
	Stuff::Round(Scalar value)
{
	int whole_part = static_cast<int>(floor(value));
	Scalar fractional_part = value - whole_part;
	if (fractional_part >= 0.5)
	{
		return whole_part + 1;
	}
	else
	{
		return whole_part;
	}
}

void
	Stuff::Find_Roots(
		Scalar a,		// a*x*x + b*x + c = 0
		Scalar b,
		Scalar c,
		Scalar *center,
		Scalar *range
	)
{

	//
	//---------------------------------
	// See if the quadratic is solvable
	//---------------------------------
	//
	*range = b*b - 4.0f*a*c;
	if (*range < 0.0f || Small_Enough(a))
	{
		*range = -1.0f;
	}

	else
	{
		//
		//---------------------------
		// Solve the single root case
		//---------------------------
		//
		a *= 2.0f;
		*center = -b / a;
		if (*range < SMALL)
		{
			*range = 0.0f;
		}

		//
		//--------------------------
		// Find the two-root extents
		//--------------------------
		//
		else
		{
			*range = Sqrt(*range);
			*range /= a;
		}
	}
}


DWORD
	Stuff::Scaled_Float_To_Bits(float in, float min, float max, int bits)
{
	Verify(bits < 32);
	Verify(bits > 0);

	Verify(min < max);
	Verify(in <= max);
	Verify(in >= min);
	

	unsigned int biggest_number = (0xffffffff>>(32-bits));
	float local_in =  in - min;
	float range = (max-min);

	
	DWORD return_value = static_cast<DWORD>(((local_in/range) * static_cast<float>(biggest_number)));
	
	Verify(static_cast<DWORD>(return_value) >= 0x00000000);
	Verify(static_cast<DWORD>(return_value) <= static_cast<DWORD>(biggest_number));


	return return_value;

}

float
	Stuff::Scaled_Float_From_Bits(DWORD in, float min, float max, int bits)
{
	Verify(bits < 32);
	Verify(bits > 0);

	Verify(min < max);


	in &= (0xffffffff>>(32-bits));

	unsigned int biggest_number = (0xffffffff>>(32-bits));

	float ratio = in/static_cast<float>(biggest_number);
	float range = (max-min);
	float return_value = (ratio * range)+min;


	return return_value;
}

DWORD
	Stuff::Scaled_Int_To_Bits(int in, int min, int max, int bits)
{

	Verify(bits < 32);
	Verify(bits > 0);

	Verify(min < max);
	Verify(in <= max);
	Verify(in >= min);

	unsigned int biggest_number = (0xffffffff>>(32-bits));
	int local_in =  in - min;
	int range = (max-min);

	
	DWORD return_value = static_cast<DWORD>(((static_cast<float>(local_in)/static_cast<float>(range)) * static_cast<float>(biggest_number)));
	
	Verify(static_cast<DWORD>(return_value) >= 0x00000000);
	Verify(static_cast<DWORD>(return_value) < static_cast<DWORD>(biggest_number));

	return return_value;

}

int
	Stuff::Scaled_Int_From_Bits(DWORD in, int min, int max, int bits)
{
	Verify(bits < 32);
	Verify(bits > 0);

	Verify(min < max);


	unsigned int biggest_number = (0xffffffff>>(32-bits));

	float ratio = static_cast<float>(in)/static_cast<float>(biggest_number);
	int range = (max-min);
	int return_value = (static_cast<int>((ratio * static_cast<float>(range))))+min;

	return return_value;
}


