// Vertex-only patch data; the shared frame block keeps the overlay ABI intact.
layout(push_constant) uniform TerrainPatchConstants {
    int blockOriginX;
    int blockOriginY;
    int quadCountX;
    int quadCountY;
    int edgeStitch;
    int visualMode;
    float skirtDepth;
} uP;
