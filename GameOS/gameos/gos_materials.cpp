// GameOS/gameos/gos_materials.cpp
//
// Material profile registry implementation.
// See gos_materials.h for full contract / texture semantic documentation.
//
// Texture semantic used by this table: MaterialTextureSemantic::RawGlId
//   normalTex / metallicRoughnessTex store raw GL texture object IDs.
//   This table is HOMOGENEOUS in RawGlId semantic (unlike the per-actor mech
//   table at binding 2 which is TextureManagerSlot).
//   Slice C (mech.frag sampling) must bind u_normalMap / u_ormMap explicitly
//   using the raw GL id from this table, NOT via mcTextureManager.
//
// Profiles registered (mutually exclusive load; only one non-default profile
// exists in the table at any given runtime):
//   index 0 : "default"         -- passthrough; no textures; flat metallic=0 rough=0.85
//   index 1 : "metal061b"       -- Metal061B normal + ORM (explicit: MC2_MECH_SURFACE_MATERIAL=metal061b)
//             "paintedmetal003" -- paired paint layer, index 2 (loaded with metal061b)
//   index 1 : "painted_subtle"  -- ORM-only + flat normal; subtle detail (MC2_MECH_SURFACE_MATERIAL=painted_subtle)
//   Unset MC2_MECH_SURFACE_MATERIAL -> passthrough only
//

#include "gos_materials.h"

#include "../../RenderCore/MaterialGpu.h"
#include "../../RenderCore/GpuBufferOwner.h"  // MECH-PROFILE-SSBO-OWNER-1: owner record
#include "utils/Image.h"
#include <GL/glew.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

namespace {

// MC2_MATERIAL_GPU defaults ON (mirrors gos_mech_batcher.cpp / gos_static_prop_batcher.cpp).
// Set MC2_MATERIAL_GPU=0 to disable all table upload and bind.
const bool s_gpuEnabled = mc2::gate::get<"MC2_MATERIAL_GPU">();

struct ProfileEntry {
    std::string    name;
    RenderCore::MaterialGpu gpu;      // uploaded to SSBO
    GLuint         ownedNormal = 0;   // raw GL tex owned by this entry; 0 = none
    GLuint         ownedOrm    = 0;   // raw GL tex owned by this entry; 0 = none
};

std::vector<ProfileEntry>                s_profiles;
std::unordered_map<std::string, uint32_t> s_nameToIndex;
// MECH-PROFILE-SSBO-OWNER-1: mech material-profile table SSBO (binding 7),
// narrowed behind a GpuBufferOwner identity record (logical id + lifetime +
// debug name + GLuint value). GL calls happen at the same sites with the same
// args/order/flags/slot; the raw handle is reached only via owner.backendHandle.
// Lifetime Persistent: created once in uploadSsbo() (from init()), destroyed in
// shutdown(); survives mission reload (not rebuilt per map). Registered at
// create, invalidated on destroy. When MC2_MECH_SURFACE_MATERIAL is unset,
// s_profiles is empty, uploadSsbo() early-returns, and glName stays 0 (no
// register) — passthrough is byte-identical.
RenderCore::GpuBufferOwner                s_ssbo{
    RenderCore::RenderResourceId::MechProfileMaterialGpuBuffer,
    RenderCore::RenderResourceLifetime::Persistent,
    "MechProfileMaterialGpuBuffer",
    0u};
bool                                      s_initialized = false;

// Binding 7: mech material profile table (temporary; binding 5 owned by static-prop batcher).
// Debt: D-material-unify — unify static-props and mechs under shared gos_materials table on binding 5.
constexpr GLuint kMechMaterialTableBinding = 7;

// ---------------------------------------------------------------------------
// Helper: upload profile table to SSBO at binding 7.
// ---------------------------------------------------------------------------
static void uploadSsbo() {
    if (!s_gpuEnabled) return;
    if (s_profiles.empty()) return;

    std::vector<RenderCore::MaterialGpu> table;
    table.reserve(s_profiles.size());
    for (const auto& p : s_profiles)
        table.push_back(p.gpu);

    const GLsizeiptr byteSize =
        static_cast<GLsizeiptr>(table.size() * sizeof(RenderCore::MaterialGpu));

    if (s_ssbo.backendHandle == 0) {
        GLuint local = 0;
        glGenBuffers(1, &local);
        s_ssbo.backendHandle = static_cast<uint32_t>(local);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_ssbo.backendHandle));
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 byteSize,
                 table.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // MECH-PROFILE-SSBO-OWNER-1: register the live profile table SSBO (observe-only
    // metadata; never read by the draw path). Reached here only when a profile was
    // loaded (s_profiles non-empty), i.e. MC2_MECH_SURFACE_MATERIAL set.
    RenderCore::RenderResourceDesc d;
    d.id        = s_ssbo.id;
    d.kind      = RenderCore::RenderResourceKind::Buffer;
    d.lifetime  = s_ssbo.lifetime;
    d.format    = RenderCore::RenderResourceFormat::BufferRaw;
    d.debugName = s_ssbo.debugName;
    d.glName    = static_cast<GLuint>(s_ssbo.backendHandle);
    d.sizeBytes = static_cast<uint64_t>(byteSize);
    d.valid     = true;
    RenderCore::registerOrUpdateRenderResource(d);

