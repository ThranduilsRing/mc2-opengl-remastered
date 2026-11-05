#pragma once
// Tangent handedness (glTF TANGENT.w) for the imported-mech face tangent.
// t = UV-gradient tangent dP/du, b = UV-gradient bitangent dP/dv (both from the same uv set,
// unnormalised), n = vertex normal. Returns +1 when cross(n,t) points along b (the unmirrored
// case the shader always assumed), -1 for a mirrored UV island. Degenerate input returns +1.
inline float mechTangentSign(const float t[3], const float b[3], const float n[3])
{
    const float cx = n[1] * t[2] - n[2] * t[1];
    const float cy = n[2] * t[0] - n[0] * t[2];
    const float cz = n[0] * t[1] - n[1] * t[0];
    const float d = cx * b[0] + cy * b[1] + cz * b[2];
    return d < 0.0f ? -1.0f : 1.0f;
}
