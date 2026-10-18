//===========================================================================//
// File:	rotation.hh                                                      //
// Contents: Interface specification for rotation classes                    //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"angle.hpp"
#include"vector3d.hpp"

namespace Stuff {
	class UnitQuaternion;
	class EulerAngles;
}

#if !defined(Spew)
	void
		Spew(
			const char* group,
			const Stuff::EulerAngles &angle
		);
	void
		Spew(
			const char* group,
			const Stuff::UnitQuaternion &angle
		);
#endif

namespace Stuff {

	class UnitQuaternion;
	class UnitVector3D;
	class LinearMatrix4D;

    // sebi
    bool Small_Enough( const EulerAngles &a, Scalar e = SMALL);

    bool Close_Enough( const EulerAngles &a1, const EulerAngles &a2, Scalar e = SMALL);
    bool Close_Enough( const UnitQuaternion &a1, const UnitQuaternion &a2, Scalar e = SMALL);

	//##########################################################################
	//#########################    EulerAngles     #############################
	//##########################################################################

	class EulerAngles
	{
	public:
		Scalar
			pitch,
			yaw,
			roll;

		static const EulerAngles
			Identity;

		//
		// Constructors
		//
		EulerAngles()
			{}
		EulerAngles(
			Scalar p,
			Scalar y,
			Scalar r
		)
			{pitch = p; yaw = y; roll = r;}
		EulerAngles(const EulerAngles &a)
			{pitch = a.pitch; yaw = a.yaw; roll = a.roll;}
		explicit EulerAngles(const UnitQuaternion &quaternion)
			{*this = quaternion;}
		explicit EulerAngles(const LinearMatrix4D &matrix)
			{*this = matrix;}

		//
		// Assignment operators
		//
		EulerAngles&
			operator=(const EulerAngles &angles)
				{
					Check_Pointer(this); Check_Object(&angles);
					pitch = angles.pitch; yaw = angles.yaw; roll = angles.roll;
					return *this;
				}
		EulerAngles&
			operator=(const UnitQuaternion &quaternion);
		EulerAngles&
			operator=(const LinearMatrix4D &matrix);

		//
		// "Close-enough" comparators
		//
		friend bool
			Small_Enough(
				const EulerAngles &a,
				Scalar e/*=SMALL*/
			);
		bool
			operator!() const
				{return Small_Enough(*this);}

		friend bool
			Close_Enough(
				const EulerAngles &a1,
				const EulerAngles &a2,
				Scalar e/*=SMALL*/
			);
		bool
			operator==(const EulerAngles& a) const
				{return Close_Enough(*this,a,SMALL);}
		bool
			operator!=(const EulerAngles& a) const
				{return !Close_Enough(*this,a,SMALL);}

		//
		// Axis index operators
		//
		static int GetMemberCount(void)
			{
				return 3;
			}

		const Scalar&
			operator[](size_t index) const
				{
					Check_Pointer(this);
					Verify(static_cast<unsigned>(index) <= Z_Axis);
					return (&pitch)[index];
				}
		Scalar&
			operator[](size_t index)
				{
					Check_Pointer(this);
					Verify(static_cast<unsigned>(index) <= Z_Axis);
					return (&pitch)[index];
				}

		//
		// Template support
		//
		EulerAngles&
			Lerp(
				const EulerAngles& v1,
				const EulerAngles& v2,
				Scalar t
			);

		//
		// Support functions
		//
		EulerAngles&
			Normalize();

		#if !defined(Spew)
			friend void
				::Spew(
					const char* group,
					const EulerAngles &angle
				);
		#endif

		//
		// Test functions
		//
		void
			TestInstance() const
				{}
		static bool
			TestClass();
	};

	//##########################################################################
	//#########################    UnitQuaternion    ###############################
	//##########################################################################

	class UnitQuaternion
	{
	public:
		static const UnitQuaternion
			Identity;

		static void
			InitializeClass();
		static void
			TerminateClass();

