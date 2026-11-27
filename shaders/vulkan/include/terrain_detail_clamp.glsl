// Skip detail reads only when every possible normal stays below the light floor.
// Detail sampling retains mv_computeLodNear's LOD0/band fade (one when disabled).
const float normalLightFloor = 0.2;
bool mv_needDetail(vec3 baseN, vec3 lightDir, vec4 materialW, float materialSnow,
                   vec3 worldPos, float normalLightFloor) {
    if (uT.csmDebugMode != 0 || lightDir.z >= 0.0 || abs(baseN.z) < 0.85) return true;
    // UNORM detail taps and anti-tile blends stay in [-1,1].
    // Bound every possible perturbed normal before its texture walk.
    // Grass: the base tap plus the 0.6x macro turf tap (terrain_detail_normal.glsl).
    float amplitude = dot(max(materialW, vec4(0)), MATV_NORMAL_BOOST * vec4(1.0 / 3.0, 1.6, 1, 1))
                    + max(materialSnow, 0.0) * 0.9;
    vec2 macroTilt = baseN.xy / max(baseN.z, 0.2) * uT.nfhStrength;
    float tilt = length(abs(macroTilt) + vec2(amplitude * abs(uT.detailStrength)));
    float upperLight = (length(lightDir.xy) * tilt + lightDir.z) * inversesqrt(1.0 + tilt * tilt);
    // Shoreline shading still consumes N.z inside its wet/foam band.
    float nz = clamp(abs(baseN.z), 0.0, 0.9995);
    float run = max(worldPos.z - uT.shorelineWaterElev, 0.0)
              * (nz / max(sqrt(max(1.0 - nz * nz, 1e-6)), 0.03));
    float reach = max(uT.shorelineWetRun, uT.shorelineFoamRun) + abs(uT.shorelineEdgeJitter);
    bool nearShore = TS_shorelineOn != 0 && run > 0.0 && run < reach;
    return nearShore || !(upperLight < normalLightFloor - 1e-4);
}
