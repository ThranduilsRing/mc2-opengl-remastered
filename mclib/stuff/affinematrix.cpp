//===========================================================================//
// File:	affnmtrx.cc                                                      //
// Contents: Implementation details for the Affine matrices                  //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"stuffheaders.hpp"

const AffineMatrix4D
	AffineMatrix4D::Identity(true);

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::BuildIdentity()
{
	Check_Pointer(this);

	entries[0] = 1.0f;
	entries[1] = 0.0f;
	entries[2] = 0.0f;
	entries[3] = 0.0f;

	entries[4] = 0.0f;
	entries[5] = 1.0f;
	entries[6] = 0.0f;
	entries[7] = 0.0f;

	entries[8] = 0.0f;
	entries[9] = 0.0f;
	entries[10] = 1.0f;
	entries[11] = 0.0f;
	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::operator=(const Matrix4D &m)
{
	Check_Pointer(this);
	Check_Object(&m);
	Verify(Small_Enough(m(0,3)));
	Verify(Small_Enough(m(1,3)));
	Verify(Small_Enough(m(2,3)));
	Verify(Close_Enough(m(3,3),1.0f));

	memcpy(entries, m.entries, sizeof(entries));
	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::operator=(const EulerAngles &angles)
{
	Check_Pointer(this);
	Check_Object(&angles);

	BuildRotation(angles);
	(*this)(3,0) = 0.0f;
	(*this)(3,1) = 0.0f;
	(*this)(3,2) = 0.0f;

	Check_Object(this);
	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::operator=(const YawPitchRoll &angles)
{
	Check_Pointer(this);
	Check_Object(&angles);

	BuildRotation(angles);
	(*this)(3,0) = 0.0f;
	(*this)(3,1) = 0.0f;
	(*this)(3,2) = 0.0f;

	Check_Object(this);
	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::operator=(const UnitQuaternion &q)
{
	Check_Pointer(this);
	Check_Object(&q);

	BuildRotation(q);
	(*this)(3,0) = 0.0f;
	(*this)(3,1) = 0.0f;
	(*this)(3,2) = 0.0f;

	Check_Object(this);
	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::operator=(const Point3D &p)
{
	Check_Pointer(this);
	Check_Object(&p);

	(*this)(0,0) = 1.0f;
	(*this)(0,1) = 0.0f;
	(*this)(0,2) = 0.0f;

	(*this)(1,0) = 0.0f;
	(*this)(1,1) = 1.0f;
	(*this)(1,2) = 0.0f;

	(*this)(2,0) = 0.0f;
	(*this)(2,1) = 0.0f;
	(*this)(2,2) = 1.0f;

	BuildTranslation(p);

	Check_Object(this);
	return *this;
}

//
//#############################################################################
//#############################################################################
//
AffineMatrix4D&
	AffineMatrix4D::BuildRotation(const EulerAngles &angles)
{
	Check_Pointer(this);
	Check_Object(&angles);

	Verify(
		Vector3D::Forward.z == 1.0f && Vector3D::Right.x == -1.0f && Vector3D::Up.y == 1.0f
		 || Vector3D::Forward.z == -1.0f && Vector3D::Right.x == 1.0f && Vector3D::Up.y == 1.0f
	);

	SinCosPair
		x,
		y,
		z;

	x = angles.pitch;
	y = angles.yaw;
	z = angles.roll;

	(*this)(0,0) = y.cosine*z.cosine;
	(*this)(0,1) = y.cosine*z.sine;
	(*this)(0,2) = -y.sine;

	(*this)(1,0) = x.sine*y.sine*z.cosine - x.cosine*z.sine;
	(*this)(1,1) = x.sine*y.sine*z.sine + x.cosine*z.cosine;
	(*this)(1,2) = x.sine*y.cosine;

	(*this)(2,0) = x.cosine*y.sine*z.cosine + x.sine*z.sine;
	(*this)(2,1) = x.cosine*y.sine*z.sine - x.sine*z.cosine;
	(*this)(2,2) = x.cosine*y.cosine;

	Check_Object(this);
	return *this;
}

//
//#############################################################################
//#############################################################################
//
AffineMatrix4D&
	AffineMatrix4D::BuildRotation(const YawPitchRoll &angles)
{
	Check_Pointer(this);
	Check_Object(&angles);

	Verify(
		Vector3D::Forward.z == 1.0f && Vector3D::Right.x == -1.0f && Vector3D::Up.y == 1.0f
		 || Vector3D::Forward.z == -1.0f && Vector3D::Right.x == 1.0f && Vector3D::Up.y == 1.0f
	);

	SinCosPair
		x,
		y,
		z;

	x = angles.pitch;
	y = angles.yaw;
	z = angles.roll;

	(*this)(0,0) = y.cosine*z.cosine + x.sine*y.sine*z.sine;
	(*this)(0,1) = x.cosine*z.sine;
	(*this)(0,2) = x.sine*y.cosine*z.sine - y.sine*z.cosine;

	(*this)(1,0) = x.sine*y.sine*z.cosine - y.cosine*z.sine;
	(*this)(1,1) = x.cosine*z.cosine;
	(*this)(1,2) = y.sine*z.sine + x.sine*y.cosine*z.cosine;

	(*this)(2,0) = x.cosine*y.sine;
	(*this)(2,1) = -x.sine;
	(*this)(2,2) = x.cosine*y.cosine;

	Check_Object(this);
	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::BuildRotation(const UnitQuaternion &q)
{
	Check_Pointer(this);
	Check_Object(&q);

	Scalar
		a = q.x*q.y,
		b = q.y*q.z,
		c = q.z*q.x,
		d = q.w*q.x,
		e = q.w*q.y,
		f = q.w*q.z,
		g = q.w*q.w,
		h = q.x*q.x,
		i = q.y*q.y,
		j = q.z*q.z;

	(*this)(0,0) = g + h - i - j;
	(*this)(1,0) = 2.0f*(a - f);
	(*this)(2,0) = 2.0f*(c + e);

	(*this)(0,1) = 2.0f*(f + a);
	(*this)(1,1) = g - h + i - j;
	(*this)(2,1) = 2.0f*(b - d);

	(*this)(0,2) = 2.0f*(c - e);
	(*this)(1,2) = 2.0f*(b + d);
	(*this)(2,2) = g - h - i + j;

	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::BuildRotation(const Vector3D &v)
{
	Check_Pointer(this);
	Check_Object(&v);

	UnitQuaternion temp_quat;
	temp_quat = v;
	operator=(temp_quat);
	return *this;
}

//
//###########################################################################
//###########################################################################
//
bool
	Stuff::Close_Enough(
		const AffineMatrix4D &m1,
		const AffineMatrix4D &m2,
		Scalar e
	)
{
	Check_Object(&m2);
	Check_Object(&m1);

	for (size_t i=0; i<ELEMENTS(m1.entries); ++i)
	{
		if (!Close_Enough(m1.entries[i], m2.entries[i], e))
		{
			return false;
		}
	}
	return true;
}


//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::Invert(const AffineMatrix4D& Source)
{
	Check_Pointer(this);
	Check_Object(&Source);
	Verify(this != &Source);

	(*this)(0,0) = Source(1,1)*Source(2,2) - Source(1,2)*Source(2,1);
	(*this)(1,0) = Source(1,2)*Source(2,0) - Source(1,0)*Source(2,2);
	(*this)(2,0) = Source(1,0)*Source(2,1) - Source(1,1)*Source(2,0);

	Scalar det =
		(*this)(0,0)*Source(0,0)
		 + (*this)(1,0)*Source(0,1)
		 + (*this)(2,0)*Source(0,2);
	Verify(!Small_Enough(det));

	(*this)(3,0) =
		-Source(3,0)*(*this)(0,0)
		 - Source(3,1)*(*this)(1,0)
		 - Source(3,2)*(*this)(2,0);

	(*this)(0,1) = Source(0,2)*Source(2,1) - Source(0,1)*Source(2,2);
	(*this)(1,1) = Source(0,0)*Source(2,2) - Source(0,2)*Source(2,0);
	(*this)(2,1) = Source(0,1)*Source(2,0) - Source(0,0)*Source(2,1);
	(*this)(3,1) =
		-Source(3,0)*(*this)(0,1)
		 - Source(3,1)*(*this)(1,1)
		 - Source(3,2)*(*this)(2,1);

	(*this)(0,2) = Source(0,1)*Source(1,2) - Source(0,2)*Source(1,1);
	(*this)(1,2) = Source(1,0)*Source(0,2) - Source(0,0)*Source(1,2);
	(*this)(2,2) = Source(0,0)*Source(1,1) - Source(0,1)*Source(1,0);
	(*this)(3,2) =
		-Source(3,0)*(*this)(0,2)
		 - Source(3,1)*(*this)(1,2)
		 - Source(3,2)*(*this)(2,2);

	det = 1.0f/det;
	for (int i=0; i<12; ++i)
	{
		entries[i] *= det;
	}

	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::Multiply(const AffineMatrix4D &m,const Vector3D &v)
{
	Check_Pointer(this);
	Check_Object(&m);
	Check_Object(&v);

	(*this)(0,0) = m(0,0)*v.x;
	(*this)(1,0) = m(1,0)*v.x;
	(*this)(2,0) = m(2,0)*v.x;
	(*this)(3,0) = m(3,0)*v.x;

	(*this)(0,1) = m(0,1)*v.y;
	(*this)(1,1) = m(1,1)*v.y;
	(*this)(2,1) = m(2,1)*v.y;
	(*this)(3,1) = m(3,1)*v.y;

	(*this)(0,2) = m(0,2)*v.z;
	(*this)(1,2) = m(1,2)*v.z;
	(*this)(2,2) = m(2,2)*v.z;
	(*this)(3,2) = m(3,2)*v.z;

	return *this;
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::Multiply(const AffineMatrix4D& m,const UnitQuaternion &q)
{
	Check_Pointer(this);
	Check_Object(&m);
	Check_Object(&q);

	LinearMatrix4D t(LinearMatrix4D::Identity);
	t.BuildRotation(q);
	return Multiply(m,t);
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::Multiply(const AffineMatrix4D &m,const Point3D& p)
{
	Check_Pointer(this);
	Check_Object(&m);
	Check_Object(&p);

	(*this)(3,0) = m(3,0) + p.x;
	(*this)(3,1) = m(3,1) + p.y;
	(*this)(3,2) = m(3,2) + p.z;

	return *this;
}

//
//###########################################################################
//###########################################################################
//
Scalar
	AffineMatrix4D::Determinant() const
{
	Check_Object(this);

	return
		(*this)(0,0)*((*this)(1,1)*(*this)(2,2) - (*this)(1,2)*(*this)(2,1))
		 + (*this)(0,1)*((*this)(1,2)*(*this)(2,0) - (*this)(1,0)*(*this)(2,2))
		 + (*this)(0,2)*((*this)(1,0)*(*this)(2,1) - (*this)(1,1)*(*this)(2,0));
}

//
//###########################################################################
//###########################################################################
//
AffineMatrix4D&
	AffineMatrix4D::Solve()
{
	Check_Object(this);

	int column;
	Scalar temp;

	//
	//------------------------------------------------------------------
	// Make sure that we get a decent value into the first diagonal spot
	//------------------------------------------------------------------
	//
	if (!(*this)(0,0))
	{
		for (column=0; column<3; ++column)
			if ((*this)(0,column))
				break;
		Verify(column != 3);

		//
		//--------------
		// Swap the columns
		//--------------
		//
		temp					= (*this)(0,0);
		(*this)(0,0)		= (*this)(0,column);
		(*this)(0,column)	= temp;

		temp					= (*this)(1,0);
		(*this)(1,0)		= (*this)(1,column);
		(*this)(1,column)	= temp;

		temp					= (*this)(2,0);
		(*this)(2,0)		= (*this)(2,column);
		(*this)(2,column)	= temp;

		temp					= (*this)(3,0);
		(*this)(3,0)		= (*this)(3,column);
		(*this)(3,column)	= temp;
	}

	//
	//------------------------------------
	// Make sure the diagonal entry is 1.0
	//------------------------------------
	//
	temp = (*this)(0,0);
	(*this)(0,0) = 1.0f;
	(*this)(1,0) /= temp;
	(*this)(2,0) /= temp;
	(*this)(3,0) /= temp;

	//
	//------------------------
	// Make the first row zero
	//------------------------
	//
	temp = (*this)(0,1);
	(*this)(0,1) = 0.0f;
	(*this)(1,1) -= temp * (*this)(1,0);
	(*this)(2,1) -= temp * (*this)(2,0);
	(*this)(3,1) -= temp * (*this)(3,0);

	temp = (*this)(0,2);
	(*this)(0,2) = 0.0f;
	(*this)(1,2) -= temp * (*this)(1,0);
	(*this)(2,2) -= temp * (*this)(2,0);
	(*this)(3,2) -= temp * (*this)(3,0);

	//
	//-------------------------------------------------------------------
	// Make sure that we get a decent value into the second diagonal spot
	//-------------------------------------------------------------------
	//
	if (!(*this)(1,1))
	{
		Verify(!(*this)(2,2));

		//
		//---------------------
		// Swap the (*this) columns
		//---------------------
		//
		temp				= (*this)(1,1);
		(*this)(1,1)	= (*this)(1,2);
		(*this)(1,2)	= temp;

		temp				= (*this)(2,1);
		(*this)(2,1)	= (*this)(2,2);
		(*this)(2,2)	= temp;

		temp				= (*this)(3,1);
		(*this)(3,1)	= (*this)(3,2);
		(*this)(3,2)	= temp;
	}

	//
	//-----------------------------------
	// Make the second diaginal entry 1.0
	//-----------------------------------
	//
	temp				= (*this)(1,1);
	(*this)(1,1)	= 1.0f;
	(*this)(2,1)	/= temp;
	(*this)(3,1)	/= temp;

	//
	//------------------------------------
	// Make the second row zeros otherwise
	//------------------------------------
	//
	temp = (*this)(1,0);
	(*this)(1,0) = 0.0f;
	(*this)(2,0) -= temp * (*this)(2,1);
	(*this)(3,0) -= temp * (*this)(3,1);

	temp = (*this)(1,2);
	(*this)(1,2) = 0.0f;
	(*this)(2,2) -= temp * (*this)(2,1);
	(*this)(3,2) -= temp * (*this)(3,1);

	//
	//---------------------------
	// Make the last diagonal 1.0
	//---------------------------
	//
	Verify((*this)(2,2));
	temp				= (*this)(2,2);
	(*this)(2,2)	= 1.0f;
	(*this)(3,2)	/= temp;

	//
	//------------------------------------
	// Make the third row zeros otherwise
	//------------------------------------
	//
	temp = (*this)(2,0);
	(*this)(2,0) = 0.0f;
	(*this)(3,0) -= temp * (*this)(3,2);

	temp = (*this)(2,1);
	(*this)(2,1) = 0.0f;
	(*this)(3,1) -= temp * (*this)(3,2);

	//
	//-------------------------
	// Return the reduced array
	//-------------------------
	//
	return *this;
}

//
//############################################################################
//############################################################################
//
#if !defined(Spew)
	void
		Spew(
			const char* group,
			const AffineMatrix4D &matrix
		)
	{
		Check_Object(&matrix);
		SPEW((
			group,
			"\n\t| %9f, %9f, %9f, 0 |",
			matrix(0,0),
			matrix(0,1),
			matrix(0,2)
		));
		SPEW((
			group,
			"\t| %9f, %9f, %9f, 0 |",
			matrix(1,0),
			matrix(1,1),
			matrix(1,2)
		));
		SPEW((
			group,
			"\t| %9f, %9f, %9f, 0 |",
			matrix(2,0),
			matrix(2,1),
			matrix(2,2)
		));
		SPEW((
			group,
			"\t| %9f, %9f, %9f, 1 |+",
			matrix(3,0),
			matrix(3,1),
			matrix(3,2)
		));
	}
#endif