		DECLARE_TIMER(static, SlerpTime);
		static DWORD
			SlerpCount;

		Scalar
			x,
			y,
			z,
			w;

		//
		// Constructors
		//
		UnitQuaternion()
			{}
		UnitQuaternion(
			Scalar x,
			Scalar y,
			Scalar z,
			Scalar w
		)
			{
				Check_Pointer(this);
				this->x = x; this->y = y; this->z = z; this->w = w;
				Check_Object(this);
			}

		//
		// Assignment operators
		//
		UnitQuaternion&
			operator=(const UnitQuaternion &q)
				{
					Check_Pointer(this); Check_Object(&q);
					x = q.x; y = q.y; z = q.z; w = q.w; return *this;
				}
		UnitQuaternion&
			operator=(const EulerAngles &angles);
		UnitQuaternion&
			operator=(const LinearMatrix4D &matrix);
		UnitQuaternion&
			operator=(const Vector3D &v);


		//
		// "Close-enough" comparators
		//
		friend bool
			Small_Enough(
				const UnitQuaternion &q,
				Scalar e=SMALL
			)
				{Check_Object(&q); return Close_Enough(q.w,1.0f,e);}
		bool
			operator!() const
				{return Small_Enough(*this,SMALL);}

		friend bool
			Close_Enough(
				const UnitQuaternion &a1,
				const UnitQuaternion &a2,
				Scalar e/*=SMALL*/
			);
		bool
			operator==(const UnitQuaternion& a) const
				{return Close_Enough(*this,a,SMALL);}
		bool
			operator!=(const UnitQuaternion& a) const
				{return !Close_Enough(*this,a,SMALL);}

		//
		// Axis index operators
		//
		const Scalar&
			operator[](size_t index) const
				{
					Check_Pointer(this);
					Verify(static_cast<unsigned>(index) <= W_Axis);
					return (&x)[index];
				}
		Scalar&
			operator[](size_t index)
				{
					Check_Pointer(this);
					Verify(static_cast<unsigned>(index) <= W_Axis);
					return (&x)[index];
				}

		Scalar
			GetAngle();
		void
			GetAxis(UnitVector3D *axis);

		//
		// Multiplication operators
		//
		UnitQuaternion&
			Multiply(
				const UnitQuaternion &q1,
				const UnitQuaternion &q2
			);
		UnitQuaternion&
			Multiply(
				const UnitQuaternion &q,
				Scalar scale
			);
		UnitQuaternion&
			MultiplyScaled(
				const UnitQuaternion &q1,
				const UnitQuaternion &q2,
				Scalar t
			);

		//
		// Transform functions
		//
		UnitQuaternion&
			Multiply(
				const UnitQuaternion &q,
				const LinearMatrix4D &m
			);
		UnitQuaternion&
			operator*=(const LinearMatrix4D &m);

 		//
		// Template support
		//

#if 0
		UnitQuaternion&
			Lerp(
				const UnitQuaternion& v1,
				const UnitQuaternion& v2,
				Scalar t
			);
#endif

		UnitQuaternion&
			Lerp(
				const UnitQuaternion& p, 
				const UnitQuaternion& q, 
				Scalar t
			);

		UnitQuaternion&
			FastLerp(
				const UnitQuaternion& p, 
				const UnitQuaternion& q, 
				Scalar t
			);
		

		UnitQuaternion&
			Lerp(
				const EulerAngles& v1,
				const EulerAngles& v2,
				Scalar t
			);

		//
		// Miscellaneous functions
		//
		UnitQuaternion&
			Normalize();

		UnitQuaternion&
			FastNormalize();

		UnitQuaternion&
			Subtract(
				const UnitQuaternion &end,
				const UnitQuaternion &start
			);
		UnitQuaternion&
			Subtract(
				const UnitVector3D &end,
				const UnitVector3D &start
			);
		UnitQuaternion&
			Subtract(
				const Vector3D &end,
				const Vector3D &start
			);
#if 0
		UnitQuaternion&
			Combine(
				const EulerAngles& v1,
				Scalar t1,
				const EulerAngles& v2,
				Scalar t2
			);
#endif




