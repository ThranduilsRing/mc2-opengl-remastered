//#version 400 (version provided by material prefix)

layout(vertices = 3) out;

in vec4 vs_Color[];
in vec2 vs_Texcoord[];
in float vs_TerrainType[];
in vec3 vs_WorldPos[];
in vec3 vs_WorldNorm[];

out vec4 tcs_Color[];
out vec2 tcs_Texcoord[];
out float tcs_TerrainType[];
out vec3 tcs_WorldPos[];
out vec3 tcs_WorldNorm[];

uniform vec4 tessLevel;
uniform vec4 tessDistanceRange;
uniform vec4 cameraPos;
uniform int  useQuadRecords;  // 0 = passthrough (default), 1 = read SSBO
uniform int  ssboRecordBase;  // global record index offset for this draw call

// M1 compact quad record — must match TerrainQuadRecord in gos_terrain_patch_stream.h.
// std430 layout; 192 bytes per record (12 vec4s).
struct TerrainQuadRecord {
    vec4 worldPos0, worldPos1, worldPos2, worldPos3;  // xyz + w=pad
    vec4 worldNorm0, worldNorm1, worldNorm2, worldNorm3;
    vec4 uvData;        // minU, minV, maxU, maxV
    ivec4 lightRGBs;   // corners 0-3, packedColor as ARGB int
    ivec4 fogRGBs;     // corners 0-3, packedColor as frgb int
    ivec4 control;     // x=terrainHandle, y=flags (bit0=uvMode,bit1=pzTri1,bit2=pzTri2), zw=pad
};

layout(std430, binding = 0) readonly buffer QuadRecordBuf {
    TerrainQuadRecord records[];
};

// Unpack ARGB int to vec4 (each component 0..255 -> 0..1).
vec4 unpackARGB(int packedColor) {
    return vec4(
        float((packedColor >> 16) & 0xFF) / 255.0,  // R
        float((packedColor >>  8) & 0xFF) / 255.0,  // G
        float((packedColor       ) & 0xFF) / 255.0,  // B
        float((packedColor >> 24) & 0xFF) / 255.0   // A
    );
}

// Retrieve ivec4 component by index 0-3.
int ivec4Idx(ivec4 v, int idx) {
    if (idx == 0) return v.x;
    if (idx == 1) return v.y;
    if (idx == 2) return v.z;
    return v.w;
}

void main()
{
    if (useQuadRecords == 0) {
        // --- Passthrough path (default, unchanged) ---
        tcs_Color[gl_InvocationID]       = vs_Color[gl_InvocationID];
        tcs_Texcoord[gl_InvocationID]    = vs_Texcoord[gl_InvocationID];
        tcs_TerrainType[gl_InvocationID] = vs_TerrainType[gl_InvocationID];
        tcs_WorldPos[gl_InvocationID]    = vs_WorldPos[gl_InvocationID];
        tcs_WorldNorm[gl_InvocationID]   = vs_WorldNorm[gl_InvocationID];
        gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;

        if (gl_InvocationID == 0) {
            float level = max(tessLevel.x, 1.0);
            gl_TessLevelOuter[0] = level;
            gl_TessLevelOuter[1] = level;
            gl_TessLevelOuter[2] = level;
            gl_TessLevelInner[0] = level;
        }
        return;
    }

    // --- Record path (MC2_PATCHSTREAM_QUAD_RECORDS_DRAW=1) ---
    // Each record maps to 2 patches: gl_PrimitiveID/2 = recordIdx,
    //                                gl_PrimitiveID%2 = triIdx (0=tri1, 1=tri2).
    int recordIdx = int(ssboRecordBase) + int(gl_PrimitiveID) / 2;
    int triIdx    = int(gl_PrimitiveID) % 2;
    int id        = int(gl_InvocationID); // 0, 1, or 2

    TerrainQuadRecord rec = records[recordIdx];
    int uvMode = rec.control.y & 1;
    int pzTri1 = (rec.control.y >> 1) & 1;
    int pzTri2 = (rec.control.y >> 2) & 1;

    // Corner index for this invocation.
    // TOPRIGHT  (uvMode=0): tri1=corners[0,1,2], tri2=corners[0,2,3]
    // BOTTOMLEFT(uvMode=1): tri1=corners[0,1,3], tri2=corners[1,2,3]
    int cornerIdx;
    if (uvMode == 0) {
        if (triIdx == 0) {
            if (id == 0) cornerIdx = 0;
            else if (id == 1) cornerIdx = 1;
            else cornerIdx = 2;
        } else {
            if (id == 0) cornerIdx = 0;
            else if (id == 1) cornerIdx = 2;
            else cornerIdx = 3;
        }
    } else {
        if (triIdx == 0) {
            if (id == 0) cornerIdx = 0;
            else if (id == 1) cornerIdx = 1;
            else cornerIdx = 3;
        } else {
            if (id == 0) cornerIdx = 1;
            else if (id == 1) cornerIdx = 2;
            else cornerIdx = 3;
        }
    }

    // World position and normal from record corners.
    vec4 wp = (cornerIdx == 0) ? rec.worldPos0
             :(cornerIdx == 1) ? rec.worldPos1
             :(cornerIdx == 2) ? rec.worldPos2
             :                    rec.worldPos3;
    vec4 wn = (cornerIdx == 0) ? rec.worldNorm0
             :(cornerIdx == 1) ? rec.worldNorm1
             :(cornerIdx == 2) ? rec.worldNorm2
             :                    rec.worldNorm3;
    tcs_WorldPos[gl_InvocationID]  = wp.xyz;
    tcs_WorldNorm[gl_InvocationID] = wn.xyz;

    // UV reconstruction (verified against quad.cpp actual UV assignment):
    //   corner 0 = (minU, minV), corner 1 = (maxU, minV)
    //   corner 2 = (maxU, maxV), corner 3 = (minU, maxV)
    // uvData = vec4(minU, minV, maxU, maxV)
    float u = (cornerIdx == 1 || cornerIdx == 2) ? rec.uvData.z : rec.uvData.x;
    float v = (cornerIdx == 0 || cornerIdx == 1) ? rec.uvData.y : rec.uvData.w;
    tcs_Texcoord[gl_InvocationID] = vec2(u, v);

    // Lighting -- unpack ARGB.
    int lrgb = ivec4Idx(rec.lightRGBs, cornerIdx);
    int frgb = ivec4Idx(rec.fogRGBs,   cornerIdx);
    tcs_Color[gl_InvocationID] = unpackARGB(lrgb);

    // TerrainType: VS computes vs_TerrainType = floor(fog.x * 255.0 + 0.5).
    // fog.x = float(frgb & 0xFF) / 255.0 in CPU packing.
    // So TerrainType = float(frgb & 0xFF), as an integer index 0-3 (NOT normalized).
    tcs_TerrainType[gl_InvocationID] = float(frgb & 0xFF);

    // gl_Position unused by TES in main path; set to safe degenerate.
    gl_out[gl_InvocationID].gl_Position = vec4(0.0, 0.0, 0.0, 1.0);

    // Tessellation levels -- only invocation 0 writes patch-level state.
    if (id == 0) {
        int pzValid = (triIdx == 0) ? pzTri1 : pzTri2;
        float level = (pzValid != 0) ? max(tessLevel.x, 1.0) : 0.0;
        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelInner[0] = level;
    }
}
