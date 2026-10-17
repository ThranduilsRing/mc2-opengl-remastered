//=======================================================================//
// File:		line.hpp                                                 //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include"stuff.hpp"
#include"point3d.hpp"
#include"unitvector.hpp"

namespace Stuff {

	class Plane;
	class Sphere;
	class OBB;

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Line3D3D ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

	// A segment: origin, unit direction and length (the Ray3D base it had is folded in; nothing else used Ray3D).
	class Line3D
		#if defined(_ARMOR)
			: public Stuff::Signature
		#endif
	{
	 public:
		Point3D
			origin;
		UnitVector3D
			direction;
		Scalar
			length;

		Line3D()
			{}
		Line3D(
			const Point3D &start,
			const UnitVector3D &direction,
			Scalar length
		):
			origin(start),
			direction(direction),
			length(length)
				{}

		void
			Project(
				Scalar length,
				Point3D *result
			)
				{
					Check_Object(this); Check_Object(result);
					Vector3D temp; temp.Multiply(direction,length);
					result->Add(origin,temp);
				}

		//
		// Assignment operators
		//
		Line3D&
			SetDirection(const Vector3D &vector);

			
		//
		// Intersection functions
		//
		Scalar
			GetDistanceTo(
				const Plane &plane,
				Scalar *product
			) const;
		Scalar
			GetDistanceTo(
				const Sphere &sphere,
				Scalar *penetration
			) const;
		Scalar
			GetDistanceTo(const OBB& box);
		Scalar
			GetDistanceTo(
				const OBB& box,
				int *axis
			);

		void
			TestInstance() const
				{}
	};

}
