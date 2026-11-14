#include "math/sim_math.h"
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MATHFUNC_H
#define MATHFUNC_H

#ifndef STUFF_STUFF_HPP
#include<stuff/stuff.hpp>
#endif

// DESCRIBES A COORDINATE SYSTEM, OR FRAME OR REFERENCE
class frameOfRef
{
public:
	glm::vec3 i;
	glm::vec3 j;
	glm::vec3 k;

	frameOfRef(void);

	frameOfRef (frameOfRef &copy)
	{
		*this = copy;
	}

	frameOfRef & set_i (glm::vec3 &new_i)
	{
		i = new_i;
		return *this;
	}

	frameOfRef & set_j (glm::vec3 &new_j)
	{
		j = new_j;
		return *this;
	}

	frameOfRef & set_k (glm::vec3 &new_k)
	{
		k = new_k;
		return *this;
	}

	frameOfRef & operator =(frameOfRef &fr);

	frameOfRef & orthonormalize ()
	{
		return orthonormalize_on_yaxis();
	}

	frameOfRef & orthonormalize_on_xaxis ();

	frameOfRef & orthonormalize_on_yaxis ();

	frameOfRef & orthonormalize_on_zaxis ();

	frameOfRef & reset_to_world_frame ();

	frameOfRef & rotate_about_i (float &angle);
	frameOfRef & rotate_about_j (float &angle);
	frameOfRef & rotate_about_k (float &angle);

	frameOfRef & rotate_about_i_orthonormal (float &angle);
	frameOfRef & rotate_about_j_orthonormal (float &angle);
	frameOfRef & rotate_about_k_orthonormal (float &angle);

	frameOfRef & rotate(float &i_angle,
						float &j_angle,
						float &k_angle);

	frameOfRef & rotate(glm::vec3 &rotation_vector);

	frameOfRef & trans_to_frame (frameOfRef &new_frame);
	frameOfRef & trans_from_frame (frameOfRef &old_frame);
	
	void trans_to_frame (glm::vec3 &vector)
	{
		float vx = vector.x;
		float vy = vector.y;
		float vz = vector.z;
		
		vector.x = i.x*vx + i.y*vy + i.z*vz;
		vector.y = j.x*vx + j.y*vy + j.z*vz;
		vector.z = k.x*vx + k.y*vy + k.z*vz;
	}	
};

void Rotate (Stuff::Vector2DOf<float> &vec, float angle);

void Rotate (glm::vec3 &vec, float angle);

void RotateLight (glm::vec3 &vec, float angle);

void OppRotate (glm::vec3 &vec, float angle);

float distance_from (glm::vec3 &v1, glm::vec3 &v2);

float my_acos (float val);

float angle_from (Stuff::Vector2DOf<float>&v1, Stuff::Vector2DOf<float> &v2);

float angle_from (glm::vec3 &v1, glm::vec3 &v2);

float world_angle_between (glm::vec3 &v1, glm::vec3 &v2);

glm::vec3 relativePositionToPoint (glm::vec3 point, float angle, float distance, unsigned long flags);

long RandomNumber (long range);

long SignedRandomNumber (long range);

bool RollDice (long percent);

// Draws from the simulation stream (RandomNumber, RollDice, SignedRandomNumber) since the mission seed; the SIMID digest
// prints it per sim frame so two runs that drew a different number of times are named before any state differs.
unsigned long SimRandomDraws (void);

void ResetSimRandomDraws (void);

// Seed the simulation stream is pinned to under MC2_DETERMINISTIC_RNG: the startup seed, then the mission seed (Mission::init).
// ABL seedRandom(-1) mixes it with a per-mission call index instead of the wall clock.
void SetSimMissionSeed (unsigned long seed);
unsigned long SimMissionSeed (void);
unsigned long NextSimSeedRandomCall (void);

inline long float2long (float val)
{
	//_ftol TRUNCS not rounds.  Processor wants to round.  Surely there is some flag to not have this happen?
	// There is but BOY is it slow.  We will try Andy's Magical formula instead.
	// Doesn't work either.  Major bug in Intel's FPU.
	// Will simply call long here now to insure working ok and address later.
	
	long result = static_cast<int32_t>(val);
	return result;
	
}

//---------------------------------------------------------------------------
inline float mc2_atan2 (float f1, float f2)
{
	//Return atan of f1/f2;
	float result = 1.570796f;
	//f2 is always assumed positive here!!!
	if (f2 > Stuff::SMALL)
	{
        result = mc2::sim::math::atan2(f1, f2);
	}
	
	return result;
}

// sebi: implemented in math.h
#if 0
//---------------------------------------------------------------------------
inline float fmax (float f1, float f2)
{
	if (f1 > f2)
		return f1;
	else
		return f2;
}

//---------------------------------------------------------------------------
inline float fmin (float f1, float f2)
{
	if (f1 < f2)
		return f1;
	else
		return f2;
}
#endif

#if 0 // defined in vec.h
#ifndef max
template<typename T> T max(const T& a, const T& b) { return a > b ? a : b; }
#endif
#ifndef min
template<typename T> T min(const T& a, const T& b) { return a < b ? a : b; }
#endif
#endif

//---------------------------------------------------------------------------
inline float sign2 (float f1)
{
	if (f1 < 0.0f)
		return -1.0f;
		
	return 0.0f;
}
#endif
