//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef STUFF_STUFF_HPP
#include <algorithm>
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include"stuff/stuff.hpp"
#endif

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef MATHFUNC_H
#include"mathfunc.h"
#endif

glm::vec3 UnitI(-1.0f,0.0f,0.0f);
glm::vec3 UnitJ(0.0f,-1.0f,0.0f);
glm::vec3 UnitK(0.0f,0.0f,1.0f);

void Rotate (Stuff::Vector2DOf<float> &vec, float angle)
{
	float tx = vec.x;
	float sine = sin(angle * DEGREES_TO_RADS);
	float cosine = cos(angle * DEGREES_TO_RADS);

	vec.x *= cosine;
	vec.x += (vec.y * sine);

	vec.y *= cosine;
	vec.y -= (tx * sine);
}

void Rotate (glm::vec3 &vec, float angle)
{
	float tx = vec.x;
	float sine = sin(angle * DEGREES_TO_RADS);
	float cosine = cos(angle * DEGREES_TO_RADS);

	vec.x *= cosine;
	vec.x += (vec.y * sine);

	vec.y *= cosine;
	vec.y -= (tx * sine);
}

void RotateLight (glm::vec3 &vec, float angle)
{
	float tx = vec.x;
	float sine = sin(angle);
	float cosine = cos(angle);

	vec.x *= cosine;
	vec.x += (vec.z * sine);

	vec.z *= cosine;
	vec.z -= (tx * sine);
}

void OppRotate (glm::vec3 &vec, float angle)
{
	float tx = vec.x;
	float sine = sin(angle * DEGREES_TO_RADS);
	float cosine = cos(angle * DEGREES_TO_RADS);

	vec.x *= cosine;
	vec.x -= (vec.y * sine);

	vec.y *= cosine;
	vec.y += (tx * sine);
}

float distance_from (glm::vec3 &v1, glm::vec3 &v2)
{
	glm::vec3 result;
	result.x = v2.x - v1.x;
	result.y = v2.y - v1.y;
	result.z = v2.z - v1.z;

	float dist = glm::length(result);
	return (dist);
}

float my_acos (float val) 
{
	//-------------------------------------------------------
	// Due to floating point round-off error, we may have
	// strayed slightly from the [-1, 1] domain for acos.
	// We may want to put an error threshold in here (like,
	// maybe, 0.00001). This can be used to check if it's
	// most likely a round-off error, or if it's actually out
	// of the domain range...

	val = std::max<double>(val, -1.0);
	val = std::min<double>(val, 1.0);

	return(acos(val));
}

float angle_from (Stuff::Vector2DOf<float>&v1, Stuff::Vector2DOf<float> &v2)
{
	float mag_product = v1.GetLength() * v2.GetLength();

	if (mag_product == 0.0)
		return mag_product;	// 0;
	else
	{
		float vecResult = v1 * v2;
		return (my_acos(vecResult / mag_product) * RADS_TO_DEGREES);
	}
}

float angle_from (glm::vec3 &v1, glm::vec3 &v2)
{
	float mag_product = glm::length(v1) * glm::length(v2);

	if (mag_product == 0.0)
		return mag_product;	// 0;
	else
	{
		float vecResult = glm::dot(v1, v2);
		return (my_acos(vecResult / mag_product) * RADS_TO_DEGREES);
	}
}

//---------------------------------------------------------------------------
float world_angle_between (glm::vec3 &v1, glm::vec3 &v2) 
{
	glm::vec3 facingVec(0.0f,1.0f,0.0f);

	glm::vec3 goalVec;
	goalVec = v1 - v2;

	float angle = angle_from(facingVec, goalVec);

	//--------------------------------
	// Get sign of relative angle.
	float z = (facingVec.x * goalVec.y) - (facingVec.y * goalVec.x);
	if (z > 0.0f)
		angle = -angle;
	
	return(angle);
}

//---------------------------------------------------------------------------
// Class frameOfRef
frameOfRef::frameOfRef ()
{
	reset_to_world_frame();
}

frameOfRef & frameOfRef::reset_to_world_frame ()
{
	i = UnitI;
	j = UnitJ;
	k = UnitK;

	return *this;
}

frameOfRef & frameOfRef::operator = (frameOfRef &fr)
{
	i = fr.i;
	j = fr.j;
	k = fr.k;
	return *this;
}

