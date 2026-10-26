// Skip detail reads only when every possible normal stays below the light floor.
// Detail sampling retains mv_computeLodNear's LOD0/band fade (one when disabled).
const float normalLightFloor = 0.2;
bool mv_needDetail(vec3 baseN, vec3 lightDir, vec4 materialW, float materialSnow,
                   vec3 worldPos, float normalLightFloor) {
    if (uT.csmDebugMode != 0 || lightDir.z >= 0.0 || abs(baseN.z) < 0.85) return true;
    // Local lamps consume detail even when the sun response is fully clamped.
    // A grid cell is only a broad phase: retain the skip outside lamp spheres.
    if (uT.numPoolLights > 0) {
        int lo = 0, hi = uT.numPoolLights;
        bool indexed = uT.lightGridDim > 0;
        if (indexed) {
            ivec2 cell = ivec2(floor((worldPos.xy - vec2(uT.lightGridMinX, uT.lightGridMinY))
                                   * vec2(uT.lightGridInvCellX, uT.lightGridInvCellY)));
            hi = 0;
            if (all(greaterThanEqual(cell, ivec2(0))) && all(lessThan(cell, ivec2(uT.lightGridDim)))) {
                uvec2 span = lightGridCells[cell.y * uT.lightGridDim + cell.x];
                lo = int(span.x); hi = int(span.x + span.y);
            }
        }
        for (int k = lo; k < hi; ++k) {
            int i = indexed ? int(lightGridIndices[k]) : k;
            if (i >= uT.numPoolLights || poolLights[i].lightType == 0u) continue;
            float radius = poolLights[i].farDistance;
            if (poolLights[i]._plPad1 > 0.5 && uT.lightPoolMoverScale < 0.999)
                radius *= max(uT.lightPoolMoverScale, 0.05);
            vec3 delta = worldPos - poolLights[i].position;
            if (radius > 0.0 && dot(delta, delta) < radius * radius) return true;
        }
    }
    // UNORM detail taps and anti-tile blends stay in [-1,1].
    // Bound every possible perturbed normal before its texture walk.
    float amplitude = dot(max(materialW, vec4(0)), MATV_NORMAL_BOOST * vec4(1.0 / 3.0, 1, 1, 1))
                    + max(materialSnow, 0.0) * 0.9;
    vec2 macroTilt = baseN.xy / max(baseN.z, 0.2) * uT.nfhStrength;
    float tilt = length(abs(macroTilt) + vec2(amplitude * abs(uT.detailStrength)));
    float upperLight = (length(lightDir.xy) * tilt + lightDir.z) * inversesqrt(1.0 + tilt * tilt);
    // Shoreline shading still consumes N.z inside its wet/foam band.
    float nz = clamp(abs(baseN.z), 0.0, 0.9995);
    float run = max(worldPos.z - uT.shorelineWaterElev, 0.0)
              * (nz / max(sqrt(max(1.0 - nz * nz, 1e-6)), 0.03));
    float reach = max(uT.shorelineWetRun, uT.shorelineFoamRun) + abs(uT.shorelineEdgeJitter);
    bool nearShore = uT.shorelineOn != 0 && run > 0.0 && run < reach;
    return nearShore || !(upperLight < normalLightFloor - 1e-4);
}