    mc2::fmt::print_err("[GOS_MATERIALS] uploaded profile table: {:d} entries, ssbo={:d}\n", mc2::fmt::as_u32(static_cast<uint32_t>(table.size())), mc2::fmt::as_u32(static_cast<uint32_t>(s_ssbo.backendHandle)));
}

// ---------------------------------------------------------------------------
// Helper: load a single-channel or RGB PNG as GL_RGB8 (linear).
// Returns GL texture object, or 0 on failure.
// w_out / h_out filled on success.
// ---------------------------------------------------------------------------
static GLuint loadLinearPng(const char* path, int& w_out, int& h_out) {
    Image img;
    if (!img.loadPNG(path)) {
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: could not load PNG: {}\n", mc2::fmt::safe(path));
        return 0;
    }
    w_out = img.getWidth();
    h_out = img.getHeight();
    FORMAT fmt = img.getFormat();

    // Determine GL internal/source formats.
    GLenum glInternal, glFormat, glType;
    switch (fmt) {
    case FORMAT_I8:   // single channel -> replicate to R channel, upload as GL_RED
        glInternal = GL_R8;
        glFormat   = GL_RED;
        glType     = GL_UNSIGNED_BYTE;
        break;
    case FORMAT_IA8:  // two channels (R, A) -> upload as RG
        glInternal = GL_RG8;
        glFormat   = GL_RG;
        glType     = GL_UNSIGNED_BYTE;
        break;
    case FORMAT_RGB8:
        glInternal = GL_RGB8;
        glFormat   = GL_RGB;
        glType     = GL_UNSIGNED_BYTE;
        break;
    case FORMAT_RGBA8:
        glInternal = GL_RGBA8;
        glFormat   = GL_RGBA;
        glType     = GL_UNSIGNED_BYTE;
        break;
    default:
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: unsupported PNG format {:d} for: {}\n", mc2::fmt::as_i32((int)fmt), mc2::fmt::safe(path));
        return 0;
    }

    GLuint tex = 0;
    // TEX-CLASS: asset-pool -- PBR material LUT content texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, glInternal,
                 w_out, h_out, 0,
                 glFormat, glType, img.getPixels());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    mc2::fmt::print_err("[GOS_MATERIALS] loaded linear PNG {} -> tex={:d} ({:d}x{:d} fmt={:d})\n", mc2::fmt::safe(path), mc2::fmt::as_u32(tex), mc2::fmt::as_i32(w_out), mc2::fmt::as_i32(h_out), mc2::fmt::as_i32((int)fmt));
    return tex;
}

