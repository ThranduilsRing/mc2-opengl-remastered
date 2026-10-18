//===========================================================================//
// File:	glmstream.hpp                                                    //
// Contents: the byte formats of the former Stuff math types, on glm          //
//===========================================================================//
// WHAT: what is left of Stuff's math after step 5 (C++23, 2026-10-18): the stream records old files carry, the axis enum the
// engine indexes matrices with, and the homogeneous divide of a Vector4D (Vector4D stays a Stuff type).
// WHY exact: these are the only E rows of step 5 (owner ruling 2026-10-18), byte formats that old files read back:
//   E2: a matrix record is the 48-byte affine block (the stock and mod .fx effect libraries, the MLR light records, mc2fx
//       output); glm::mat4 is 64 bytes, so only its first three columns go to the stream and column 3 comes back (0, 0, 0, 1).
//       An OBB record is that block, then the 12-byte extents, then the 4-byte radius (64 bytes).
//   E1/E3: quaternions (x, y, z, w), vectors and Euler angles are their raw bytes (glm_config.h asserts the sizes).
#pragma once

#include "memorystream.hpp"
#include "../math/mc2_xform.h"

namespace Stuff {

	using mc2::math::Axes;
	using mc2::math::X_Axis;
	using mc2::math::Y_Axis;
	using mc2::math::Z_Axis;
	using mc2::math::W_Axis;

	class Vector4D;

}

// The engine's base coordinate system as the former vector3d.hpp macros (matrix row/column numbers and axis signs): forward
// +Z, right -X, up +Y. Yaw is about up/down, pitch about left/right, roll about forward/back.
#define UP_AXIS Y_Axis
#define DOWN_AXIS Y_Axis
#define FORWARD_AXIS Z_Axis
#define BACKWARD_AXIS Z_Axis
#define RIGHT_AXIS X_Axis
#define LEFT_AXIS X_Axis
#define APPLY_UP_SIGN(y) (y)
#define APPLY_FORWARD_SIGN(z) (z)
#define APPLY_RIGHT_SIGN(x) (-(x))
#define APPLY_DOWN_SIGN(y) (-(y))
#define APPLY_BACKWARD_SIGN(z) (-(z))
#define APPLY_LEFT_SIGN(x) (x)

namespace MemoryStreamIO {

	inline Stuff::MemoryStream& Read(Stuff::MemoryStream* stream, glm::vec3* output)
		{return stream->ReadBytes(output, sizeof(*output));}
	inline Stuff::MemoryStream& Write(Stuff::MemoryStream* stream, const glm::vec3* input)
		{return stream->WriteBytes(input, sizeof(*input));}

	inline Stuff::MemoryStream& Read(Stuff::MemoryStream* stream, glm::quat* output)
		{return stream->ReadBytes(output, sizeof(*output));}
	inline Stuff::MemoryStream& Write(Stuff::MemoryStream* stream, const glm::quat* input)
		{return stream->WriteBytes(input, sizeof(*input));}

	inline Stuff::MemoryStream& Read(Stuff::MemoryStream* stream, mc2::math::Euler* output)
		{return stream->ReadBytes(output, sizeof(*output));}
	inline Stuff::MemoryStream& Write(Stuff::MemoryStream* stream, const mc2::math::Euler* input)
		{return stream->WriteBytes(input, sizeof(*input));}

	// E2: the 48-byte affine record.
	inline Stuff::MemoryStream& Read(Stuff::MemoryStream* stream, glm::mat4* output)
	{
		*output = glm::mat4(1.0f);
		return stream->ReadBytes(glm::value_ptr(*output), 12 * sizeof(float));
	}
	inline Stuff::MemoryStream& Write(Stuff::MemoryStream* stream, const glm::mat4* input)
		{return stream->WriteBytes(glm::value_ptr(*input), 12 * sizeof(float));}

	// E2: the 64-byte OBB record (affine block, extents, radius).
	inline Stuff::MemoryStream& Read(Stuff::MemoryStream* stream, mc2::math::Obb* output)
	{
		Read(stream, &output->localToParent);
		Read(stream, &output->axisExtents);
		return stream->ReadBytes(&output->sphereRadius, sizeof(float));
	}
	inline Stuff::MemoryStream& Write(Stuff::MemoryStream* stream, const mc2::math::Obb* input)
	{
		Write(stream, &input->localToParent);
		Write(stream, &input->axisExtents);
		return stream->WriteBytes(&input->sphereRadius, sizeof(float));
	}

}
