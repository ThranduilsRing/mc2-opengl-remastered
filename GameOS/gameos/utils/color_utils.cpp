#include "color_utils.h"

uint32_t vec4_to_uint32(const vec4& v) {

    uint32_t x = (uint32_t)(clamp(v.x, 0.0f, 1.0f) * 255.0f);
    uint32_t y = (uint32_t)(clamp(v.y, 0.0f, 1.0f) * 255.0f);
    uint32_t z = (uint32_t)(clamp(v.z, 0.0f, 1.0f) * 255.0f);
    uint32_t w = (uint32_t)(clamp(v.w, 0.0f, 1.0f) * 255.0f);

    uint32_t res = x | (y<<8) | (z<<16) | (w<<24);
    return res;
}

vec4 uint32_to_vec4(uint32_t v) {

	float x = static_cast<float>(v & 0xff);
	float y = static_cast<float>((v>>8) & 0xff);
	float z = static_cast<float>((v>>16) & 0xff);
	float w = static_cast<float>((v>>24) & 0xff);

	return (1.0f / 255.0f) * vec4(x, y, z, w);
}