// ---------------------------------------------------------------------------
// Helper: build packed ORM texture (GL_RGB8, linear).
//   R = AO       (no AO source -> fill 255)
//   G = Roughness (from roughPath, any channel)
//   B = Metallic  (from metalPath, any channel)
// Both source images must be the same size.
// Returns GL texture object, or 0 on any failure.
// ---------------------------------------------------------------------------
static GLuint buildPackedOrm(const char* roughPath, const char* metalPath,
                              int expectedW, int expectedH) {
    // Load roughness channel.
    Image roughImg;
    if (!roughImg.loadPNG(roughPath)) {
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: could not load roughness PNG: {}\n", mc2::fmt::safe(roughPath));
        return 0;
    }
    // Load metalness channel.
    Image metalImg;
    if (!metalImg.loadPNG(metalPath)) {
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: could not load metalness PNG: {}\n", mc2::fmt::safe(metalPath));
        return 0;
    }

    const int rw = roughImg.getWidth();
    const int rh = roughImg.getHeight();
    const int mw = metalImg.getWidth();
    const int mh = metalImg.getHeight();

    if (rw != mw || rh != mh) {
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: roughness ({:d}x{:d}) and metalness ({:d}x{:d}) size mismatch\n", mc2::fmt::as_i32(rw), mc2::fmt::as_i32(rh), mc2::fmt::as_i32(mw), mc2::fmt::as_i32(mh));
        return 0;
    }

    const int w = rw;
    const int h = rh;
    if (expectedW > 0 && (w != expectedW || h != expectedH)) {
        // Size mismatch vs normal map -- log but continue (shader uses same UV).
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: ORM ({:d}x{:d}) differs from normal ({:d}x{:d}) -- continuing\n",
            mc2::fmt::as_i32(w), mc2::fmt::as_i32(h), mc2::fmt::as_i32(expectedW), mc2::fmt::as_i32(expectedH));
    }

    // Determine bytes per pixel for each source.
    const int rBpp = getBytesPerPixel(roughImg.getFormat());
    const int mBpp = getBytesPerPixel(metalImg.getFormat());
    if (rBpp == 0 || mBpp == 0) {
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: zero-bpp format in ORM sources\n");
        return 0;
    }

    const unsigned char* rPixels = roughImg.getPixels();
    const unsigned char* mPixels = metalImg.getPixels();

    // Pack into RGB: R=255(AO), G=roughness(channel0), B=metalness(channel0).
    std::vector<uint8_t> packed(static_cast<size_t>(w * h * 3));
    for (int i = 0; i < w * h; ++i) {
        packed[i * 3 + 0] = 255u;                       // R = AO (no source, fill 1.0)
        packed[i * 3 + 1] = rPixels[i * rBpp + 0];      // G = Roughness (channel 0)
        packed[i * 3 + 2] = mPixels[i * mBpp + 0];      // B = Metallic  (channel 0)
    }

    GLuint tex = 0;
    // TEX-CLASS: asset-pool -- PBR material LUT content texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, packed.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    mc2::fmt::print_err("[GOS_MATERIALS] packed ORM tex={:d} ({:d}x{:d}) rough={} metal={}\n", mc2::fmt::as_u32(tex), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h), mc2::fmt::safe(roughPath), mc2::fmt::safe(metalPath));
    return tex;
}

// ---------------------------------------------------------------------------
// Helper: build a 1x1 flat normal GL texture (RGB=128,128,255, linear, no perturbation).
// Used by painted_subtle profile so no normal detail is applied.
// ---------------------------------------------------------------------------
static GLuint buildFlatNormalTex() {
    const uint8_t pixel[3] = {128u, 128u, 255u};
    GLuint tex = 0;
    // TEX-CLASS: asset-pool -- PBR material LUT content texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, pixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    mc2::fmt::print_err("[GOS_MATERIALS] built flat normal tex={:d}\n", mc2::fmt::as_u32(tex));
    return tex;
}