frameOfRef & frameOfRef::rotate(glm::vec3 &rotation_vector)
{
	rotate_about_k(rotation_vector.z);
	rotate_about_i(rotation_vector.x);
	rotate_about_j(rotation_vector.y);

	return *this;
}

frameOfRef & frameOfRef::rotate_about_i (float &angle)
{
	float cosine, sine;
	glm::vec3 oldj, oldK;

	cosine = cos(angle * DEGREES_TO_RADS);
	sine = sin(angle * DEGREES_TO_RADS);

	oldj = j;
	oldK = k;

	j *= cosine;	// j = J*cos + K*sin
	oldK *= sine;
	j += oldK;

	k *= cosine;	// k = K*cos - J*sin
	oldj *= sine;
	k -= oldj;

	return *this;
}

frameOfRef & frameOfRef::rotate_about_j (float &angle)
{
	float cosine, sine;
	glm::vec3 oldi,oldK;

	cosine = cos(angle * DEGREES_TO_RADS);
	sine = sin(angle * DEGREES_TO_RADS);

	oldi = i;
	oldK = k;

	i *= cosine;	// i = I*cos - K*sin
	oldK *= sine;
	i -= oldK;

	k *= cosine;	// k = K*cos + I*sin
	oldi *= sine;
	k += oldi;

	return *this;
}

frameOfRef & frameOfRef::rotate_about_k (float &angle)
{
	float cosine, sine;
	glm::vec3 oldi,oldJ;

	cosine = cos(angle * DEGREES_TO_RADS);
	sine = sin(angle * DEGREES_TO_RADS);

	oldi = i;
	oldJ = j;

	i *= cosine;
	oldJ *= sine;
	i += oldJ;

	j *= cosine;
	oldi *= sine;
	j -= oldi;

	return *this;
}

frameOfRef & frameOfRef::rotate_about_i_orthonormal (float &angle)
{
	rotate_about_i(angle);
	orthonormalize_on_xaxis();
	
	return *this;
}

frameOfRef & frameOfRef::rotate_about_j_orthonormal (float &angle)
{
	rotate_about_j(angle);
	orthonormalize_on_yaxis();
	
	return *this;
}

frameOfRef & frameOfRef::rotate_about_k_orthonormal (float &angle)
{
	rotate_about_k(angle);
	orthonormalize_on_zaxis();

	return *this;
}

frameOfRef & frameOfRef::rotate(float &i_angle, float &j_angle, float &k_angle)
{
	rotate_about_i_orthonormal(i_angle);
	rotate_about_j_orthonormal(j_angle);
	rotate_about_k_orthonormal(k_angle);
	
	return *this;
}

frameOfRef & frameOfRef::orthonormalize_on_xaxis ()
{
		// DON'T CHANGE THIS ORDER OF OPERATIONS WITHOUT CHANGING THE
		// ROTATE FUNCTIONS
		//-----------------------------------------------------------
	k = glm::cross(i, j);
	j = glm::cross(k, i);

		// TO AVOID MIND-NUMBING OCTAGONAL ROTATION THROBBING
	i = glm::normalize(i);
	j = glm::normalize(j);
	k = glm::normalize(k);

	return *this;
}

frameOfRef & frameOfRef::orthonormalize_on_yaxis ()
{
		// DON'T CHANGE THIS ORDER OF OPERATIONS WITHOUT CHANGING THE
		// ROTATE FUNCTIONS
		//-----------------------------------------------------------
	k = glm::cross(i, j);
	i = glm::cross(j, k);

		// TO AVOID MIND-NUMBING OCTAGONAL ROTATION THROBBING
	i = glm::normalize(i);
	j = glm::normalize(j);
	k = glm::normalize(k);

	return *this;
}

frameOfRef & frameOfRef::orthonormalize_on_zaxis ()
{
		// DON'T CHANGE THIS ORDER OF OPERATIONS WITHOUT CHANGING THE 
		// ROTATE FUNCTIONS
		//-----------------------------------------------------------
	i = glm::cross(j, k);
	j = glm::cross(k, i);

		// TO AVOID MIND-NUMBING OCTAGONAL ROTATION THROBBING
	i = glm::normalize(i);
	j = glm::normalize(j);
	k = glm::normalize(k);

	return *this;
}

/* ---------------------------------------------------------------------- */


