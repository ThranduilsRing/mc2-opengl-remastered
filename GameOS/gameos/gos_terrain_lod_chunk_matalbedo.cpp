// gos_terrain_lod_chunk: terrain material albedo texture array: setters from terrain_material_lib.cpp and the lazy BC7 array load.
// Declared in the class headers; shared names in gos_terrain_lod_chunk_internal.h.
#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"


// ---------------------------------------------------------------------------
// TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB albedo GL_TEXTURE_2D_ARRAY
// (rock/grass/dirt/concrete/snow/cliff, layer order = MAT_LAYER_* 0..5) loaded
// straight from the cooked data/terrain_layers/<channel>_albedo.ktx2 files.
// Plain GLuint, same self-contained single-owner pattern as s_controlMapTex.
// glName 0 = not loaded (gate off / load failed) -> u_useMatAlbedo uploads 0
// at draw -> frag takes the verbatim colormap-tint else-path (byte-identical).
// Built lazily at first gate-ON bind (GL context guaranteed live at draw).
// ---------------------------------------------------------------------------
GLuint s_matAlbedoArrayTex   = 0;
bool   s_matAlbedoLoadTried  = false;
// JSON overrides (terrain_materials.json "layers.<channel>.albedo" +
// "textureRoot"), applied by terrainMaterials_apply BEFORE the first draw
// (mission load precedes the first chunk bind). Empty = shipped default path.
static const int kMatAlbedoLayerCount = 6;
static const char* const kMatAlbedoChannelNames[kMatAlbedoLayerCount] = {
    "rock", "grass", "dirt", "concrete", "snow", "cliff"
};
// TERRAIN-MATERIAL-TEXTURES-1-FIX (fix A, INDEX ORACLE -- shared source of truth):
// This table is the C++ mirror of the frag's MAT_LAYER_* constants
// (shaders/include/terrain_mat_layers.hglsl). The albedo array is uploaded in
// kMatAlbedoChannelNames[] order (layer i = channel i), and the frag samples
// channel C at texture()'s layer arg = MAT_LAYER_<C>. Those two orders MUST agree
// or dirt renders with the concrete/road albedo (the reported bug). kMatAlbedoExpectedLayer[i]
// records what the frag's constant table says layer i should be; a load-time HARD
// ERROR fires (and the array is disabled -> legacy tint path) if array index i !=
// its frag constant. Keep these six values equal to their index -- if the frag
// header ever reorders, update BOTH here and terrain_mat_layers.hglsl in lockstep.
//   MAT_LAYER_ROCK=0 GRASS=1 DIRT=2 CONCRETE=3 SNOW=4 MARBLE_CLIFF=5
static const int kMatAlbedoExpectedLayer[kMatAlbedoLayerCount] = {
    0,  // rock     == MAT_LAYER_ROCK
    1,  // grass    == MAT_LAYER_GRASS
    2,  // dirt     == MAT_LAYER_DIRT
    3,  // concrete == MAT_LAYER_CONCRETE
    4,  // snow     == MAT_LAYER_SNOW
    5,  // cliff    == MAT_LAYER_MARBLE_CLIFF
};
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