		// JSE -------------------------------------------------------
		// ---- UnitQuaternion Slerping and Splining routines 
		// ---- USE AT OWN RISK!!!! 
		// ---- This code is as of yet untested!!!
		//------------------------------------------------------------

		

		UnitQuaternion
			Squad(
				const UnitQuaternion& p, // start quaternion
				const UnitQuaternion& a, // start tangent quaternion
				const UnitQuaternion& b, // end tangent quaternion
				const UnitQuaternion& q, // end quaternion
				Scalar t
			);

		UnitQuaternion
			SquadRev(
				Scalar angle,			// angle of rotation 
				const Point3D& axis,	// the axis of rotation 
				const UnitQuaternion& p,	// start quaternion 
				const UnitQuaternion& a, 	// start tangent quaternion 
				const UnitQuaternion& b, 	// end tangent quaternion 
				const UnitQuaternion& q,	// end quaternion 
				Scalar t 				// parameter, in range [0.0,1.0] 
			);
		
		UnitQuaternion& 
			MakeClosest(const UnitQuaternion& qto);

		Scalar
			Dot(
				const UnitQuaternion& p, 
				const UnitQuaternion& q
			);
		
		void
			Negate()
		{
			x = -x;
			y = -y;
			z = -z;
			w = -w;
		}

		UnitQuaternion&
			Inverse(
				const UnitQuaternion& q
			);

		UnitQuaternion&
			Exp(
				const UnitQuaternion& q
			);

		UnitQuaternion&
			Divide(
				const UnitQuaternion& p, 
				const UnitQuaternion& q
			);

		UnitQuaternion&
			LnDif(
				const UnitQuaternion& p, 
				const UnitQuaternion& q
			);

		UnitQuaternion&
			LogN(
				const UnitQuaternion& q
			);

		UnitQuaternion
			CompA(
				const UnitQuaternion& qprev,
				const UnitQuaternion& q,
				const UnitQuaternion& qnext
			);

		UnitQuaternion&
			Orthog(
				const UnitQuaternion& p, 
				const Point3D& axis
			);

		// END JSE ---------------------------------------
		//------------------------------------------------






		//
		// Support functions
		//
		#if !defined(Spew)
			friend void
				::Spew(
					const char* group,
					const UnitQuaternion &quat
				);
		#endif
		void
			TestInstance() const;
		static bool
			TestClass();
	};

	//
	// Notation-file rotations are yaw, pitch and roll (S5.9 folded the YawPitchRoll type into these two; the formulas are its
	// matrix conversions, verbatim, so a notation entry reads and writes the same bits).
	//
	UnitQuaternion
		Quaternion_From_Yaw_Pitch_Roll(Scalar yaw, Scalar pitch, Scalar roll);
	void
		Yaw_Pitch_Roll_From_Quaternion(const UnitQuaternion &q, Scalar *yaw, Scalar *pitch, Scalar *roll);

}

namespace MemoryStreamIO {

	inline Stuff::MemoryStream&
		Read(
			Stuff::MemoryStream* stream,
			Stuff::EulerAngles *output
		)
			{return stream->ReadBytes(output, sizeof(*output));}
	inline Stuff::MemoryStream&
		Write(
			Stuff::MemoryStream* stream,
			const Stuff::EulerAngles *input
		)
			{return stream->WriteBytes(input, sizeof(*input));}

	inline Stuff::MemoryStream&
		Read(
			Stuff::MemoryStream* stream,
			Stuff::UnitQuaternion *output
		)
			{return stream->ReadBytes(output, sizeof(*output));}
	inline Stuff::MemoryStream&
		Write(
			Stuff::MemoryStream* stream,
			const Stuff::UnitQuaternion *input
		)
			{return stream->WriteBytes(input, sizeof(*input));}

}