// ---------------------------------------------------------------------------
// Register "default" profile at index 0.
// Passthrough: no textures; flat scalars only.
// ---------------------------------------------------------------------------
static void registerDefault() {
    ProfileEntry e;
    e.name = "default";

    RenderCore::MaterialGpu& g = e.gpu;
    g.albedoTex            = RenderCore::kMaterialTexAbsent;
    g.normalTex            = RenderCore::kMaterialTexAbsent;
    g.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
    g.emissiveTex          = RenderCore::kMaterialTexAbsent;
    g.flags                = 0u;
    g.baseColorFactor      = 1.0f;
    g.metallicFactor       = 0.0f;
    g.roughnessFactor      = 0.85f;

    s_nameToIndex["default"] = static_cast<uint32_t>(s_profiles.size());
    s_profiles.push_back(std::move(e));
}

// ---------------------------------------------------------------------------
// Register "metal061b" profile.
// Source files (1K-PNG set from ambientCG Metal061B download):
//   NormalGL  -> normalTex  (GL_RGB8, linear)
//   Roughness + Metalness -> metallicRoughnessTex  (packed ORM, GL_RGB8, linear)
// ---------------------------------------------------------------------------
static void registerMetal061b() {
    static const char* kNormal = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_NormalGL.png";
    static const char* kRough  = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_Roughness.png";
    static const char* kMetal  = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_Metalness.png";

    ProfileEntry e;
    e.name = "metal061b";

    // Load normal map (linear -- NOT sRGB).
    int nw = 0, nh = 0;
    e.ownedNormal = loadLinearPng(kNormal, nw, nh);

    // Build packed ORM texture.
    e.ownedOrm = buildPackedOrm(kRough, kMetal, nw, nh);

    RenderCore::MaterialGpu& g = e.gpu;
    g.albedoTex            = RenderCore::kMaterialTexAbsent;
    // Store raw GL texture IDs (RawGlId semantic -- see header).
    // Slice C (mech.frag) must bind these as raw GL textures, NOT via mcTextureManager.
    g.normalTex            = e.ownedNormal  != 0
                                 ? e.ownedNormal
                                 : RenderCore::kMaterialTexAbsent;
    g.metallicRoughnessTex = e.ownedOrm != 0
                                 ? e.ownedOrm
                                 : RenderCore::kMaterialTexAbsent;
    g.emissiveTex          = RenderCore::kMaterialTexAbsent;

    // Only set map flags when textures loaded successfully.
    g.flags = 0u;
    if (e.ownedNormal != 0)
        g.flags |= RenderCore::MaterialFlags::kNormalMap;
    if (e.ownedOrm != 0)
        g.flags |= RenderCore::MaterialFlags::kMetallicRoughness;

    g.baseColorFactor = 1.0f;
    g.metallicFactor  = 1.0f;
    g.roughnessFactor = 1.0f;

    s_nameToIndex["metal061b"] = static_cast<uint32_t>(s_profiles.size());
    s_profiles.push_back(std::move(e));

    mc2::fmt::print_err("[GOS_MATERIALS] registered metal061b: normalTex={:d} ormTex={:d} flags=0x{:x}\n", mc2::fmt::as_u32(g.normalTex), mc2::fmt::as_u32(g.metallicRoughnessTex), mc2::fmt::as_u32(g.flags));
}

