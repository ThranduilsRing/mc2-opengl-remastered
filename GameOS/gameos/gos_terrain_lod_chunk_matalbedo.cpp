// gos_terrain_lod_chunk: terrain material albedo texture array: setters from terrain_material_lib.cpp and the lazy BC7 array load.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"

static const int kMatAlbedoLayerCount = 6;
static const char* const kMatAlbedoChannelNames[kMatAlbedoLayerCount] = {
    "rock", "grass", "dirt", "concrete", "snow", "cliff"
};
// Channel IDs match shaders/include/terrain_mat_layers.hglsl and the terrain-layer
// cooker. The public lookup returns the name-table index directly.
static char  s_matAlbedoLayerPath[kMatAlbedoLayerCount][512] = {{0}};
static char  s_matAlbedoTextureRoot[512] = {0};  // default data/terrain_layers/
// JSON strength (matAlbedoStrength). <0 = "no JSON value" sentinel; the bind
// resolves env MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH > JSON > 0.7 default.
float s_matAlbedoStrengthJson = -1.0f;

// Setters consumed by terrain_material_lib.cpp (TinyJson layers{} extension).
// Local externs per the established accessor pattern in that file.
void gos_SetTerrainMatAlbedoStrength(float s) { s_matAlbedoStrengthJson = s; }
void gos_SetTerrainMatAlbedoTextureRoot(const char* root) {
    if (!root) { s_matAlbedoTextureRoot[0] = '\0'; return; }
    mc2::fmt::to_buf(s_matAlbedoTextureRoot, "{}", mc2::fmt::safe(root));
}
void gos_SetTerrainMatAlbedoLayerPath(int layer, const char* path) {
    if (layer < 0 || layer >= kMatAlbedoLayerCount) return;
    if (!path) { s_matAlbedoLayerPath[layer][0] = '\0'; return; }
    mc2::fmt::to_buf_n(s_matAlbedoLayerPath[layer], sizeof(s_matAlbedoLayerPath[layer]), "{}", mc2::fmt::safe(path));
}
int gos_GetTerrainMatAlbedoLayerIndex(const char* channelName) {
    if (!channelName) return -1;
    for (int i = 0; i < kMatAlbedoLayerCount; ++i)
        if (strcmp(channelName, kMatAlbedoChannelNames[i]) == 0) return i;
    return -1;
}
