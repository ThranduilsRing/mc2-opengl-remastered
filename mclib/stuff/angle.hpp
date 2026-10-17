//===========================================================================//
// File:	angle.hh                                                         //
// Contents: Interface specification for angle classes                       //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"scalar.hpp"

namespace Stuff {
	class Radian;
}

#if !defined(Spew)
	void
		Spew(
			const char* group,
			const Stuff::Radian& angle
		);
#endif

namespace Stuff {

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Radian ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	class Radian
	{
	public:
		Scalar
			angle;

		//
		// Constructors
		//
		Radian()
			{}
		Radian(Scalar a)
			{angle = a;}
		Radian(const Radian &a)
			{angle = a.angle;}

		//
		// Assignment operators
		//
		Radian&
			operator=(Scalar angle)
				{
					Check_Pointer(this);
					this->angle = angle; return *this;
				}
		Radian&
			operator=(const Radian &radian)
				{
					Check_Pointer(this); Check_Object(&radian);
					angle = radian.angle; return *this;
				}

		//
		// Casting
		//
		operator Scalar() const
			{Check_Object(this); return angle;}

		//
		// These comparator functions are not designed to make exact comparisons
		// of Scalaring point numbers, but rather to compare them to within some
		// specified error threshold
		//
		bool
			operator!() const
				{Check_Object(this); return Small_Enough(angle);}

		bool
			operator==(const Radian &r) const
				{Check_Object(this); Check_Object(&r); return Close_Enough(angle,r.angle);}
		bool
			operator==(float r) const
				{Check_Object(this); return Close_Enough(angle,r);}

		bool
			operator!=(const Radian &r) const
				{Check_Object(this); Check_Object(&r); return !Close_Enough(angle,r.angle);}
		bool
			operator!=(float r) const
				{Check_Object(this); return !Close_Enough(angle,r);}

		//
		// Math operators
		//
		Radian&
			Negate(Scalar r)
				{Check_Pointer(this); angle = -r; return *this;}

		Radian&
			Add(Scalar r1,Scalar r2)
				{Check_Pointer(this); angle = r1 + r2; return *this;}
		Radian&
			operator+=(Scalar r)
				{Check_Object(this); angle += r; return *this;}

		Radian&
			Subtract(Scalar r1,Scalar r2)
				{Check_Pointer(this); angle = r1 - r2; return *this;}
		Radian&
			operator-=(Scalar r)
				{Check_Object(this); angle -= r; return *this;}

		Radian&
			Multiply(Scalar r1,Scalar r2)
				{Check_Pointer(this); angle = r1 * r2; return *this;}
		Radian&
			operator*=(Scalar r)
				{Check_Object(this); angle *= r; return *this;}

		Radian&
			Divide(Scalar r1,Scalar r2)
				{
					Check_Pointer(this); Verify(!Small_Enough(r2));
					angle = r1 / r2; return *this;
				}
		Radian&
			operator/=(Scalar r)
				{Check_Object(this); Verify(!Small_Enough(r)); angle /= r; return *this;}

		//
		// Template support
		//
		Radian&
			Lerp(
				const Radian &a,
				const Radian &b,
				Scalar t
			);

		//
		// Support functions
		//
		static Scalar
			Normalize(Scalar Value);
		Radian&
			Normalize();

		#if !defined(Spew)
			friend void
				::Spew(
					const char* group,
					const Radian& angle
				);
		#endif
		void
			TestInstance() const
				{}
		static bool
			TestClass();
	};

}

namespace MemoryStreamIO {

	inline Stuff::MemoryStream&
		Read(
			Stuff::MemoryStream* stream,
			Stuff::Radian *output
		)
			{return stream->ReadBytes(output, sizeof(*output));}
	inline Stuff::MemoryStream&
		Write(
			Stuff::MemoryStream* stream,
			const Stuff::Radian *input
		)
			{return stream->WriteBytes(input, sizeof(*input));}

}