// ---------------------------------------------------------------------------
// Register "paintedmetal003" profile.
// Source files (1K-PNG set from ambientCG PaintedMetal003 download):
//   NormalGL  -> normalTex  (GL_RGB8, linear)
//   Roughness + Metalness -> metallicRoughnessTex  (packed ORM, GL_RGB8, linear)
// ---------------------------------------------------------------------------
static void registerPaintedMetal003() {
    static const char* kNormal = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\PaintedMetal003_1K-PNG\\PaintedMetal003_1K-PNG_NormalGL.png";
    static const char* kRough  = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\PaintedMetal003_1K-PNG\\PaintedMetal003_1K-PNG_Roughness.png";
    static const char* kMetal  = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\PaintedMetal003_1K-PNG\\PaintedMetal003_1K-PNG_Metalness.png";

    ProfileEntry e;
    e.name = "paintedmetal003";

    int nw = 0, nh = 0;
    e.ownedNormal = loadLinearPng(kNormal, nw, nh);
    e.ownedOrm    = buildPackedOrm(kRough, kMetal, nw, nh);

    RenderCore::MaterialGpu& g = e.gpu;
    g.albedoTex            = RenderCore::kMaterialTexAbsent;
    g.normalTex            = e.ownedNormal != 0 ? e.ownedNormal : RenderCore::kMaterialTexAbsent;
    g.metallicRoughnessTex = e.ownedOrm    != 0 ? e.ownedOrm    : RenderCore::kMaterialTexAbsent;
    g.emissiveTex          = RenderCore::kMaterialTexAbsent;

    g.flags = 0u;
    if (e.ownedNormal != 0) g.flags |= RenderCore::MaterialFlags::kNormalMap;
    if (e.ownedOrm    != 0) g.flags |= RenderCore::MaterialFlags::kMetallicRoughness;

    g.baseColorFactor = 1.0f;
    g.metallicFactor  = 1.0f;
    g.roughnessFactor = 1.0f;

    s_nameToIndex["paintedmetal003"] = static_cast<uint32_t>(s_profiles.size());
    s_profiles.push_back(std::move(e));

    mc2::fmt::print_err("[GOS_MATERIALS] registered paintedmetal003: normalTex={:d} ormTex={:d} flags=0x{:x}\n", mc2::fmt::as_u32(g.normalTex), mc2::fmt::as_u32(g.metallicRoughnessTex), mc2::fmt::as_u32(g.flags));
}