// TERRAIN-MATERIAL-TEXTURES-1 lazy loader: called at the first gate-ON bind
// (GL context live, like the other lazy chunk resources). Loads the 6 cooked
// BC7 sRGB KTX2 layers and uploads the stored block streams VERBATIM
// (glCompressedTexSubImage3D) into an immutable
// GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM array -- sRGB decode happens in the
// sampler (the BC7-sRGB audit finding; a linear internalformat here would
// double-bright the terrain after the shader's own lighting).
// ALL-or-nothing: any layer failing (missing file, not BC7-sRGB, dim/mip
// mismatch vs layer 0) leaves glName 0 and logs why -- the draw then uploads
// u_useMatAlbedo=0 and the frag stays on the verbatim tint path (fail-soft).
void tglc_EnsureMatAlbedoArrayLoaded()
{
    if (s_matAlbedoLoadTried) return;
    s_matAlbedoLoadTried = true;

    if (!GLEW_ARB_texture_compression_bptc) {
        mc2::fmt::print_out("[TERRAIN_MAT_TEX] BPTC unsupported on this GL -- albedo array disabled\n");
        fflush(stdout);
        return;
    }

    // TERRAIN-MATERIAL-TEXTURES-1-FIX (fix A, INDEX ORACLE): print the
    // channel->array-layer table the loader will build, alongside the frag's
    // MAT_LAYER_* constant each channel is sampled at, and HARD-ERROR (disable the
    // array -> legacy tint) on any mismatch. This is the shared-source-of-truth
    // guard: the manifest cook order, this upload order, and the frag constants
    // must all agree or the reported "dirt shows road albedo" index bug recurs.
    {
        bool orderOk = true;
        mc2::fmt::print_out("[TERRAIN_MAT_TEX] channel->layer table (loader upload order vs frag MAT_LAYER_* constant):\n");
        for (int i = 0; i < kMatAlbedoLayerCount; ++i) {
            const int frag = kMatAlbedoExpectedLayer[i];
            const bool match = (frag == i);
            if (!match) orderOk = false;
            mc2::fmt::print_out("[TERRAIN_MAT_TEX]   loader layer {:d} = '{}'  frag MAT_LAYER = {:d}  {}\n", mc2::fmt::as_i32(i), mc2::fmt::safe(kMatAlbedoChannelNames[i]), mc2::fmt::as_i32(frag), mc2::fmt::safe(match ? "OK" : "*** MISMATCH ***"));
        }
        fflush(stdout);
        if (!orderOk) {
            mc2::fmt::print_out("[TERRAIN_MAT_TEX] ERROR: channel->layer order disagrees with the frag MAT_LAYER_* constants -- albedo array DISABLED to avoid a wrong-material splat (fix A oracle). Sync kMatAlbedoChannelNames[]/kMatAlbedoExpectedLayer[] with terrain_mat_layers.hglsl.\n");
            fflush(stdout);
            return;
        }
    }

    RenderCore::KtxImage imgs[kMatAlbedoLayerCount];
    int refW = 0, refH = 0, refMips = 0;
    for (int i = 0; i < kMatAlbedoLayerCount; ++i) {
        char path[1024];
        if (s_matAlbedoLayerPath[i][0] != '\0') {
            mc2::fmt::to_buf(path, "{}", mc2::fmt::safe(s_matAlbedoLayerPath[i]));
        } else {
            const char* root = (s_matAlbedoTextureRoot[0] != '\0')
                             ? s_matAlbedoTextureRoot : "data/terrain_layers/";
            const size_t rl = strlen(root);
            const bool needSlash = (rl > 0 && root[rl - 1] != '/' && root[rl - 1] != '\\');
            mc2::fmt::to_buf(path, "{}{}{}_albedo.ktx2", mc2::fmt::safe(root), mc2::fmt::safe(needSlash ? "/" : ""), mc2::fmt::safe(kMatAlbedoChannelNames[i]));
        }
        if (!RenderCore::ktxLoadRgba8(path, imgs[i])) {
            mc2::fmt::print_out("[TERRAIN_MAT_TEX] layer {:d} ({}): load FAILED '{}' -- albedo array disabled\n", mc2::fmt::as_i32(i), mc2::fmt::safe(kMatAlbedoChannelNames[i]), mc2::fmt::safe(path));
            fflush(stdout);
            return;
        }
        const RenderCore::KtxImage& img = imgs[i];
        if (!img.isCompressed || img.vkFormat != 146u) {  // VK_FORMAT_BC7_SRGB_BLOCK
            printf("[TERRAIN_MAT_TEX] layer %d (%s): '%s' vkFormat=%u is not BC7 sRGB (146) -- albedo array disabled\n",
                   i, kMatAlbedoChannelNames[i], path, img.vkFormat);
            fflush(stdout);
            return;
        }
        if (i == 0) { refW = img.width; refH = img.height; refMips = img.mipCount; }
        if (img.width != refW || img.height != refH || img.mipCount != refMips) {
            mc2::fmt::print_out("[TERRAIN_MAT_TEX] layer {:d} ({}): {:d}x{:d} mips={:d} != layer0 {:d}x{:d} mips={:d} -- albedo array disabled\n", mc2::fmt::as_i32(i), mc2::fmt::safe(kMatAlbedoChannelNames[i]), mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height), mc2::fmt::as_i32(img.mipCount), mc2::fmt::as_i32(refW), mc2::fmt::as_i32(refH), mc2::fmt::as_i32(refMips));
            fflush(stdout);
            return;
        }
        mc2::fmt::print_out("[TERRAIN_MAT_TEX] layer {:d} ({}): {} {:d}x{:d} BC7-sRGB mips={:d}\n", mc2::fmt::as_i32(i), mc2::fmt::safe(kMatAlbedoChannelNames[i]), mc2::fmt::safe(path), mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height), mc2::fmt::as_i32(img.mipCount));
        fflush(stdout);
    }

    const GLenum internalformat = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
    const int levels = (refMips > 0) ? refMips : 1;
    GLint prevActive = 0, prevArrayBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevArrayBinding);
    // TEX-CLASS: asset-pool -- terrain per-layer BC7 sRGB albedo 2D_ARRAY (content)
    glGenTextures(1, &s_matAlbedoArrayTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_matAlbedoArrayTex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                   refW, refH, kMatAlbedoLayerCount);
    size_t totalBytes = 0;
    for (int layer = 0; layer < kMatAlbedoLayerCount; ++layer) {
        const RenderCore::KtxImage& img = imgs[layer];
        for (int lvl = 0; lvl < levels; ++lvl) {
            const int lw = (refW >> lvl) ? (refW >> lvl) : 1;
            const int lh = (refH >> lvl) ? (refH >> lvl) : 1;
            const GLsizei imageSize =
                static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);  // BC7: 16 B / 4x4 block
            glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer,
                                      lw, lh, 1, internalformat, imageSize,
                                      img.pixels.data() + img.mipByteOffsets[static_cast<size_t>(lvl)]);
            totalBytes += static_cast<size_t>(imageSize);
        }
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, levels - 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                    (levels > 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);  // world-space tiling
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(prevArrayBinding));
    glActiveTexture(static_cast<GLenum>(prevActive));
    mc2::fmt::print_out("[TERRAIN_MAT_TEX] albedo array READY: {:d} layers {:d}x{:d} BC7-sRGB levels={:d} vram={:.1f} MiB tex={:d}\n", mc2::fmt::as_i32(kMatAlbedoLayerCount), mc2::fmt::as_i32(refW), mc2::fmt::as_i32(refH), mc2::fmt::as_i32(levels), totalBytes / (1024.0 * 1024.0), mc2::fmt::as_u32(s_matAlbedoArrayTex));
    // TERRAIN-MATERIAL-TEXTURES-1-FIX (fix D, MIPS GUARD): a mip-incomplete
    // (levels==1) array shimmers/aliases badly when tiled at terrain distances.
    // The cook writes 12 levels for a 2048^2 layer; levels==1 at runtime means a
    // STALE pre-mip cook was deployed (the cook output post-dates the deploy pickup
    // -- deploy_payload copies whatever _albedo.ktx2 is on disk). Loud so a bad
    // deploy is caught in the console instead of shipping as a soft visual bug.
    if (levels <= 1) {
        mc2::fmt::print_out("[TERRAIN_MAT_TEX] WARNING: albedo array has levels=1 (NO MIPS). Tiled terrain albedo will shimmer. A mip-complete cook was NOT deployed -- re-cook (tools/mc2texcook/cook_terrain_layers.py) THEN re-deploy so the mip-complete data/terrain_layers/*_albedo.ktx2 reach the payload.\n");
    }
    fflush(stdout);
}