// ---------------------------------------------------------------------------
// Register "painted_subtle" profile.
// ORM-only: same roughness/metalness sources as metal061b.
// Normal = flat (1x1 128,128,255) so no normal perturbation is applied.
// ---------------------------------------------------------------------------
static void registerPaintedSubtle() {
    static const char* kRough = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_Roughness.png";
    static const char* kMetal = "C:\\Users\\Joe\\Downloads\\GameAsset\\Materials"
        "\\Metal061B_1K-PNG\\Metal061B_1K-PNG_Metalness.png";

    ProfileEntry e;
    e.name = "painted_subtle";

    e.ownedNormal = buildFlatNormalTex();
    e.ownedOrm    = buildPackedOrm(kRough, kMetal, 0, 0);

    RenderCore::MaterialGpu& g = e.gpu;
    g.albedoTex            = RenderCore::kMaterialTexAbsent;
    g.normalTex            = e.ownedNormal != 0 ? e.ownedNormal : RenderCore::kMaterialTexAbsent;
    g.metallicRoughnessTex = e.ownedOrm    != 0 ? e.ownedOrm    : RenderCore::kMaterialTexAbsent;
    g.emissiveTex          = RenderCore::kMaterialTexAbsent;

    g.flags = 0u;
    if (e.ownedNormal != 0) g.flags |= RenderCore::MaterialFlags::kNormalMap;
    if (e.ownedOrm    != 0) g.flags |= RenderCore::MaterialFlags::kMetallicRoughness;

    g.baseColorFactor = 1.0f;
    g.metallicFactor  = 1.0f;
    g.roughnessFactor = 1.0f;

    s_nameToIndex["painted_subtle"] = static_cast<uint32_t>(s_profiles.size());
    s_profiles.push_back(std::move(e));

    mc2::fmt::print_err("[GOS_MATERIALS] registered painted_subtle: normalTex={:d} ormTex={:d} flags=0x{:x}\n", mc2::fmt::as_u32(g.normalTex), mc2::fmt::as_u32(g.metallicRoughnessTex), mc2::fmt::as_u32(g.flags));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace gos_materials {

void init() {
    if (s_initialized) return;
    s_initialized = true;

    if (!s_gpuEnabled) {
        mc2::fmt::print_err("[GOS_MATERIALS] MC2_MATERIAL_GPU=0: profile table disabled (no-op)\n");
        return;
    }

    // Always register index-0 default.
    registerDefault();

    // Gate: MC2_MECH_SURFACE_MATERIAL controls which profiles to load.
    // Unset/null -> passthrough only (no surface detail).
    // "metal061b"     -> load metal061b + paintedmetal003 (debug/exposed-metal).
    // "painted_subtle"-> load painted_subtle (ORM-only, flat normal).
    const bool loadMetal061b = mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "metal061b">();
    const bool loadPaintedSubtle = mc2::gate::is<"MC2_MECH_SURFACE_MATERIAL", "painted_subtle">();

    if (loadMetal061b) {
        registerMetal061b();
        registerPaintedMetal003();
    } else if (loadPaintedSubtle) {
        registerPaintedSubtle();
    } else if (mc2::gate::isNonEmpty<"MC2_MECH_SURFACE_MATERIAL">()) {
        mc2::fmt::print_err("[GOS_MATERIALS] WARN: unrecognized MC2_MECH_SURFACE_MATERIAL='{}': falling back to passthrough\n", mc2::fmt::safe(mc2::gate::raw<"MC2_MECH_SURFACE_MATERIAL">()));   // echoed as typed
    } else {
        mc2::fmt::print_err("[GOS_MATERIALS] MC2_MECH_SURFACE_MATERIAL={}: passthrough only\n", mc2::fmt::safe(mc2::gate::isSet<"MC2_MECH_SURFACE_MATERIAL">() ? "(empty)" : "(null)"));
    }

    uploadSsbo();

    mc2::fmt::print_err("[GOS_MATERIALS] init complete: {:d} profiles, ssbo={:d}\n", mc2::fmt::as_u32(static_cast<uint32_t>(s_profiles.size())), mc2::fmt::as_u32(static_cast<uint32_t>(s_ssbo.backendHandle)));
}

void shutdown() {
    // Delete all owned GL textures.
    for (auto& p : s_profiles) {
        if (p.ownedNormal) {
            glDeleteTextures(1, &p.ownedNormal);
            p.ownedNormal = 0;
        }
        if (p.ownedOrm) {
            glDeleteTextures(1, &p.ownedOrm);
            p.ownedOrm = 0;
        }
    }
    s_profiles.clear();
    s_nameToIndex.clear();

    if (s_ssbo.backendHandle != 0) {
        GLuint local = static_cast<GLuint>(s_ssbo.backendHandle);
        glDeleteBuffers(1, &local);
        s_ssbo.backendHandle = 0;

        // MECH-PROFILE-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id    = RenderCore::RenderResourceId::MechProfileMaterialGpuBuffer;
        invalid.valid = false;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }

    s_initialized = false;
    mc2::fmt::print_err("[GOS_MATERIALS] shutdown complete\n");
}

uint32_t getProfileIndex(const char* name) {
    if (!name) return 0u;
    auto it = s_nameToIndex.find(name);
    if (it == s_nameToIndex.end()) return 0u;
    return it->second;
}

uint32_t profileCount() {
    return static_cast<uint32_t>(s_profiles.size());
}

void bindMaterialTable() {
    if (!s_gpuEnabled) return;
    if (s_ssbo.backendHandle == 0) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kMechMaterialTableBinding, static_cast<GLuint>(s_ssbo.backendHandle));
}

uint32_t getProfileNormalTex(uint32_t index) {
    if (index >= s_profiles.size()) return 0u;
    return s_profiles[index].ownedNormal;
}

uint32_t getProfileOrmTex(uint32_t index) {
    if (index >= s_profiles.size()) return 0u;
    return s_profiles[index].ownedOrm;
}

} // namespace gos_materials
