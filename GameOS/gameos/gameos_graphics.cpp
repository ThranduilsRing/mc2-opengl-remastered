#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <string>
#include <cstdlib>
#include <cstring>
#include <climits>

#include "gameos.hpp"
#include "font3d.hpp"
#include "gos_font.h"

#ifdef LINUX_BUILD
#include <cstdarg>
#include <chrono>                // MC2_VK_WATER_PERF thin-readback timing
#endif

#include "platform_stdlib.h"
#include "platform_str.h"

#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/Image.h"
#include "utils/vec.h"
#include "utils/string_utils.h"
#include "utils/timing.h"
#include "gos_render.h"
#include "../../RenderCore/PipelineRegistry.h"   // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1
#include "pipeline_binder.h"                      // applyPipeline — shadow bracket FF state
#include "render_frame_plan.h"                    // RENDER-FRAME-PLAN-SCAFFOLD-1
#include "mdi_submit.h"                            // MDI-SUBMISSION-SCAFFOLD-1
#include "gos_postprocess.h"
#include "gos_profiler.h"
#include "gos_gpu_sync.h"
#include "gos_validate.h"  // drainGLErrors (Tier-1 instr §4)
#include "gl_state_guard.h"  // GlScopedTextureUnit (GLSTATE-TEXTURE-UNIT0-RESTORE-1)
#include "gos_smoke.h"     // S9E: SmokeMode fixed deterministic render-shader clock
#include "../../mclib/cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline
#include "../../mclib/camera_frustum_math.h"  // CAMERA-FRUSTUM-HARNESS-1 pure rect/frustum math
#include "../../mclib/render_contract.h"      // [RENDER_PASS v1] noteRenderPass
#include "RenderCore/top_level_pass_executor.h" // APPLY-STATE-TERRAINDECAL-1: findTopLevelStateDesc
#include "RenderCore/top_level_apply_axes.h"   // FRAMEGRAPH-APPLY-STATE-EXTEND-1: applyTopLevelGenericAxes
#include "../../RenderCore/frame_executor.h"     // PER-PASS-APPLY-COUNTERS-1: ApplyPassId
#include "Stuff/Stuff.hpp"                     // Stuff::Matrix4D for gos_SetWorldToClipGL (full chain required; matrix.hpp alone creates circular include ordering)

// [B1 C16] (diagnostic) gosFX heap + child accumulation counters; env-gated on
// MC2_GPU_PARTICLES=1. Forward-decl to avoid Stuff/gosfx header chain here.
namespace gosFX { void DiagFrameTick(); }
#include "debug_state_dump.h"    // mc2_debug_state::getSessionId, writeShutdownState
#include "diagnostic_trace.h"   // mc2_diag::init, shutdown
#include "build_fingerprint.h"  // MC2_BUILD_GIT_SHA/BRANCH/DIRTY/TIME_ISO
#include <process.h>             // _getpid()

// Build config label for BUILD diagnostic event
#if defined(_DEBUG)
#  define MC2_DIAG_BUILD_CONFIG "Debug"
#elif defined(NDEBUG)
#  define MC2_DIAG_BUILD_CONFIG "Release"
#else
#  define MC2_DIAG_BUILD_CONFIG "RelWithDebInfo"
#endif
#include "gos_terrain_bridge.h"
#include "gos_terrain_lod_chunk.h"
#include "gos_terrain_patch_stream.h"
#include "gos_terrain_indirect.h"
#include "gos_terrain_surface.h"                 // [TERRAIN_SURFACE] PR-2 producer
#include "gos_terrain_height_tex.h"              // TERRAIN-NORMALS-FROM-HEIGHT-1
#include "../../mclib/terrain_surface_trace.h"   // [TERRAIN_SURFACE v1] channel
#include "../../mclib/terrain_surface_bands.h"   // [TERRAIN_SURFACE] PR-3 band config (single source)
#include "gos_terrain_water_stream.h"
#include "gpu_driven_common.h"
#include "mc2_hitch_trace.h"  // GPU-UPDATE-BUFFER-COUNTER-1: MC2_GL_BufferData_Owner (include LAST, after all GL headers)
#include "utils/gpu_ring_buffer.h"  // GPU-BUFFER-WRAPPER-TIER0-HUD-1: gated HUD/gosMesh persistent-coherent ring
#ifdef MC2_VULKAN
#include "engine_present_vk.h"
#endif
#include "vk_shared_gbuffer_gate.h"  // VK-FIELD-ENV-CONTENT-STACK-ARM-1: umbrella HUD arming

class gosRenderer;
class gosFont;

// C1 tactical material profile (defined in mclib/terrain.cpp; declared in
// mclib/terrain.h). Disposable; removed when real material-palette
// architecture lands. Declared here rather than via `#include "terrain.h"`
// to avoid pulling in mclib headers gameos_graphics.cpp doesn't already use.
extern int g_terrainMaterialProfile;

// Fix B: forward-declared TU-wide so the symmetric-mirror at the GPU-water
// binds in renderWaterFastPath (earlier in this file than the historical
// probe-accessor block) can see them. Linkage matches definitions:
// gos_GetTerrainMVPMat4 is C++ linkage (defined later in this file). The raw
// dispatch-MVP snapshot getter + the view-epoch-checked object accessor
// (gos_GetObjectDrawMVP) come from gos_object_draw_mvp.h (RENDER-VIEW-CURRENCY-1).
// This TU is a MIXED consumer: terrain-phase reads (water/overlay/decal) use the
// raw snapshot legitimately and are tagged TERRAIN-PHASE-RAW-MVP-OK; object-phase
// reads (object shadows) MUST use gos_GetObjectDrawMVP. Enforced by
// scripts/check-object-mvp-currency.py.
#include "gos_object_draw_mvp.h"
#include "../../RenderCore/terrain_path_telemetry.h"  // TERRAIN-PATH-TELEMETRY-1
#include "../../RenderCore/RenderResourceRegistry.h"  // LIGHTDATA-SSBO-OWNER-1: registry + ids
#include "../../RenderCore/GpuBufferOwner.h"           // LIGHTDATA-SSBO-OWNER-1: owner record
extern const float*     gos_GetTerrainMVPMat4();

static const DWORD INVALID_TEXTURE_ID = 0;

static gosRenderer* g_gos_renderer = NULL;

static bool debugEnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

static bool vkScopedGlCanaryEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}

static GLuint vkScopedGlCanaryBegin(const char* name) {
    if (!vkScopedGlCanaryEnabled(name))
        return 0;
    GLuint query = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_ANY_SAMPLES_PASSED, query);
    return query;
}

static void vkScopedGlCanaryEnd(GLuint query, const char* tag, int draws) {
    if (!query)
        return;
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    GLuint anySamples = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &anySamples);
    std::fprintf(stderr, "[VK_SCENE_GL_DRAW_CANARY v1] tag=%s any_samples=%u draws=%d\n",
                 tag, (unsigned)anySamples, draws);
    glDeleteQueries(1, &query);
}

static bool isAllConcreteTerrainBatch(const gos_VERTEX* vertices, int count) {
    if (!vertices || count <= 0)
        return false;

    for (int i = 0; i < count; ++i) {
        if ((vertices[i].frgb & 0x000000ffu) != 3u)
            return false;
    }

    return true;
}

// TERRAIN-TEX-UNIT-MAP-1: canonical texture unit assignments for the terrain pass.
// Never write raw integers for terrain units — use these names.
// History: matNormal4 (snow) was incorrectly placed at unit 9, which is also the
// static shadow map. Shadow binding ran after the matNormal loop and silently
// overwrote unit 9, making snow normals always sample the shadow depth texture.
// Fix: snow moved to unit 12. All terrain bind sites updated to use this table.
static constexpr GLint kTerrainTexUnitStaticShadow  = 9;
static constexpr GLint kTerrainTexUnitDynamicShadow = 10;
static constexpr GLint kTerrainTexUnitHeight        = 11;
// Per-cascade shadow resolution: separate full-map (last) cascade depth texture.
// Free unit (terrain uses 5-12); under the 16-unit min-spec floor.
static constexpr GLint kTerrainTexUnitDynFullMap    = 13;
// matNormal0-3: units 5-8 (rock/grass/dirt/concrete); matNormal4 (snow): unit 12
static constexpr GLint kTerrainMatNormalUnits[5]    = { 5, 6, 7, 8, 12 };
// When MC2_TERRAIN_NORMAL_ARRAY=1 (future plan), the array texture occupies unit 5.
static constexpr GLint kTerrainTexUnitNormalArray   = 5;
// ROAD-PBR-ASPHALT-1: overlay (terrain_overlay.frag) asphalt material units.
// The overlay program uses unit 0 (tex1) + 9/10/11/13 (shadows/height); units
// 4 and 5 are free on that program. Asphalt albedo -> unit 4; the shared
// terrain normal array (matNormalArray) -> unit 5 (kTerrainTexUnitNormalArray).
static constexpr GLint kOverlayTexUnitAsphaltAlbedo = 4;
static constexpr GLint kOverlayTexUnitMatNormalArr  = kTerrainTexUnitNormalArray;
// ROAD-MATERIAL-GRAVEL-1: high-res tiling gravel albedo for DIRT_ROAD overlays
// (terrain_overlay.frag v_matId==2). Overlay program uses units 0 (tex1), 4
// (asphalt), 5 (normal array), 9/10/11/13 (shadows/height/CSM); unit 6 is free.
static constexpr GLint kOverlayTexUnitGravelAlbedo  = 6;
static_assert(kTerrainMatNormalUnits[4] != kTerrainTexUnitStaticShadow,
              "matNormal4 collides with static shadow map unit");
static_assert(kTerrainMatNormalUnits[4] != kTerrainTexUnitDynamicShadow,
              "matNormal4 collides with dynamic shadow map unit");
static_assert(kTerrainMatNormalUnits[4] != kTerrainTexUnitHeight,
              "matNormal4 collides with height texture unit");
static_assert(kTerrainMatNormalUnits[4] < 16,
              "matNormal4 exceeds OpenGL min-spec 16-unit floor");

// TERRAIN-NORMAL-ARRAY: save/restore all GL pixel transfer, PBO, and texture
// binding state that glGetTexImage / glTexSubImage3D are sensitive to.
// Construct once, destructor restores. Use in any function that calls
// glGetTexImage or bulk texture upload to avoid clobbering surrounding state.
struct GlPixelStoreGuard {
    GLint packBuffer = 0, unpackBuffer = 0;
    GLint packAlign = 0, unpackAlign = 0;
    GLint packRowLen = 0, unpackRowLen = 0;
    GLint packSkipRows = 0, packSkipPixels = 0;
    GLint unpackSkipRows = 0, unpackSkipPixels = 0;
    GLint activeTex = 0;
    GLint binding2D = 0, binding2DArray = 0;

    GlPixelStoreGuard() {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,   &packBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT,    &packAlign);
        glGetIntegerv(GL_UNPACK_ALIGNMENT,  &unpackAlign);
        glGetIntegerv(GL_PACK_ROW_LENGTH,   &packRowLen);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLen);
        glGetIntegerv(GL_PACK_SKIP_ROWS,    &packSkipRows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS,  &packSkipPixels);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS,  &unpackSkipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&unpackSkipPixels);
        glGetIntegerv(GL_ACTIVE_TEXTURE,    &activeTex);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,       &binding2D);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &binding2DArray);

        glBindBuffer(GL_PIXEL_PACK_BUFFER,   0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT,    1);
        glPixelStorei(GL_PACK_ROW_LENGTH,   0);
        glPixelStorei(GL_PACK_SKIP_ROWS,    0);
        glPixelStorei(GL_PACK_SKIP_PIXELS,  0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,  1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,  0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    }

    ~GlPixelStoreGuard() {
        glBindBuffer(GL_PIXEL_PACK_BUFFER,   (GLuint)packBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer);
        glPixelStorei(GL_PACK_ALIGNMENT,    packAlign);
        glPixelStorei(GL_PACK_ROW_LENGTH,   packRowLen);
        glPixelStorei(GL_PACK_SKIP_ROWS,    packSkipRows);
        glPixelStorei(GL_PACK_SKIP_PIXELS,  packSkipPixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT,  unpackAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLen);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,  unpackSkipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,unpackSkipPixels);
        glBindTexture(GL_TEXTURE_2D,       (GLuint)binding2D);
        glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)binding2DArray);
        glActiveTexture((GLenum)activeTex);
    }
};

// Single-source gate for the sampler2DArray terrain normal path.
// Used by shader prefix injection AND all bind paths — must agree for the
// lifetime of compiled programs. Evaluated once at startup via static.
static bool terrainNormalArrayEnabled() {
    // DEFAULT ON (was opt-in). The sampler2DArray path is the only one whose
    // build runs at the clean terrain/shadow bind sites; the terrain LOD chunk
    // renderer needs the array populated to apply material detail normals.
    // Visual is identical to the legacy individual-sampler path by design
    // (validated 5/5 ON). Explicit MC2_TERRAIN_NORMAL_ARRAY=0 forces the old
    // individual-sampler path.
    static const bool enabled = []() {
        const char* v = std::getenv("MC2_TERRAIN_NORMAL_ARRAY");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return enabled;
}

// TERRAIN-MATERIAL-LIB-1: default OFF. Drives u_useMaterialLib (chunk-path
// roughness/AO branch). Evaluated once (static) -- matches terrainNormalArrayEnabled().
static bool terrainMaterialLibEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("MC2_TERRAIN_MATERIAL_LIB");
        return v && v[0] && v[0] != '0';
    }();
    return enabled;
}

gosRenderer* getGosRenderer() {
    return g_gos_renderer;
}

// Public helper for consumers outside this TU (e.g. gos_static_prop_batcher)
// that need to turn a gosTextureHandle into a raw GL texture name. Declared
// here so local code can reference it; defined at end of this file after
// the gosRenderer / gosTexture class bodies are in scope.
uint32_t gos_GetGLTextureId(uint32_t gosHandle);

struct gosTextureInfo {
    int width_;
    int height_;
    gos_TextureFormat format_;
};

////////////////////////////////////////////////////////////////////////////////
class gosBuffer {
	friend class gosRenderer;
public:
	GLuint buffer_;
	int element_size_;
	uint32_t count_;
	gosBUFFER_TYPE type_;
	gosBUFFER_USAGE usage_;
};

GLenum getGLVertexAttribType(gosVERTEX_ATTRIB_TYPE type) {
	GLenum t = -1;
	switch (type)
	{
	case gosVERTEX_ATTRIB_TYPE::BYTE: return GL_BYTE;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_BYTE: return GL_UNSIGNED_BYTE;
	case gosVERTEX_ATTRIB_TYPE::SHORT: return GL_SHORT;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_SHORT: return GL_UNSIGNED_SHORT;
	case gosVERTEX_ATTRIB_TYPE::INT: return GL_INT;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_INT: return GL_UNSIGNED_INT;
	case gosVERTEX_ATTRIB_TYPE::FLOAT: return GL_FLOAT;
	default:
		gosASSERT(0 && "unknows vertex attrib type");
	}

	return t;
};

////////////////////////////////////////////////////////////////////////////////
class gosVertexDeclaration {
	friend class gosRenderer;

	gosVERTEX_FORMAT_RECORD* vf_;
	uint32_t count_;

	gosVertexDeclaration() :vf_(0), count_(0) {}
public:

	static gosVertexDeclaration* create(gosVERTEX_FORMAT_RECORD* vf, int count)
	{
		gosVertexDeclaration* vdecl = new gosVertexDeclaration();
		if (!vdecl)
			return nullptr;

		vdecl->vf_ = new gosVERTEX_FORMAT_RECORD[count];
		memcpy(vdecl->vf_, vf, count * sizeof(gosVERTEX_FORMAT_RECORD));
		vdecl->count_ = count;

		return vdecl;
	}

	static void destroy(gosVertexDeclaration* vdecl)
	{
		delete[] vdecl->vf_;
		vdecl->count_ = -1;
		vdecl->vf_ = nullptr;
		delete vdecl;
	}

	void apply() {

		for (uint32_t i = 0; i < count_; ++i) {

			gosVERTEX_FORMAT_RECORD* rec = vf_ + i;

			GLuint type = getGLVertexAttribType(rec->type);

			glEnableVertexAttribArray(rec->index);
			glVertexAttribPointer(rec->index, rec->num_components, type, rec->normalized ? GL_TRUE : GL_FALSE, rec->stride, BUFFER_OFFSET(rec->offset));
		}
	}

	void end() {

		for (uint32_t i = 0; i < count_; ++i) {
			gosVERTEX_FORMAT_RECORD* rec = vf_ + i;
			glDisableVertexAttribArray(rec->index);
		}
	}

};

class gosMaterialVariationHelper;
class gosMaterialVariation {
        friend class gosMaterialVariationHelper;
        char* defines_;
        char* unique_name_suffix_;

    public:
        gosMaterialVariation():defines_(nullptr), unique_name_suffix_(nullptr) {}
        const char* getDefinesString() const { return defines_; }
        const char* getUniqueSuffix() const { return unique_name_suffix_; }

        ~gosMaterialVariation()
        {
            delete[] defines_;
            delete[] unique_name_suffix_;
        }
};

class gosMaterialVariationHelper {
        std::vector<std::string> defines;
    public:

        void addDefine(const char* define)
        {
            defines.push_back(std::string(define));
        }

        void addDefines(const char** define)
        {
            gosASSERT(define);
            while(*define)
            {
                defines.push_back(std::string(*define));
                define++;
            }
        }

        void addDefines(const std::vector<std::string>& define)
        {
            defines.insert(defines.end(), define.begin(), define.end());
        }

        void getMaterialVariation(gosMaterialVariation& variation)
        {
            std::string defines_str = "#version 430\n";
            std::string unique_suffix_str = "#";
            for(auto d : defines)
            {
                defines_str.append("#define ");
                defines_str.append(d);
                defines_str.append(" = 1\n");

                unique_suffix_str.append(d);
                unique_suffix_str.append("#");
            }
            // Add MRT_ENABLED when normal buffer is attached (AMD requires
            // GBuffer1 to only be declared when the FBO actually has attachment 1)
            {
                gosPostProcess* pp = getGosPostProcess();
                if (pp && pp->getSceneNormalTexture()) {
                    defines_str.append("#define MRT_ENABLED 1\n");
                }
            }
            if (terrainNormalArrayEnabled())
                defines_str.append("#define TERRAIN_NORMAL_ARRAY\n");
            // Item 1: Cascaded Shadow Maps. When the gate is ON, shadow.hglsl
            // compiles its sampler2DArrayShadow + mat4[N] variant of
            // calcDynamicShadow (signature frozen; no call-site changes).
            if (mc2ShadowCsmEnabled()) {
                char csmDef[64];
                snprintf(csmDef, sizeof(csmDef),
                         "#define MC2_SHADOW_CSM 1\n#define MC2_SHADOW_CSM_MAX %d\n",
                         mc2ShadowCsmCount());
                defines_str.append(csmDef);
            }
            defines_str.append("\n");

            if(variation.defines_)
                delete[] variation.defines_;

            if(variation.unique_name_suffix_)
                delete[] variation.unique_name_suffix_;

            size_t size = defines_str.size() + 1;
            variation.defines_ = new char[size];
            memcpy(variation.defines_, defines_str.c_str(), size);
            variation.defines_[size-1]='\0';

            size = unique_suffix_str.size() + 1;
            variation.unique_name_suffix_ = new char[size];
            memcpy(variation.unique_name_suffix_, unique_suffix_str.c_str(), size);
            variation.unique_name_suffix_[size-1]='\0';
        }
};

enum class gosGLOBAL_SHADER_FLAGS : unsigned int
{
    ALPHA_TEST = 0,
    IS_OVERLAY = 1
};

#define SHADER_FLAG_INDEX_TO_MASK(x) (1 << ((uint32_t)x))
#define SHADER_FLAG_MASK_TO_INDEX(x) (ffs(x)-1) // use __popcnt etc for windows, and move to platform_*.h

static const char* const g_shader_flags[] = {
    "ALPHA_TEST",
    "IS_OVERLAY"
};


class gosRenderMaterial {

		static const std::string s_mvp;
		static const std::string s_fog_color;
    public:
        static gosRenderMaterial* load(const char* shader, const gosMaterialVariation& mvar) {
            gosASSERT(shader);
            gosRenderMaterial* pmat = new gosRenderMaterial();
            char vs[256];
            char ps[256];
            StringFormat(vs, 255, "shaders/%s.vert", shader);
            StringFormat(ps, 255, "shaders/%s.frag", shader);

            std::string sh_name = shader;
            if (mvar.getUniqueSuffix())
                sh_name.append(mvar.getUniqueSuffix());

            // Terrain gets TCS/TES for tessellation
            if (strcmp(shader, "gos_terrain") == 0 || strcmp(shader, "shadow_terrain") == 0) {
                char tcs[256], tes[256];
                StringFormat(tcs, 255, "shaders/%s.tesc", shader);
                StringFormat(tes, 255, "shaders/%s.tese", shader);
                printf("[TESS] Loading terrain shader with TCS=%s TES=%s\n", tcs, tes);
                pmat->program_ = glsl_program::makeProgram2(sh_name.c_str(),
                    vs, tcs, tes, nullptr, ps, 0, nullptr, mvar.getDefinesString());
            } else {
                pmat->program_ = glsl_program::makeProgram(sh_name.c_str(), vs, ps, mvar.getDefinesString());
            }

            if(!pmat->program_) {
                SPEW(("SHADERS", "Failed to create %s material\n", shader));
                printf("[TESS] FAILED to create shader: %s\n", shader);
                delete pmat;
                return NULL;
            }

            pmat->name_ = new char[strlen(shader) + 1];
            strcpy(pmat->name_, shader);

            pmat->onLoad();

            return pmat;
        }


        void onLoad() {
            gosASSERT(program_);

            pos_loc = program_->getAttribLocation("pos");
            color_loc = program_->getAttribLocation("color");
            spec_color_and_fog_loc = program_->getAttribLocation("fog");
            texcoord_loc = program_->getAttribLocation("texcoord");
        }

        static void destroy(gosRenderMaterial* pmat) {
            gosASSERT(pmat);
            if(pmat->program_) {
                glsl_program::deleteProgram(pmat->name_);
                pmat->program_ = 0;
            }

            delete[] pmat->name_;
            pmat->name_ = 0;
        }

        void checkReload()
        {
            if(program_) {
                if(program_->needsReload()) {
                    if(program_->reload())
                        onLoad();
                }
            }
        }

        void applyVertexDeclaration() {
            
            const int stride = sizeof(gos_VERTEX);
            
            // gos_VERTEX structure
	        //float x,y;
	        //float z;
	        //float rhw;
	        //DWORD argb;
	        //DWORD frgb;
	        //float u,v;	

            gosASSERT(pos_loc >= 0);
            glEnableVertexAttribArray(pos_loc);
            glVertexAttribPointer(pos_loc, 4, GL_FLOAT, GL_FALSE, stride, (void*)0);

            if(color_loc != -1) {
                glEnableVertexAttribArray(color_loc);
                glVertexAttribPointer(color_loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, 
                        BUFFER_OFFSET(4*sizeof(float)));
            }

            if(spec_color_and_fog_loc != -1) {
                glEnableVertexAttribArray(spec_color_and_fog_loc);
                glVertexAttribPointer(spec_color_and_fog_loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                        BUFFER_OFFSET(4*sizeof(float) + sizeof(uint32_t)));
            }

            if(texcoord_loc != -1) {
                glEnableVertexAttribArray(texcoord_loc);
                glVertexAttribPointer(texcoord_loc, 2, GL_FLOAT, GL_FALSE, stride, 
                        BUFFER_OFFSET(4*sizeof(float) + 2*sizeof(uint32_t)));
            }
        }

        bool setSamplerUnit(const std::string& sampler_name, uint32_t unit) {
            gosASSERT(!sampler_name.empty());
            // TODO: may also check that current program is equal to our program
            if(program_->samplers_.count(sampler_name)) {
                glUniform1i(program_->samplers_[sampler_name]->index_, unit);
                return true;
            }
            return false;
        }

        bool setUniformBlock(const std::string& uniform_block_name, uint32_t unit) {
            gosASSERT(!uniform_block_name.empty());
            if(program_->uniform_blocks_.count(uniform_block_name)) {
				//glBindBufferBase(GL_UNIFORM_BUFFER, , unit);
				glUniformBlockBinding(program_->shp_, program_->uniform_blocks_[uniform_block_name]->index_, unit);
                return true;
            }
            return false;
        }

        bool setTransform(const mat4& m) {
            program_->setMat4(s_mvp, m);
            return true;
        }

		bool setFogColor(const vec4& fog_color) {
            program_->setFloat4(s_fog_color, fog_color);
            return true;
		}

        void apply() {
            gosASSERT(program_);
            program_->apply();
        }

        const char* getName() const { return name_; }

        // TODO: think how to not expose this
        glsl_program* getShader() { return program_; }

		void endVertexDeclaration() {

			glDisableVertexAttribArray(pos_loc);

			if (color_loc != -1) {
				glDisableVertexAttribArray(color_loc);
			}

			if (spec_color_and_fog_loc != -1) {
				glDisableVertexAttribArray(spec_color_and_fog_loc);
			}

			if (texcoord_loc != -1) {
				glDisableVertexAttribArray(texcoord_loc);
			}
		}

		void end() {
            glUseProgram(0);
        }

    private:
        gosRenderMaterial():
            program_(NULL)
            , name_(NULL)
            , pos_loc(-1)
            , color_loc(-1)
            , spec_color_and_fog_loc(-1)
            , texcoord_loc(-1)
        {
        }

        glsl_program* program_;
        char* name_;
        GLint pos_loc;
        GLint color_loc;
        GLint spec_color_and_fog_loc;
        GLint texcoord_loc;
};

const std::string gosRenderMaterial::s_mvp = std::string("mvp");
const std::string gosRenderMaterial::s_fog_color = std::string("fog_color");

// ---------------------------------------------------------------------------
// GPU-BUFFER-WRAPPER-TIER0-HUD-1 — gate state (default OFF).
//   MC2_GPUBUF_RING            : route gosMesh::draw/drawIndexed through the
//                                persistent-coherent fenced ring instead of the
//                                per-batch glBufferData orphan. OFF => legacy
//                                path, byte-identical GL stream + output.
//   MC2_GPUBUF_RING_FORCE_WRAP : negative test — deterministically advance the
//                                ring twice without an intervening endFrame so
//                                the one-endFrame-per-beginFrame invariant trips
//                                (proves the fence-per-frame guard is live).
//   MC2_GPUBUF_RESIDENCY       : list the live HUD ring buffers once per frame.
// Cached once on first query (single predicted-false branch when unset).
static bool gosHudRingEnabled() {
    static const bool s_on = (std::getenv("MC2_GPUBUF_RING") != nullptr);
    return s_on;
}
static bool gosHudRingForceWrap() {
    static const bool s_on = (std::getenv("MC2_GPUBUF_RING_FORCE_WRAP") != nullptr);
    return s_on;
}
static bool gosHudRingResidency() {
    static const bool s_on = (std::getenv("MC2_GPUBUF_RESIDENCY") != nullptr);
    return s_on;
}

// LIGHT-GROW-ONCE-SUBDATA-1 — gate state (default OFF). The AMD-safe
// intermediate for the light SSBO orphan churn (NOT the full GpuStorageRing).
//   MC2_GPUBUF_LIGHT_GROWONCE : keep the single persistent light SSBO (same
//                               handle, slot 20 binding, shader/layout contract),
//                               but stop the per-frame FULL glBufferData orphan
//                               re-spec. Per frame upload ONLY the live used
//                               bytes via glBufferSubData; grow the GL buffer
//                               ONLY when used bytes exceed current capacity
//                               (rare, amortized with +128-record headroom).
// OFF (default) => the existing LIGHTSSBO-ORPHAN-1 path runs completely
// UNCHANGED (byte-identical GL stream + output) — this is the kill switch.
// Cached once on first query (single predicted-false branch when unset).
static bool gosLightGrowOnceEnabled() {
    static const bool s_on = (std::getenv("MC2_GPUBUF_LIGHT_GROWONCE") != nullptr);
    return s_on;
}

class gosMesh {
    public:
        typedef WORD INDEX_TYPE;

        static gosMesh* makeMesh(gosPRIMITIVETYPE prim_type, int vertex_capacity, int index_capacity = 0) {
            GLuint vb = makeBuffer(GL_ARRAY_BUFFER, 0, sizeof(gos_VERTEX)*vertex_capacity, GL_DYNAMIC_DRAW);
            if(!vb)
                return NULL;

            GLuint ib = 0;
            if(index_capacity > 0) {
                ib = makeBuffer(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(INDEX_TYPE)*index_capacity, GL_DYNAMIC_DRAW);
                if(!ib)
                    return NULL;
            }

            gosMesh* mesh = new gosMesh(prim_type, vertex_capacity, index_capacity);
            mesh->vb_ = vb;
            mesh->ib_ = ib;
            mesh->pvertex_data_ = new gos_VERTEX[vertex_capacity];
            mesh->pindex_data_ = new INDEX_TYPE[index_capacity];
            return mesh;
        }

        static void destroy(gosMesh* pmesh) {

            gosASSERT(pmesh);

            // GPU-BUFFER-WRAPPER-TIER0-HUD-1 (WATCHPOINT 4): drain + unmap +
            // delete the ring buffers before the mesh goes away. ~GpuMeshRing
            // does this; explicit delete keeps the lifetime obvious and ensures
            // no GL buffer leaks across renderer teardown / device reset.
            delete pmesh->ring_;
            pmesh->ring_ = nullptr;

            delete[] pmesh->pvertex_data_;
            delete[] pmesh->pindex_data_;

            GLuint b[] = {pmesh->vb_, pmesh->ib_};
            glDeleteBuffers(sizeof(b)/sizeof(b[0]), b);
        }

        // Lazily create (gate ON) and return the persistent-coherent ring for
        // this mesh, or nullptr if creation/mapping failed (caller falls back to
        // the legacy orphan path). Sized to this mesh's FIXED capacity x N slots.
        mc2gpu::GpuMeshRing* ensureRing() {
            if (ring_) return ring_->valid() ? ring_ : nullptr;
            ring_ = new mc2gpu::GpuMeshRing();
            char tag[48];
            std::snprintf(tag, sizeof(tag), "gosMesh_p%d_v%d",
                          (int)prim_type_, vertex_capacity_);
            if (!ring_->create(tag,
                               (uint32_t)sizeof(gos_VERTEX), (uint32_t)vertex_capacity_,
                               (uint32_t)sizeof(INDEX_TYPE), (uint32_t)index_capacity_)) {
                delete ring_;
                ring_ = nullptr;
                return nullptr;
            }
            return ring_;
        }

        mc2gpu::GpuMeshRing* ringIfLive() const { return ring_; }

        bool addVertices(gos_VERTEX* vertices, int count) {
            if(num_vertices_ + count <= vertex_capacity_) {
                memcpy(pvertex_data_ + num_vertices_, vertices, sizeof(gos_VERTEX)*count);
                num_vertices_ += count;
                return true;
            }
            return false;
        }

        bool addIndices(INDEX_TYPE* indices, int count) {
            if(num_indices_ + count <= index_capacity_) {
                memcpy(pindex_data_ + num_indices_, indices, sizeof(INDEX_TYPE)*count);
                num_indices_ += count;
                return true;
            }
            return false;
        }

        int getVertexCapacity() const { return vertex_capacity_; }
        int getIndexCapacity() const { return index_capacity_; }
        int getNumVertices() const { return num_vertices_; }
        int getNumIndices() const { return num_indices_; }
        const gos_VERTEX* getVertices() const { return pvertex_data_; }
        const WORD* getIndices() const { return pindex_data_; }

        int getIndexSizeBytes() const { return sizeof(INDEX_TYPE); }

        GLuint getVB() const { return vb_; }
        GLuint getIB() const { return ib_; }

        void uploadBuffers() {
            if (num_vertices_ > 0) {
                updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_,
                    num_vertices_ * sizeof(gos_VERTEX), GL_DYNAMIC_DRAW);
            }
            if (num_indices_ > 0) {
                updateBuffer(ib_, GL_ELEMENT_ARRAY_BUFFER, pindex_data_,
                    num_indices_ * sizeof(INDEX_TYPE), GL_DYNAMIC_DRAW);
            }
        }

        void rewind() { num_vertices_ = 0; num_indices_ = 0; }

        void draw(gosRenderMaterial* material) const;
        void drawIndexed(gosRenderMaterial* material) const;

		static void drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, gosRenderMaterial* material);
		static void drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl);

		static const std::string s_tex1;

    private:

        gosMesh(gosPRIMITIVETYPE prim_type, int vertex_capacity, int index_capacity)
            : vertex_capacity_(vertex_capacity)
            , index_capacity_(index_capacity)
            , num_vertices_(0)
            , num_indices_(0)
            , pvertex_data_(NULL)    
            , pindex_data_(NULL)    
            , prim_type_(prim_type)
            , vb_(-1)
            ,ib_(-1)
            , ring_(nullptr)
         {
         }

        int vertex_capacity_;
        int index_capacity_;
        int num_vertices_;
        int num_indices_;
        gos_VERTEX* pvertex_data_;
        INDEX_TYPE* pindex_data_;
        gosPRIMITIVETYPE prim_type_;

        GLuint vb_;
        GLuint ib_;

        // GPU-BUFFER-WRAPPER-TIER0-HUD-1: lazily-created persistent-coherent ring
        // (gate ON only). Owned by this mesh; destroyed in destroy(). nullptr =>
        // legacy orphan path.
        mc2gpu::GpuMeshRing* ring_;
};

const std::string gosMesh::s_tex1 = std::string("tex1");

void gosMesh::draw(gosRenderMaterial* material) const
{
    gosASSERT(material);

    if(num_vertices_ == 0)
        return;

    // GPU-BUFFER-WRAPPER-TIER0-HUD-1: ring path (gate ON) vs legacy orphan (OFF).
    // The two branches differ ONLY in (a) how the current frame's vertices reach
    // the GPU (memcpy into a coherent map vs glBufferData orphan) and (b) the
    // first-vertex of the draw (slot base vs 0). Same data, same count, same
    // primitive, same bind-to-0 epilogue => byte-identical draw-visible result.
    mc2gpu::GpuMeshRing* ring =
        gosHudRingEnabled() ? const_cast<gosMesh*>(this)->ensureRing() : nullptr;

    GLint firstVertex = 0;
    GLuint drawVb = vb_;
    if (ring) {
        ring->beginFrame();
        if (gosHudRingForceWrap()) {
            // NEGATIVE TEST: advance again with no intervening endFrame so the
            // one-endFrame-per-beginFrame invariant trips (proves the guard).
            ring->beginFrame();
        }
        firstVertex = ring->writeVertices(pvertex_data_, (uint32_t)num_vertices_);
        drawVb = ring->vb();
    } else {
        updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_, num_vertices_*sizeof(gos_VERTEX), GL_DYNAMIC_DRAW_ARB);
    }

    material->apply();

    material->setSamplerUnit(s_tex1, 0);

	glBindBuffer(GL_ARRAY_BUFFER, drawVb);
    CHECK_GL_ERROR;

    material->applyVertexDeclaration();
    CHECK_GL_ERROR;

    GLenum pt = GL_TRIANGLES;
    switch(prim_type_) {
        case PRIMITIVE_POINTLIST:
            pt = GL_POINTS;
            break;
        case PRIMITIVE_LINELIST:
            pt = GL_LINES;
            break;
        case PRIMITIVE_TRIANGLELIST:
            pt = GL_TRIANGLES;
            break;
        default:
            gosASSERT(0 && "Wrong primitive type");
    }

    glDrawArrays(pt, firstVertex, num_vertices_);

    material->endVertexDeclaration();
    material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (ring)
        ring->endFrame();   // fence the slot AFTER the draw consumes it.
}

void gosMesh::drawIndexed(gosRenderMaterial* material) const
{
    gosASSERT(material);

    if(num_vertices_ == 0)
        return;

    // GPU-BUFFER-WRAPPER-TIER0-HUD-1: ring path (gate ON) vs legacy orphan (OFF).
    // Ring path uses glDrawElementsBaseVertex so the same index data (offset 0
    // relative to the slot) addresses the same vertices (shifted by baseVertex).
    // The legacy branch is the EXACT original two-orphan + glDrawElements path.
    mc2gpu::GpuMeshRing* ring =
        gosHudRingEnabled() ? const_cast<gosMesh*>(this)->ensureRing() : nullptr;

    GLint  baseVertex   = 0;
    GLvoid* indexOffset = NULL;
    GLuint drawVb = vb_;
    GLuint drawIb = ib_;
    if (ring) {
        ring->beginFrame();
        if (gosHudRingForceWrap()) {
            // NEGATIVE TEST: see gosMesh::draw — trips the fence-per-frame guard.
            ring->beginFrame();
        }
        baseVertex  = ring->writeVertices(pvertex_data_, (uint32_t)num_vertices_);
        indexOffset = (GLvoid*)(uintptr_t)ring->writeIndices(pindex_data_, (uint32_t)num_indices_);
        drawVb = ring->vb();
        drawIb = ring->ib();
    } else {
        updateBuffer(vb_, GL_ARRAY_BUFFER, pvertex_data_, num_vertices_*sizeof(gos_VERTEX), GL_DYNAMIC_DRAW);
        updateBuffer(ib_, GL_ELEMENT_ARRAY_BUFFER, pindex_data_, num_indices_*sizeof(INDEX_TYPE), GL_DYNAMIC_DRAW);
    }

    material->apply();

    material->setSamplerUnit(s_tex1, 0);

	glBindBuffer(GL_ARRAY_BUFFER, drawVb);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, drawIb);
    CHECK_GL_ERROR;

    material->applyVertexDeclaration();
    CHECK_GL_ERROR;

    GLenum pt = GL_TRIANGLES;
    switch(prim_type_) {
        case PRIMITIVE_POINTLIST:
            pt = GL_POINTS;
            break;
        case PRIMITIVE_LINELIST:
            pt = GL_LINES;
            break;
        case PRIMITIVE_TRIANGLELIST:
            pt = GL_TRIANGLES;
            break;
        default:
            gosASSERT(0 && "Wrong primitive type");
    }

    const GLenum idxType = getIndexSizeBytes()==2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    if (ring)
        glDrawElementsBaseVertex(pt, num_indices_, idxType, indexOffset, baseVertex);
    else
        glDrawElements(pt, num_indices_, idxType, indexOffset);

    material->endVertexDeclaration();
    material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    if (ring)
        ring->endFrame();   // fence the slot AFTER the draw consumes it.
}

void gosMesh::drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, gosRenderMaterial* material)
{
	gosASSERT(material);

	int index_size = ib->element_size_;
	gosASSERT(index_size == 2 || index_size == 4);

	if (ib->count_ == 0)
		return;

	material->apply();
	CHECK_GL_ERROR;

	material->setSamplerUnit(s_tex1, 0);
	CHECK_GL_ERROR;

	glBindBuffer(GL_ARRAY_BUFFER, vb->buffer_);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->buffer_);
	CHECK_GL_ERROR;

	vdecl->apply();
	CHECK_GL_ERROR;

	GLenum pt = GL_TRIANGLES;
	glDrawElements(pt, ib->count_, index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

	vdecl->end();
	material->end();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

}

void gosMesh::drawIndexed(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
	int index_size = ib->element_size_;
	gosASSERT(index_size == 2 || index_size == 4);

	if (ib->count_ == 0)
		return;

	CHECK_GL_ERROR;

	glBindBuffer(GL_ARRAY_BUFFER, vb->buffer_);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->buffer_);
	CHECK_GL_ERROR;

	vdecl->apply();
	CHECK_GL_ERROR;

	GLenum pt = GL_TRIANGLES;
	glDrawElements(pt, ib->count_, index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

	vdecl->end();

	//material->end();
	glUseProgram(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

}



class gosTexture {
    public:
        gosTexture(gos_TextureFormat fmt, const char* fname, DWORD hints, BYTE* pdata, DWORD size, bool from_memory)
        {

	        //if(fmt == gos_Texture_Detect || /*fmt == gos_Texture_Keyed ||*/ fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(fname) {
                filename_ = new char[strlen(fname)+1];
                strcpy(filename_, fname);
            } else {
                filename_ = 0;
            }
            texname_ = NULL;

            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            if(size) {
                size_ = size;
                pcompdata_ = new BYTE[size];
                memcpy(pcompdata_, pdata, size);
            }

            is_locked_ = false;
            is_from_memory_ = from_memory;
            content_serial_ = 0;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        gosTexture(gos_TextureFormat fmt, DWORD hints, DWORD w, DWORD h, const char* texname)
        {
	        //if(fmt == gos_Texture_Detect /*|| fmt == gos_Texture_Keyed*/ || fmt == gos_Texture_Bump || fmt == gos_Texture_Normal)
            //     PAUSE((""));

            format_ = fmt;
            if(texname) {
                texname_ = new char[strlen(texname)+1];
                strcpy(texname_, texname);
            } else {
                texname_ = 0;
            }
            filename_ = NULL;
            hints_ = hints;

            plocked_area_ = NULL;

            size_ = 0;
            pcompdata_ = NULL;
            tex_.w = w;
            tex_.h = h;

            is_locked_ = false;
            is_from_memory_ = true;
            content_serial_ = 0;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        // TEXMGR-COMPRESSED-UPLOAD-1: wrap a pre-built (already GL-uploaded)
        // Texture so the handle integrates with textureList_/bind/destroy
        // exactly like the other gosTexture flavors. No createHardwareTexture()
        // call — the GL object is supplied ready. Used by
        // gos_NewCompressedTexture2D for BC7 .ktx2 sidecar uploads.
        gosTexture(const Texture& prebuilt, gos_TextureFormat fmt, const char* name)
        {
            format_ = fmt;
            if(name) {
                texname_ = new char[strlen(name)+1];
                strcpy(texname_, name);
            } else {
                texname_ = 0;
            }
            filename_ = NULL;
            hints_ = 0;
            plocked_area_ = NULL;
            size_ = 0;
            pcompdata_ = NULL;
            tex_ = prebuilt;
            is_locked_ = false;
            is_from_memory_ = true;
            content_serial_ = 1;
            mirror_w_ = 0;
            mirror_h_ = 0;
        }

        bool createHardwareTexture();

        ~gosTexture() {

            //SPEW(("Destroying texture: %s\n", filename_));

            gosASSERT(is_locked_ == false);

            if(pcompdata_)
                delete[] pcompdata_;
            if(filename_)
                delete[] filename_;
            if(texname_)
                delete[] texname_;

            destroyTexture(&tex_);
        }

        uint32_t getTextureId() const { return tex_.id; }
        TexType getTextureType() const { return tex_.type_; }
        const Texture& getTexture() const { return tex_; }
        uint32_t getContentSerial() const { return content_serial_; }
        void markContentMutated() { ++content_serial_; }
        const std::vector<uint8_t>& getRgbaMirror() const { return rgba_mirror_; }
        bool hasRgbaMirror() const { return !rgba_mirror_.empty() && mirror_w_ == tex_.w && mirror_h_ == tex_.h; }

        void setRgbaMirror(int w, int h, TexFormat format, const void* pixels) {
            if(w <= 0 || h <= 0 || pixels == nullptr) {
                rgba_mirror_.clear();
                mirror_w_ = 0;
                mirror_h_ = 0;
                return;
            }
            const size_t pixelCount = (size_t)w * (size_t)h;
            rgba_mirror_.resize(pixelCount * 4u);
            mirror_w_ = w;
            mirror_h_ = h;
            if(format == TF_RGBA8) {
                std::memcpy(rgba_mirror_.data(), pixels, rgba_mirror_.size());
            } else if(format == TF_RGB8) {
                const uint8_t* src = (const uint8_t*)pixels;
                for(size_t i = 0; i < pixelCount; ++i) {
                    rgba_mirror_[i * 4u + 0u] = src[i * 3u + 0u];
                    rgba_mirror_[i * 4u + 1u] = src[i * 3u + 1u];
                    rgba_mirror_[i * 4u + 2u] = src[i * 3u + 2u];
                    rgba_mirror_[i * 4u + 3u] = 0xffu;
                }
            } else {
                rgba_mirror_.clear();
                mirror_w_ = 0;
                mirror_h_ = 0;
            }
        }

        BYTE* Lock(int mipl_level, bool is_read_only, int* pitch) {
            gosASSERT(is_locked_ == false);
            is_locked_ = true;
            // TODO:
            gosASSERT(pitch);
            *pitch = tex_.w;

            gosASSERT(!plocked_area_);
#if 0 
            glBindTexture(GL_TEXTURE_2D, tex_.id);
            GLint pack_row_length;
            GLint pack_alignment;
            glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
            glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
            glBindTexture(GL_TEXTURE_2D, 0);
#endif
            // always return rgba8 formatted data
            lock_type_read_only_ = is_read_only;
            const uint32_t ts = tex_.w*tex_.h * getTexFormatPixelSize(TF_RGBA8);
            plocked_area_ = new BYTE[ts];
            // Zero before readback: getTextureData early-returns WITHOUT writing
            // for block-compressed (BC7/TF_NONE) textures, which would otherwise
            // leave this buffer full of heap garbage that the paint-scheme
            // classifier then re-uploads. (A paint texture can land on BC7 when
            // its paintInstance hashes low.)
            memset(plocked_area_, 0, ts);
            // glGetTexImage readback is sensitive to inherited GL_PACK_* state and
            // a left-bound GL_PIXEL_PACK_BUFFER; guard save/resets/restores it so
            // the mech-paint recolour reads the real texels on NVIDIA.
            GlPixelStoreGuard pixelStoreGuard;
            getTextureData(tex_, 0, plocked_area_, TF_RGBA8);
            for(int y=0;y<tex_.h;++y) {
                for(int x=0;x<tex_.w;++x) {
                    DWORD rgba = ((DWORD*)plocked_area_)[tex_.w*y + x];
                    DWORD r = rgba&0xff;
                    DWORD g = (rgba&0xff00)>>8;
                    DWORD b = (rgba&0xff0000)>>16;
                    DWORD a = (rgba&0xff000000)>>24;
                    DWORD bgra = (a<<24) | (r<<16) | (g<<8) | b;
                    ((DWORD*)plocked_area_)[tex_.w*y + x] = bgra;
                }
            }
            return plocked_area_;
        }

        void Unlock() {
            gosASSERT(is_locked_ == true);
        
            if(!lock_type_read_only_) {
                for(int y=0;y<tex_.h;++y) {
                    for(int x=0;x<tex_.w;++x) {
                        DWORD bgra = ((DWORD*)plocked_area_)[tex_.w*y + x];
                        DWORD b = bgra&0xff;
                        DWORD g = (bgra&0xff00)>>8;
                        DWORD r = (bgra&0xff0000)>>16;
                        DWORD a = (bgra&0xff000000)>>24;
                        DWORD argb = (a<<24) | (b<<16) | (g<<8) | r;
                        ((DWORD*)plocked_area_)[tex_.w*y + x] = argb;
                    }
                }
                // Same hazard as Lock's readback, upload side: glTexSubImage2D
                // reads from a left-bound GL_PIXEL_UNPACK_BUFFER (see applyPBO)
                // instead of client memory, and honours inherited GL_UNPACK_*.
                // Guard neutralises both so the recoloured texels actually land.
                GlPixelStoreGuard pixelStoreGuard;
                updateTexture(tex_, plocked_area_, TF_RGBA8);
                setRgbaMirror(tex_.w, tex_.h, TF_RGBA8, plocked_area_);
                markContentMutated();
            }

            delete[] plocked_area_;
            plocked_area_ = NULL;

            is_locked_ = false;
        }

        void getTextureInfo(gosTextureInfo* texinfo) const {
            gosASSERT(texinfo);
            texinfo->width_ = tex_.w;
            texinfo->height_ = tex_.h;
            texinfo->format_ = format_;
        }

    private:
        BYTE* pcompdata_;
        BYTE* plocked_area_;
        DWORD size_;
        Texture tex_;

        gos_TextureFormat format_;
        char* filename_;
        char* texname_;
        DWORD hints_;

        bool is_locked_;
        bool lock_type_read_only_;
        bool is_from_memory_; // not loaded from file
        uint32_t content_serial_;
        std::vector<uint8_t> rgba_mirror_;
        int mirror_w_;
        int mirror_h_;
};

struct gosTextAttribs {
    HGOSFONT3D FontHandle;
    DWORD Foreground;
    float Size;
    bool WordWrap;
    bool Proportional;
    bool Bold;
    bool Italic;
    DWORD WrapType;
    bool DisableEmbeddedCodes;
};

static void makeKindaSolid(Image& img) {
    // have to do this, otherwise texutre with zero alpha could be drawn with alpha blend enabled, evel though logically aplha blend should not be enabled!
    // (happens when drawing terrain, see TerrainQuad::draw() case when no detail and no owerlay bu t isCement is true)
    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            pixels[y*img.getWidth() + x] = pix | 0xff000000;
        }
    }
}

static bool doesLookLikeAlpha(const Image& img) {
    gosASSERT(img.getFormat() == FORMAT_RGBA8);

    DWORD* pixels = (DWORD*)img.getPixels();
    for(int y=0;y<img.getHeight(); ++y) {
        for(int x=0;x<img.getWidth(); ++x) {
            DWORD pix = pixels[y*img.getWidth() + x];
            if((0xFF000000 & pix) != 0xFF000000)
                return true;
        }
    }
    return false;
}

static gos_TextureFormat convertIfNecessary(Image& img, gos_TextureFormat gos_format) {

    const bool has_alpha_channel = FORMAT_RGBA8 == img.getFormat();

    if(gos_format == gos_Texture_Detect) {
        bool has_alpha = has_alpha_channel ? doesLookLikeAlpha(img) : false;
        gos_format = has_alpha ? gos_Texture_Alpha : gos_Texture_Solid;
    }

    if(gos_format == gos_Texture_Solid && has_alpha_channel)
        makeKindaSolid(img);

    return gos_format;
}

bool gosTexture::createHardwareTexture() {

    // Opt-in to mipmaps via gosHint_MipmapFilter0. MC2's original convention
    // was "absence of DisableMipmap means mipmaps on," but in this port many
    // HUD/GUI/tacmap loads pass hints=0 without DisableMipmap and must stay
    // non-mipmapped for pixel-perfect sampling. We use MipmapFilter0 as a
    // positive opt-in instead -- no existing code sets this bit, so only
    // explicitly-updated load sites enable mipmaps. DisableMipmap wins if
    // both are set (defensive).
    const bool wantMipmaps = (hints_ & gosHint_MipmapFilter0) != 0
                          && (hints_ & gosHint_DisableMipmap) == 0;

    if(!is_from_memory_) {

        gosASSERT(filename_);

        Image img;
        if(!img.loadFromFile(filename_)) {
            SPEW(("DBG", "failed to load texture from file: %s\n", filename_));
            return false;
        }

        // check for only those formats, because lock.unlock may incorrectly work with different channes size (e.g. 16 or 32bit or floats)
        FORMAT img_fmt = img.getFormat();
        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels(), wantMipmaps);
        if(tex_.isValid()) {
            setRgbaMirror(img.getWidth(), img.getHeight(), tf, img.getPixels());
            markContentMutated();
        }
        return tex_.isValid();

    } else if(pcompdata_ && size_ > 0) {

        // TODO: this is texture from memory, so maybe do not load it from file eh?

        Image img;
        if(!img.loadTGA(pcompdata_, size_)) {
            SPEW(("DBG", "failed to load texture from data, filename: %s, texname: %s\n", filename_? filename_ : "NO FILENAME", texname_?texname_:"NO TEXNAME"));
            return false;
        }

        FORMAT img_fmt = img.getFormat();

        if(img_fmt != FORMAT_RGB8 && img_fmt != FORMAT_RGBA8) {
            STOP(("Unsupported texture format when loading %s\n", filename_));
        }

        TexFormat tf = img_fmt == FORMAT_RGB8 ? TF_RGB8 : TF_RGBA8;

        format_ = convertIfNecessary(img, format_);

        tex_ = create2DTexture(img.getWidth(), img.getHeight(), tf, img.getPixels(), wantMipmaps);
        if(tex_.isValid()) {
            setRgbaMirror(img.getWidth(), img.getHeight(), tf, img.getPixels());
            markContentMutated();
        }
        return tex_.isValid();
    } else {
        gosASSERT(tex_.w >0 && tex_.h > 0);

        TexFormat tf = TF_RGBA8; // TODO: check format_ and do appropriate stuff
        DWORD* pdata = new DWORD[tex_.w*tex_.h];
        for(int i=0;i<tex_.w*tex_.h;++i)
            pdata[i] = 0xFF00FFFF;

        // OVERLAY-MAGENTA-TEXTURE-RECON-1 (Source A): this texture object has w/h but
        // NO source path and NO compressed data -> filled solid magenta. Emit WHICH
        // texture resolved to nothing (the highest-value magenta probe). Gated
        // MC2_OVERLAY_MAGENTA_TRACE + MC2_DIAG_TAGS=OVERLAY_MAGENTA; read via
        // get_diagnostic_events("OVERLAY_MAGENTA"). No behavior change.
        {
            static const bool s_magentaTrace = (std::getenv("MC2_OVERLAY_MAGENTA_TRACE") != nullptr);
            if (s_magentaTrace && mc2_diag::tagEnabled("OVERLAY_MAGENTA")) {
                char _mg_fn[256]; char _mg_tn[256];
                const char* _mg_sfn = filename_ ? filename_ : "";
                const char* _mg_stn = texname_  ? texname_  : "";
                size_t _mg_k;
                for (_mg_k=0; _mg_k<sizeof(_mg_fn)-1 && _mg_sfn[_mg_k]; ++_mg_k)
                    _mg_fn[_mg_k] = (_mg_sfn[_mg_k]=='\\') ? '/' : _mg_sfn[_mg_k];
                _mg_fn[_mg_k] = '\0';
                for (_mg_k=0; _mg_k<sizeof(_mg_tn)-1 && _mg_stn[_mg_k]; ++_mg_k)
                    _mg_tn[_mg_k] = (_mg_stn[_mg_k]=='\\') ? '/' : _mg_stn[_mg_k];
                _mg_tn[_mg_k] = '\0';
                char _mg_buf[600];
                snprintf(_mg_buf, sizeof(_mg_buf),
                         "{\"site\":\"fallback_fill\",\"filename\":\"%s\",\"texname\":\"%s\",\"w\":%d,\"h\":%d}",
                         _mg_fn, _mg_tn, (int)tex_.w, (int)tex_.h);
                mc2_diag::writeEvent("OVERLAY_MAGENTA", 1, 0, _mg_buf);
            }
        }
        tex_ = create2DTexture(tex_.w, tex_.h, tf, (const uint8_t*)pdata, wantMipmaps);
        if(tex_.isValid()) {
            setRgbaMirror(tex_.w, tex_.h, tf, (const uint8_t*)pdata);
            markContentMutated();
        }
        delete[] pdata;
        return tex_.isValid();
    }

}

static gosTexture* lookupBatchTextureOrWarn(const std::vector<gosTexture*>& textureList,
                                            DWORD textureId,
                                            const char* batchName) {
    if (textureId == INVALID_TEXTURE_ID)
        return nullptr;

    if (textureId >= textureList.size() || textureList[textureId] == nullptr) {
        static unsigned int warnCount = 0;
        if (warnCount < 16) {
            printf("%s: dropping invalid texture handle %u (texture count=%zu)\n",
                   batchName, textureId, textureList.size());
            ++warnCount;
        }
        return nullptr;
    }

    return textureList[textureId];
}

// OMT-1-OVERLAY-MISSING-TEXTURE-GUARD ─────────────────────────────────────────
// The overlay/decal batch draws previously did `glBindTexture(GL_TEXTURE_2D,
// t ? id : 0)` — binding GL name 0 on a resolve-fail makes the sampler read an
// INCOMPLETE texture, whose result is driver-defined (black on some GPUs,
// garbage/magenta on others = a vendor-dependent mystery; cf. OMT recon). Route
// resolve-fails to an explicit 1x1 magenta fallback + a deterministic
// once-per-handle log + counters, so a missing overlay texture is visible AND
// explained instead of undefined. NO bake/composite/cement/shader change — this
// only hardens the per-draw texture BIND. Verbose summary: MC2_OVERLAY_TEXTURE_TRACE=1.
namespace {
struct OverlayTexStats {
    uint64_t pushes = 0, resolved = 0, resolve_failed = 0,
             bound_zero_prevented = 0, fallback_bound = 0;
};
OverlayTexStats g_overlayTexStats;
GLuint g_overlayFallbackTex = 0;

GLuint getOverlayFallbackTexture() {
    if (g_overlayFallbackTex == 0) {
        const unsigned char magenta[4] = { 255, 0, 255, 255 };  // 1x1 RGBA debug
        // TEX-CLASS: asset-pool -- overlay magenta/fallback content texture
        glGenTextures(1, &g_overlayFallbackTex);
        glBindTexture(GL_TEXTURE_2D, g_overlayFallbackTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, magenta);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return g_overlayFallbackTex;
}

bool overlayTexTraceOn() {
    static int c = -1;
    if (c < 0) c = (getenv("MC2_OVERLAY_TEXTURE_TRACE") != nullptr) ? 1 : 0;
    return c != 0;
}

// Once-per-handle dedup without pulling in <set> (handles are few + bounded to 64).
bool overlayHandleAlreadyLogged(DWORD h) {
    static DWORD seen[64];
    static int   seenCount = 0;
    for (int i = 0; i < seenCount; ++i) if (seen[i] == h) return true;
    if (seenCount < 64) { seen[seenCount++] = h; return false; }  // new → log + record
    return true;  // table full → suppress (bounds log volume)
}

// Bind the resolved batch texture, or an explicit 1x1 magenta fallback on
// resolve-fail. NEVER binds GL name 0 on the resolve-fail path. Logs once per
// failing handle (deterministic, with reason) and counts every category.
void bindBatchTextureOrFallback(gosTexture* t, DWORD texHandle, const char* batchName) {
    ++g_overlayTexStats.pushes;
    const GLuint id = t ? (GLuint)t->getTextureId() : 0u;
    if (t && id != 0u) {
        ++g_overlayTexStats.resolved;
        glBindTexture(GL_TEXTURE_2D, id);
        return;
    }
    ++g_overlayTexStats.resolve_failed;
    ++g_overlayTexStats.bound_zero_prevented;   // the bug this guard prevents
    ++g_overlayTexStats.fallback_bound;
    if (!overlayHandleAlreadyLogged(texHandle)) {
        fprintf(stderr,
            "[OVERLAY_TEXTURE v1] event=resolve_fail batch=%s texHandle=%u "
            "fallback_bound=1 texture=<1x1 magenta debug> reason=%s\n",
            batchName, (unsigned)texHandle,
            (t && id == 0u) ? "resolved_but_gl_id_0" : "null_or_invalid_handle");
        fflush(stderr);
    }
    glBindTexture(GL_TEXTURE_2D, getOverlayFallbackTexture());
}

// Cumulative counter summary; rate-limited, gated by MC2_OVERLAY_TEXTURE_TRACE.
// Uses an internal tick (this file has no g_mc2FrameCounter in scope); the tick
// advances once per drawTerrainOverlays call ≈ once per overlay-active frame.
void overlayTexTraceSummary() {
    if (!overlayTexTraceOn()) return;
    static uint64_t s_tick = 0;
    ++s_tick;
    if ((s_tick % 300u) != 1u) return;
    fprintf(stderr,
        "[OVERLAY_TEXTURE v1] event=summary tick=%llu pushes=%llu resolved=%llu "
        "resolve_failed=%llu bound_zero_prevented=%llu fallback_bound=%llu\n",
        (unsigned long long)s_tick,
        (unsigned long long)g_overlayTexStats.pushes,
        (unsigned long long)g_overlayTexStats.resolved,
        (unsigned long long)g_overlayTexStats.resolve_failed,
        (unsigned long long)g_overlayTexStats.bound_zero_prevented,
        (unsigned long long)g_overlayTexStats.fallback_bound);
    fflush(stderr);
}
} // anonymous namespace (OMT-1)

////////////////////////////////////////////////////////////////////////////////
class gosFont {
        friend class gosRenderer;
    public:
        static gosFont* load(const char* fontFile);

        int getMaxCharWidth() const { return gi_.max_advance_; }
        int getMaxCharHeight() const { return gi_.font_line_skip_; }
        int getFontAscent() const { return gi_.font_ascent_; }

        int getCharWidth(int c) const;
        void getCharUV(int c, uint32_t* u, uint32_t* v) const;
        int getCharAdvance(int c) const;
        const gosGlyphMetrics& getGlyphMetrics(int c) const;
        const gosGlyphInfo& getGlyphInfo() const { return gi_; }


        DWORD getTextureId() const { return tex_id_; }
        const char* getName() const { return font_name_; }
        const char* getId() const { return font_id_; }

        uint32_t getRefCount() { return ref_count_; }
        uint32_t addRef() { return ++ref_count_; }
        uint32_t decRef() { gosASSERT(ref_count_>0); return --ref_count_; }

    private:
        static uint32_t destroy(gosFont* font);
        gosFont():font_name_(0), font_id_(0), tex_id_(0), ref_count_(1) {};
        ~gosFont();

        char* font_name_;
        char* font_id_;
        gosGlyphInfo gi_;
        DWORD tex_id_;
        uint32_t ref_count_;
};


enum HudDrawKind { kHudQuadBatch, kHudLineBatch, kHudTriBatch, kHudTextQuadBatch };

struct HudDrawCall {
    HudDrawKind              kind;
    std::vector<gos_VERTEX>  vertices;
    uint32_t                 stateSnapshot[gos_MaxState];
    mat4                     projection;
    DWORD                    fontTexId;       // kHudTextQuadBatch only
    DWORD                    foregroundColor; // kHudTextQuadBatch only
    bool                     scaleExempt = false; // skip s_hud_scale shrink (cursor, modal dialogs)
};

////////////////////////////////////////////////////////////////////////////////
class gosRenderer {

    friend class gosShapeRenderer;

    typedef uint32_t RenderState[gos_MaxState];
	static const std::string s_Foreground;

    public:
        gosRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {
            width_ = w;
            height_ = h;
            ctx_h_ = ctx_h;
            win_h_ = win_h;
            hudFlushed_ = false;
        }

        uint32_t addTexture(gosTexture* texture) {
            gosASSERT(texture);
            textureList_.push_back(texture);
            return (uint32_t)(textureList_.size()-1);
        }

        uint32_t addFont(gosFont* font) {
            gosASSERT(font);
            fontList_.push_back(font);
            return (uint32_t)(fontList_.size()-1);
        }

		uint32_t addBuffer(gosBuffer* buffer) {
			gosASSERT(buffer);
			bufferList_.push_back(buffer);
			return (uint32_t)(bufferList_.size() - 1);
		}

		uint32_t addVertexDeclaration(gosVertexDeclaration* vdecl) {
			gosASSERT(vdecl);
			vertexDeclarationList_.push_back(vdecl);
			return (uint32_t)(vertexDeclarationList_.size() - 1);
		}

        // TODO: do same as with texture?
        void deleteFont(gosFont* font) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            
            struct equals_to {
                gosFont* fnt_;
                bool operator()(gosFont* fnt) {
                    return fnt == fnt_;
                }
            };

            equals_to eq;
            eq.fnt_ = font;

            std::vector<gosFont*>::iterator it = 
                std::find_if(fontList_.begin(), fontList_.end(), eq);
            if(it != fontList_.end())
            {
                gosFont* font = *it;
                if(0 == gosFont::destroy(font))
                    fontList_.erase(it);
            }
        }

		bool deleteBuffer(gosBuffer* buffer) {
			std::vector<gosBuffer*>::iterator it = std::find(bufferList_.begin(), bufferList_.end(), buffer);
			if (it != bufferList_.end())
			{
				bufferList_.erase(it);
				return true;
			}
			return false;
		}

		bool deleteVertexDeclaration(gosVertexDeclaration* vdecl) {
			std::vector<gosVertexDeclaration*>::iterator it = std::find(vertexDeclarationList_.begin(), vertexDeclarationList_.end(), vdecl);
			if (it != vertexDeclarationList_.end())
			{
				vertexDeclarationList_.erase(it);
				return true;
			}
			return false;
		}

        gosFont* findFont(const char* font_id) {
            
            struct equals_to {
                const char* font_id_;
                bool operator()(const gosFont* fnt) {
                    return strcmp(fnt->getId(), font_id_)==0;
                }
            };

            equals_to eq;
            eq.font_id_ = font_id;

            std::vector<gosFont*>::iterator it = 
                std::find_if(fontList_.begin(), fontList_.end(), eq);
            if(it != fontList_.end())
                return *it;
            return NULL;
        }

        gosTexture* getTexture(DWORD texture_id) {
            // TODO: return default texture
            if(texture_id == INVALID_TEXTURE_ID) {
                gosASSERT(0 && "Should not be requested");
                return NULL;
            }
            gosASSERT(textureList_.size() > texture_id);
            gosASSERT(textureList_[texture_id] != 0);
            return textureList_[texture_id];
        }

        // Bound-check helper for gos_GetGLTextureId. Avoids the hard
        // assert in getTexture() for bogus handle values seen on
        // TG_TinyTexture entries that were never loaded (e.g. 0xFFFFFFFF).
        size_t getTextureListSize() const { return textureList_.size(); }

        void deleteTexture(DWORD texture_id) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            gosASSERT(textureList_.size() > texture_id);
            delete textureList_[texture_id];
            textureList_[texture_id] = 0;
        }

        uint32_t getFlagsFromStates()
        {
            return curStates_[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;
        }

        gosRenderMaterial* getRenderMaterial(const char* name) {

            uint32_t flags = getFlagsFromStates();
            return materialDB_[name][flags];
        }

        gosTextAttribs& getTextAttributes() { return curTextAttribs_; }
        void setTextPos(int x, int y) { curTextPosX_ = x; curTextPosY_ = y; }
        void getTextPos(int& x, int& y) { x = curTextPosX_; y = curTextPosY_; }
        void setTextRegion(int Left, int Top, int Right, int Bottom) {
            curTextLeft_ = Left;
            curTextTop_ = Top;
            curTextRight_ = Right;
            curTextBottom_ = Bottom;
        }

        int getTextRegionWidth() { return curTextRight_ - curTextLeft_; }
        int getTextRegionHeight() { return curTextBottom_ - curTextTop_; }

        void setupViewport(bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil = 0, DWORD StencilValue = 0) {

            clearDepth_ = FillZ;
            clearDepthValue_ = ZBuffer;
            clearColor_ = FillBG;
            clearColorValue_ = BGColor;
            clearStencil_ = ClearStencil;
            clearStencilValue_ = StencilValue;
            viewportTop_ = top;
            viewportLeft_ = left;
            viewportBottom_ = bottom;
            viewportRight_ = right;
        }

        void getViewportTransform(float* viewMulX, float* viewMulY, float* viewAddX, float* viewAddY) {
            gosASSERT(viewMulX && viewMulY && viewAddX && viewAddY);
            *viewMulX = (viewportRight_ - viewportLeft_)*width_;
            *viewMulY = (viewportBottom_ - viewportTop_)*height_;
            *viewAddX = viewportLeft_ * width_;
            *viewAddY = viewportTop_ * height_;
        }

		void setRenderViewport(const vec4& vp) { render_viewport_ = vp; }
		vec4 getRenderViewport() { return render_viewport_; }

		const mat4& getProj2Screen() { return projection_; }
		int getWidth()  const { return width_; }
		int getHeight() const { return height_; }
        const vec4& getFogColor() const { return fog_color_; }

        void setRenderState(gos_RenderState RenderState, int Value) {
            renderStates_[RenderState] = Value;
        }

        int getRenderState(gos_RenderState RenderState) const {
            return renderStates_[RenderState];
        }

        void setScreenMode(DWORD width, DWORD height, DWORD bit_depth, bool GotoFullScreen, bool anti_alias) {
            reqWidth = width;
            reqHeight = height;
            reqBitDepth = bit_depth;
            reqAntiAlias = anti_alias;
            reqGotoFullscreen = GotoFullScreen;
            pendingRequest = true;
        }

        void pushRenderStates();
        void popRenderStates();

        void applyRenderStates();

        void drawQuads(gos_VERTEX* vertices, int count);
        void drawLines(gos_VERTEX* vertices, int count);
        void flushHUDBatch();
        void replayTextQuads(const HudDrawCall& call);
        void drawPoints(gos_VERTEX* vertices, int count);
        void drawTris(gos_VERTEX* vertices, int count);
        void drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices);
		void drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp);
		void drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl);
        void drawText(const char* text);
        void terrainBindUniformsForPatchStream(gosRenderMaterial* material);
        // Returns ssboRecordBase uniform location in the thin program, or -1.
        // If overrideProg is non-null, binds uniforms to that program instead
        // (B4 Stage 1b: mask-SOLID shader shares all uniform names with thin
        // shader so this is safe).
        int terrainBindThinUniformsForPatchStream(glsl_program* overrideProg = nullptr);
        // Fix A (2026-05-14): re-upload an externally-provided terrainMVP over
        // the one just bound by terrainBindThinUniformsForPatchStream.  Used by
        // the indirect bridge to align the VS projection MVP with the MVP that
        // compute used when writing the thin records being drawn this frame.
        // Uses the already-cached thinTerrainLocs_.terrainMVP — no extra
        // glGetUniformLocation.  Must be called AFTER terrainBindThinUniformsForPatchStream
        // (which caches the location) and while the thin program is bound.
        void terrainOverrideThinMVP(const float* mvp4x4);
        // Returns the glsl_program for the thin terrain shader. Used by bridge exports.
        glsl_program* getThinTerrainProgram()      const { return thin_terrain_prog_;        }
        // [TERRAIN_SURFACE] PR-2: indexed continuous-surface VS + gos_terrain.frag.
        glsl_program* getTerrainSurfaceProgram()   const { return terrain_surface_prog_;     }
        glsl_program* getWaterFastProgram()        const { return water_fast_prog_;           }
        glsl_program* getMineStaticProgram()       const { return mine_static_prog_;          }  // PR2c Stage 2c
        // TERRAIN-SPINE-0: read-only inspector accessor for the terrain overlay program.
        glsl_program* getTerrainOverlayProgram()   const { return overlayProg_;                }
        glsl_program* getMaskSolidProgram()        const { return mask_solid_prog_;           }  // B4 Stage 1b — mask-SOLID draw
        glsl_program* getMaskWaterProgram()        const { return mask_water_prog_;           }  // B4 Stage 1c — mask-water draw
        // GPU-driven dynamic sun shadow -- Phase 1 getters.
        glsl_program* getShadowMechProg()          const { return shadow_mech_prog_;          }
        glsl_program* getShadowStaticPropProg()    const { return shadow_static_prop_prog_;   }

        // Water fast path (Stage 2 of renderWater architectural slice).
        // Issues 1-2 instanced draws against the WaterRecipe + WaterFrame SSBOs.
        // Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.
        void renderWaterFastPath(
            unsigned int recordCount,
            unsigned int waterGosHandle,
            unsigned int waterDetailGosHandle,
            float waterElevation,
            float alphaDepth,
            unsigned int alphaEdgeByte,
            unsigned int alphaMiddleByte,
            unsigned int alphaDeepByte,
            float mapTopLeftX,
            float mapTopLeftY,
            float frameCos,
            float frameCosAlpha,
            float oneOverTF,
            float oneOverWaterTF,
            float cloudOffsetX,
            float cloudOffsetY,
            float sprayOffsetX,
            float sprayOffsetY,
            float maxMinUV);

        void beginFrame();
        void endFrame();

        void init();
        void destroy();
        void flush();

        // debug interface
        void setNumDrawCallsToDraw(uint32_t num) { num_draw_calls_to_draw_ = num; }
        uint32_t getNumDrawCallsToDraw() { return num_draw_calls_to_draw_; }
        void setBreakOnDrawCall(bool b_break) { break_on_draw_call_ = b_break; }
        bool getBreakOnDrawCall() { return break_on_draw_call_; }
        void setBreakDrawCall(uint32_t num) { break_draw_call_num_ = num; }

        graphics::RenderContextHandle getRenderContextHandle() { return ctx_h_; }

        uint32_t getRenderState(gos_RenderState render_state) { return curStates_[render_state]; }

        gosRenderMaterial* selectBasicRenderMaterial(const RenderState& rs) const ;
        gosRenderMaterial* selectLightedRenderMaterial(const RenderState& rs) const ;

		void handleEvents();

        // Terrain tessellation
        void setTerrainTessParams(float level, float near_dist, float far_dist) {
            terrain_tess_level_ = level;
            terrain_tess_dist_near_ = near_dist;
            terrain_tess_dist_far_ = far_dist;
        }
        void setTerrainPhongAlpha(float a) { terrain_phong_alpha_ = a; }
        void setTerrainDisplaceScale(float s) { terrain_displace_scale_ = s; }
        void setTerrainWireframe(bool w) { terrain_wireframe_ = w; }
        void setTerrainDebugMode(float mode) {
            terrain_debug_mode_ = mode;
        }
        void setTerrainMVP(const float* m) {
            memcpy(&terrain_mvp_, m, 16 * sizeof(float));
            terrain_mvp_valid_ = true;
            // Compute inverse VP for post-process depth reconstruction
            mat4 invVP = inverseMat4(terrain_mvp_);
            gosPostProcess* pp = getGosPostProcess();
            if (pp) {
                pp->setInverseViewProj((const float*)&invVP);
                pp->setViewProj(m);
            }
        }
        void setTerrainCameraPos(float x, float y, float z) {
            terrain_camera_pos_ = vec4(x, y, z, 1.0f);
        }
        // setTerrainViewport: stores (vmx,vmy,vax,vay) for EditorCamera.h shim.
        // gamecam.cpp does not call this (it reads viewport from gos_GetViewport
        // inline); this exists so the editor can set the params the same way.
        void setTerrainViewport(float vmx, float vmy, float vax, float vay) {
            terrain_viewport_ = vec4(vmx, vmy, vax, vay);
        }
        void terrainExtraReset() { terrain_extra_count_ = 0; terrain_extra_draw_offset_ = 0; terrain_batch_extras_ = nullptr; terrain_batch_extras_count_ = 0; }
        void terrainExtraAdd(const gos_TERRAIN_EXTRA* data, int count) {
            if (terrain_extra_count_ + count <= terrain_extra_capacity_) {
                memcpy(terrain_extra_data_ + terrain_extra_count_, data, sizeof(gos_TERRAIN_EXTRA) * count);
                terrain_extra_count_ += count;
            }
        }
        void setTerrainBatchExtras(const gos_TERRAIN_EXTRA* extras, int count) {
            terrain_batch_extras_ = extras;
            terrain_batch_extras_count_ = count;
        }
        int getTerrainExtraCount() const { return terrain_extra_count_; }
        float getTerrainTessLevel() const { return terrain_tess_level_; }
        float getTerrainTessDistNear() const { return terrain_tess_dist_near_; }
        float getTerrainTessDistFar() const { return terrain_tess_dist_far_; }
        float getTerrainPhongAlpha() const { return terrain_phong_alpha_; }
        float getTerrainDisplaceScale() const { return terrain_displace_scale_; }
        float getTerrainDetailTiling() const { return terrain_detail_tiling_; }
        float getTerrainDetailStrength() const { return terrain_detail_strength_; }
        float getTerrainPOMScale() const { return terrain_pom_scale_; }
        bool getTerrainWireframe() const { return terrain_wireframe_; }
        gos_TERRAIN_EXTRA* getTerrainExtraData() const { return terrain_extra_data_; }
        bool isTerrainMVPValid() const { return terrain_mvp_valid_; }
        const mat4& getTerrainMVP() const { return terrain_mvp_; }
        // TERRAIN-SHORELINE-MASK-1: shared render-shader clock accessor (same
        // epoch + SmokeMode fixed-timestep override as the water fast-path's
        // "time" uniform, gameos_graphics.cpp:~3343). Exposed so gos_terrain_lod_chunk.cpp
        // can upload an identical f(worldPos,time)-only clock to the chunk frag
        // (camera-INDEPENDENT by construction — no view matrix involved).
        float getShaderClockSeconds() const {
            return SmokeMode::fixedTimestepEnabled()
                       ? (float)SmokeMode::fixedClockSeconds()
                       : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0);
        }
        gosRenderMaterial* getTerrainMaterial() const { return terrain_material_; }
        const vec4& getTerrainCameraPos() const { return terrain_camera_pos_; }
        // Shadow mode
        void setShadowMode(bool enabled) { shadow_mode_ = enabled; }
        bool getShadowMode() const { return shadow_mode_; }
        gosRenderMaterial* getShadowTerrainMaterial() const { return shadow_terrain_material_; }

        // Shadow softness (penumbra radius in texels)
        void setTerrainShadowSoftness(float s) { terrain_shadow_softness_ = s; }
        float getTerrainShadowSoftness() const { return terrain_shadow_softness_; }

        // Per-material normal boost + global tint strength
        void setTerrainMatNormalBoost(float r, float g, float d, float c) {
            terrain_mat_normal_boost_[0] = r;
            terrain_mat_normal_boost_[1] = g;
            terrain_mat_normal_boost_[2] = d;
            terrain_mat_normal_boost_[3] = c;
        }
        void getTerrainMatNormalBoost(float* r, float* g, float* d, float* c) const {
            *r = terrain_mat_normal_boost_[0];
            *g = terrain_mat_normal_boost_[1];
            *d = terrain_mat_normal_boost_[2];
            *c = terrain_mat_normal_boost_[3];
        }
        void setTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow) {
            terrain_mat_tiling_[0] = rock;
            terrain_mat_tiling_[1] = grass;
            terrain_mat_tiling_[2] = dirt;
            terrain_mat_tiling_[3] = concrete;
            terrain_mat_tiling_snow_ = snow;
        }
        void getTerrainMatTiling(float* rock, float* grass, float* dirt, float* concrete, float* snow) const {
            *rock = terrain_mat_tiling_[0];
            *grass = terrain_mat_tiling_[1];
            *dirt = terrain_mat_tiling_[2];
            *concrete = terrain_mat_tiling_[3];
            *snow = terrain_mat_tiling_snow_;
        }
        // TERRAIN-CLASSIFY-TUNING-1
        void setTerrainClassGrass(const float* v) { memcpy(terrain_class_grass_, v, 4 * sizeof(float)); }
        void getTerrainClassGrass(float* v) const  { memcpy(v, terrain_class_grass_, 4 * sizeof(float)); }
        void setTerrainClassDirt(const float* v)   { memcpy(terrain_class_dirt_,  v, 4 * sizeof(float)); }
        void getTerrainClassDirt(float* v) const   { memcpy(v, terrain_class_dirt_,  4 * sizeof(float)); }
        void  setTerrainTintStrengthScale(float s) { terrain_tint_strength_scale_ = s; }
        float getTerrainTintStrengthScale() const   { return terrain_tint_strength_scale_; }
        // TERRAIN-CONTROLMAP-ALBEDO-1: default 0.0 -> shader's mix(x,1.0,0.0)==x
        // -> byte-identical when gate is OFF.
        void  setTerrainControlAlbedoStrength(float s) { terrain_control_albedo_strength_ = s; }
        float getTerrainControlAlbedoStrength() const   { return terrain_control_albedo_strength_; }
        // TERRAIN-MATERIAL-LIB-1: no setter existed prior (member only had an
        // env-read static initializer); added so the JSON reader can apply it
        // when MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN is unset (env still wins).
        void  setTerrainSnowBrightnessDampen(float v) { terrain_snow_brightness_dampen_ = v; }
        float getTerrainSnowBrightnessDampen() const  { return terrain_snow_brightness_dampen_; }
        // TERRAIN-TINT-UI-1
        void setTerrainTintRock(float r, float g, float b)  { terrain_tint_rock_[0]=r; terrain_tint_rock_[1]=g; terrain_tint_rock_[2]=b; }
        void getTerrainTintRock(float* r, float* g, float* b) const { *r=terrain_tint_rock_[0]; *g=terrain_tint_rock_[1]; *b=terrain_tint_rock_[2]; }
        void setTerrainTintGrass(float r, float g, float b)  { terrain_tint_grass_[0]=r; terrain_tint_grass_[1]=g; terrain_tint_grass_[2]=b; }
        void getTerrainTintGrass(float* r, float* g, float* b) const { *r=terrain_tint_grass_[0]; *g=terrain_tint_grass_[1]; *b=terrain_tint_grass_[2]; }
        void setTerrainTintDirt(float r, float g, float b)  { terrain_tint_dirt_[0]=r; terrain_tint_dirt_[1]=g; terrain_tint_dirt_[2]=b; }
        void getTerrainTintDirt(float* r, float* g, float* b) const { *r=terrain_tint_dirt_[0]; *g=terrain_tint_dirt_[1]; *b=terrain_tint_dirt_[2]; }
        // TERRAIN-MATERIAL-LIB-1: promoted frag-literal tints (concrete/snow).
        void setTerrainTintConcrete(float r, float g, float b) { terrain_tint_concrete_[0]=r; terrain_tint_concrete_[1]=g; terrain_tint_concrete_[2]=b; }
        void getTerrainTintConcrete(float* r, float* g, float* b) const { *r=terrain_tint_concrete_[0]; *g=terrain_tint_concrete_[1]; *b=terrain_tint_concrete_[2]; }
        void setTerrainTintSnow(float r, float g, float b) { terrain_tint_snow_[0]=r; terrain_tint_snow_[1]=g; terrain_tint_snow_[2]=b; }
        void getTerrainTintSnow(float* r, float* g, float* b) const { *r=terrain_tint_snow_[0]; *g=terrain_tint_snow_[1]; *b=terrain_tint_snow_[2]; }
        // TERRAIN-MATERIAL-LIB-1: per-layer roughness/AO scalars (rock,grass,dirt,concrete).
        void setTerrainMatRoughness(float rock, float grass, float dirt, float concrete) {
            terrain_mat_roughness_[0]=rock; terrain_mat_roughness_[1]=grass;
            terrain_mat_roughness_[2]=dirt; terrain_mat_roughness_[3]=concrete;
        }
        void getTerrainMatRoughness(float* rock, float* grass, float* dirt, float* concrete) const {
            *rock=terrain_mat_roughness_[0]; *grass=terrain_mat_roughness_[1];
            *dirt=terrain_mat_roughness_[2]; *concrete=terrain_mat_roughness_[3];
        }
        void setTerrainMatAO(float rock, float grass, float dirt, float concrete) {
            terrain_mat_ao_[0]=rock; terrain_mat_ao_[1]=grass;
            terrain_mat_ao_[2]=dirt; terrain_mat_ao_[3]=concrete;
        }
        void getTerrainMatAO(float* rock, float* grass, float* dirt, float* concrete) const {
            *rock=terrain_mat_ao_[0]; *grass=terrain_mat_ao_[1];
            *dirt=terrain_mat_ao_[2]; *concrete=terrain_mat_ao_[3];
        }
        void  setTerrainNormalsFromHeightStrength(float s) { terrain_nfh_strength_ = s; }
        float getTerrainNormalsFromHeightStrength() const  { return terrain_nfh_strength_; }
        void  setTerrainLightingV1Strength(float s) { terrain_lighting_v1_strength_ = s; }
        float getTerrainLightingV1Strength() const  { return terrain_lighting_v1_strength_; }
        void  setTerrainLightingV2Floor(float f) { terrain_lighting_v2_floor_ = f; }
        float getTerrainLightingV2Floor() const  { return terrain_lighting_v2_floor_; }

        // Terrain draw killswitch
        void setTerrainDrawEnabled(bool e) { terrain_draw_enabled_ = e; }
        bool getTerrainDrawEnabled() const { return terrain_draw_enabled_; }
        float getTerrainDebugMode() const { return terrain_debug_mode_; }

        // F1 Stage A-pre Task 7b: store probe-only worldToClipGL for upload at
        // draw time inside terrainBindUniformsForPatchStream (terrainDrawIndexedPatches retired).
        void setWorldToClipGLProbeMatrix(const float* M16) {
            memcpy(probeWorldToClipGL_, M16, 16 * sizeof(float));
            probeWorldToClipGLValid_ = true;
        }

        // Shadow pre-pass (separate pass over all terrain batches before shading)
        void beginShadowPrePass(bool clearDepth = true);
        void drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
            WORD* indices, int numIndices,
            const gos_TERRAIN_EXTRA* extras, int extraCount);
        void drawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
            HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4);
        void endShadowPrePass();

        // Dynamic object shadow pass (camera-centered, per-frame)
        void beginDynamicShadowPass();
        void endDynamicShadowPass();

        // Terrain splatting setters
        void setTerrainLightDir(float x, float y, float z) { terrain_light_dir_ = vec4(x, y, z, 0.0f); }
        const vec4& getTerrainLightDir() const { return terrain_light_dir_; }
        void setTerrainDetailParams(float tiling, float strength) { terrain_detail_tiling_ = tiling; terrain_detail_strength_ = strength; }
        void setTerrainMaterialNormal(int idx, GLuint texId) {
            if (idx >= 0 && idx < 9) {
                terrain_mat_normal_[idx] = texId;
                terrain_normal_array_dirty_ = true;
            }
        }
        // ROAD-PBR-ASPHALT-1: store the asphalt albedo GL handle (terrtxm2 loads
        // the TGA + creates the texture; we just hold the id for overlay binding).
        void setTerrainAsphaltAlbedoTexture(GLuint texId) {
            terrain_asphalt_albedo_tex_ = texId;
            terrain_asphalt_load_tried_ = true;
        }
        // ROAD-MATERIAL-GRAVEL-1: store the gravel albedo GL handle (terrtxm2 loads
        // the TGA + creates the texture; we just hold the id for overlay binding).
        void setTerrainGravelAlbedoTexture(GLuint texId) {
            terrain_gravel_albedo_tex_ = texId;
            terrain_gravel_load_tried_ = true;
        }
        void buildTerrainNormalArray() {
            // Require slots 0-4 (rock/grass/dirt/concrete/snow). Slots 5-8 are optional.
            for (int i = 0; i < 5; ++i) {
                if (terrain_mat_normal_[i] == 0) {
                    static bool s_warned = false;
                    if (!s_warned) {
                        s_warned = true;
                        SPEW(("GRAPHICS", "buildTerrainNormalArray: slot %d is zero -- deferring build until slots 0-4 are set\n", i));
                    }
                    return;
                }
            }

            GlPixelStoreGuard guard;  // saves and restores pixel-store, PBO, active tex, bindings

            // Query slot 0 for reference dimensions and internal format.
            GLint refW = 0, refH = 0, refFmt = 0;
            GLint baseLevel = 0, maxLevel = 0;
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[0]);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &refW);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &refH);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &refFmt);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &baseLevel);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  &maxLevel);
            glBindTexture(GL_TEXTURE_2D, 0);

            if (refW <= 0 || refH <= 0)
                STOP(("buildTerrainNormalArray: slot 0 has invalid size %dx%d", refW, refH));

            // Assert all present textures (required 1-4 and any loaded optional 5-8)
            // match slot 0 in dimensions and internal format.
            for (int i = 1; i < 9; ++i) {
                if (terrain_mat_normal_[i] == 0) continue;  // optional slot absent
                GLint wi = 0, hi = 0, fmti = 0;
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &wi);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &hi);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmti);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (wi != refW || hi != refH)
                    STOP(("buildTerrainNormalArray: slot %d size %dx%d != slot 0 size %dx%d",
                          i, wi, hi, refW, refH));
                if (fmti != refFmt)
                    STOP(("buildTerrainNormalArray: slot %d internal format 0x%X != slot 0 format 0x%X",
                          i, fmti, refFmt));
            }

            // Determine the actual mip chain depth.
            int numLevels = maxLevel - baseLevel + 1;
            if (numLevels < 1) numLevels = 1;
            {
                int maxPossible = 0, d = (refW > refH ? refW : refH);
                while (d > 0) { ++maxPossible; d >>= 1; }
                if (numLevels > maxPossible) numLevels = maxPossible;
            }

            // Delete the previous array texture if it exists.
            if (terrain_normal_array_tex_ != 0) {
                glDeleteTextures(1, &terrain_normal_array_tex_);
                terrain_normal_array_tex_ = 0;
            }

            // Allocate 9-layer GL_TEXTURE_2D_ARRAY (slots 0-4 required, 5-8 optional).
            // glGetTexImage always decompresses if the source is compressed; we
            // receive RGBA8 regardless of the source internal format.
            // TEX-CLASS: asset-pool -- terrain per-material normal-map 2D_ARRAY (content)
            glGenTextures(1, &terrain_normal_array_tex_);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                         refW, refH, 9 /*layers*/,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

            // Copy all mip levels from each source texture into the array.
            // Normal-map mips are authored — do not use glGenerateMipmap here
            // (averaging would shorten normals; hardware-generated mips differ
            // from the source importer's output).
            // Missing optional slots (terrain_mat_normal_[i] == 0) get a flat
            // neutral normal (128,128,255,255) so sampling them is always valid.
            std::vector<uint8_t> pixels;
            int levelsCopied = 0;
            for (int level = baseLevel; level < baseLevel + numLevels; ++level) {
                GLint mipW = 0, mipH = 0;
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[0]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH,  &mipW);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &mipH);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (mipW <= 0 || mipH <= 0) break;
                ++levelsCopied;

                glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
                glTexImage3D(GL_TEXTURE_2D_ARRAY, level, GL_RGBA8,
                             mipW, mipH, 9, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

                const size_t mipBytes = (size_t)mipW * mipH * 4;
                pixels.resize(mipBytes);

                for (int layer = 0; layer < 9; ++layer) {
                    if (terrain_mat_normal_[layer] != 0) {
                        glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[layer]);
                        glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                        glBindTexture(GL_TEXTURE_2D, 0);
                    } else {
                        // Flat neutral normal (128,128,255,255) for absent optional slot.
                        auto* p32 = reinterpret_cast<uint32_t*>(pixels.data());
                        std::fill(p32, p32 + mipW * mipH, 0xFFFF8080u);
                    }

                    glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level,
                                    0, 0, layer,
                                    mipW, mipH, 1,
                                    GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                }
            }

            // Sampling params. CRITICAL: MAX_LEVEL must match the levels ACTUALLY
            // copied, not the pre-computed numLevels. The copy loop breaks early
            // when a source mip is absent; if we declare more levels than exist,
            // the array is mip-INCOMPLETE -> every sample returns black under a
            // mipmap min filter (the chunk material-detail "all black" bug). If
            // only level 0 exists, drop to a non-mip filter so it stays complete.
            if (levelsCopied < 1) levelsCopied = 1;
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, baseLevel);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  baseLevel + levelsCopied - 1);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                            (levelsCopied > 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            // guard destructor restores all state

            terrain_normal_array_dirty_ = false;
        }
        // Phase 10 Step 5: expose the merged material normal sampler2DArray so the
        // terrain LOD chunk path (a bolt-on draw) can bind the SAME texture the
        // legacy terrain uses. Builds lazily if dirty/never-built; returns 0 until
        // required slots 0-4 are populated (slots 5-8 optional). GL context assumed
        // live (call from a draw, like the legacy bind sites).
        GLuint getTerrainNormalArrayTexEnsureBuilt() {
            if (terrain_normal_array_dirty_ || terrain_normal_array_tex_ == 0)
                buildTerrainNormalArray();
            return terrain_normal_array_tex_;
        }
        void setTerrainCellBombParams(float s, float j, float r) { terrain_cell_scale_ = s; terrain_cell_jitter_ = j; terrain_cell_rotation_ = r; }
        void setTerrainPOMParams(float scale, float, float) { terrain_pom_scale_ = scale; }
        void setTerrainWorldScale(float scale) { terrain_world_scale_ = scale; }

        // World-space overlay batch API (thin public surface — internals are private)
        void pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void drawTerrainOverlays();
        void drawDecals();
        // APPLY-STATE-TERRAINDECAL-1: first top-level scene-pass apply-state.
        // When MC2_FRAMEGRAPH_EXECUTOR is ON, the executor pre-applies the
        // TerrainDecal pipeline (the sole entry render-state of drawDecals) and
        // sets decalStateAppliedByExecutor_; drawDecals() then skips its own
        // applyPipeline (one-shot reset). Gate OFF -> flag stays false -> body
        // applies as before -> byte-identical.
        void executorApplyTerrainDecalState();
        bool decalStateAppliedByExecutor_ = false;
        // APPLY-STATE-TERRAINOVERLAY-1: second top-level scene-pass apply-state.
        // Same pattern as decal — when MC2_FRAMEGRAPH_EXECUTOR is ON the executor
        // pre-applies the TerrainOverlay pipeline (the sole entry render-state of
        // drawTerrainOverlays) and sets overlayStateAppliedByExecutor_; the body
        // then skips its own applyPipeline (one-shot). Gate OFF -> byte-identical.
        void executorApplyTerrainOverlayState();
        bool overlayStateAppliedByExecutor_ = false;
        // APPLY-STATE-WATER-1: fifth top-level scene-pass apply-state. ★BODY-SITE apply
        // (NOT a begin-seam pre-apply): renderWaterFastPath has a mid-body COMPUTE
        // dispatch (WaterStream::ComputeDispatchAndBindThinRecords -> glUseProgram(compute))
        // BETWEEN the begin seam and the applyPipeline(WaterArmed), so the executor apply is
        // dispatched at the live draw site (immediately before the body's :3276 applyPipeline,
        // AFTER the compute dispatch) and the body skips its own applyPipeline (one-shot).
        // Pipeline-only lift (water inherits the scene/MainColor FBO + viewport from the
        // caller). Gate OFF -> flag stays false -> body applies as before -> byte-identical.
        void executorApplyWaterState();
        bool waterStateAppliedByExecutor_ = false;
        // APPLY-STATE-SHADOW-1: first render-target-MODE apply-state consumer.
        // When MC2_FRAMEGRAPH_EXECUTOR is ON, the executor (at the dynamic-shadow seam,
        // beginDynamicShadowPass) applies the ShadowMech BASE pipeline + the forward-Z
        // depth clear (DepthForwardZ) via the EXTEND helper, and sets this flag; the body
        // then skips its own applyPipeline(ShadowMech) AND its forward-Z clear (one-shot
        // reset). FBO bind (ShadowDynamicMap) + viewport (ShadowMap size) stay body-owned
        // this slice (deferred to SHADOW-2). Gate OFF -> flag stays false -> body applies +
        // clears as before -> byte-identical. The capture/restore bracket, AMD feedback-unbind,
        // GL_TEXTURE_COMPARE_MODE flip, material/lsm upload, per-caster ShadowStaticProp, and
        // the noteRenderPass(ShadowCaster) seam all remain body-owned.
        void executorApplyShadowState();
        bool shadowStateAppliedByExecutor_ = false;
        // Slice A — draw the mission-static cement-overlay bake. Reproduces
        // drawTerrainOverlays()'s exact state/shader/uniforms/VAO but draws
        // the passed static VBO with per-overlayTexId ranges and does NOT
        // clear (mirrors DrawMineStatic). draws/drawCount are forwarded from
        // gos_terrain_indirect::DrawDecalStatic via the bridge.
        bool drawDecalStaticBatch(unsigned int vboGL,
                                  const struct GosDecalStaticDraw* draws,
                                  int drawCount);

        // RENDER_STATES v1: external invalidation hook (public).
        //
        // LOAD-BEARING CONTRACT — DO NOT REMOVE INVALIDATION CALLS.
        // Any code path that mutates GL state outside applyRenderStates'
        // tracked-slot set (program/depth/blend/cull/sampler/texture-bind,
        // including per-texture-object wrap/filter via glTexParameteri) MUST
        // call this hook AT THE END of the path so the next applyRenderStates
        // re-applies fully. Cache is consulted on the next call without
        // further bookkeeping; missed invalidation = silent visual corruption.
        // Origin: MAJOR-3 from 2026-05-08 adversarial review of the cache;
        // CRITICAL-1 was caller-side coverage gap of this exact contract.
        // Current invalidation sites (grep gos_InvalidateRenderStateCache /
        // invalidateRenderStateCache): gos_terrain_bridge_drawSingleBucket,
        // renderWaterFastPath, gos_terrain_bridge_drawIndirect,
        // endShadowPrePass, endDynamicShadowPass, gosPostProcess::endScene,
        // GpuStaticPropBatcher::flush, drawTerrainOverlays, drawDecals,
        // gos_ForceApplyRenderStates, gosRenderer::beginFrame (defensive).
        // When adding a new fast path: invalidate or break the cache contract.
        void invalidateRenderStateCache() { stateCacheValid_ = false; }

    private:

        bool beforeDrawCall();
        void afterDrawCall();

        // render target size
        int width_;
        int height_;
        graphics::RenderContextHandle ctx_h_;
        graphics::RenderWindowHandle win_h_;

        // fits vertices into viewport
        mat4 projection_;

		vec4 fog_color_;

        void initRenderStates();

        std::vector<gosTexture*> textureList_;
        std::vector<gosFont*> fontList_;
        std::vector<gosBuffer*> bufferList_;
        std::vector<gosVertexDeclaration*> vertexDeclarationList_;
        std::vector<gosRenderMaterial*> materialList_;
		typedef std::map<uint32_t, gosRenderMaterial*> MaterialDBValue_t;
		typedef std::map<std::string, MaterialDBValue_t> MaterialDB_t;
        MaterialDB_t materialDB_;

        DWORD reqWidth;
        DWORD reqHeight;
        DWORD reqBitDepth;
        DWORD reqAntiAlias;
        bool reqGotoFullscreen;
        bool pendingRequest;

        // states data
        RenderState curStates_;
        RenderState renderStates_;

        // RENDER_STATES v1: state-equality early-out cache for applyRenderStates().
        // stateCacheValid_=false forces a full apply on the next call (used after
        // any external GL state disturbance — bridges, post-process, shadow draws).
        // cachedResolvedTexId_[u] tracks the resolved GL texture id we last bound
        // on unit u; texture handles mutate per-frame (see memory note
        // mc2_texture_handle_is_live.md), so an unchanged gos_State_TextureN
        // does NOT imply the underlying GL texture is the same.
        bool stateCacheValid_ = false;
        uint32_t cachedResolvedTexId_[3] = {0u, 0u, 0u};
        // 600-frame summary counters
        uint32_t rsCalls_ = 0;
        uint32_t rsSkipped_ = 0;
        uint32_t rsApplied_ = 0;
        uint32_t rsFrames_ = 0;

        static const int RENDER_STATES_STACK_SIZE = 16;
        int renderStatesStackPointer;
        RenderState statesStack_[RENDER_STATES_STACK_SIZE];
        //

        // text data
        gosTextAttribs curTextAttribs_;

        int curTextPosX_;
        int curTextPosY_;

        int curTextLeft_;
        int curTextTop_;
        int curTextRight_;
        int curTextBottom_;
        //
        
        // viewport config
        bool clearDepth_;
        float clearDepthValue_;
        bool clearColor_;
        DWORD clearColorValue_;
        bool clearStencil_;
        DWORD clearStencilValue_;
        float viewportTop_;
        float viewportLeft_;
        float viewportBottom_;
        float viewportRight_;
        //

		vec4 render_viewport_;
        
        gosMesh* quads_;
        gosMesh* tris_;
        gosMesh* indexed_tris_;
        gosMesh* lines_;
        gosMesh* points_;
        gosMesh* text_;
        gosRenderMaterial* basic_material_;
        gosRenderMaterial* basic_tex_material_;
        gosRenderMaterial* text_material_;

        gosRenderMaterial* basic_lighted_material_;
        gosRenderMaterial* basic_tex_lighted_material_;
        //
        uint32_t num_draw_calls_;
        uint32_t num_draw_calls_to_draw_;
        bool break_on_draw_call_;
        uint32_t break_draw_call_num_;

        // HUD command buffer
        std::vector<HudDrawCall> hudBatch_;
        bool                     hudFlushed_;

        // Tessellation
        float terrain_tess_level_ = 4.0f;          // base inner/outer tessellation factor
        float terrain_tess_dist_near_ = 200.0f;    // full tess below this distance
        float terrain_tess_dist_far_ = 2000.0f;    // tess=1 beyond this distance
        float terrain_phong_alpha_ = 0.5f;         // Phong smoothing strength
        float terrain_displace_scale_ = 2.0f;      // displacement amplitude (dirt-only in TES)
        bool terrain_wireframe_ = false;            // wireframe overlay toggle
        float terrain_debug_mode_ = 0.0f;          // 0=off, 1..9 fragment debug modes (see GraphicsOptionsWindow kTerrainModes), -1 tess-alive probe. Env MC2_TERRAIN_DEBUG_MODE overrides at upload.

        // Terrain extra VBO for world pos + normal
        GLuint terrain_extra_vb_ = 0;
        gos_TERRAIN_EXTRA* terrain_extra_data_ = nullptr;
        int terrain_extra_count_ = 0;
        int terrain_extra_capacity_ = 0;
        int terrain_extra_draw_offset_ = 0;  // consumed offset for per-batch alignment (legacy)

        // Per-batch extras from texture manager (replaces global offset)
        const gos_TERRAIN_EXTRA* terrain_batch_extras_ = nullptr;
        int terrain_batch_extras_count_ = 0;

        // Terrain tessellation MVP (world-to-NDC)
        mat4 terrain_mvp_;
        bool terrain_mvp_valid_ = false;
        vec4 terrain_camera_pos_;  // MC2 world space camera position for TCS LOD
        vec4 terrain_viewport_;    // (vmx, vmy, vax, vay) for EditorCamera shim

        // F1 Stage A-pre Task 7b: probe-only worldToClipGL cache. Written by the
        // now-retired probe setter; retained for diagnostic retirement batch.
        float probeWorldToClipGL_[16] = {};
        bool probeWorldToClipGLValid_ = false;

        // Terrain tessellation material
        gosRenderMaterial* terrain_material_ = nullptr;
        glsl_program* thin_terrain_prog_ = nullptr;  // gos_terrain_thin.vert + gos_terrain.frag
        glsl_program* terrain_surface_prog_ = nullptr;  // [TERRAIN_SURFACE] PR-2 gos_terrain_surface.vert + gos_terrain.frag
        glsl_program* water_fast_prog_   = nullptr;  // gos_terrain_water_fast.vert + gos_tex_vertex.frag
        glsl_program* mine_static_prog_  = nullptr;  // PR2c Stage 2c — gos_terrain_mine_static.vert + .frag
        glsl_program* mask_solid_prog_   = nullptr;  // B4 Stage 1b — gos_terrain_mask_solid.vert + gos_terrain.frag
        glsl_program* mask_water_prog_   = nullptr;  // B4 Stage 1c — gos_terrain_mask_water.vert + gos_tex_vertex.frag

        // Shadow mode
        gosRenderMaterial* shadow_terrain_material_ = nullptr;
        gosRenderMaterial* shadow_object_material_ = nullptr;
        // GPU-driven dynamic sun shadow -- depth-only instanced programs (Phase 1).
        // Registered at init; called by Tasks 3+ to draw GPU-batched mechs/buildings
        // into the dynamic shadow FBO.
        glsl_program* shadow_mech_prog_        = nullptr;  // shadow_mech.vert + shadow_instanced.frag
        glsl_program* shadow_static_prop_prog_ = nullptr;  // shadow_static_prop.vert + shadow_instanced.frag
        bool shadow_mode_ = false;
        bool shadow_prepass_active_ = false;
        float terrain_shadow_softness_ = mc2ShadowCsmSoftness();  // env-overridable via MC2_SHADOW_CSM_SOFTNESS
        bool terrain_draw_enabled_ = true;
        GLint shadow_prepass_prev_fbo_ = 0;
        GLint shadow_prepass_prev_viewport_[4] = {0};
        const float* active_light_space_matrix_ = nullptr;  // routes static or dynamic LSM

        // Terrain splatting textures (from main source, needed for material normal maps)
        vec4 terrain_light_dir_;
        float terrain_detail_tiling_ = 1.0f;
        float terrain_detail_strength_ = 1.0f;  // global detail-normal multiplier; was 4.0 (too loud, carpet at distance)
        GLuint terrain_mat_normal_[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        GLuint terrain_normal_array_tex_   = 0;     // GL_TEXTURE_2D_ARRAY; 0 = not built
        bool   terrain_normal_array_dirty_ = false; // rebuild needed at next bind
        // ROAD-PBR-ASPHALT-1: seamless tiling asphalt albedo (GL_TEXTURE_2D).
        // Lazily loaded from data/textures/asphalt_albedo.tga on first overlay
        // upload; 0 = not loaded (load attempted once, failure leaves it 0 and
        // the frag falls back to stock tile RGB because the sampler reads black
        // → but the v_matId path still mixes; we guard the bind so a failed load
        // simply leaves unit 4 unbound and the asphalt branch reads whatever was
        // last bound there — acceptable, but the load is robust below).
        GLuint terrain_asphalt_albedo_tex_  = 0;
        bool   terrain_asphalt_load_tried_  = false;
        // World-units -> UV scale for asphalt tiling. Repeat ~ 1/scale world
        // units; 0.14 ~= one tile every ~7 world units. Tune later.
        float  terrain_asphalt_scale_       = 0.14f;
        // ROAD-MATERIAL-GRAVEL-1: seamless tiling gravel albedo (GL_TEXTURE_2D),
        // lazily loaded from data/textures/gravel_albedo.tga; 0 = not loaded. Same
        // graceful-degrade contract as the asphalt handle above.
        GLuint terrain_gravel_albedo_tex_   = 0;
        bool   terrain_gravel_load_tried_   = false;
        float terrain_cell_scale_ = 8.0f;
        float terrain_cell_jitter_ = 0.8f;
        float terrain_cell_rotation_ = 1.0f;
        float terrain_pom_scale_ = 0.02f;
        float terrain_world_scale_ = 15360.0f;
        // Per-material normal boost: [rock, grass, dirt, concrete]; matches shader const default.
        float terrain_mat_normal_boost_[4] = { 0.9f, 0.5f, 1.1f, 2.5f };  // grass 1.1->0.5: detail normal was too loud, formed tiling carpet at distance
        // Per-material UV tiling: [rock, grass, dirt, concrete]; snow separate.
        // Grass default lowered 12→2 to reduce excessive normal-map repetition.
        float terrain_mat_tiling_[4] = { 3.0f, 2.0f, 1.0f, 6.0f };
        float terrain_mat_tiling_snow_ = 1.0f;
        // TERRAIN-CLASSIFY-TUNING-1: colormap RGB channel-delta classifier thresholds.
        //   grass = (gMinusRLo, gMinusRHi, gBrightLo, gBrightHi)
        //   dirt  = (rMinusGLo, rMinusGHi, rBrightLo, rBrightHi)
        // Sand_M24 profile widens the dirt gate at mission start (see mclib/terrain.cpp).
        float terrain_class_grass_[4] = { -0.02f, 0.06f, 0.22f, 0.40f };
        float terrain_class_dirt_[4]  = { -0.02f, 0.06f, 0.22f, 0.45f };
        float terrain_tint_strength_scale_ = 1.0f;  // 0=colormap passthrough, 1=full tint
        // TERRAIN-CONTROLMAP-ALBEDO-1: default 0.0 -- gate OFF uploads this
        // verbatim, and the frag's mix(x,1.0,0.0)==x is algebraically the
        // pre-slice expression (byte-identical). Gate ON + JSON/env authors
        // this toward 1.0 so authored control-map weights fully repaint albedo.
        float terrain_control_albedo_strength_ = 0.0f;
        // Snow brightness dampen: <1 darkens detected-snow fragments. Default 0.78
        // (visibly turned down); MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN overrides.
        float terrain_snow_brightness_dampen_ = [](){ const char* v = getenv("MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN"); return v ? (float)atof(v) : 0.78f; }();
        // TERRAIN-TINT-UI-1: material base tint colors (defaults match shader)
        float terrain_tint_rock_[3]  = { 0.36f, 0.37f, 0.40f };
        float terrain_tint_grass_[3] = { 0.35f, 0.42f, 0.25f };
        float terrain_tint_dirt_[3]  = { 0.48f, 0.42f, 0.33f };
        // TERRAIN-MATERIAL-LIB-1: the two tint constants that were frag literals
        // (terrain_lod_chunk.frag:715-716 / gos_terrain.frag:787-788) promoted to
        // uniforms so terrain_materials.json can cover them. Defaults are the
        // EXACT literal values -- byte-identity requirement.
        float terrain_tint_concrete_[3] = { 0.55f, 0.53f, 0.50f };
        float terrain_tint_snow_[3]     = { 0.75f, 0.78f, 0.84f };
        // Per-layer roughness/AO scalars (rock, grass, dirt, concrete). Neutral
        // 1.0 defaults so u_useMaterialLib==0 (gate OFF) skips the branch that
        // would consume them, and gate-ON with defaults reproduces current
        // lighting exactly (dot(weights, 1.0) == 1.0).
        float terrain_mat_roughness_[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float terrain_mat_ao_[4]        = { 1.0f, 1.0f, 1.0f, 1.0f };
        // TERRAIN-TUNING-UI-1: per-frame multiplier on the additive height-
        // derived normal term in gos_terrain.frag. 1.0 = full slope tilt
        // (current behavior; byte-equivalent to pre-slice). 0.0 = no slope
        // contribution. Slider range 0..1.5.
        float terrain_nfh_strength_ = 1.0f;
        // TERRAIN-LIGHTING-1: hemisphere ambient strength. Authoritative
        // gate is MC2_TERRAIN_LIGHTING_V1 — when gate OFF the upload site
        // force-zeroes the uniform so the shader branch short-circuits
        // (byte-equivalent to pre-slice). Member is ImGui-tunable when
        // gate is ON.
        float terrain_lighting_v1_strength_ = 1.0f;
        // TERRAIN-LIGHTING-2: shadow-aware fill floor for the V1 hemisphere
        // ambient. Slider 0..1; default 0.3 (member value used when env
        // gate MC2_TERRAIN_LIGHTING_V2 is ON). 1.0 = pure V1 behavior (no
        // shadow modulation); 0.0 = hemi follows shadow exactly. When the
        // V2 gate is OFF the upload site forces the uploaded value to 1.0
        // so V1 behavior is preserved (byte-equivalent to pre-slice).
        float terrain_lighting_v2_floor_ = 0.3f;

        // Cached uniform locations for terrain shader (avoid per-draw glGetUniformLocation)
        struct TerrainUniformLocs {
            GLint tessLevel = -1, tessDistanceRange = -1, tessDisplace = -1;
            GLint cameraPos = -1, tessDebug = -1, terrainMVP = -1;
            GLint pathTint = -1;  // MC2_SHADER_PATH_TINT debug
            GLint terrainLightDir = -1, detailNormalTiling = -1, detailNormalStrength = -1;
            GLint pomParams = -1, terrainWorldScale = -1, cellBombParams = -1;
            GLint matNormal[5] = {-1, -1, -1, -1, -1};
            GLint matNormalArray = -1;   // sampler2DArray (TERRAIN_NORMAL_ARRAY path)
            GLint lightSpaceMatrix = -1, enableShadows = -1, shadowSoftness = -1, shadowMap = -1;
            GLint dynamicLightSpaceMatrix = -1, enableDynamicShadows = -1, dynamicShadowMap = -1;
            // Item 1 CSM: array-variant dynamic shadow uniforms (only valid when ON)
            GLint dynamicShadowArray = -1, dynamicCascadeMatrices = -1, dynamicCsmCount = -1;
            GLint dynamicCascadeTexelWorld = -1, csmDepthSpan = -1;  // Stage 3 texel bias
            // Per-cascade shadow resolution: separate full-map (last) cascade.
            GLint dynamicFullMapShadow = -1, dynamicFullMapTexelWorld = -1;
            GLint time = -1;
            GLint mapHalfExtent = -1;
            GLint terrainMaterialProfile = -1;  // C1 tactical (mclib/terrain.h)
            GLint worldToClipGL = -1;            // F1 Task 7b probe uniform
            GLint matNormalBoost = -1;           // per-material normal strength (vec4)
            GLint matTiling = -1;                // per-material UV tiling (vec4: rock,grass,dirt,concrete)
            GLint matTilingSnow = -1;            // snow UV tiling (float)
            GLint tintStrengthScale = -1;        // global tint blend scalar
            GLint snowBrightnessDampen = -1;     // <1 darkens detected snow
            // TERRAIN-TINT-UI-1
            GLint tintRock  = -1;                // material base tint (vec3)
            GLint tintGrass = -1;
            GLint tintDirt  = -1;
            // TERRAIN-CLASSIFY-TUNING-1
            GLint terrainClassGrass = -1;        // colormap RGB grass thresholds (vec4)
            GLint terrainClassDirt  = -1;        // colormap RGB dirt thresholds (vec4)
            // TERRAIN-NORMALS-FROM-HEIGHT-1
            GLint terrainHeightTex = -1;             // sampler2D, unit 11 (R32F)
            GLint terrainHeightParams = -1;          // vec4 (gridSide, 1/wuPerVertex, topLeftX, topLeftY)
            GLint useTerrainNormalsFromHeight = -1;  // 0=off, 1=on
            // TERRAIN-TUNING-UI-1
            GLint terrainNormalsFromHeightStrength = -1; // float 0..1.5
            // TERRAIN-LIGHTING-1
            GLint terrainLightingV1Strength = -1; // float, 0=off; effective only when env gate ON
            // TERRAIN-LIGHTING-2
            GLint terrainLightingV2ShadowFillFloor = -1; // float, 1=V1 (no shadow influence)
            // TERRAIN-MATERIAL-LIB-1
            GLint tintConcrete = -1;              // promoted frag-literal (vec3)
            GLint tintSnow     = -1;               // promoted frag-literal (vec3)
            GLint matRoughness = -1;               // per-layer roughness scalar (vec4)
            GLint matAO        = -1;               // per-layer AO scalar (vec4)
            GLint useMaterialLib = -1;             // 0=off (byte-identical); 1=roughness/AO term applied
            // TERRAIN-CONTROLMAP-ALBEDO-1
            GLint controlAlbedoStrength = -1;      // 0=byte-identical, 1=full weight-composed albedo
            GLuint program = 0;
        } terrainLocs_;

        struct ThinTerrainUniformLocs {
            GLint terrainMVP = -1, mvp = -1;
            GLint cameraPos = -1, terrainLightDir = -1;
            GLint detailNormalTiling = -1, detailNormalStrength = -1;
            GLint pomParams = -1, terrainWorldScale = -1, cellBombParams = -1;
            GLint matNormal[5] = {-1,-1,-1,-1,-1};
            GLint matNormalArray = -1;
            GLint tex1 = -1;
            GLint lightSpaceMatrix = -1, enableShadows = -1, shadowSoftness = -1, shadowMap = -1;
            GLint dynamicLightSpaceMatrix = -1, enableDynamicShadows = -1, dynamicShadowMap = -1;
            // Item 1 CSM: array-variant dynamic shadow uniforms (only valid when ON)
            GLint dynamicShadowArray = -1, dynamicCascadeMatrices = -1, dynamicCsmCount = -1;
            GLint dynamicCascadeTexelWorld = -1, csmDepthSpan = -1;  // Stage 3 texel bias
            // Per-cascade shadow resolution: separate full-map (last) cascade.
            GLint dynamicFullMapShadow = -1, dynamicFullMapTexelWorld = -1;
            GLint time = -1, mapHalfExtent = -1;
            GLint ssboRecordBase = -1;
            GLint terrainMaterialProfile = -1;  // C1 tactical (mclib/terrain.h)
            GLint tessDebug = -1;               // shader debug-viz mode (frag mode 1..8)
            GLint pathTint = -1;                // MC2_SHADER_PATH_TINT debug
            GLint matNormalBoost = -1;          // per-material normal strength (vec4)
            GLint matTiling = -1;               // per-material UV tiling (vec4: rock,grass,dirt,concrete)
            GLint matTilingSnow = -1;           // snow UV tiling (float)
            GLint tintStrengthScale = -1;       // global tint blend scalar
            GLint snowBrightnessDampen = -1;    // <1 darkens detected snow
            // TERRAIN-TINT-UI-1
            GLint tintRock  = -1;               // material base tint (vec3)
            GLint tintGrass = -1;
            GLint tintDirt  = -1;
            // TERRAIN-CLASSIFY-TUNING-1
            GLint terrainClassGrass = -1;       // colormap RGB grass thresholds (vec4)
            GLint terrainClassDirt  = -1;       // colormap RGB dirt thresholds (vec4)
            // TERRAIN-NORMALS-FROM-HEIGHT-1
            GLint terrainHeightTex = -1;
            GLint terrainHeightParams = -1;
            GLint useTerrainNormalsFromHeight = -1;
            // TERRAIN-TUNING-UI-1
            GLint terrainNormalsFromHeightStrength = -1;
            // TERRAIN-LIGHTING-1
            GLint terrainLightingV1Strength = -1;
            // TERRAIN-LIGHTING-2
            GLint terrainLightingV2ShadowFillFloor = -1;
            // TERRAIN-MATERIAL-LIB-1: shares gosRenderer members with the chunk
            // path (see file header); these locs stay -1 on gos_terrain.frag
            // until the uniforms are declared there too, so the upload calls
            // below are no-ops (>= 0 guarded) and this path is unaffected.
            GLint tintConcrete = -1;
            GLint tintSnow     = -1;
            GLint matRoughness = -1;
            GLint matAO        = -1;
            GLint useMaterialLib = -1;
            // TERRAIN-CONTROLMAP-ALBEDO-1: shares the same identity contract as
            // the chunk path above (default 0.0 -> byte-identical when unbound
            // too, since loc stays -1 until the uniform is declared here).
            GLint controlAlbedoStrength = -1;
            GLuint program = 0;
        } thinTerrainLocs_;

        // Cached uniform locations for shadow terrain shader
        struct ShadowUniformLocs {
            GLint lightSpaceMatrix = -1, tessLevel = -1, tessDistanceRange = -1;
            GLint tessDisplace = -1, cameraPos = -1, mvp = -1;
            GLint matNormal2 = -1, detailNormalTiling = -1, tex1 = -1;
            GLint matNormalArray = -1;
            GLint terrainMaterialProfile = -1;  // C1 tactical (mclib/terrain.h)
            GLuint program = 0;
        } shadowLocs_;

        void cacheTerrainUniformLocations(GLuint shp) {
            if (terrainLocs_.program == shp) return;  // already cached
            terrainLocs_.program = shp;
            // TERRAIN-TEX-UNIT-MAP-1: verify the highest unit we use fits on this GPU.
            {
                GLint maxUnits = 0;
                glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits);
                if (kTerrainMatNormalUnits[4] >= maxUnits || kTerrainTexUnitHeight >= maxUnits)
                    STOP(("GL_MAX_TEXTURE_IMAGE_UNITS=%d too small; terrain needs unit %d (height) and %d (snow normal)",
                          maxUnits, kTerrainTexUnitHeight, kTerrainMatNormalUnits[4]));
            }
            terrainLocs_.tessLevel = glGetUniformLocation(shp, "tessLevel");
            terrainLocs_.tessDistanceRange = glGetUniformLocation(shp, "tessDistanceRange");
            terrainLocs_.tessDisplace = glGetUniformLocation(shp, "tessDisplace");
            terrainLocs_.cameraPos = glGetUniformLocation(shp, "cameraPos");
            terrainLocs_.tessDebug = glGetUniformLocation(shp, "tessDebug");
            terrainLocs_.pathTint = glGetUniformLocation(shp, "u_pathTint");
            terrainLocs_.terrainMVP = glGetUniformLocation(shp, "u_worldToClipGL");
            terrainLocs_.terrainLightDir = glGetUniformLocation(shp, "terrainLightDir");
            terrainLocs_.detailNormalTiling = glGetUniformLocation(shp, "detailNormalTiling");
            terrainLocs_.detailNormalStrength = glGetUniformLocation(shp, "detailNormalStrength");
            terrainLocs_.pomParams = glGetUniformLocation(shp, "pomParams");
            terrainLocs_.terrainWorldScale = glGetUniformLocation(shp, "terrainWorldScale");
            terrainLocs_.cellBombParams = glGetUniformLocation(shp, "cellBombParams");
            terrainLocs_.matNormal[0] = glGetUniformLocation(shp, "matNormal0");
            terrainLocs_.matNormal[1] = glGetUniformLocation(shp, "matNormal1");
            terrainLocs_.matNormal[2] = glGetUniformLocation(shp, "matNormal2");
            terrainLocs_.matNormal[3] = glGetUniformLocation(shp, "matNormal3");
            terrainLocs_.matNormal[4] = glGetUniformLocation(shp, "matNormal4");
            terrainLocs_.matNormalArray = glGetUniformLocation(shp, "matNormalArray");
            terrainLocs_.lightSpaceMatrix = glGetUniformLocation(shp, "lightSpaceMatrix");
            terrainLocs_.enableShadows = glGetUniformLocation(shp, "enableShadows");
            terrainLocs_.shadowSoftness = glGetUniformLocation(shp, "shadowSoftness");
            terrainLocs_.shadowMap = glGetUniformLocation(shp, "shadowMap");
            terrainLocs_.dynamicLightSpaceMatrix = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
            terrainLocs_.enableDynamicShadows = glGetUniformLocation(shp, "enableDynamicShadows");
            terrainLocs_.dynamicShadowMap = glGetUniformLocation(shp, "dynamicShadowMap");
            terrainLocs_.dynamicShadowArray = glGetUniformLocation(shp, "dynamicShadowArray");
            terrainLocs_.dynamicCascadeMatrices = glGetUniformLocation(shp, "dynamicCascadeMatrices");
            terrainLocs_.dynamicCsmCount = glGetUniformLocation(shp, "dynamicCsmCount");
            terrainLocs_.dynamicCascadeTexelWorld = glGetUniformLocation(shp, "dynamicCascadeTexelWorld");
            terrainLocs_.dynamicFullMapShadow = glGetUniformLocation(shp, "dynamicFullMapShadow");
            terrainLocs_.dynamicFullMapTexelWorld = glGetUniformLocation(shp, "dynamicFullMapTexelWorld");
            terrainLocs_.csmDepthSpan = glGetUniformLocation(shp, "csmDepthSpan");
            terrainLocs_.time = glGetUniformLocation(shp, "time");
            terrainLocs_.mapHalfExtent = glGetUniformLocation(shp, "mapHalfExtent");
            terrainLocs_.terrainMaterialProfile = glGetUniformLocation(shp, "g_terrainMaterialProfile");
            // F1 Task 7b: probe uniform location (flat uniform, not UBO member).
            terrainLocs_.worldToClipGL    = glGetUniformLocation(shp, "u_worldToClipGL");
            terrainLocs_.matNormalBoost   = glGetUniformLocation(shp, "matNormalBoost");
            terrainLocs_.matTiling        = glGetUniformLocation(shp, "matTiling");
            terrainLocs_.matTilingSnow    = glGetUniformLocation(shp, "matTilingSnow");
            terrainLocs_.tintStrengthScale = glGetUniformLocation(shp, "tintStrengthScale");
            terrainLocs_.snowBrightnessDampen = glGetUniformLocation(shp, "snowBrightnessDampen");
            // TERRAIN-TINT-UI-1
            terrainLocs_.tintRock  = glGetUniformLocation(shp, "tintRock");
            terrainLocs_.tintGrass = glGetUniformLocation(shp, "tintGrass");
            terrainLocs_.tintDirt  = glGetUniformLocation(shp, "tintDirt");
            // TERRAIN-CLASSIFY-TUNING-1
            terrainLocs_.terrainClassGrass = glGetUniformLocation(shp, "terrainClassGrass");
            terrainLocs_.terrainClassDirt  = glGetUniformLocation(shp, "terrainClassDirt");
            // TERRAIN-NORMALS-FROM-HEIGHT-1
            terrainLocs_.terrainHeightTex             = glGetUniformLocation(shp, "terrainHeightTex");
            terrainLocs_.terrainHeightParams          = glGetUniformLocation(shp, "terrainHeightParams");
            terrainLocs_.useTerrainNormalsFromHeight  = glGetUniformLocation(shp, "useTerrainNormalsFromHeight");
            // TERRAIN-TUNING-UI-1
            terrainLocs_.terrainNormalsFromHeightStrength =
                glGetUniformLocation(shp, "terrainNormalsFromHeightStrength");
            // TERRAIN-LIGHTING-1
            terrainLocs_.terrainLightingV1Strength =
                glGetUniformLocation(shp, "terrainLightingV1Strength");
            // TERRAIN-LIGHTING-2
            terrainLocs_.terrainLightingV2ShadowFillFloor =
                glGetUniformLocation(shp, "terrainLightingV2ShadowFillFloor");
            // TERRAIN-MATERIAL-LIB-1
            terrainLocs_.tintConcrete   = glGetUniformLocation(shp, "tintConcrete");
            terrainLocs_.tintSnow       = glGetUniformLocation(shp, "tintSnow");
            terrainLocs_.matRoughness   = glGetUniformLocation(shp, "matRoughness");
            terrainLocs_.matAO          = glGetUniformLocation(shp, "matAO");
            terrainLocs_.useMaterialLib = glGetUniformLocation(shp, "u_useMaterialLib");
            // TERRAIN-CONTROLMAP-ALBEDO-1
            terrainLocs_.controlAlbedoStrength = glGetUniformLocation(shp, "u_controlAlbedoStrength");
        }

        void cacheThinTerrainUniformLocations(GLuint shp) {
            if (thinTerrainLocs_.program == shp) return;
            thinTerrainLocs_.program            = shp;
            thinTerrainLocs_.terrainMVP         = glGetUniformLocation(shp, "u_worldToClipGL");
            thinTerrainLocs_.cameraPos          = glGetUniformLocation(shp, "cameraPos");
            thinTerrainLocs_.terrainLightDir    = glGetUniformLocation(shp, "terrainLightDir");
            thinTerrainLocs_.detailNormalTiling   = glGetUniformLocation(shp, "detailNormalTiling");
            thinTerrainLocs_.detailNormalStrength = glGetUniformLocation(shp, "detailNormalStrength");
            thinTerrainLocs_.pomParams          = glGetUniformLocation(shp, "pomParams");
            thinTerrainLocs_.terrainWorldScale  = glGetUniformLocation(shp, "terrainWorldScale");
            thinTerrainLocs_.cellBombParams     = glGetUniformLocation(shp, "cellBombParams");
            thinTerrainLocs_.matNormal[0]       = glGetUniformLocation(shp, "matNormal0");
            thinTerrainLocs_.matNormal[1]       = glGetUniformLocation(shp, "matNormal1");
            thinTerrainLocs_.matNormal[2]       = glGetUniformLocation(shp, "matNormal2");
            thinTerrainLocs_.matNormal[3]       = glGetUniformLocation(shp, "matNormal3");
            thinTerrainLocs_.matNormal[4]       = glGetUniformLocation(shp, "matNormal4");
            thinTerrainLocs_.matNormalArray     = glGetUniformLocation(shp, "matNormalArray");
            thinTerrainLocs_.tex1               = glGetUniformLocation(shp, "tex1");
            thinTerrainLocs_.lightSpaceMatrix   = glGetUniformLocation(shp, "lightSpaceMatrix");
            thinTerrainLocs_.enableShadows      = glGetUniformLocation(shp, "enableShadows");
            thinTerrainLocs_.shadowSoftness     = glGetUniformLocation(shp, "shadowSoftness");
            thinTerrainLocs_.shadowMap          = glGetUniformLocation(shp, "shadowMap");
            thinTerrainLocs_.dynamicLightSpaceMatrix = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
            thinTerrainLocs_.enableDynamicShadows    = glGetUniformLocation(shp, "enableDynamicShadows");
            thinTerrainLocs_.dynamicShadowMap        = glGetUniformLocation(shp, "dynamicShadowMap");
            thinTerrainLocs_.dynamicShadowArray      = glGetUniformLocation(shp, "dynamicShadowArray");
            thinTerrainLocs_.dynamicCascadeMatrices  = glGetUniformLocation(shp, "dynamicCascadeMatrices");
            thinTerrainLocs_.dynamicCsmCount         = glGetUniformLocation(shp, "dynamicCsmCount");
            thinTerrainLocs_.dynamicCascadeTexelWorld = glGetUniformLocation(shp, "dynamicCascadeTexelWorld");
            thinTerrainLocs_.dynamicFullMapShadow = glGetUniformLocation(shp, "dynamicFullMapShadow");
            thinTerrainLocs_.dynamicFullMapTexelWorld = glGetUniformLocation(shp, "dynamicFullMapTexelWorld");
            thinTerrainLocs_.csmDepthSpan            = glGetUniformLocation(shp, "csmDepthSpan");
            thinTerrainLocs_.time               = glGetUniformLocation(shp, "time");
            thinTerrainLocs_.mapHalfExtent      = glGetUniformLocation(shp, "mapHalfExtent");
            thinTerrainLocs_.ssboRecordBase     = glGetUniformLocation(shp, "ssboRecordBase");
            thinTerrainLocs_.terrainMaterialProfile = glGetUniformLocation(shp, "g_terrainMaterialProfile");
            thinTerrainLocs_.tessDebug          = glGetUniformLocation(shp, "tessDebug");
            thinTerrainLocs_.pathTint           = glGetUniformLocation(shp, "u_pathTint");
            thinTerrainLocs_.matNormalBoost     = glGetUniformLocation(shp, "matNormalBoost");
            thinTerrainLocs_.matTiling          = glGetUniformLocation(shp, "matTiling");
            thinTerrainLocs_.matTilingSnow      = glGetUniformLocation(shp, "matTilingSnow");
            thinTerrainLocs_.tintStrengthScale  = glGetUniformLocation(shp, "tintStrengthScale");
            thinTerrainLocs_.snowBrightnessDampen = glGetUniformLocation(shp, "snowBrightnessDampen");
            // TERRAIN-TINT-UI-1
            thinTerrainLocs_.tintRock  = glGetUniformLocation(shp, "tintRock");
            thinTerrainLocs_.tintGrass = glGetUniformLocation(shp, "tintGrass");
            thinTerrainLocs_.tintDirt  = glGetUniformLocation(shp, "tintDirt");
            // TERRAIN-CLASSIFY-TUNING-1
            thinTerrainLocs_.terrainClassGrass = glGetUniformLocation(shp, "terrainClassGrass");
            thinTerrainLocs_.terrainClassDirt  = glGetUniformLocation(shp, "terrainClassDirt");
            // TERRAIN-NORMALS-FROM-HEIGHT-1
            thinTerrainLocs_.terrainHeightTex             = glGetUniformLocation(shp, "terrainHeightTex");
            thinTerrainLocs_.terrainHeightParams          = glGetUniformLocation(shp, "terrainHeightParams");
            thinTerrainLocs_.useTerrainNormalsFromHeight  = glGetUniformLocation(shp, "useTerrainNormalsFromHeight");
            // TERRAIN-TUNING-UI-1
            thinTerrainLocs_.terrainNormalsFromHeightStrength =
                glGetUniformLocation(shp, "terrainNormalsFromHeightStrength");
            // TERRAIN-LIGHTING-1
            thinTerrainLocs_.terrainLightingV1Strength =
                glGetUniformLocation(shp, "terrainLightingV1Strength");
            // TERRAIN-LIGHTING-2
            thinTerrainLocs_.terrainLightingV2ShadowFillFloor =
                glGetUniformLocation(shp, "terrainLightingV2ShadowFillFloor");
            // TERRAIN-CONTROLMAP-ALBEDO-1: stays -1 until gos_terrain_thin's frag
            // declares the uniform too (thin path is the dead/legacy indirect
            // shader per recon) -- upload call below is >=0 guarded, no-op.
            thinTerrainLocs_.controlAlbedoStrength =
                glGetUniformLocation(shp, "u_controlAlbedoStrength");
        }

        void cacheShadowUniformLocations(GLuint shp) {
            if (shadowLocs_.program == shp) return;
            shadowLocs_.program = shp;
            shadowLocs_.lightSpaceMatrix = glGetUniformLocation(shp, "lightSpaceMatrix");
            shadowLocs_.tessLevel = glGetUniformLocation(shp, "tessLevel");
            shadowLocs_.tessDistanceRange = glGetUniformLocation(shp, "tessDistanceRange");
            shadowLocs_.tessDisplace = glGetUniformLocation(shp, "tessDisplace");
            shadowLocs_.cameraPos = glGetUniformLocation(shp, "cameraPos");
            shadowLocs_.mvp = glGetUniformLocation(shp, "mvp");
            shadowLocs_.matNormal2 = glGetUniformLocation(shp, "matNormal2");
            shadowLocs_.matNormalArray = glGetUniformLocation(shp, "matNormalArray");
            shadowLocs_.detailNormalTiling = glGetUniformLocation(shp, "detailNormalTiling");
            shadowLocs_.tex1 = glGetUniformLocation(shp, "tex1");
            shadowLocs_.terrainMaterialProfile = glGetUniformLocation(shp, "g_terrainMaterialProfile");
        }

        // ── World-space overlay batches ────────────────────────────────────────
        // Both terrain overlays (cement perimeter) and decals (craters, footprints)
        // use the same vertex layout (WorldOverlayVert, 28 bytes) and VAO setup.
        // Per-draw texture grouping: multiple addTriangle calls within a frame with
        // different texHandle values are batched into separate draw entries.
        struct OverlayBatchEntry_ {
            unsigned int texHandle;
            unsigned int firstVert;    // index into verts vector
            unsigned int vertCount;
        };
        struct OverlayBatch_ {
            GLuint vbo = 0, vao = 0;
            std::vector<WorldOverlayVert> verts;
            std::vector<OverlayBatchEntry_> draws;
        };

        OverlayBatch_ terrainOverlayBatch_;
        OverlayBatch_ decalBatch_;

        glsl_program* overlayProg_ = nullptr;  // terrain_overlay.vert + terrain_overlay.frag
        glsl_program* decalProg_   = nullptr;  // terrain_overlay.vert + decal.frag

        struct OverlayUniformLocs_ {
            GLint terrainMVP     = -1;
            GLint mvp            = -1;
            GLint tex1           = -1;
            GLint fog_color      = -1;
            GLint time           = -1;
            GLint cameraPos      = -1;
            GLint surfaceDebugMode = -1;
            GLint pathTint        = -1;  // MC2_SHADER_PATH_TINT debug
            GLint terrainLightDir = -1;
            GLint mapHalfExtent  = -1;
            // TERRAIN-DECAL-LIGHTING-1a: shared terrain-lighting uniforms on
            // the cement overlay program. Cached for both overlayProg_ and
            // decalProg_ via the same lambda — decalProg_'s shader doesn't
            // declare these, so glGetUniformLocation returns -1 there and
            // the helper skips them (no behavioral change to decal path).
            GLint terrainHeightTex                   = -1;
            GLint terrainHeightParams                = -1;
            GLint useTerrainNormalsFromHeight        = -1;
            GLint terrainNormalsFromHeightStrength   = -1;
            GLint terrainLightingV1Strength          = -1;
            GLint terrainLightingV2ShadowFillFloor   = -1;
            // ROAD-PBR-ASPHALT-1: asphalt material samplers + UV scale. Declared
            // only in terrain_overlay.frag → populated only on overlayProg_;
            // decalProg_ (decal.frag) leaves these -1 and the helper skips them.
            GLint asphaltAlbedo                      = -1;  // sampler2D
            GLint matNormalArray                     = -1;  // sampler2DArray
            GLint asphaltScale                       = -1;  // float
            // ROAD-MATERIAL-GRAVEL-1: gravel albedo sampler (terrain_overlay.frag
            // v_matId==2). Only declared on overlayProg_; decalProg_ leaves it -1.
            GLint gravelAlbedo                       = -1;  // sampler2D
        };
        OverlayUniformLocs_ overlayLocs_;
        OverlayUniformLocs_ decalLocs_;
        uint64_t timeStart_ = 0;          // shared epoch for all time-animated shaders

        // private helpers
        void pushToOverlayBatch_(OverlayBatch_& b, const WorldOverlayVert* v3, unsigned int texHandle);
        void uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed, const float* terrainMvpOverride = nullptr);
        // ── End world-space overlay batch members ──────────────────────────────
};

const std::string gosRenderer::s_Foreground = std::string("Foreground");

// TERRAIN-SPINE-0: free-function program-id accessors for the read-only
// Terrain Pass inspector. Defined here (after the gosRenderer class body)
// so other TUs (gameosmain.cpp) can reference them without seeing the full
// class. Each returns the raw GL program object id, or 0 if not linked.
uint32_t gos_getTerrainSurfaceProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getTerrainSurfaceProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
uint32_t gos_getThinTerrainProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getThinTerrainProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
uint32_t gos_getWaterFastProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getWaterFastProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
uint32_t gos_getTerrainOverlayProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getTerrainOverlayProgram();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}

// SHADOW-SPINE-0: read-only program-id accessor for the static-prop shadow
// program (shadow_static_prop.vert + shadow_instanced.frag). Mirrors the
// terrain accessors above. Returns 0 when not linked.
uint32_t gos_getStaticPropShadowProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    glsl_program* p = r->getShadowStaticPropProg();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
// SHADOW-SPINE-0: terrain shadow program is owned by the shadow_terrain
// gosRenderMaterial (not a direct glsl_program* slot). Look up via the
// material's shader. Returns 0 when not yet loaded.
uint32_t gos_getTerrainShadowProgramId() {
    gosRenderer* r = getGosRenderer();
    if (!r) return 0u;
    gosRenderMaterial* m = r->getShadowTerrainMaterial();
    if (!m) return 0u;
    glsl_program* p = m->getShader();
    return (p && p->shp_) ? (uint32_t)p->shp_ : 0u;
}
// SHADOW-SPINE-0: read-only accessors for shadow-pass pipeline state.
// All read from the existing gosPostProcess singleton; no new state.
bool gos_getShadowsEnabled() {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->shadowsEnabled_;
}
int gos_getShadowMapSize() {
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->getShadowMapSize() : 0;
}
int gos_getDynShadowMapSize() {
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->getDynamicShadowMapSize() : 0;
}
// Phase 10 Step 5: material normal sampler2DArray handle for the LOD chunk path.
unsigned int gos_GetTerrainNormalArrayTex() {
    gosRenderer* r = getGosRenderer();
    return r ? r->getTerrainNormalArrayTexEnsureBuilt() : 0u;
}

// ─── gos_terrain_bridge implementation ────────────────────────────────────
// Defined here because the full gosRenderer type is visible in this TU.
// Declarations live in gos_terrain_bridge.h.

gosRenderMaterial* gos_terrain_bridge_getMaterial() {
    return g_gos_renderer ? g_gos_renderer->getTerrainMaterial() : nullptr;
}

unsigned int gos_terrain_bridge_getShaderProgram() {
    if (!g_gos_renderer) return 0;
    gosRenderMaterial* mat = g_gos_renderer->getTerrainMaterial();
    if (!mat || !mat->getShader()) return 0;
    return (unsigned int)mat->getShader()->shp_;
}

void gos_terrain_bridge_bindUniforms(gosRenderMaterial* material) {
    if (g_gos_renderer && material)
        g_gos_renderer->terrainBindUniformsForPatchStream(material);
}

// gos_terrain_bridge_applyVertexDeclaration deleted by TERRAIN-BRIDGE-BODY-DELETE-2:
// callerless after TerrainPatchStream::flush() retired (TERRAIN-BRIDGE-BODY-DELETE-1).

// gos_terrain_bridge_endVertexDeclaration and gos_terrain_bridge_end deleted by
// TERRAIN-BRIDGE-BODY-DELETE-1: these were called only from TerrainPatchStream::flush(),
// which was retired in PATCHSTREAM-THIN-RETIRE-1 (026e7276).

unsigned int gos_terrain_bridge_glTextureForGosHandle(unsigned int gosHandle) {
    if (!g_gos_renderer) return 0;
    if (gosHandle == 0u || gosHandle == INVALID_TEXTURE_ID) return 0;
    gosTexture* tex = g_gos_renderer->getTexture(gosHandle);
    if (!tex) return 0;
    return (unsigned int)tex->getTextureId();
}

// gos_terrain_bridge_drawPatchStreamBucket deleted by TERRAIN-BRIDGE-BODY-DELETE-2:
// callerless after TerrainPatchStream::flush() retired (TERRAIN-BRIDGE-BODY-DELETE-1).

static const bool s_patchStreamDirectBind =
    (getenv("MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND") != nullptr);

// GL sampler object used in the direct-bind fast path to enforce correct
// filter/wrap state on unit 0 without per-bucket glTexParameteri calls.
// Sampler objects override per-texture-object sampler state while bound.
// Terrain colormaps have no mipmaps → GL_LINEAR + GL_CLAMP_TO_EDGE.
static GLuint s_terrainBucketSampler = 0;

// gos_terrain_bridge_beginBucketLoop and gos_terrain_bridge_drawSingleBucket deleted by
// TERRAIN-BRIDGE-BODY-DELETE-1: called only from TerrainPatchStream::flush() (retired
// in PATCHSTREAM-THIN-RETIRE-1, 026e7276).

// gos_terrain_bridge_endBucketLoop deleted by TERRAIN-BRIDGE-BODY-DELETE-2:
// callerless after TerrainPatchStream::flush() retired (TERRAIN-BRIDGE-BODY-DELETE-1).

unsigned int gos_terrain_bridge_getThinShaderProgram() {
    if (!g_gos_renderer) return 0u;
    glsl_program* p = g_gos_renderer->getThinTerrainProgram();
    return (p && p->shp_) ? (unsigned int)p->shp_ : 0u;
}

int gos_terrain_bridge_bindThinUniforms() {
    if (!g_gos_renderer) return -1;
    return g_gos_renderer->terrainBindThinUniformsForPatchStream();
}

// gos_terrain_bridge_drawSingleBucketTriangles deleted by TERRAIN-BRIDGE-BODY-DELETE-2:
// callerless after TerrainPatchStream::flush() retired (TERRAIN-BRIDGE-BODY-DELETE-1).

unsigned int gos_terrain_bridge_getWaterFastShaderProgram() {
    if (!g_gos_renderer) return 0u;
    glsl_program* p = g_gos_renderer->getWaterFastProgram();
    return (p && p->shp_) ? (unsigned int)p->shp_ : 0u;
}

// WaterPerCmd — per-draw data for glMultiDrawArraysIndirect, indexed by gl_DrawID.
// 32 B std430-aligned; lockstep with gos_terrain_water_fast_mdi.vert binding 7.
struct WaterPerCmd {
    uint32_t textureSlot;   // reserved; texture unit selection driven by o_isWater in FS
    int32_t  isWater;       // 1 = base, 2 = detail (matches legacy isWater uniform)
    int32_t  detailMode;    // 0 = base, 1 = detail
    float    uvScale;
    float    uvOffsetX;
    float    uvOffsetY;
    uint32_t pad0_;
    uint32_t pad1_;
};
static_assert(sizeof(WaterPerCmd) == 32, "WaterPerCmd must be 32 B");

// WATER-DEBUG-VIEWS-1: fragment/material-space debug mode for the MDI water FS
// (gos_terrain_water_mdi.frag uniform u_waterDebugMode). Sentinel -1 = uninit:
// resolve once from MC2_WATER_DEBUG_MODE. ImGui (EditorInspector) may overwrite
// with a live value (>=0). Distinct from the VS geometry-space debugMode latch
// (MC2_RENDER_WATER_FASTPATH_DEBUG). 0=Final default.
int g_waterFsDebugMode = -1;
int gos_GetWaterFsDebugMode()
{
    if (g_waterFsDebugMode < 0) {
        const char* wdm = getenv("MC2_WATER_DEBUG_MODE");
        g_waterFsDebugMode = wdm ? atoi(wdm) : 0;
    }
    return g_waterFsDebugMode;
}
void gos_SetWaterFsDebugMode(int m) { g_waterFsDebugMode = (m < 0) ? 0 : m; }

// WATER-TUNING-UI-1: runtime-tunable MDI water material params. Defaults MUST
// match the former compile-time consts in gos_terrain_water_mdi.frag EXACTLY
// (byte-identical default render). Uploaded each frame in the MDI bind block;
// driven live by Graphics Options > Water. Only the MDI FS consumes these.
float g_waterAbsorptionDensity = 0.022f;
float g_waterMaxAlpha          = 0.87f;
float g_waterRippleGain        = 0.22f;
float g_waterGlintGain         = 0.30f;
float g_waterDeepColor[3]      = { 0.03f, 0.13f, 0.20f };
float g_waterShallowColor[3]   = { 0.22f, 0.45f, 0.38f };

float gos_GetWaterAbsorptionDensity()       { return g_waterAbsorptionDensity; }
void  gos_SetWaterAbsorptionDensity(float v){ g_waterAbsorptionDensity = v; }
float gos_GetWaterMaxAlpha()                { return g_waterMaxAlpha; }
void  gos_SetWaterMaxAlpha(float v)         { g_waterMaxAlpha = v; }
float gos_GetWaterRippleGain()              { return g_waterRippleGain; }
void  gos_SetWaterRippleGain(float v)       { g_waterRippleGain = v; }
float gos_GetWaterGlintGain()               { return g_waterGlintGain; }
void  gos_SetWaterGlintGain(float v)        { g_waterGlintGain = v; }
void  gos_GetWaterDeepColor(float* rgb)     { rgb[0]=g_waterDeepColor[0]; rgb[1]=g_waterDeepColor[1]; rgb[2]=g_waterDeepColor[2]; }
void  gos_SetWaterDeepColor(float r,float g,float b)    { g_waterDeepColor[0]=r; g_waterDeepColor[1]=g; g_waterDeepColor[2]=b; }
void  gos_GetWaterShallowColor(float* rgb)  { rgb[0]=g_waterShallowColor[0]; rgb[1]=g_waterShallowColor[1]; rgb[2]=g_waterShallowColor[2]; }
void  gos_SetWaterShallowColor(float r,float g,float b) { g_waterShallowColor[0]=r; g_waterShallowColor[1]=g; g_waterShallowColor[2]=b; }

// WATER-VISUAL-FIRST-SLICE: gated camera-INDEPENDENT sky/horizon tint. Strength
// default 0.0 = exact no-op (byte-identical). Env MC2_WATER_SKYTINT=1 bumps the
// default to a small value for quick A/B; the ImGui slider is authoritative once
// touched. Sentinel -1 on strength = uninit (resolve env once). NOT fresnel /
// reflection (those stay shelved per the 2026-05-17 camera-independence ruling).
float g_waterSkyTintStrength = -1.0f;
float g_waterSkyTintColor[3] = { 0.55f, 0.70f, 0.85f };  // soft sky-blue/horizon
float gos_GetWaterSkyTintStrength()
{
    if (g_waterSkyTintStrength < 0.0f) {
        const char* v = getenv("MC2_WATER_SKYTINT");
        g_waterSkyTintStrength = (v && v[0] && v[0] != '0') ? 0.15f : 0.0f;
    }
    return g_waterSkyTintStrength;
}
void  gos_SetWaterSkyTintStrength(float v) { g_waterSkyTintStrength = (v < 0.0f) ? 0.0f : v; }
void  gos_GetWaterSkyTintColor(float* rgb) { rgb[0]=g_waterSkyTintColor[0]; rgb[1]=g_waterSkyTintColor[1]; rgb[2]=g_waterSkyTintColor[2]; }
void  gos_SetWaterSkyTintColor(float r,float g,float b) { g_waterSkyTintColor[0]=r; g_waterSkyTintColor[1]=g; g_waterSkyTintColor[2]=b; }

// WATER-SKY-REFLECTION-1: gated camera-DEPENDENT SH-L2 sky reflection on the MDI
// water FS. Default 0.12 = un-gated (sky reflection is ON by default as of
// WATER-SUN-SPEC-1; MC2_WATER_REFLECTION=0 kills it back to 0.0 byte-identical).
// Env MC2_WATER_REFLECTION=0 -> 0.0 no-op; =1 or unset -> 0.12 default.
// Slider authoritative once touched. Sentinel -1 = uninit (resolve env once).
float g_waterReflStrength = -1.0f;
float gos_GetWaterReflStrength()
{
    if (g_waterReflStrength < 0.0f) {
        const char* v = getenv("MC2_WATER_REFLECTION");
        // WATER-REFL-DEFAULT-ON: default ON (1.5); MC2_WATER_REFLECTION=0 kills it.
        // Higher default exposes the HDRI specular contribution at gameplay camera.
        const bool reflOff = (v && v[0] == '0');
        g_waterReflStrength = reflOff ? 0.0f : 1.5f;
    }
    return g_waterReflStrength;
}
void  gos_SetWaterReflStrength(float v) { g_waterReflStrength = (v < 0.0f) ? 0.0f : v; }
// Whether reflection is enabled (default: ON). Env MC2_WATER_REFLECTION=0 kills it.
// Resolved live each call (cheap). UI uses this to toggle gate-state label.
int   gos_GetWaterReflectionGate()
{
    const char* v = getenv("MC2_WATER_REFLECTION");
    return (v && v[0] == '0') ? 0 : 1;  // default ON; =0 disables
}

// WATER-REFLECTION-SAMPLE-1: strength of the terrain reflection RT blended OVER
// the SH sky in the MDI water FS. Gate MC2_WATER_REFLECTION_RT (same env that
// drives the C1 RT fill pass). Default 0 = no RT blend -> SH sky only ->
// byte-identical to Phase A. Sentinel -1 = uninit (resolve env once).
float g_waterRtStrength = -1.0f;
float gos_GetWaterRtStrength()
{
    if (g_waterRtStrength < 0.0f) {
        const char* v = getenv("MC2_WATER_REFLECTION_RT");
        g_waterRtStrength = (v && v[0] && v[0] != '0') ? 0.85f : 0.0f;
    }
    return g_waterRtStrength;
}
void gos_SetWaterRtStrength(float v) { g_waterRtStrength = (v < 0.0f) ? 0.0f : v; }
int  gos_GetWaterReflectionRtGate()
{
    const char* v = getenv("MC2_WATER_REFLECTION_RT");
    return (v && v[0] && v[0] != '0') ? 1 : 0;
}

// WATER-SUN-SPEC-1: Blinn-Phong sun specular intensity on MDI water FS.
// Default 0.0 = exact no-op (byte-identical). Env MC2_WATER_SHINE=1 bumps
// the default to 0.25 for quick A/B; the ImGui slider is authoritative once
// touched. Sentinel -1 = uninit (resolve env once).
float g_waterSpecIntensity = -1.0f;
float gos_GetWaterSpecIntensity()
{
    if (g_waterSpecIntensity < 0.0f) {
        const char* v = getenv("MC2_WATER_SHINE");
        g_waterSpecIntensity = (v && v[0] && v[0] != '0') ? 0.25f : 0.0f;
    }
    return g_waterSpecIntensity;
}
void gos_SetWaterSpecIntensity(float v) { g_waterSpecIntensity = (v < 0.0f) ? 0.0f : v; }

void gos_terrain_bridge_renderWaterFast(
    unsigned int recordCount,
    unsigned int waterGosHandle,
    unsigned int waterDetailGosHandle,
    float waterElevation,
    float alphaDepth,
    unsigned int alphaEdgeByte,
    unsigned int alphaMiddleByte,
    unsigned int alphaDeepByte,
    float mapTopLeftX,
    float mapTopLeftY,
    float frameCos,
    float frameCosAlpha,
    float oneOverTF,
    float oneOverWaterTF,
    float cloudOffsetX,
    float cloudOffsetY,
    float sprayOffsetX,
    float sprayOffsetY,
    float maxMinUV)
{
    if (!g_gos_renderer || recordCount == 0) return;
    g_gos_renderer->renderWaterFastPath(
        recordCount, waterGosHandle, waterDetailGosHandle,
        waterElevation, alphaDepth,
        alphaEdgeByte, alphaMiddleByte, alphaDeepByte,
        mapTopLeftX, mapTopLeftY,
        frameCos, frameCosAlpha,
        oneOverTF, oneOverWaterTF,
        cloudOffsetX, cloudOffsetY,
        sprayOffsetX, sprayOffsetY,
        maxMinUV);
}

#ifdef MC2_VULKAN_ISLAND
// VK-P9a WATER-INTEROP: the water island entry points (defined in
// GameOS/gameos/vulkan_water_island.cpp under the same #ifdef). File-scope
// declarations so renderWaterFastPath can call them (extern "C" must be at
// namespace scope, not inside a function body).
extern "C" int mc2_vk_water_island_enabled();
extern "C" int mc2_vk_water_island_run(
    unsigned int sceneColorTex, unsigned int sceneDepthTex, int w, int h,
    const void* thinRecords, unsigned int thinCount,
    const void* recipes, unsigned int recipeCount,
    const void* vsParams, const void* fsParams,
    unsigned int hdriGlTex);
#endif

// WATER-HDRI-REFL-PERF-1 LOD default, shared by the GL MDI bind block and the
// VK water island fsParams pack (RUNG-6.5) so the two paths cannot drift.
// MC2_WATER_HDRI_LOD (float) overrides; else MC2_WATER_HDRI_REFL_FULL=1 -> 1.0,
// default 4.0 (256x128 mip of the 4K source — see the bind-block comment).
static float mc2WaterHdriLodDefault()
{
    static float s_lod = -999.0f;
    if (s_lod < -1.0f) {
        const char* lv = getenv("MC2_WATER_HDRI_LOD");
        if (lv && lv[0]) {
            s_lod = (float)atof(lv);
        } else {
            const char* full = getenv("MC2_WATER_HDRI_REFL_FULL");
            const bool fullRate = (full && full[0] && full[0] != '0');
            s_lod = fullRate ? 1.0f : 4.0f;
        }
    }
    return s_lod;
}

void gosRenderer::renderWaterFastPath(
    unsigned int recordCount,
    unsigned int waterGosHandle,
    unsigned int waterDetailGosHandle,
    float waterElevation,
    float alphaDepth,
    unsigned int alphaEdgeByte,
    unsigned int alphaMiddleByte,
    unsigned int alphaDeepByte,
    float mapTopLeftX,
    float mapTopLeftY,
    float frameCos,
    float frameCosAlpha,
    float oneOverTF,
    float oneOverWaterTF,
    float cloudOffsetX,
    float cloudOffsetY,
    float sprayOffsetX,
    float sprayOffsetY,
    float maxMinUV)
{
    ZoneScopedN("renderWaterFastPath");
    TracyGpuZone("renderWaterFastPath");
    if (!water_fast_prog_ || !water_fast_prog_->shp_ || recordCount == 0) return;
    // WATER-SAME-ORDER-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end. Placed AFTER the
    // early-return guards so begin only fires on real draws; the RAII end fires on all exit paths.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::Water,
                                              "gosRenderer_renderWaterFastPath");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::Water,
                                                    "gosRenderer_renderWaterFastPath");
        }
    } _tlGuard;
    // Water now HAS an AmbientContract row (depthFunc=GEQUAL, depthWrite=On, colorMask/blend
    // Inherit) and an fbo_ledger MainColor row -> ambient/fbo guards validate (no longer skip).
    render_contract::noteRenderPass(render_contract::PassIdentity::Water,
                                    "gosRenderer::renderWaterFastPath");
    GLuint prog = water_fast_prog_->shp_;

    // Lazy-compile the MDI variant program (MDI VS + MDI FS) the first time it's needed.
    static GLuint s_waterMdiProg = 0;
    static GLuint s_perCmdSsbo   = 0;
    if (s_waterMdiProg == 0 && gpu_driven::IsWaterEnabled()) {
        // ARB_shader_draw_parameters in prefix (same pattern as static_prop batcher).
        // gl_DrawIDARB is used in the VS; the unsuffixed gl_DrawID is core only in 4.6.
        static const char* kWaterMdiPrefix =
            "#version 430\n"
            "#extension GL_ARB_shader_draw_parameters : require\n";
        glsl_program* mdi = glsl_program::makeProgram(
            "gos_terrain_water_mdi",
            "shaders/gos_terrain_water_fast_mdi.vert",
            "shaders/gos_terrain_water_mdi.frag",
            kWaterMdiPrefix);
        if (mdi && mdi->shp_) {
            s_waterMdiProg = mdi->shp_;
            printf("[WATER_MDI v1] event=prog_compiled prog=%u\n", (unsigned)s_waterMdiProg);
            fflush(stdout);
        } else {
            // Permanently disable MDI path so we don't attempt re-compile every frame.
            // Use a sentinel GLuint of UINT_MAX (not a valid GL object name).
            s_waterMdiProg = (GLuint)~0u;
            fprintf(stderr, "[WATER_MDI v1] event=prog_compile_fail — MDI path disabled\n");
            fflush(stderr);
        }
    }
    if (s_perCmdSsbo == 0 && gpu_driven::IsWaterEnabled()) {
        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &s_perCmdSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perCmdSsbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 2 * (GLsizeiptr)sizeof(WaterPerCmd), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // Save state for restore. Water is alpha-blended overlay; we set blend +
    // depth-mask off temporarily.
    GLint savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLint savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    GLint savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLint savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);

    // VAO must be bound before any glDrawArrays — AMD silently drops draws
    // when VAO is 0 (memory:projectz_overlay_findings.md). The fast path
    // runs INSIDE renderWater after Object/HUD passes that may have left
    // VAO unbound. Same fix the overlay path uses.
    GLint savedVAO = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();

    glUseProgram(prog);

    // --- One-time uniform binds. setMat4Direct = GL_FALSE (row-major direct,
    // for terrainMVP per terrain_mvp_gl_false.md: GL_FALSE + row-major
    // cancels to the right math). setMat4Std = GL_TRUE (transpose to GL
    // column-major, the canonical convention for `mvp`/`projection_` —
    // see orchestrator "GL_FALSE → GL_TRUE for thin VS projection_" entry).
    auto setMat4Direct = [&](const char* name, const float* v) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, v);
    };
    auto setMat4Std = [&](const char* name, const float* v) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, v);
    };
    auto setVec4 = [&](const char* name, const float* v) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform4fv(loc, 1, v);
    };
    auto setVec2 = [&](const char* name, float a, float b) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform2f(loc, a, b);
    };
    auto setF = [&](const char* name, float a) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform1f(loc, a);
    };
    auto setI = [&](const char* name, int a) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform1i(loc, a);
    };

    const float* wMvpWaterNonMdi =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!wMvpWaterNonMdi) wMvpWaterNonMdi = gos_GetTerrainMVPMat4();  // safety: pre-arm/first frame
    setMat4Direct("u_worldToClipGL", wMvpWaterNonMdi);
    setMat4Std   ("mvp",             (const float*)&projection_);

    // Debug-mode override gated by MC2_RENDER_WATER_FASTPATH_DEBUG=N.
    // 0 = normal, 1 = magenta solid, 2 = green, 3 = yellow.
    static int s_debugMode = -1;
    if (s_debugMode < 0) {
        const char* dm = getenv("MC2_RENDER_WATER_FASTPATH_DEBUG");
        s_debugMode = dm ? atoi(dm) : 0;
    }
    setI("debugMode", s_debugMode);
    setF   ("waterElevation",  waterElevation);
    setF   ("alphaDepth",      alphaDepth);
    // Alpha-band byte uniforms are `int` in the shader because `uniform uint`
    // crashes the project's shader_builder (memory: uniform_uint_crash.md).
    // The values are 0..255 and round-trip safely through int.
    setI   ("alphaEdgeByte",   (int)alphaEdgeByte);
    setI   ("alphaMiddleByte", (int)alphaMiddleByte);
    setI   ("alphaDeepByte",   (int)alphaDeepByte);
    setVec2("mapTopLeft",      mapTopLeftX, mapTopLeftY);
    setF   ("frameCos",        frameCos);
    setF   ("frameCosAlpha",   frameCosAlpha);
    setF   ("maxMinUV",        maxMinUV);
    setI   ("tex1",            0);

    // Fragment-shader-side fog_color + time uniforms (gos_tex_vertex.frag).
    // 2026-04-30: fog_color was hardcoded to zeros, which disables the FS
    // fog branch (`if (fog_color.x>0 || ...)`). The legacy water flush goes
    // through gosRenderMaterial::apply()→setFogColor(fog_color_), so legacy
    // gets per-pixel atmospheric mixing that softens shoreline alpha-band
    // transitions. Without fog mixing, the 3-band classifier's tile-aligned
    // staircase is fully visible at the shore.
    //
    // CINEMATIC-WATER-WHITE-1: do NOT use the cached fog_color_ here.
    // fog_color_ holds the last value written by applyRenderStates(), which
    // may be a stale sky/atmosphere white from the most-recent terrain batch.
    // By water-render time, renderLists() has finished and every render path
    // resets gos_State_Fog to 0 — so read the current render-state value
    // directly. This gives vec4(0) (fog branch disabled) and fixes the
    // "water white in cinematic" regression. (time uses the SmokeMode
    // fixed-timestep clock when active, for deterministic capture.)
    {
        const vec4 waterFog = uint32_to_vec4(renderStates_[gos_State_Fog]);
        setF   ("time", SmokeMode::fixedTimestepEnabled()
                            ? (float)SmokeMode::fixedClockSeconds()
                            : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0));
        setVec4("fog_color", (const float*)&waterFog);
    }

    // GPU-driven path: dispatch compute cull/pack + cmd-patch if enabled.
    // This MUST happen before EnsureRecipeBufferUploaded so the thin records
    // are ready and slot 6 is correctly restored to thin data (M2 fix).
    const bool gpuArmed = WaterStream::ComputeDispatchAndBindThinRecords(frameCos);

    // Ensure the recipe buffer is uploaded (idempotent, mission-static).
    GLuint recipeBuf = (GLuint)WaterStream::EnsureRecipeBufferUploaded();
    if (recipeBuf == 0) {
        glDepthMask(savedDepthMask);
        if (!savedBlend) glDisable(GL_BLEND);
        glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
        glUseProgram((GLuint)savedProgram);
        return;
    }

    // Alpha-blend overlay state.
    // 2026-04-30 shoreline fix: explicitly set depth test ON + LEQUAL.
    // The legacy water flush goes through applyRenderStates which sets these
    // from gos_State_ZCompare/gos_State_ZWrite. The bridge originally only
    // disabled depth-write, leaving depth test inheriting whatever the prior
    // pass left. Without depth test, water fragments draw over above-water
    // land tiles, producing a tile-aligned staircase at the shoreline. With
    // depth test ON + LEQUAL, land's higher Z occludes water's lower Z so
    // water only renders on actually-submerged pixels — matching the legacy
    // result where the shore looks effortlessly smooth.
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    // CINEMATIC-WATER-CULL-1: the water fast path is a flat overlay mesh and
    // must NOT be subject to the caller's backface-cull state. The SimpleCamera
    // intro/cinematic path runs default_state.SetBackFaceOn() (simplecamera.cpp),
    // leaving GL_CULL_FACE enabled with a winding that culls EVERY water
    // triangle -> water silently vanishes on the cinematic pan. (RenderDoc pixel
    // history: water frags PASSED=no FLAGS=backfaceCulled.) GameCamera gameplay
    // leaves a cull state that lets water through, which is why this only bit the
    // cinematic. Neutralize cull for the water draw and restore after — same
    // discipline as the blend / depth-mask / depth-func state this path already
    // saves+restores below.
    GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);
    // WATER-ARMED-APPLYPIPELINE-ROUTING-1: drive base FF state from the WaterArmed
    // row (AlphaBlend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, depth-test on, reverse-Z
    // GEQUAL, cull None) — byte-identical to the old hand-set (+ no-op
    // frontFace(CCW)/offset-disable). glProgramName=0 -> applyPipeline SKIPs
    // program (the glUseProgram(prog) above stays). depthMask is NOT owned here:
    // the debug-gated glDepthMask(s_waterNoDepthWrite?...) below sets it (and
    // overrides applyPipeline's depthWriteEnable=true), preserving the
    // MC2_WATER_NO_DEPTH_WRITE A/B gate. The existing save block + restore
    // epilogue still own teardown.
    //
    // APPLY-STATE-WATER-1: when MC2_FRAMEGRAPH_EXECUTOR is ON the executor APPLIES the
    // WaterArmed pipeline here (the body's live draw site, AFTER the compute dispatch at
    // :3232 — a begin-seam pre-apply would be clobbered by glUseProgram(compute)). The
    // dispatch sets waterStateAppliedByExecutor_; the body applyPipeline below is then
    // skipped (one-shot reset). Gate OFF -> flag stays false -> body applies as before ->
    // byte-identical.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::Water) != nullptr) {
        executorApplyWaterState();
    }
    if (!waterStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::WaterArmed), "WaterArmed");
    }
    waterStateAppliedByExecutor_ = false;
    render_frame_plan::trace(render_frame_plan::Phase::Water, "WaterFastPath",
        render_frame_plan::PathKind::ApplyPipeline, 1, "WaterArmed");
    // OOB-FOG-WATER-DEPTH-1: water must write depth so runFogOob() (which
    // fires on rawDepth==0 far-plane pixels) skips water-covered pixels.
    // Without depth writes, OOB fog classifies water pixels as empty sky and
    // overlays near-opaque white clouds over them.  MC2_WATER_NO_DEPTH_WRITE=1
    // restores the old broken behaviour for A/B bisect.
    static const bool s_waterNoDepthWrite = [] {
        return getenv("MC2_WATER_NO_DEPTH_WRITE") != nullptr;
    }();
    glDepthMask(s_waterNoDepthWrite ? GL_FALSE : GL_TRUE);
    // Invalidate after direct GL state set so subsequent applyRenderStates()
    // calls between here and the draw cannot short-circuit on stale cache.
    invalidateRenderStateCache();

    // Install a REPEAT/LINEAR sampler on unit 0 for the water draw. The
    // patch-stream's bucket sampler (CLAMP_TO_EDGE) may still be bound from
    // the prior terrain pass; CLAMP would reduce the world-scale water UVs
    // (0..MaxMinUV ≈ 0..8) to texture-edge samples, making water invisible.
    // The legacy water path runs gos_State_TextureAddress = gos_TextureWrap,
    // matching REPEAT.
    static GLuint s_waterFastSampler = 0;
    if (s_waterFastSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- water-fast sampler object (REPEAT+LINEAR)
        glGenSamplers(1, &s_waterFastSampler);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    GLuint savedSampler = 0;
    {
        GLint q = 0;
        glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q);
        savedSampler = (GLuint)q;
    }
    glBindSampler(0, s_waterFastSampler);

    GLuint baseTex   = (GLuint)gos_terrain_bridge_glTextureForGosHandle(waterGosHandle);
    GLuint detailTex = (waterDetailGosHandle != 0xffffffffu)
                       ? (GLuint)gos_terrain_bridge_glTextureForGosHandle(waterDetailGosHandle)
                       : 0u;

    // Determine whether MDI path is fully operational.
    const bool mdiValid = gpuArmed
                          && s_waterMdiProg != 0 && s_waterMdiProg != (GLuint)~0u
                          && s_perCmdSsbo  != 0
                          && baseTex       != 0;

    if (mdiValid) {
        // ─────────────────────────────────────────────────────────────
        // GPU-driven MDI path
        // Thin records: already cull-packed by compute + bound to slot 6
        // by ComputeDispatchAndBindThinRecords() (M2 fix).
        // ─────────────────────────────────────────────────────────────

        // Atlas accessors: defined only in gos_terrain_indirect.cpp (no header).
        // Precedent: sibling terrain-solid functions declare identical extern blocks
        // (gameos_graphics.cpp:2584-2588, :2877-2881, :3100).
        extern GLuint gos_terrain_indirect_getAtlasGLTex();
        extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
        extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
        extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();

        // Upload per-draw data (base + detail).
        WaterPerCmd cmds[2];
        cmds[0] = { 0u, 1, 0, oneOverTF,      cloudOffsetX, cloudOffsetY, 0u, 0u };
        cmds[1] = { 1u, 2, 1, oneOverWaterTF, sprayOffsetX, sprayOffsetY, 0u, 0u };
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perCmdSsbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(cmds), cmds);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // Switch to MDI program.
        glUseProgram(s_waterMdiProg);

        // Re-define lambdas that target s_waterMdiProg.
        auto setMF = [&](const char* name, float a) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform1f(loc, a);
        };
        auto setMI = [&](const char* name, int a) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform1i(loc, a);
        };
        auto setMMat4Direct = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, v);
        };
        auto setMMat4Std = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, v);
        };
        auto setMVec4 = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform4fv(loc, 1, v);
        };
        auto setMVec2 = [&](const char* name, float a, float b) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform2f(loc, a, b);
        };
        auto setMVec3 = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform3fv(loc, 1, v);
        };

        const float* wMvpWaterMdi =
            gos_terrain_indirect::IsFrameSolidArmed()
                ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK  // TERRAIN-PHASE-RAW-MVP-OK
                : gos_GetTerrainMVPMat4();
        if (!wMvpWaterMdi) wMvpWaterMdi = gos_GetTerrainMVPMat4();  // safety: pre-arm/first frame
        setMMat4Direct("u_worldToClipGL", wMvpWaterMdi);
        setMMat4Std   ("mvp",             (const float*)&projection_);
        setMI         ("debugMode",       s_debugMode);
        setMI         ("u_waterDebugMode", gos_GetWaterFsDebugMode());  // WATER-DEBUG-VIEWS-1 (FS material-space)
        // WATER-TUNING-UI-1: runtime MDI water material params (default-seeded
        // to the former consts -> byte-identical default; live via Graphics
        // Options > Water). loc<0 (uniform optimized out) is silently skipped.
        setMF         ("ABSORPTION_DENSITY", g_waterAbsorptionDensity);
        setMF         ("WATER_MAX_ALPHA",    g_waterMaxAlpha);
        setMF         ("RIPPLE_GAIN",        g_waterRippleGain);
        setMF         ("GLINT_GAIN",         g_waterGlintGain);
        setMVec3      ("DEEP_COLOR",         g_waterDeepColor);
        setMVec3      ("SHALLOW_COLOR",      g_waterShallowColor);
        // WATER-VISUAL-FIRST-SLICE: gated sky tint (strength 0 default = no-op).
        setMF         ("u_waterSkyTintStrength", gos_GetWaterSkyTintStrength());
        setMVec3      ("u_waterSkyTintColor",    g_waterSkyTintColor);
        // WATER-SKY-REFLECTION-1: gated SH-L2 sky reflection (strength 0 = no-op).
        setMF         ("u_waterReflStrength",    gos_GetWaterReflStrength());
        setMF         ("waterElevation",  waterElevation);
        setMF         ("alphaDepth",      alphaDepth);
        setMI         ("alphaEdgeByte",   (int)alphaEdgeByte);
        setMI         ("alphaMiddleByte", (int)alphaMiddleByte);
        setMI         ("alphaDeepByte",   (int)alphaDeepByte);
        setMVec2      ("mapTopLeft",      mapTopLeftX, mapTopLeftY);
        setMF         ("frameCos",        frameCos);
        setMF         ("frameCosAlpha",   frameCosAlpha);
        setMF         ("maxMinUV",        maxMinUV);
        // WATER-EDGE-FEATHER-1: dissolve the hard map-edge water rim into the
        // fog'd horizon (companion to TERRAIN-EDGE-FEATHER-1). Gate
        // MC2_WATER_EDGE_FEATHER default-OFF -> upload 0 -> frag block skipped
        // (byte-identical). Strength MC2_WATER_EDGE_FEATHER_STRENGTH (default 1.0).
        // halfMap = the canonical WORLD-unit half-extent (gosPostProcess::
        // getMapHalfExtent), the SAME value the legacy terrain edge-haze + post-fx
        // edge_fog use against WorldPos.xy (raw MC2 world units). Origin-centered.
        {
            static const int s_waterEdgeFeather = []() {
                const char* e = getenv("MC2_WATER_EDGE_FEATHER");
                return (e && e[0] && e[0] != '0') ? 1 : 0;
            }();
            static const float s_waterEdgeFeatherStr = []() {
                const char* s = getenv("MC2_WATER_EDGE_FEATHER_STRENGTH");
                float v = (s ? (float)atof(s) : 0.0f);
                return v > 0.0f ? v : 1.0f;
            }();
            gosPostProcess* ppW = getGosPostProcess();
            setMI("u_waterEdgeFeather",         s_waterEdgeFeather);
            setMF("u_waterEdgeFeatherStrength", s_waterEdgeFeatherStr);
            setMF("u_halfMap",                  ppW ? ppW->getMapHalfExtent() : 0.0f);
        }
        // CINEMATIC-WATER-WHITE-1: same fix as non-MDI path above — read current
        // render state, not stale fog_color_. See comment block above. (time uses
        // the SmokeMode fixed-timestep clock when active, for deterministic capture.)
        {
            const vec4 waterFog = uint32_to_vec4(renderStates_[gos_State_Fog]);
            setMF         ("time",  SmokeMode::fixedTimestepEnabled()
                                        ? (float)SmokeMode::fixedClockSeconds()
                                        : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0));
            setMVec4("fog_color", (const float*)&waterFog);
        }
        // WATER reflection frame fix: terrain_camera_pos_ is the Stuff/MLR eye
        // (.x=left, .y=elevation, .z=forward); the water FS consumes cameraPos in
        // RAW MC2 (matching WorldPos: .x=east, .y=north, .z=up). Apply the
        // documented Stuff->MC2 swap MC2=(-Stuff.x, Stuff.z, Stuff.y) here so the
        // reflect vdir + Fresnel + waveLOD respond to camera PITCH/zoom, not just
        // pan. (Other terrain/lighting consumers swap shader-side; water did not.)
        const float camMC2[4] = { -terrain_camera_pos_.x, terrain_camera_pos_.z,
                                   terrain_camera_pos_.y,  1.0f };
        setMVec4      ("cameraPos", camMC2);  // water-v1 reflection/Fresnel (raw MC2)
        // WATER-SUN-SPEC-1: upload terrainLightDir RAW MC2 (no camMC2 swap).
        // The camMC2 swap above is for cameraPos only (Stuff->MC2 frame fix).
        // Light dir is already raw MC2 from gamecam.cpp:314 ("NOT swizzled — MC2 Z-up");
        // terrain uploads it raw at :6055/6177/6354. Applying the swap would double-
        // transform -> sun on wrong axis. The shader uses it directly in MC2 Z-up.
        setMVec4      ("terrainLightDir", (const float*)&terrain_light_dir_);
        setMF         ("u_waterSpecIntensity", gos_GetWaterSpecIntensity());
        setMI         ("tex1",  0);
        setMI         ("tex2",  1);

        // WATER-REFLECTION-SAMPLE-1: bind the terrain reflection RT (filled by
        // the Phase C1 RenderWaterReflectionPass) to unit 2 and upload the
        // sample uniforms. Replaces the old dead terrain-colormap atlas bind.
        // u_waterRtStrength 0 (gate MC2_WATER_REFLECTION_RT off) -> the FS skips
        // the RT sample and falls back to the SH sky -> byte-identical to Phase A.
        gosPostProcess* ppRefl = getGosPostProcess();
        GLuint reflRtTex = ppRefl ? (GLuint)ppRefl->getWaterReflectionTexture() : 0u;
        const bool bindRefl = (reflRtTex != 0u);
        setMI         ("u_waterReflRT",     2);
        setMF         ("u_waterRtStrength", gos_GetWaterRtStrength());
        setMVec2      ("u_waterScreenSize",
                       ppRefl ? (float)ppRefl->getWidth()  : 1.0f,
                       ppRefl ? (float)ppRefl->getHeight() : 1.0f);

        // WATER-HDRI-REFL-1: bind HDRI equirect on unit 3 for direct specular
        // sampling (sun disk + sky color, LOD-filtered via mipmaps added in gos_hdri.cpp).
        // u_waterHdriLod < 0 signals no HDRI -> FS falls back to SH-L2.
        // MC2_WATER_HDRI_LOD env (float) overrides the default.
        //
        // WATER-HDRI-REFL-PERF-1: the per-fragment HDRI path (waterEvalHdri:
        // atan2+asin + textureLod against the live equirect) measured ~10ms/frame
        // GPU at LOD 1.0 (2K mip of the 4K source) across a full-map water quad
        // (451c9e49). The HDRI reflection is a heavily blurred, low-frequency sky
        // read by design (Reinhard-toned broad color, not a sharp sun disk) — a
        // much coarser mip costs the same ALU per fragment but a fraction of the
        // texture bandwidth/cache pressure, with no visible quality loss at the
        // gameplay camera. Default bumped 1.0 -> 4.0 (256x128 mip of a 4K source;
        // recon's own "keep-cheap" recommendation). MC2_WATER_HDRI_REFL_FULL=1
        // restores the old full-rate LOD 1.0 for A/B or regression checks.
        {
            // LOD env resolve hoisted to mc2WaterHdriLodDefault() (file scope) so
            // the RUNG-6.5 VK island fsParams pack shares the exact same value.
            const bool hdriAvail = (ppRefl && ppRefl->isHdriReady());
            setMI("u_hdri", 3);
            setMF("u_skyYaw", hdriAvail ? ppRefl->getSkyYaw() : 0.0f);
            setMF("u_waterHdriLod", hdriAvail ? mc2WaterHdriLodDefault() : -1.0f);
        }

        // Bind SSBOs.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, recipeBuf);
        // Slot 6 (thin records) was already bound by ComputeDispatchAndBindThinRecords.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_perCmdSsbo);

        // Bind textures: base to unit 0, detail (or base) to unit 1.
        // Save+restore the unit-1 sampler: prior passes (e.g. patch-stream bucket)
        // may have left CLAMP_TO_EDGE on unit 1. The detail UV range (0..MaxMinUV ≈
        // 0..8) would collapse to edge-texel smear under CLAMP. Bind s_waterFastSampler
        // (REPEAT/LINEAR) for the duration of the MDI draw, then restore.
        GLuint savedSampler1 = 0;
        {
            GLint q = 0;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &q);
            savedSampler1 = (GLuint)q;
        }
        glBindSampler(1, s_waterFastSampler);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, baseTex);
        glActiveTexture(GL_TEXTURE1);
        GLuint detailOrBase = (detailTex != 0) ? detailTex : baseTex;
        glBindTexture(GL_TEXTURE_2D, detailOrBase);
        glActiveTexture(GL_TEXTURE0);

        // WATER-REFLECTION-SAMPLE-1: bind the reflection RT on unit 2 (mirrors the
        // unit-1 save/restore idiom). When the RT is unavailable, unit 2 is left
        // untouched; the FS only samples it when u_waterRtStrength>0 (gate ON).
        GLuint savedSampler2 = 0;
        if (bindRefl) {
            GLint q = 0;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 2, &q);
            savedSampler2 = (GLuint)q;
            glBindSampler(2, 0);   // use the RT texture's own LINEAR/CLAMP_TO_EDGE params
            glActiveTexture(GL_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, reflRtTex);
            glActiveTexture(GL_TEXTURE0);
        }

        // WATER-HDRI-REFL-1: bind HDRI equirect on unit 3 (with mipmaps for LOD).
        const bool bindHdri = (ppRefl && ppRefl->isHdriReady() && ppRefl->getHdriTex() != 0u);
        if (bindHdri) {
            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, ppRefl->getHdriTex());
            glActiveTexture(GL_TEXTURE0);
        }

        // VK-WATER-TIER1: when the shared-GBuffer water island is armed, render the
        // BASE layer in Vulkan in-place and skip the GL MDI draw. Feed remains the
        // CPU-visible thin-record/readback path used by the island. On any fail-soft
        // (returns 0), fall through to the GL draw. Gate OFF -> GL MDI draw as before
        // (byte-identical).
        bool vkWaterHandled = false;
#ifdef MC2_VULKAN_ISLAND
        if (mc2_vk_water_island_enabled()) {
            // Feed (STEP 0 revised): read back the thin records the armed GPU compute
            // ACTUALLY packed this frame (ground truth) — the CPU pack yields 0 on the
            // full-recipe-authoritative path (no CPU projection state). ReadbackGpuThinForVk
            // reads the bucket-header count + the packed ring slot into a CPU copy after
            // the compute's final barrier. No fenced GL buffer crosses into VK.
            const void*    vkThinPtr   = nullptr;
            // MC2_VK_WATER_PERF: time the GL thin-buffer readback (2x glGetBufferSubData
            // = blocking sync on the GL water cull/pack compute). Reported alongside the
            // island run()'s [VK_WATER_PERF] line so the full per-frame cost is visible.
            static int s_vkWaterPerf = -1;
            if (s_vkWaterPerf == -1) {
                const char* e = std::getenv("MC2_VK_WATER_PERF");
                s_vkWaterPerf = (e && e[0] == '1') ? 1 : 0;
            }
            const auto tThinRb0 = std::chrono::steady_clock::now();
            const uint32_t vkThinCount = WaterStream::ReadbackGpuThinForVk(&vkThinPtr);
            if (s_vkWaterPerf) {
                const double us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - tThinRb0).count();
                std::fprintf(stderr, "[VK_WATER_PERF] thin_readback_us=%.1f thin=%u\n",
                             us, vkThinCount);
                std::fflush(stderr);
            }
            const void*    vkRecipes   = (const void*)WaterStream::GetRecipes();
            const uint32_t vkRecipeN   = WaterStream::GetRecipeCount();
            gosPostProcess* ppVk = getGosPostProcess();
            if (vkThinCount > 0 && vkThinPtr && vkRecipes && vkRecipeN > 0 && ppVk) {
                // Pack VS/FS UBO params LOCKSTEP with water_geom.{vert,frag}.
                struct VkWaterVsP { float m[16]; float p0[4],p1[4],p2[4],p3[4]; } vsP{};
                struct VkWaterFsP { float fog[4],f0[4],f1[4],f2[4],f3[4],deep[4],shallow[4]; } fsP{};
                memcpy(vsP.m, wMvpWaterMdi, 16 * sizeof(float));
                vsP.p0[0]=waterElevation; vsP.p0[1]=alphaDepth; vsP.p0[2]=mapTopLeftX; vsP.p0[3]=mapTopLeftY;
                vsP.p1[0]=frameCos; vsP.p1[1]=maxMinUV; vsP.p1[2]=oneOverTF; vsP.p1[3]=cloudOffsetX;
                // WATER_DEPTH_FUDGE_FAST — LOCKSTEP with mclib/terrain_depth_bias.h
                // (mc2depth::WATER_DEPTH_FUDGE_FAST) + shaders/include/terrain_depth_bias.hglsl
                // (WATER_DEPTH_FUDGE_FAST). The VS applies clip.z += this * clip.w.
                vsP.p2[0]=cloudOffsetY; vsP.p2[1]=0.00025f;
                vsP.p2[2]=(float)alphaEdgeByte; vsP.p2[3]=(float)alphaMiddleByte;
                vsP.p3[0]=(float)alphaDeepByte;
                {
                    const vec4 waterFog = uint32_to_vec4(renderStates_[gos_State_Fog]);
                    memcpy(fsP.fog, &waterFog, 4*sizeof(float));
                }
                fsP.f0[0]= SmokeMode::fixedTimestepEnabled()
                             ? (float)SmokeMode::fixedClockSeconds()
                             : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0);
                fsP.f0[1]=camMC2[0]; fsP.f0[2]=camMC2[1]; fsP.f0[3]=camMC2[2];
                fsP.f1[0]=alphaDepth; fsP.f1[1]=g_waterAbsorptionDensity;
                fsP.f1[2]=g_waterMaxAlpha; fsP.f1[3]=g_waterRippleGain;
                fsP.f2[0]=g_waterGlintGain;
                // RUNG-6.5 WATER-REFLECTION PORT: feed the reflection inputs with
                // the EXACT values the GL MDI bind block uploads (gate parity:
                // MC2_WATER_REFLECTION=0 -> strength 0.0 -> FS block skipped both
                // sides; !isHdriReady -> lod -1 -> SH-L2 fallback both sides).
                {
                    const bool vkHdriAvail = ppVk->isHdriReady();
                    fsP.f2[1] = gos_GetWaterReflStrength();                       // u_waterReflStrength
                    fsP.f2[2] = vkHdriAvail ? ppVk->getSkyYaw() : 0.0f;           // u_skyYaw
                    fsP.f2[3] = vkHdriAvail ? mc2WaterHdriLodDefault() : -1.0f;   // u_waterHdriLod
                    fsP.f3[0] = (float)ppVk->getWidth();                          // u_waterScreenSize
                    fsP.f3[1] = (float)ppVk->getHeight();
                }
                memcpy(fsP.deep,    g_waterDeepColor,    3*sizeof(float));
                memcpy(fsP.shallow, g_waterShallowColor, 3*sizeof(float));
                vkWaterHandled = (1 == mc2_vk_water_island_run(
                    (unsigned int)ppVk->getSceneColorTexture(),
                    (unsigned int)ppVk->getSceneDepthTexture(),
                    ppVk->getWidth(), ppVk->getHeight(),
                    vkThinPtr, vkThinCount, vkRecipes, vkRecipeN, &vsP, &fsP,
                    ppVk->isHdriReady() ? (unsigned int)ppVk->getHdriTex() : 0u));
            }
        }
#endif

        // MDI: 2 draws (base + detail); 1 if detail not present.
        const GLsizei drawCount = (detailTex != 0) ? 2 : 1;
        if (!vkWaterHandled) {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, WaterStream::GetIndirectCmdBuffer());
            glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, drawCount, 0);
            mdi_submit::trace("WaterFastPath", "MultiDrawArraysIndirect", (int)drawCount,
                              "GPU", "WaterStreamIndirect", false);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        }

        // Restore unit 2 — mirror unit-1 force-clear + sampler restore.
        if (bindRefl) {
            glActiveTexture(GL_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, 0);    // force-clear; mirrors unit-1 post-draw
            glActiveTexture(GL_TEXTURE0);
            glBindSampler(2, savedSampler2);    // restore sampler only
        }

        // Restore unit 3 — WATER-HDRI-REFL-1: unbind HDRI.
        if (bindHdri) {
            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
        }

        // Restore unit 1 — don't leave a texture bound on unit 1 for the legacy path.
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(1, savedSampler1);

        // Parity check: when MC2_GPU_DRIVEN_PARITY=1, runs CPU pack and compares
        // against the GPU compute output byte-for-byte. No-op when disabled.
        WaterStream::ComputeDispatchParity_Check();

        // Restore GL state.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, 0);
        glBindSampler(0, savedSampler);
        glDepthMask(savedDepthMask);
        glDepthFunc((GLenum)savedDepthFunc);
        if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
        if (savedCullFace) glEnable(GL_CULL_FACE);
        if (!savedBlend) glDisable(GL_BLEND);
        glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
        glUseProgram((GLuint)savedProgram);
        glBindVertexArray((GLuint)savedVAO);
    } else {
        // ─────────────────────────────────────────────────────────────
        // Legacy CPU-pack path (unchanged)
        // ─────────────────────────────────────────────────────────────
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, recipeBuf);
        const uint32_t thinCount = WaterStream::UploadAndBindThinRecords();
        if (thinCount == 0) {
            glBindSampler(0, savedSampler);
            glDepthMask(savedDepthMask);
            glDepthFunc((GLenum)savedDepthFunc);
            if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
            if (savedCullFace) glEnable(GL_CULL_FACE);
            if (!savedBlend) glDisable(GL_BLEND);
            glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
            glUseProgram((GLuint)savedProgram);
            glBindVertexArray((GLuint)savedVAO);
            return;
        }

        // One-time diagnostic for the fast path.
        static bool s_fastDiagPrinted = false;
        if (!s_fastDiagPrinted) {
            s_fastDiagPrinted = true;
            while (glGetError() != GL_NO_ERROR) {}
            GLint curVAO=0, curFBO=0, curVP[4]={0};
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curVAO);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curFBO);
            glGetIntegerv(GL_VIEWPORT, curVP);
            GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
            GLboolean cullFace  = glIsEnabled(GL_CULL_FACE);
            GLint curDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &curDepthFunc);
            fprintf(stderr,
                    "[WATER_FAST v1] event=first_draw prog=%u recipeBuf=%u "
                    "thin=%u baseGosH=%u baseGLTex=%u detailGosH=%u detailGLTex=%u "
                    "VAO=%d FBO=%d viewport=[%d,%d,%d,%d] depthTest=%d cullFace=%d depthFunc=0x%x\n",
                    (unsigned)prog, (unsigned)recipeBuf,
                    (unsigned)thinCount,
                    (unsigned)waterGosHandle, (unsigned)baseTex,
                    (unsigned)waterDetailGosHandle, (unsigned)detailTex,
                    curVAO, curFBO, curVP[0], curVP[1], curVP[2], curVP[3],
                    depthTest, cullFace, (unsigned)curDepthFunc);
            fflush(stderr);
        }

        const GLsizei drawVerts = (GLsizei)(thinCount * 6u);

        // --- Base water layer ---
        if (baseTex != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, baseTex);

            setI("isWater",    1);
            setI("detailMode", 0);
            setF("uvScale",    oneOverTF);
            setVec2("uvOffset", cloudOffsetX, cloudOffsetY);

            glDrawArrays(GL_TRIANGLES, 0, drawVerts);
            // GL error diagnostic.
            {
                GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    static bool s_errPrinted = false;
                    if (!s_errPrinted) {
                        s_errPrinted = true;
                        fprintf(stderr, "[WATER_FAST v1] event=base_draw_gl_err err=0x%x verts=%d\n",
                                (unsigned)err, (int)drawVerts);
                        fflush(stderr);
                    }
                }
            }
        }

        // --- Detail/spray layer ---
        if (detailTex != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, detailTex);

            setI("isWater",    2);
            setI("detailMode", 1);
            setF("uvScale",    oneOverWaterTF);
            setVec2("uvOffset", sprayOffsetX, sprayOffsetY);

            glDrawArrays(GL_TRIANGLES, 0, drawVerts);
        }

        // Restore GL state.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
        glBindSampler(0, savedSampler);
        glDepthMask(savedDepthMask);
        glDepthFunc((GLenum)savedDepthFunc);
        if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
        if (savedCullFace) glEnable(GL_CULL_FACE);
        if (!savedBlend) glDisable(GL_BLEND);
        glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
        glUseProgram((GLuint)savedProgram);
        glBindVertexArray((GLuint)savedVAO);
    }

    // WATER-THINRING-FENCE-1: fence the thin-record ring slot just drawn (both the
    // armed-MDI and legacy-CPU branches reach here after their draw). The two
    // early-returns above (recipeBuf==0, legacy thinCount==0) intentionally skip
    // this — no draw consumed the slot in those cases.
    WaterStream::EndThinRingFrameFence();

    // RENDER_STATES v1: water fast path bound textures directly on unit 0; the
    // applyRenderStates cache is now stale. Force a full re-apply on next call.
    invalidateRenderStateCache();
}

// Probe 8: forward decls for cross-TU MVP fingerprint accessors (defined in
// gos_terrain_indirect.cpp inside its extern "C" block).
extern "C" uint32_t gos_terrain_indirect_getDispatchMvpFp();
extern "C" uint64_t gos_terrain_indirect_getDispatchMvpFrameIdx();
extern "C" void     gos_terrain_indirect_getDispatchMvpFloats4(float out[4]);
// gos_terrain_indirect_getDispatchMvp16 and gos_GetTerrainMVPMat4 are declared
// TU-wide near the top of this file (after g_terrainMaterialProfile) so that
// renderWaterFastPath (which precedes this block) can see them.

// ──────────────────────────────────────────────────────────────────────────
// [TERRAIN_SURFACE] PR-2 — continuous indexed-surface validation draw bridge.
//
// Plan : docs/superpowers/plans/.../terrain-continuous-surface-producer-plan.md
//        PR-2 (Wave 1, ADDITIVE / DEFAULT-OFF / DELETES NOTHING). Behind the
//        MC2_TERRAIN_SURFACE path-select kill-switch (gos_terrain_surface::
//        IsEnabled()); a no-op when OFF (behaviour-neutral). When ON it draws
//        the surface ON TOP of the still-running legacy/indirect path purely
//        for visual validation of the V-ssbo VS + Fork D reverse-Z bias --
//        PR-2 lands NO deletion and NO legacy kill site (that is PR-4).
//
// Fork V = V-ssbo (RULED): the surface vertex SSBO (binding 11) + the baked
// mission-static index SSBO (binding 12) are uploaded once per generation
// epoch (gos_terrain_surface::GetGenerationEpoch()); the VS pulls
// surfaceVerts[ surfaceIndices[gl_VertexID] ]. NO IBO / VAO element-array
// state (memory/element_array_buffer_is_vao_state_new_draw_paths_own_their_vao.md).
//
// Screen-agnostic (design Convergence C-1): the draw does NOT test
// IsFrameSolidArmed() -- surface EXISTENCE is decoupled from arming. PR-2
// draws at a single (fixed) LOD with draw-all visibility on BOTH armed and
// unarmed frames; the per-frame band/visibility refinement is PR-3 scope.
//
// State save/restore + AMD VAO-0 / attr-0 mitigations mirror
// gos_terrain_bridge_drawIndirect exactly (the proven pattern in this file).
//
void gos_terrain_surface_bridge_draw()
{
    if (!gos_terrain_surface::IsEnabled())   return;   // kill-switch OFF: no-op
    if (!gos_terrain_surface::IsGenerated()) return;   // nothing generated yet
    if (!g_gos_renderer)                     return;

    glsl_program* p = g_gos_renderer->getTerrainSurfaceProgram();
    if (!p || !p->shp_) return;   // compile failed (logged once at init)
    const GLuint prog = p->shp_;

    const uint32_t indexCount = gos_terrain_surface::GetIndexCount();
    const uint32_t vertCount  = gos_terrain_surface::GetVertexCount();
    if (indexCount == 0u || vertCount == 0u) return;

    ZoneScopedN("Terrain::SurfaceValidationDraw");
    TracyGpuZone("Terrain::SurfaceValidationDraw");

    // ---- Lazy GPU upload, epoch-tracked (re-upload on regeneration) --------
    // s_surfaceVB/IB: mission-static vertex + finest index (PR-1/PR-2).
    // s_surfaceTB: mission-static per-tile material table (META-FIX). All
    // mission-static; the topology/VB never changes -- the SSE/meshlet
    // drop-in invariant (design 3.4). Band-LOD per-frame buffers deleted in
    // the 2026-05-19 STOP+HARVEST (uniform finest, no per-frame compute).
    static GLuint   s_surfaceVB     = 0;
    static GLuint   s_surfaceIB     = 0;
    static GLuint   s_surfaceTB     = 0;   // META-FIX: per-tile material SSBO
    static uint32_t s_uploadedEpoch = 0;
    const uint32_t  epoch           = gos_terrain_surface::GetGenerationEpoch();
    // surface bridge behind IsEnabled() kill-switch; s_surfaceVB/IB/TB
    // SSBO-bound at slots 20/21/22 for the meshlet-validation draw only.
    // TIER2-EXCLUDED: substrate-gated
    if (s_surfaceVB == 0) glGenBuffers(1, &s_surfaceVB);
    // TIER2-EXCLUDED: substrate-gated
    if (s_surfaceIB == 0) glGenBuffers(1, &s_surfaceIB);
    // TIER2-EXCLUDED: substrate-gated
    if (s_surfaceTB == 0) glGenBuffers(1, &s_surfaceTB);
    if (epoch != s_uploadedEpoch) {
        const void* vd = gos_terrain_surface::GetVertexData();
        const void* id = gos_terrain_surface::GetIndexData();
        const void* td = gos_terrain_surface::GetTileData();
        if (!vd || !id || !td) return;
        const uint32_t tileCount = gos_terrain_surface::GetTileCount();
        // 32 B / TerrainSurfaceVertex (std430 vec4[2]); 4 B / uint32 index;
        // 32 B / TerrainSurfaceTile (std430 uvec4 wp + vec4 uvExt).
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_surfaceVB);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)vertCount * 32, vd, GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_surfaceIB);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)indexCount * 4, id, GL_STATIC_DRAW);
        // META-FIX: mission-static per-tile material table. Row-major-keyed
        // (mx + my*cells), uploaded byte-identical to the dense recipe; the
        // surface VS recovers the row-major key from the cell's top-left
        // vertexNum so it is independent of the ADJUST-1 block-clustered
        // index emission order.
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_surfaceTB);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)tileCount * 32, td, GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_uploadedEpoch = epoch;
        TS_TRACE("event=ssbo_upload epoch=%u verts=%u indices=%u vb=%u ib=%u "
                 "tb=%u",
                 epoch, vertCount, indexCount,
                 (unsigned)s_surfaceVB, (unsigned)s_surfaceIB,
                 (unsigned)s_surfaceTB);
    }

    // ---- Save state (mirrors gos_terrain_bridge_drawIndirect) -------------
    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,    &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,      &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,      &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,    &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,         &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&savedVAO);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK,     savedColorMask);
    GLuint    savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- VAO rebind (AMD VAO-0 trap) + attr-0 (AMD attribute-0 trap) ------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms (reuse the thin binder: it sets mvp /
    //      camera / shadows / tex1 / matNormal* / atlas
    //      uniforms for the override program). terrainMVP IS declared by the
    //      surface VS (it projects world->clip itself, unlike the thin VS),
    //      so the binder's terrainMVP upload is consumed here. ----
    g_gos_renderer->terrainBindThinUniformsForPatchStream(p);

    // ---- Depth + color state for opaque terrain (inherit scene reverse-Z;
    //      do NOT set glClearDepth; design Section 4.4). GL_GEQUAL is the
    //      scene-global reverse-Z compare. ----
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);

    // ---- Sampler 0: CLAMP_TO_EDGE / LINEAR (matches indirect atlas path) --
    static GLuint s_surfaceSampler = 0;
    if (s_surfaceSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- surface sampler object (CLAMP+LINEAR)
        glGenSamplers(1, &s_surfaceSampler);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_surfaceSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_surfaceSampler);

    // ---- Bind merged colormap atlas at unit 0; per-fragment WorldPos->tile
    //      material model (design Section 2 NC1: useAtlasColormap=1, the frag
    //      reconstructs atlas-absolute UV from WorldPos). useCementAtlas=0 so
    //      the frag never dereferences the binding-2 thin SSBO (PR-2 does not
    //      bind one; the cement-atlas path is unchanged legacy scope). ----
    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    extern float  gos_terrain_indirect_getNumTexturesAcross();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
    extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();

    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
    {
        const GLint locNTA  = glGetUniformLocation(prog, "atlasNumTexturesAcross");
        const GLint locMTX  = glGetUniformLocation(prog, "atlasMapTopLeftX");
        const GLint locMTY  = glGetUniformLocation(prog, "atlasMapTopLeftY");
        const GLint locOOWS = glGetUniformLocation(prog, "atlasOneOverWorldUnits");
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        const GLint locUCA  = glGetUniformLocation(prog, "useCementAtlas");
        if (locNTA  >= 0) glUniform1f(locNTA,  gos_terrain_indirect_getNumTexturesAcross());
        if (locMTX  >= 0) glUniform1f(locMTX,  gos_terrain_indirect_getAtlasMapTopLeftX());
        if (locMTY  >= 0) glUniform1f(locMTY,  gos_terrain_indirect_getAtlasMapTopLeftY());
        if (locOOWS >= 0) glUniform1f(locOOWS, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        if (locUAC  >= 0) glUniform1i(locUAC,  1);
        if (locUCA  >= 0) glUniform1i(locUCA,  0);
    }

    // ---- Bind the mission-static vertex SSBO at binding 20 (the VS always
    //      pulls surfaceVerts[surfaceIndices[gl_VertexID]] from here). High
    //      bindings (20-24) chosen clear of the live thin path (1=recipe,
    //      2=thin) and compute-cull (11/12) so this additive PR-3 draw never
    //      disturbs the armed indirect path.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, s_surfaceVB);

    // ---- META-FIX 2026-05-19: bind the mission-static per-tile material SSBO
    //      at binding 22 so the surface VS can forward the REAL Texcoord /
    //      TerrainType varyings (replacing the PR-2 placeholders that broke
    //      the detail / cement frag layer close-up). The VS recovers the
    //      row-major tile key from the cell's top-left vertexNum via
    //      u_mapSide, so the lookup is correct under the ADJUST-1
    //      block-clustered index emission order.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, s_surfaceTB);
    {
        const GLint locMS = glGetUniformLocation(prog, "u_mapSide");
        if (locMS >= 0)
            glUniform1i(locMS, (GLint)gos_terrain_surface::GetMapSide());
    }

    // ── Screen-agnostic surface draw (band-LOD deferred, user-ruled
    //    2026-05-19 STOP+HARVEST per the 2026-05-19 architecture
    //    re-evaluation): draw the PR-1 mission-static FINEST index buffer
    //    directly every frame. Uniform finest => zero band-boundary artifact
    //    class, tessLevel>=1, draw-all (over-draws, never distance-culls --
    //    correctness-safe re the long-sightline invariant). The discrete
    //    band-LOD subsystem is DELETED, not parked; future LOD is a
    //    re-chartered NS2 slice. Surface EXISTS every frame regardless of
    //    arming; no C-1 *repoint* here (legacy + Fix-B retained).
    const bool armed = gos_terrain_indirect::IsFrameSolidArmed();

    // Unconditional PR-1 mission-static FINEST index-buffer draw
    // (binding 21 = s_surfaceIB). Band-LOD deleted (not parked) per the
    // STOP+HARVEST; uniform finest => zero band-boundary artifact class,
    // tessLevel>=1, draw-all. Surface EXISTS every frame regardless of
    // arming; arming no longer selects a path here.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 21, s_surfaceIB);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)indexCount);
    // first_draw lifecycle print -- once per process (coarse, never per-quad).
    static bool s_firstSurfaceDrawPrinted = false;
    if (!s_firstSurfaceDrawPrinted) {
        s_firstSurfaceDrawPrinted = true;
        TS_TRACE("event=first_surface_draw verts=%u indices=%u "
                 "armed=%d screen_agnostic=1 fork_v=ssbo "
                 "lod=uniform_finest fork_d=clip_pre_divide",
                 vertCount, indexCount, (int)armed);
    }

    // ---- Reset shared-frag flags so legacy/indirect path doesn't inherit --
    {
        const GLint locUAC = glGetUniformLocation(prog, "useAtlasColormap");
        if (locUAC >= 0) glUniform1i(locUAC, 0);
    }

    // ---- Restore state ----------------------------------------------------
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 20, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 21, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 22, 0);  // META-FIX tile SSBO
    glBindSampler(0, savedSampler);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1],
                savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    else                 glEnable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND);
    else            glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
}

// ──────────────────────────────────────────────────────────────────────────
// Terrain indirect draw bridge (Stage 3 of indirect-terrain SOLID PR1)
//
// Called by gos_terrain_indirect::DrawIndirect() after preflight arming.
// Mirrors gos_terrain_bridge_renderWaterFast structure but for opaque terrain:
//  - Depth writes ON (terrain is the primary depth source)
//  - CLAMP_TO_EDGE / LINEAR sampler (atlas-tiled, matches M2 path)
//  - ColorMask save/restore (M5: shadow pass leaves it FALSE)
//  - AMD attr-0: glEnableVertexAttribArray(0) after VAO rebind
//  - glMultiDrawArraysIndirect — NO EBO (thin VS reads SSBO via gl_VertexID)
//
// State save/restore covers: Program, Blend enable, BlendSrcDstRGB, DepthMask,
// DepthTest, DepthFunc, VAO, ColorMask, Sampler[0].
// Returns false only if the thin program is not available.
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawIndirect(int cmdCount, unsigned int recipeSSBO,
                                     unsigned int thinRecordSSBO,
                                     unsigned int indirectCmdBuffer)
{
    ZoneScopedN("Terrain::IndirectDraw");
    TracyGpuZone("Terrain::IndirectDraw");
    if (!g_gos_renderer) return false;
    // Honour the ImGui "Terrain Draw" toggle on the GPU-driven path too.
    if (!g_gos_renderer->getTerrainDrawEnabled()) return false;
    glsl_program* p = g_gos_renderer->getThinTerrainProgram();
    if (!p || !p->shp_) return false;
    if (thinRecordSSBO == 0 || indirectCmdBuffer == 0) return false;

    const GLuint prog = p->shp_;

    // ---- Save state --------------------------------------------------------
    GLint       savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,   &savedProgram);
    GLboolean   savedBlend     = glIsEnabled(GL_BLEND);
    GLint       savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,     &savedSrcRGB);
    GLint       savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,     &savedDstRGB);
    GLint       savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,   &savedDepthMask);
    GLboolean   savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint       savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,        &savedDepthFunc);
    GLint       savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLboolean   savedColorMask[4];  glGetBooleanv(GL_COLOR_WRITEMASK,   savedColorMask);
    GLuint      savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- VAO rebind (AMD VAO-0 trap) + attr-0 (AMD attribute-0 trap) -------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    // AMD silently drops draws when attribute 0 is disabled and the VS has no
    // layout(location=0) input. The thin VS reads from SSBOs only and has no
    // vertex attribute declarations (docs/amd-driver-rules.md:5). Enable a
    // dummy attr-0 array on the rebound VAO as the runtime mitigation.
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms ------------------------------------------------
    // terrainBindThinUniformsForPatchStream sets the program AND all uniforms
    // (projection chain, camera, shadow maps, PBR params, tex1 sampler).
    // It also calls glUseProgram, so we can safely use the uniform locations
    // it caches.  This mirrors how the M2 thin path sets up uniforms before
    // issueDraws, without duplicating the uniform logic.
    // ssboRecordBase is set to 0 — the indirect thin-record SSBO is indexed
    // globally (slot 2); recordIdx in the VS = 0 + vid/6.
    const int ssboRecordBaseLoc =
        g_gos_renderer->terrainBindThinUniformsForPatchStream();
    if (ssboRecordBaseLoc >= 0)
        glUniform1i(ssboRecordBaseLoc, 0);

    // Fix A (2026-05-14): override the just-uploaded terrainMVP with the
    // per-ring-slot snapshot captured at compute-dispatch time.  Compute
    // wrote pzOk gates into the thin records using that MVP; the VS must
    // project them with the same MVP or fast-rotation frames produce the
    // giant grey-banded terrain triangle.  See
    // docs/superpowers/progress/2026-05-14-raster-triangle-handoff.md.
    extern const float* gos_terrain_indirect_getRingSlotMvp();
    if (const float* slotMvp = gos_terrain_indirect_getRingSlotMvp()) {
        g_gos_renderer->terrainOverrideThinMVP(slotMvp);
    }

    // ---- Depth + color state for opaque terrain ----------------------------
    // TERRAIN-SOLID-APPLYPIPELINE-ROUTING-1: drive depth/blend/cull from the
    // TerrainSolid pipeline row instead of hand-setting them. Byte-identical:
    // depthTest ON, GEQUAL reverse-Z, depthWrite ON, blend OFF (Opaque), and
    // cull None / frontFace Ccw — the latter two are the EMPIRICALLY PROBED ambient
    // (TERRAIN-CULL-STATE-PROBE-1), so naming them is a no-op, not a change.
    // glProgramName=0: the program is already bound above by
    // terrainBindThinUniformsForPatchStream.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainSolid), "TerrainSolid");
    // M5 (LOAD-BEARING — DO NOT REMOVE): undo any prior shadow-pass
    // glColorMask(FALSE,...) from gos_postprocess.cpp:1134/1156. applyPipeline does
    // NOT own colorMask state, so this repair MUST stay explicit here — without it
    // the indirect path draws nothing on the frame following a shadow pass. It is
    // NOT redundant until the pipeline system owns color-mask state.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    render_contract::assertPassContract(render_contract::PassIdentity::TerrainBase,
                                        "gos_TerrainLodChunk_SubmitDrawCommands");

    // ---- Sampler: CLAMP_TO_EDGE / LINEAR (matches M2 atlas-tiled path) ----
    static GLuint s_indirectTerrainSampler = 0;
    if (s_indirectTerrainSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- indirect-terrain sampler object
        glGenSamplers(1, &s_indirectTerrainSampler);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_indirectTerrainSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_indirectTerrainSampler);

    // ---- Bind merged colormap atlas at unit 0 --------------------------------
    // The legacy per-bucket path binds each tile's gosHandle before glDrawArrays.
    // The indirect path issues one glMultiDrawArraysIndirect for ALL quads, so
    // per-bucket binding is impossible.  Instead we bind the full merged colormap
    // atlas (cpuColorMap uploaded once by BuildColormapAtlas at mission load) and
    // let the thin VS compute atlas-absolute UV from world position.
    // State save: capture current GL_TEXTURE_BINDING_2D on unit 0 so the next
    // renderer doesn't see the atlas leak.
    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    extern float  gos_terrain_indirect_getNumTexturesAcross();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
    extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
    extern GLuint gos_terrain_indirect_getCementAtlasGLTex();
    extern int    gos_terrain_indirect_getCementAtlasGridSide();
    extern bool   gos_terrain_indirect_isCementAtlasReady();
    extern float  gos_terrain_indirect_getWorldUnitsPerVertex();
    extern GLuint gos_terrain_indirect_getTransitionMaskArrayGL();
    extern bool   gos_terrain_indirect_isTransitionMaskReady();

    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());

    // Upload atlas UV decomposition uniforms (set AFTER glUseProgram via
    // terrainBindThinUniformsForPatchStream above; prog is already in scope).
    {
        const GLint locNTA  = glGetUniformLocation(prog, "atlasNumTexturesAcross");
        const GLint locMTX  = glGetUniformLocation(prog, "atlasMapTopLeftX");
        const GLint locMTY  = glGetUniformLocation(prog, "atlasMapTopLeftY");
        const GLint locOOWS = glGetUniformLocation(prog, "atlasOneOverWorldUnits");
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        if (locNTA  >= 0) glUniform1f(locNTA,  gos_terrain_indirect_getNumTexturesAcross());
        if (locMTX  >= 0) glUniform1f(locMTX,  gos_terrain_indirect_getAtlasMapTopLeftX());
        if (locMTY  >= 0) glUniform1f(locMTY,  gos_terrain_indirect_getAtlasMapTopLeftY());
        if (locOOWS >= 0) glUniform1f(locOOWS, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        // useAtlasColormap=1: frag samples tex1 from AtlasUV (atlas-absolute) instead
        // of Texcoord (per-tile). Texcoord remains per-tile so detail/POM/anti-tile
        // math stays unchanged. Legacy paths leave this at 0 (default) and continue
        // to sample tex1 with per-tile Texcoord against per-tile bound textures.
        if (locUAC  >= 0) glUniform1i(locUAC,  1);
    }

    // ---- Bind cement catalog atlas at unit 3 (Stage 4 / PR2) ---------------
    // Sampler-object override safety: clear unit 3's sampler with
    // glBindSampler(3, 0) so the texture-object wrap (GL_REPEAT) is the binding
    // (M1/B3 of plan v2.1).
    GLint  savedTex3Binding = 0;
    GLuint savedTex3Sampler = 0;
    const bool cementAtlasReady = gos_terrain_indirect_isCementAtlasReady();
    if (cementAtlasReady) {
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex3Binding);
        { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &q); savedTex3Sampler = (GLuint)q; }
        glBindSampler(3, 0);
        glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getCementAtlasGLTex());
        glActiveTexture(GL_TEXTURE0);
    }

    {
        const GLint locTex3   = glGetUniformLocation(prog, "tex3");
        const GLint locUCA    = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locGSide  = glGetUniformLocation(prog, "atlasCementGridSide");
        const GLint locWUPT   = glGetUniformLocation(prog, "atlasCementWorldUnitsPerTile");

        // Lifecycle warning (subagent M2-v2): if any uniform location is missing
        // when the atlas is ready, the frag failed to compile with the new
        // uniforms — print once per process so the failure is visible in the log.
        static bool s_warnedCementUniforms = false;
        if (cementAtlasReady && !s_warnedCementUniforms &&
            (locTex3 < 0 || locUCA < 0 || locGSide < 0 || locWUPT < 0)) {
            printf("[TERRAIN_INDIRECT v1] event=cement_uniform_missing "
                   "tex3=%d useCementAtlas=%d gridSide=%d wupt=%d\n",
                   locTex3, locUCA, locGSide, locWUPT);
            fflush(stdout);
            s_warnedCementUniforms = true;
        }

        if (locTex3  >= 0) glUniform1i(locTex3, 3);
        if (cementAtlasReady) {
            if (locUCA   >= 0) glUniform1i(locUCA,   1);
            if (locGSide >= 0) glUniform1i(locGSide, gos_terrain_indirect_getCementAtlasGridSide());
            if (locWUPT  >= 0) glUniform1f(locWUPT,  gos_terrain_indirect_getWorldUnitsPerVertex());
        } else {
            if (locUCA   >= 0) glUniform1i(locUCA,   0);
        }
    }

    // ---- Bind transition mask array at unit 4 (Stage B) --------------------
    GLint  savedTex4Binding2D     = 0;
    GLuint savedTex4Binding2DArr  = 0;
    const bool transitionMaskReady = gos_terrain_indirect_isTransitionMaskReady();
    if (transitionMaskReady) {
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex4Binding2D);
        { GLint q = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &q); savedTex4Binding2DArr = (GLuint)q; }
        glBindTexture(GL_TEXTURE_2D_ARRAY, gos_terrain_indirect_getTransitionMaskArrayGL());
        glActiveTexture(GL_TEXTURE0);
    }
    {
        const GLint locTMArr = glGetUniformLocation(prog, "u_transitionMaskArray");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locTMArr >= 0) glUniform1i(locTMArr, 4);
        if (locUseTM >= 0) glUniform1i(locUseTM, transitionMaskReady ? 1 : 0);
    }

    // ---- SSBO bindings + indirect buffer -----------------------------------
    // Slot 1 = recipe (static, mission-stable)
    // Slot 2 = thin record (per-frame ring) — will be re-bound via range below
    if (recipeSSBO != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, (GLuint)recipeSSBO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, (GLuint)indirectCmdBuffer);

    // Bind the thin-record slice at the correct ring offset.
    // The ring slot was advanced by PackThinRecordsForFrame; we don't
    // re-derive the slot offset here — instead we bind the whole buffer
    // at slot 2 and configure ssboRecordBase = 0 with base-instance 0.
    // The thin VS reads thinRecs[recordIdx] where recordIdx = ssboRecordBase + vid/6.
    // Because we uploaded at offset ringSlot*kThinRecordBytes, we need to bind
    // the sub-range at that offset so index 0 in the shader == our first record.
    // Re-bind as a range binding to point ssboRecordBase=0 to the current slot.
    {
        // Ring slot offset in bytes.  Each slot holds kMaxRecs records of
        // sizeof(TerrainQuadThinRecord) each — post Fix B (2026-05-14) the
        // record size is 96 B (was 32 B), so this offset would silently
        // diverge from gos_terrain_indirect.cpp's kThinRecordBytes if the
        // multiplier were left hardcoded.  Source the size from the struct.
        extern int gos_terrain_indirect_getRingSlot();
        const int    slot       = gos_terrain_indirect_getRingSlot();
        const size_t kRecordSz  = sizeof(TerrainQuadThinRecord);
        const size_t kMaxRecs   = 65536u;
        const GLintptr offset   = (GLintptr)(slot * kMaxRecs * kRecordSz);
        const GLsizeiptr sz     = (GLsizeiptr)(kMaxRecs * kRecordSz);
        gpuBindSsboRange(2, (GLuint)thinRecordSSBO, (long long)offset, (long long)sz,
                         "gameos.thinRecord");
    }

    // ── Probe 8: compare draw-time MVP fingerprint vs compute-time fingerprint.
    // Both compute and the thin VS use gos_GetTerrainMVPMat4().  If the matrix
    // has been mutated between those two read points (in the same frame), the
    // compute's pzOk gate decisions disagree with the VS's projection — quads
    // that compute marked as visible may project to wild screen positions.
    // Hypothesis after probes 1-7a all silent + bug persists: MVP delta is the
    // last data-source candidate we haven't tested.
    // VPL retirement deferred #4 (2026-05-16): inert post-Fix-B (no
    // load-bearing consumer); gated behind MC2_RING_TRACE like the rest of
    // the Step 9 demotion so the per-frame FNV + glGetBufferSubData on the
    // indirect buffer cost nothing in the default config.
    { static const bool s_ringMvpProbe = (getenv("MC2_RING_TRACE") != nullptr); if (s_ringMvpProbe) {
        const float* drawMvp = gos_GetTerrainMVPMat4();
        if (drawMvp) {
            uint32_t drawFp = 2166136261u;
            for (int k = 0; k < 12; ++k) {
                uint32_t bits = 0;
                memcpy(&bits, &drawMvp[k], sizeof(bits));
                drawFp ^= bits; drawFp *= 16777619u;
            }
            const uint32_t dispatchFp     = gos_terrain_indirect_getDispatchMvpFp();
            const uint64_t dispatchFrame  = gos_terrain_indirect_getDispatchMvpFrameIdx();
            static uint64_t s_bridgeFrame = 0;
            ++s_bridgeFrame;
            if (drawFp != dispatchFp) {
                static FILE* s_probeSink2 = []{ FILE* f = fopen("ring_trace.log", "a"); return f; }();
                static uint32_t s_mvpMismatchCount = 0;
                ++s_mvpMismatchCount;
                if (s_mvpMismatchCount == 1 || s_mvpMismatchCount % 100 == 0) {
                    // Probe 8b: read back compute-time matrix bytes for byte-level verification.
                    float dispatchFloats[4] = { 0, 0, 0, 0 };
                    gos_terrain_indirect_getDispatchMvpFloats4(dispatchFloats);
                    fprintf(stderr,
                        "[RING_MVP_DELTA v1] bridge_frame=%llu dispatch_frame=%llu dispatch_fp=0x%08x draw_fp=0x%08x count=%u "
                        "disp_mvp[0..3]=[%.6f,%.6f,%.6f,%.6f] "
                        "draw_mvp[0..3]=[%.6f,%.6f,%.6f,%.6f]\n",
                        (unsigned long long)s_bridgeFrame,
                        (unsigned long long)dispatchFrame, dispatchFp, drawFp, s_mvpMismatchCount,
                        dispatchFloats[0], dispatchFloats[1], dispatchFloats[2], dispatchFloats[3],
                        drawMvp[0], drawMvp[1], drawMvp[2], drawMvp[3]);
                    fflush(stderr);
                    if (s_probeSink2) {
                        fprintf(s_probeSink2,
                            "[RING_MVP_DELTA v1] bridge_frame=%llu dispatch_frame=%llu dispatch_fp=0x%08x draw_fp=0x%08x count=%u "
                            "disp_mvp[0..3]=[%.6f,%.6f,%.6f,%.6f] "
                            "draw_mvp[0..3]=[%.6f,%.6f,%.6f,%.6f]\n",
                            (unsigned long long)s_bridgeFrame,
                            (unsigned long long)dispatchFrame, dispatchFp, drawFp, s_mvpMismatchCount,
                            dispatchFloats[0], dispatchFloats[1], dispatchFloats[2], dispatchFloats[3],
                            drawMvp[0], drawMvp[1], drawMvp[2], drawMvp[3]);
                        fflush(s_probeSink2);
                    }
                }
            }
        }

        // Also dump cmd block (terrain-indirect-expert's probe 8 secondary):
        // check cmd.first and cmd.baseInstance fields are zero.  cmd-patch
        // shader sets them to 0 explicitly; non-zero indicates a race or wrong
        // upload.
        uint32_t cmdBlock[4] = {0,0,0,0};
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, (GLuint)indirectCmdBuffer);
        glGetBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0, sizeof(cmdBlock), cmdBlock);
        if (cmdBlock[2] != 0u || cmdBlock[3] != 0u) {
            static FILE* s_probeSink3 = []{ FILE* f = fopen("ring_trace.log", "a"); return f; }();
            static uint32_t s_cmdFieldsBad = 0;
            ++s_cmdFieldsBad;
            if (s_cmdFieldsBad == 1 || s_cmdFieldsBad % 50 == 0) {
                fprintf(stderr, "[RING_CMDFIELDS v1] count=%u inst=%u first=%u base=%u bad_count=%u\n",
                    cmdBlock[0], cmdBlock[1], cmdBlock[2], cmdBlock[3], s_cmdFieldsBad);
                fflush(stderr);
                if (s_probeSink3) {
                    fprintf(s_probeSink3, "[RING_CMDFIELDS v1] count=%u inst=%u first=%u base=%u bad_count=%u\n",
                        cmdBlock[0], cmdBlock[1], cmdBlock[2], cmdBlock[3], s_cmdFieldsBad);
                    fflush(s_probeSink3);
                }
            }
        }
    } }
    // ── end probe 8 ────────────────────────────────────────────────────────

    // ---- Draw (wireframe support mirrors tessellated path at ~5386) ---------
    const bool indirectWireframe = g_gos_renderer->getTerrainWireframe();
    if (indirectWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, (GLsizei)cmdCount, 0);
    mdi_submit::trace("TerrainSolid", "MultiDrawArraysIndirect", (int)cmdCount,
                      "GPU", "g_indirectCmdBuffer", false);
    if (indirectWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // Reset useAtlasColormap so M2 fast path (shares this program) doesn't
    // inherit the atlas-mode flag and sample from AtlasUV against a per-tile
    // bound texture.
    {
        const GLint locUAC = glGetUniformLocation(prog, "useAtlasColormap");
        if (locUAC >= 0) glUniform1i(locUAC, 0);
    }
    // Reset useCementAtlas so the M2 fast path doesn't inherit the cement flag.
    {
        const GLint locUCA  = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locUCA  >= 0) glUniform1i(locUCA,  0);
        if (locUseTM >= 0) glUniform1i(locUseTM, 0);
    }
    // Restore unit 3 (texture binding + sampler).
    if (cementAtlasReady) {
        glActiveTexture(GL_TEXTURE3);
        glBindSampler(3, savedTex3Sampler);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex3Binding);
        glActiveTexture(GL_TEXTURE0);
    }
    // Restore unit 4 (transition mask array).
    if (transitionMaskReady) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, savedTex4Binding2DArr);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex4Binding2D);
        glActiveTexture(GL_TEXTURE0);
    }

    // ---- Restore state -----------------------------------------------------
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);
    glBindSampler(0, savedSampler);
    // Restore atlas texture bind so next renderer doesn't see the atlas.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1],
                savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    else                 glEnable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND);
    else            glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // RENDER_STATES v1: indirect terrain bound textures on units 0 and 3 directly;
    // the applyRenderStates cache is now stale. Force a full re-apply on next call.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// VK-OPT-4: class-D orphan-then-upload for the terrain-mask indirect command
// buffers (s_indirectCmdBuf / s_waterIndirectCmdBuf). These are per-frame
// CPU-written DrawArraysIndirect command buffers that were previously
// glBufferSubData'd WITHOUT orphaning/fencing, so the GL driver implicit-sync
// stalls (waits for the prior frame's glDrawArraysIndirect to finish reading
// the buffer before overwriting). Orphaning renames the storage so the driver
// hands back fresh memory instead of stalling — same bytes, byte-identical
// output, only the driver serialization is removed. Mirrors the reusable
// tubeOrphanUpload template from gos_particle_bridge.cpp (VK-OPT-1).
//
// The orphan reuses the buffer's fixed capacity (a single 16-byte command),
// so it is a cheap rename, not a reallocation. On Vulkan this class of
// implicit-sync buffer does not exist at all — this is also VK-migration prep.
//
// Optional, zero-cost-when-off instrumentation behind MC2_TERRAIN_MASK_UPLOAD_STATS
// makes the orphaning measurable / regression-guardable (parallels
// MC2_VFX_TUBE_UPLOAD_STATS / MC2_LIGHT_UPLOAD_STATS).
namespace {
struct MaskUploadStats {
    unsigned long long uploadBytes = 0;  // command bytes pushed this frame
    unsigned long long orphans     = 0;  // orphan-then-upload calls this frame
};
MaskUploadStats s_maskUploadStats;

bool maskUploadStatsEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_TERRAIN_MASK_UPLOAD_STATS");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

// Orphan-then-upload the indirect command: rename storage (glBufferData NULL at
// the buffer's existing size) before the glBufferSubData so the driver does not
// implicit-sync-stall on the previous frame's indirect read. Buffer must be
// bound to `target` by the caller (it already is at the draw site).
void maskOrphanUploadCmd(GLenum target, GLsizeiptr bytes, const void* data) {
    glBufferData(target, bytes, nullptr, GL_DYNAMIC_DRAW);  // orphan/rename
    glBufferSubData(target, 0, bytes, data);
    if (maskUploadStatsEnabled()) {
        s_maskUploadStats.uploadBytes += (unsigned long long)bytes;
        ++s_maskUploadStats.orphans;
        std::fprintf(stderr,
            "[TERRAIN_MASK_UPLOAD_STATS] orphan+upload bytes=%lld orphans=%llu totalBytes=%llu\n",
            (long long)bytes, s_maskUploadStats.orphans, s_maskUploadStats.uploadBytes);
        std::fflush(stderr);
    }
}
}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// B4 Slice Stage 1b — Mask-SOLID dual-run draw bridge.
//
// Called by gos_terrain_mask_dispatch::DrawMaskSolid() from
// Render.TerrainMask.Solid. Reads the per-frame SOLID mask SSBO at binding 17,
// the dense recipe SSBO at binding 19, and the per-vertex lighting SSBO at
// binding 2. Issues ONE glDrawArraysIndirect covering all quadCount*6 vertices;
// the VS emits degenerate triangles for quads outside the mask.
// State save/restore mirrors gos_terrain_bridge_drawIndirect.
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawMaskSolid(uint32_t solidMaskSSBO,
                                      uint32_t recipeSSBO,
                                      uint32_t lightingSSBO,
                                      int      quadCount,
                                      int      mapSide)
{
    ZoneScopedN("Terrain::MaskSolidDraw");
    if (!g_gos_renderer) return false;
    glsl_program* p = g_gos_renderer->getMaskSolidProgram();
    if (!p || !p->shp_) return false;
    if (solidMaskSSBO == 0 || recipeSSBO == 0 || quadCount <= 0 || mapSide <= 0) return false;

    const GLuint prog = p->shp_;

    // ---- Save state (same set as drawIndirect) -----------------------------
    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,     &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,       &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,       &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,     &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,          &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&savedVAO);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);
    GLuint    savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- AMD VAO-0 + attr-0 traps ------------------------------------------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms ------------------------------------------------
    // Pass overrideProg so terrainBindThinUniformsForPatchStream binds + sets
    // uniforms directly on mask_solid_prog_. All uniform names match the thin
    // shader (cache is keyed by program, so it re-fetches locations on switch).
    g_gos_renderer->terrainBindThinUniformsForPatchStream(p);

    // ---- mapSide uniform ---------------------------------------------------
    {
        const GLint locMS = glGetUniformLocation(prog, "mapSide");
        if (locMS >= 0) glUniform1i(locMS, mapSide);
    }

    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    extern float  gos_terrain_indirect_getNumTexturesAcross();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
    extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
    extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
    extern GLuint gos_terrain_indirect_getCementAtlasGLTex();
    extern int    gos_terrain_indirect_getCementAtlasGridSide();
    extern bool   gos_terrain_indirect_isCementAtlasReady();
    extern float  gos_terrain_indirect_getWorldUnitsPerVertex();
    extern GLuint gos_terrain_indirect_getTransitionMaskArrayGL();
    extern bool   gos_terrain_indirect_isTransitionMaskReady();

    // ---- Depth + color state -----------------------------------------------
    // Stage 1b dual-run soak: suppress all framebuffer writes so the mask draw
    // validates the pipeline (mask culling, shader compile/run, GL errors) without
    // z-fighting the concurrent PR1 indirect draw. Stage 1d flips this when PR1
    // is retired and the mask draw becomes the sole SOLID path.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (mask soak 1b)
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_BLEND);

    // ---- Sampler unit 0: CLAMP_TO_EDGE / LINEAR (matches drawIndirect) -----
    static GLuint s_maskSolidSampler = 0;
    if (s_maskSolidSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- mask-solid sampler object
        glGenSamplers(1, &s_maskSolidSampler);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_maskSolidSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_maskSolidSampler);

    // ---- Atlas colormap at unit 0 ------------------------------------------
    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
    {
        const GLint locNTA  = glGetUniformLocation(prog, "atlasNumTexturesAcross");
        const GLint locMTX  = glGetUniformLocation(prog, "atlasMapTopLeftX");
        const GLint locMTY  = glGetUniformLocation(prog, "atlasMapTopLeftY");
        const GLint locOOWS = glGetUniformLocation(prog, "atlasOneOverWorldUnits");
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        if (locNTA  >= 0) glUniform1f(locNTA,  gos_terrain_indirect_getNumTexturesAcross());
        if (locMTX  >= 0) glUniform1f(locMTX,  gos_terrain_indirect_getAtlasMapTopLeftX());
        if (locMTY  >= 0) glUniform1f(locMTY,  gos_terrain_indirect_getAtlasMapTopLeftY());
        if (locOOWS >= 0) glUniform1f(locOOWS, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        if (locUAC  >= 0) glUniform1i(locUAC,  1);
    }

    // ---- Cement atlas at unit 3 (if ready) ---------------------------------
    GLint  savedTex3Binding = 0;
    GLuint savedTex3Sampler = 0;
    const bool cementReady = gos_terrain_indirect_isCementAtlasReady();
    if (cementReady) {
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex3Binding);
        { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &q); savedTex3Sampler = (GLuint)q; }
        glBindSampler(3, 0);
        glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getCementAtlasGLTex());
        glActiveTexture(GL_TEXTURE0);
    }
    {
        const GLint locTex3  = glGetUniformLocation(prog, "tex3");
        const GLint locUCA   = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locGSide = glGetUniformLocation(prog, "atlasCementGridSide");
        const GLint locWUPT  = glGetUniformLocation(prog, "atlasCementWorldUnitsPerTile");
        if (locTex3  >= 0) glUniform1i(locTex3, 3);
        if (cementReady) {
            if (locUCA   >= 0) glUniform1i(locUCA,   1);
            if (locGSide >= 0) glUniform1i(locGSide, gos_terrain_indirect_getCementAtlasGridSide());
            if (locWUPT  >= 0) glUniform1f(locWUPT,  gos_terrain_indirect_getWorldUnitsPerVertex());
        } else {
            if (locUCA   >= 0) glUniform1i(locUCA,   0);
        }
    }

    // ---- Bind transition mask array at unit 4 (Stage B) --------------------
    GLint  savedTex4Binding2D_b    = 0;
    GLuint savedTex4Binding2DArr_b = 0;
    const bool transitionMaskReady2 = gos_terrain_indirect_isTransitionMaskReady();
    if (transitionMaskReady2) {
        glActiveTexture(GL_TEXTURE4);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex4Binding2D_b);
        { GLint q = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &q); savedTex4Binding2DArr_b = (GLuint)q; }
        glBindTexture(GL_TEXTURE_2D_ARRAY, gos_terrain_indirect_getTransitionMaskArrayGL());
        glActiveTexture(GL_TEXTURE0);
    }
    {
        const GLint locTMArr = glGetUniformLocation(prog, "u_transitionMaskArray");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locTMArr >= 0) glUniform1i(locTMArr, 4);
        if (locUseTM >= 0) glUniform1i(locUseTM, transitionMaskReady2 ? 1 : 0);
    }

    // ---- SSBO bindings -----------------------------------------------------
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, (GLuint)solidMaskSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, (GLuint)recipeSSBO);
    if (lightingSSBO != 0)
        // VK-H2-BINDING-SPLIT-1: LightingBuf moved slot 2 -> 4 (lockstep with
        // gos_terrain_mask_solid.vert). Slot 2 kept clear for ThinRecordBufFrag.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, (GLuint)lightingSSBO);

    // ---- Build and issue one DrawArraysIndirect ----------------------------
    struct DrawArraysIndirectCommand {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t first;
        uint32_t baseInstance;
    };
    static GLuint s_indirectCmdBuf = 0;
    if (s_indirectCmdBuf == 0) {
        glGenBuffers(1, &s_indirectCmdBuf);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_indirectCmdBuf);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(DrawArraysIndirectCommand),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    DrawArraysIndirectCommand cmd = { uint32_t(quadCount * 6), 1u, 0u, 0u };
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_indirectCmdBuf);
    // VK-OPT-4 (class-D fix): orphan-then-upload so the driver renames storage
    // instead of implicit-sync-stalling on the previous frame's indirect read.
    maskOrphanUploadCmd(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), &cmd);
    glDrawArraysIndirect(GL_TRIANGLES, nullptr);
    mdi_submit::trace("TerrainMaskSolid", "DrawArraysIndirect", 1,
                      "CPU", "s_indirectCmdBuf", false);

    // ---- Restore -----------------------------------------------------------
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 17, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 19, 0);
    if (lightingSSBO != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);  // VK-H2-BINDING-SPLIT-1: LightingBuf slot 2 -> 4

    // Reset useAtlasColormap on this program so re-entry doesn't inherit atlas mode.
    {
        const GLint locUAC  = glGetUniformLocation(prog, "useAtlasColormap");
        const GLint locUCA  = glGetUniformLocation(prog, "useCementAtlas");
        const GLint locUseTM = glGetUniformLocation(prog, "u_useTransitionMask");
        if (locUAC  >= 0) glUniform1i(locUAC,  0);
        if (locUCA  >= 0) glUniform1i(locUCA,  0);
        if (locUseTM >= 0) glUniform1i(locUseTM, 0);
    }

    if (cementReady) {
        glActiveTexture(GL_TEXTURE3);
        glBindSampler(3, savedTex3Sampler);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex3Binding);
        glActiveTexture(GL_TEXTURE0);
    }
    if (transitionMaskReady2) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, savedTex4Binding2DArr_b);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex4Binding2D_b);
        glActiveTexture(GL_TEXTURE0);
    }
    glBindSampler(0, savedSampler);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // Mark render-state cache stale since we bound textures + program directly.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// B4 Slice Stage 1c — Mask-water dual-run draw bridge.
//
// Called by gos_terrain_mask_dispatch::DrawMaskWater() from
// Render.TerrainMask.Water. Reads the per-frame water mask SSBO at binding 18,
// the WaterRecipe SSBO at binding 5, and the per-vertex lighting SSBO at
// binding 2. Issues ONE glDrawArraysIndirect covering all recipeCount*6 vertices;
// the VS emits degenerate triangles for quads outside the mask.
// State save/restore mirrors gos_terrain_bridge_drawMaskSolid.
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawMaskWater(uint32_t waterMaskSSBO,
                                      uint32_t recipeSSBO,
                                      uint32_t lightingSSBO,
                                      int      recipeCount,
                                      float    waterElevation,
                                      float    frameCos)
{
    ZoneScopedN("Terrain::MaskWaterDraw");
    if (!g_gos_renderer) return false;
    glsl_program* p = g_gos_renderer->getMaskWaterProgram();
    if (!p || !p->shp_) return false;
    if (waterMaskSSBO == 0 || recipeSSBO == 0 || recipeCount <= 0) return false;

    const GLuint prog = p->shp_;

    // ---- Save state --------------------------------------------------------
    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,      &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,        &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,        &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,      &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,           &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);
    GLuint    savedSampler   = 0;
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q); savedSampler = (GLuint)q; }

    // ---- AMD VAO-0 + attr-0 traps ------------------------------------------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();
    glEnableVertexAttribArray(0);

    // ---- Program + uniforms ------------------------------------------------
    // terrainBindThinUniformsForPatchStream sets terrainMVP, mvp,
    // and other terrain-shared uniforms (same uniform names as thin/mask-solid).
    g_gos_renderer->terrainBindThinUniformsForPatchStream(p);

    // waterElevation and frameCos are not set by terrainBindThinUniformsForPatchStream.
    {
        const GLint locWE = glGetUniformLocation(prog, "waterElevation");
        const GLint locFC = glGetUniformLocation(prog, "frameCos");
        if (locWE >= 0) glUniform1f(locWE, waterElevation);
        if (locFC >= 0) glUniform1f(locFC, frameCos);
    }

    // ---- Depth + color state -----------------------------------------------
    // Stage 1c dual-run soak: suppress all framebuffer writes so the mask draw
    // validates the pipeline without z-fighting the concurrent water fast path.
    // Water is translucent so depth writes are off anyway; keep depth test for
    // correct mask geometry.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (mask soak 1c)
    glDepthMask(GL_FALSE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    // Blend on (water is semi-transparent); blend func matches water fast path.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- Sampler unit 0 ----------------------------------------------------
    static GLuint s_maskWaterSampler = 0;
    if (s_maskWaterSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- mask-water sampler object
        glGenSamplers(1, &s_maskWaterSampler);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_maskWaterSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindSampler(0, s_maskWaterSampler);

    // ---- Bind a valid texture at unit 0 (terrain atlas) to prevent undefined sampling.
    // glColorMask(GL_FALSE) suppresses output, but we still bind something safe.
    extern GLuint gos_terrain_indirect_getAtlasGLTex();
    GLint savedTex0Binding = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());

    // ---- SSBO bindings -----------------------------------------------------
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, (GLuint)waterMaskSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,  5, (GLuint)recipeSSBO);
    if (lightingSSBO != 0)
        // VK-H2-BINDING-SPLIT-1: LightingBuf moved slot 2 -> 4 (lockstep with
        // gos_terrain_mask_water.vert). Slot 2 kept clear for ThinRecordBufFrag.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, (GLuint)lightingSSBO);

    // ---- Build and issue one DrawArraysIndirect ----------------------------
    struct DrawArraysIndirectCommand {
        uint32_t count;
        uint32_t instanceCount;
        uint32_t first;
        uint32_t baseInstance;
    };
    static GLuint s_waterIndirectCmdBuf = 0;
    if (s_waterIndirectCmdBuf == 0) {
        glGenBuffers(1, &s_waterIndirectCmdBuf);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_waterIndirectCmdBuf);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(DrawArraysIndirectCommand),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    DrawArraysIndirectCommand cmd = { uint32_t(recipeCount * 6), 1u, 0u, 0u };
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_waterIndirectCmdBuf);
    // VK-OPT-4 (class-D fix): orphan-then-upload so the driver renames storage
    // instead of implicit-sync-stalling on the previous frame's indirect read.
    maskOrphanUploadCmd(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), &cmd);
    glDrawArraysIndirect(GL_TRIANGLES, nullptr);
    mdi_submit::trace("TerrainMaskWater", "DrawArraysIndirect", 1,
                      "CPU", "s_waterIndirectCmdBuf", false);

    // ---- Restore -----------------------------------------------------------
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 18, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,  5, 0);
    if (lightingSSBO != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0);  // VK-H2-BINDING-SPLIT-1: LightingBuf slot 2 -> 4

    glBindSampler(0, savedSampler);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // Mark render-state cache stale since we bound textures + program directly.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// PR2c Stage 2c — Mine static-bake bridge.
//
// Issues ONE glDrawArrays(GL_TRIANGLES) against the mission-static MineStaticVBO,
// with the 2-layer mine sprite array bound at unit 5. State save/restore mirrors
// gos_terrain_bridge_drawIndirect (Program, Blend, Depth, ColorMask, VAO,
// sampler unit 5, texture binding on unit 5, vertex attrib state). Per-frame
// CPU work is dominated by this state churn — the actual draw is one dispatch
// over a few hundred verts max (mines are sparse: 2-3 missions x small
// overlay each per user direction 2026-05-08).
// ──────────────────────────────────────────────────────────────────────────
bool gos_terrain_bridge_drawMineStatic(int          vertCount,
                                       unsigned int vboGL,
                                       unsigned int textureArrayGL)
{
    ZoneScopedN("Terrain::MineStaticDraw");
    if (!g_gos_renderer) return false;
    if (vertCount <= 0 || vboGL == 0 || textureArrayGL == 0) return false;
    glsl_program* p = g_gos_renderer->getMineStaticProgram();
    if (!p || !p->shp_) return false;

    const GLuint prog = p->shp_;

    // ---- Save state ---------------------------------------------------------
    GLint     savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM,         &savedProgram);
    GLboolean savedBlend     = glIsEnabled(GL_BLEND);
    GLint     savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB,           &savedSrcRGB);
    GLint     savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB,           &savedDstRGB);
    GLint     savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK,         &savedDepthMask);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint     savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC,              &savedDepthFunc);
    GLint     savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING,    &savedVAO);
    GLint     savedActiveTex = GL_TEXTURE0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    GLboolean savedColorMask[4]; glGetBooleanv(GL_COLOR_WRITEMASK,           savedColorMask);
    GLint     savedArrayBuf  = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING,    &savedArrayBuf);
    GLint     savedTex5Bind  = 0;
    GLuint    savedTex5Sampler = 0;
    glActiveTexture(GL_TEXTURE5);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &savedTex5Bind);
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 5, &q); savedTex5Sampler = (GLuint)q; }

    // ---- VAO rebind (AMD VAO-0 trap) ---------------------------------------
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();

    // ---- Program -----------------------------------------------------------
    glUseProgram(prog);

    // terrainMVP (mirrors PR1 thin-VS upload — GL_FALSE + row-major per
    // terrain_mvp_gl_false.md).
    {
        const GLint loc = glGetUniformLocation(prog, "u_worldToClipGL");
        if (loc >= 0)
            glUniformMatrix4fv(loc, 1, GL_FALSE,
                               (const float*)&g_gos_renderer->getTerrainMVP());
    }

    // mineSpriteArray uniform → unit 5
    {
        const GLint loc = glGetUniformLocation(prog, "mineSpriteArray");
        if (loc >= 0) glUniform1i(loc, 5);
    }

    // ---- Vertex attrib setup (GL_ARRAY_BUFFER + 3 attrs at locations 0/1/2)
    // Vertex format must match MineVert in gos_terrain_indirect.cpp:
    //   offset  0: vec3 pos    (location 0)
    //   offset 12: vec2 uv     (location 1)
    //   offset 20: uint layer  (location 2)
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vboGL);
    constexpr GLsizei kStride = 24;  // sizeof(MineVert)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, kStride, (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, kStride, (const void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT,    kStride, (const void*)20);

    // ---- Bind texture-array at unit 5 + clear sampler-object override ------
    glActiveTexture(GL_TEXTURE5);
    glBindSampler(5, 0);  // use the texture's own params (NEAREST/CLAMP_TO_EDGE)
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)textureArrayGL);
    glActiveTexture(GL_TEXTURE0);

    // ---- Depth + blend + color state ---------------------------------------
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (scene terrain)
    glDepthMask(GL_TRUE);
    // M5: undo any prior shadow-pass glColorMask(FALSE,...) leakage.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // Mine sprite uses alpha-test discard (in FS); no blend needed.
    glDisable(GL_BLEND);

    // ---- Draw --------------------------------------------------------------
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertCount);

    // ---- Restore state -----------------------------------------------------
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(1);
    // Leave attr-0 enabled — AMD attribute-0 trap (other paths assume it on).

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)savedTex5Bind);
    glBindSampler(5, savedTex5Sampler);
    glActiveTexture((GLenum)savedActiveTex);

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)savedArrayBuf);
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glDepthFunc((GLenum)savedDepthFunc);
    glDepthMask((GLboolean)savedDepthMask);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedBlend)     glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    glBindVertexArray((GLuint)savedVAO);
    glUseProgram((GLuint)savedProgram);

    // RENDER_STATES v1: invalidate cache since we touched units + state.
    if (g_gos_renderer) g_gos_renderer->invalidateRenderStateCache();
    return true;
}

// Slice A — cement-overlay static-bake draw bridge. Thin forwarder to the
// gosRenderer member (which owns the private overlay program/locs/texture
// list), mirroring how gos_DrawTerrainOverlays forwards to drawTerrainOverlays
// and how gos_terrain_bridge_drawMineStatic gates on g_gos_renderer.
bool gos_terrain_bridge_drawDecalStatic(unsigned int               vboGL,
                                        const GosDecalStaticDraw*  draws,
                                        int                        drawCount)
{
    ZoneScopedN("Terrain::DecalStaticDraw");
    if (!g_gos_renderer) return false;
    return g_gos_renderer->drawDecalStaticBatch(vboGL, draws, drawCount);
}

// ──────────────────────────────────────────────────────────────────────────

static GLuint gVAO = 0;
static float  s_hud_scale = 0.85f;  // default while iterating; RAlt+5 cycles
static bool   s_hud_scale_active = false;  // gated: only shrink during mission
// When set, HUD draw calls recorded while it is on are tagged scaleExempt and
// are NOT shrunk by the bottom-band s_hud_scale transform in flushHUDBatch.
// Bracketed around the mouse cursor sprite and modal dialogs (quit prompt, etc.)
// so the HUD-fit shrink only ever moves the in-game HUD chrome -- never the
// pointer or a popup. Without this the cursor snapped from unscaled to 0.85x as
// it crossed the 60%-height centroid gate (jump-at-center + 0.9x drift) and
// dialogs straddling the gate tore at the seam.
static bool   s_hud_scale_exempt = false;


void gosRenderer::init() {
    ZoneScopedN("gosRenderer::init");
    initRenderStates();

    // x = 1/w; x =2*x - 1;
    // y = 1/h; y= 1- y; y =2*y - 1;
    // z = z;
    projection_ = mat4(
            2.0f / (float)width_, 0, 0.0f, -1.0f,
            0, -2.0f / (float)height_, 0.0f, 1.0f,
            0, 0, 1.0f, 0.0f,
            0, 0, 0.0f, 1.0f);

	graphics::get_drawable_size(win_h_, &Environment.drawableWidth, &Environment.drawableHeight);

    // setup viewport
    setupViewport(true, 1.0f, true, 0, 0.0f, 0.0f, 1.0f, 1.0f);

    {
        ZoneScopedN("gosRenderer::init meshes");
        quads_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*10);
        gosASSERT(quads_);
        tris_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*10);
        gosASSERT(tris_);
        indexed_tris_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 1024*60, 1024*60);
        gosASSERT(indexed_tris_);
        lines_ = gosMesh::makeMesh(PRIMITIVE_LINELIST, 1024*10);
        gosASSERT(lines_);
        points_= gosMesh::makeMesh(PRIMITIVE_POINTLIST, 1024*10);
        gosASSERT(points_);
        text_ = gosMesh::makeMesh(PRIMITIVE_TRIANGLELIST, 4024 * 6);
        gosASSERT(text_);
    }


    gosRenderMaterial* building_pbr_material = nullptr;
    const char* shader_list[] = {"gos_vertex", "gos_tex_vertex", "gos_text", "gos_vertex_lighted", "gos_tex_vertex_lighted", "building_pbr"};
    gosRenderMaterial** shader_ptr_list[] = { &basic_material_, &basic_tex_material_, &text_material_, &basic_lighted_material_, &basic_tex_lighted_material_, &building_pbr_material };

    static_assert(COUNTOF(shader_list) == COUNTOF(shader_ptr_list), "Arrays myst have same size");
    uint32_t combinations[] = {
        0,
        SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST),
        SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::IS_OVERLAY),
        SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) | SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::IS_OVERLAY)
    };

    {
    ZoneScopedN("gosRenderer::init baseMaterials");
    for(size_t i=0; i<COUNTOF(combinations); ++i)
    {
        std::vector<std::string> defines;
        for(size_t bit = 0; bit < 32; ++bit)
        {
            uint32_t bit_mask = 1<<bit;
            if(bit_mask & combinations[i])
            {
                std::string s = g_shader_flags[bit];
                defines.push_back(s);
            }
        }

        gosMaterialVariationHelper helper;
        helper.addDefines(defines);
        gosMaterialVariation mvar;
        helper.getMaterialVariation(mvar);

        //TODO: remove texture / no texture variants and move it to flags
        
        for(uint32_t sh_idx = 0; sh_idx < COUNTOF(shader_list); ++sh_idx)
        {
            gosRenderMaterial* pmat = gosRenderMaterial::load(shader_list[sh_idx], mvar);
            gosASSERT(pmat);
            materialList_.push_back(pmat);
            materialDB_[ shader_list[sh_idx] ].insert(std::make_pair(combinations[i], pmat));

            *shader_ptr_list[sh_idx] = pmat;
        }
    }
    }


    { ZoneScopedN("gosRenderer::init vao"); glGenVertexArrays(1, &gVAO); }

    pendingRequest = false;

    num_draw_calls_ = 0;
    num_draw_calls_to_draw_ = 0;
    break_on_draw_call_ = false;
    break_draw_call_num_ = 0;

    // add fake texture so that no one will get 0 index, as it is invalid in this game
    DWORD tex_id;
    { ZoneScopedN("gosRenderer::init dummyTexture"); tex_id = gos_NewEmptyTexture( gos_Texture_Solid, "DEBUG_this_is_not_a_real_texture_debug_it!", 1); }
    (void)tex_id;
    gosASSERT(tex_id == INVALID_TEXTURE_ID);

	fog_color_ = vec4(1.0f, 1.0f, 1.0f, 1.0f);

    // Terrain tessellation extra VBO
    terrain_extra_capacity_ = 1024 * 120;  // large buffer for extended view distance
    {
        ZoneScopedN("gosRenderer::init terrainExtra");
        terrain_extra_data_ = new gos_TERRAIN_EXTRA[terrain_extra_capacity_];
        terrain_extra_vb_ = makeBuffer(GL_ARRAY_BUFFER, 0,
            sizeof(gos_TERRAIN_EXTRA) * terrain_extra_capacity_, GL_DYNAMIC_DRAW);
    }
    printf("[TESS] Extra VBO created: capacity=%d vb=%u\n", terrain_extra_capacity_, terrain_extra_vb_);

    // Load terrain tessellation material (TCS/TES shaders)
    {
        ZoneScopedN("gosRenderer::init terrainMaterial");
        printf("[TESS] About to load terrain shader...\n"); fflush(stdout);
        gosMaterialVariationHelper terrainHelper;
        gosMaterialVariation mvar;
        terrainHelper.getMaterialVariation(mvar);
        terrain_material_ = gosRenderMaterial::load("gos_terrain", mvar);
        if (terrain_material_) {
            materialList_.push_back(terrain_material_);
            printf("[TESS] Terrain material loaded successfully\n");
        } else {
            printf("[TESS] WARNING: Terrain material failed to load — tessellation disabled\n");
        }
        fflush(stdout);
    }

    // Load shadow terrain material (VS+FS only, no tessellation)
    {
        ZoneScopedN("gosRenderer::init shadowTerrainMaterial");
        gosMaterialVariationHelper helper;
        gosMaterialVariation mvar;
        helper.getMaterialVariation(mvar);
        shadow_terrain_material_ = gosRenderMaterial::load("shadow_terrain", mvar);
        if (shadow_terrain_material_) {
            materialList_.push_back(shadow_terrain_material_);
        }
    }

    // Load shadow object material (VS+FS only, no tessellation)
    {
        ZoneScopedN("gosRenderer::init shadowObjectMaterial");
        gosMaterialVariationHelper helper;
        gosMaterialVariation mvar;
        helper.getMaterialVariation(mvar);
        shadow_object_material_ = gosRenderMaterial::load("shadow_object", mvar);
        if (shadow_object_material_) {
            materialList_.push_back(shadow_object_material_);
        }
    }

    // GPU-driven dynamic sun shadow Phase 1 -- depth-only instanced programs.
    // shadow_mech.vert: skinned mech instances (SSBOs binding0=InstanceBuffer,
    //   binding1=BoneBuffer). Mirrors mech.vert position-only path.
    // shadow_static_prop.vert: static-prop instances (SSBO binding0=Instances).
    //   Mirrors static_prop.vert legacy non-coalesce path (gl_InstanceID bare;
    //   caller binds per-type SSBO range via glBindBufferRange).
    // Nothing calls these yet -- registration only (Task 1).
    {
        ZoneScopedN("gosRenderer::init shadowMechProg");
        static const char* kShadowInstPrefix = "#version 430\n";
        shadow_mech_prog_ = glsl_program::makeProgram(
            "shadow_mech",
            "shaders/shadow_mech.vert",
            "shaders/shadow_instanced.frag",
            kShadowInstPrefix);
        if (!shadow_mech_prog_ || !shadow_mech_prog_->shp_)
            fprintf(stderr, "[SHADOW_MECH] WARNING: failed to compile shadow_mech shader"
                            " -- GPU-driven mech shadow draw disabled\n");
        else
            printf("[SHADOW_MECH] Shadow mech shader loaded: prog=%u\n",
                   (unsigned)shadow_mech_prog_->shp_);
        fflush(stdout);
    }
    {
        ZoneScopedN("gosRenderer::init shadowStaticPropProg");
        static const char* kShadowStaticPropPrefix = "#version 430\n";
        shadow_static_prop_prog_ = glsl_program::makeProgram(
            "shadow_static_prop",
            "shaders/shadow_static_prop.vert",
            "shaders/shadow_instanced.frag",
            kShadowStaticPropPrefix);
        if (!shadow_static_prop_prog_ || !shadow_static_prop_prog_->shp_)
            fprintf(stderr, "[SHADOW_STATIC_PROP] WARNING: failed to compile shadow_static_prop shader"
                            " -- GPU-driven static-prop shadow draw disabled\n");
        else
            printf("[SHADOW_STATIC_PROP] Shadow static-prop shader loaded: prog=%u\n",
                   (unsigned)shadow_static_prop_prog_->shp_);
        fflush(stdout);
    }
    {
        // SHADOW-PROP-ALPHA-1: dedicated alpha-tested prop shadow program.
        // shadow_static_prop.vert (now forwards a_uv->v_uv) + the new
        // shadow_static_prop.frag (legacy non-coalesce alpha-test discard).
        // Used ONLY by drawDynamicPropShadows when MC2_SHADOW_PROP_ALPHA != 0
        // so foliage/tree cards cast a leaf-shaped silhouette instead of a
        // square card. Mechs stay on the empty shadow_instanced.frag (above).
        ZoneScopedN("gosRenderer::init shadowStaticPropAlphaProg");
        static const char* kShadowPropAlphaPrefix = "#version 430\n";
        glsl_program* p = glsl_program::makeProgram(
            "shadow_static_prop_alpha",
            "shaders/shadow_static_prop.vert",
            "shaders/shadow_static_prop.frag",
            kShadowPropAlphaPrefix);
        if (!p || !p->shp_)
            fprintf(stderr, "[SHADOW_STATIC_PROP] WARNING: failed to compile "
                            "shadow_static_prop_alpha shader -- foliage shadow "
                            "casters fall back to square (empty-frag) silhouette\n");
        else
            printf("[SHADOW_STATIC_PROP] Shadow static-prop ALPHA shader loaded: prog=%u\n",
                   (unsigned)p->shp_);
        fflush(stdout);
    }

    // Load thin-record terrain program (gos_terrain_thin.vert + gos_terrain.frag, no tess).
    // Used by PatchStream M1g to draw thin records via GL_TRIANGLES, avoiding tessellation overhead.
    {
        ZoneScopedN("gosRenderer::init thinTerrainProg");
        std::string kThinPrefix = "#version 430\n";
        if (terrainNormalArrayEnabled())
            kThinPrefix += "#define TERRAIN_NORMAL_ARRAY\n";
        thin_terrain_prog_ = glsl_program::makeProgram(
            "gos_terrain_thin",
            "shaders/gos_terrain_thin.vert",
            "shaders/gos_terrain.frag",
            kThinPrefix.c_str());
        if (!thin_terrain_prog_ || !thin_terrain_prog_->shp_)
            fprintf(stderr, "[THIN_TERRAIN] WARNING: failed to compile thin terrain shader"
                            " — thin draw path disabled\n");
        else
            printf("[THIN_TERRAIN] Thin terrain shader loaded: prog=%u\n",
                   (unsigned)thin_terrain_prog_->shp_);
        fflush(stdout);
    }

    // [TERRAIN_SURFACE] PR-2 — load the continuous indexed-surface program
    // (gos_terrain_surface.vert + gos_terrain.frag, V-ssbo vertex-pull, no
    // tess). #version provided by the prefix here; NEVER a #version line in
    // the shader file (worktree CLAUDE.md). Shader hot-reload fails SILENTLY
    // (bad compile -> old shader stays active) -- a failed compile here is a
    // hard WARNING so the smoke log shows it; PR-2's automated gate greps the
    // run log for shader compile errors.
    {
        ZoneScopedN("gosRenderer::init terrainSurfaceProg");
        std::string kSurfacePrefix = "#version 430\n";
        if (terrainNormalArrayEnabled())
            kSurfacePrefix += "#define TERRAIN_NORMAL_ARRAY\n";
        terrain_surface_prog_ = glsl_program::makeProgram(
            "gos_terrain_surface",
            "shaders/gos_terrain_surface.vert",
            "shaders/gos_terrain.frag",
            kSurfacePrefix.c_str());
        if (!terrain_surface_prog_ || !terrain_surface_prog_->shp_)
            fprintf(stderr, "[TERRAIN_SURFACE v1] event=shader_compile_fail "
                            "vs=gos_terrain_surface.vert fs=gos_terrain.frag "
                            "-- MC2_TERRAIN_SURFACE draw disabled\n");
        else
            printf("[TERRAIN_SURFACE v1] event=shader_loaded prog=%u\n",
                   (unsigned)terrain_surface_prog_->shp_);
        fflush(stdout);
    }

    // PR2c Stage 2c — load mine static-bake program.
    {
        ZoneScopedN("gosRenderer::init mineStaticProg");
        static const char* kMinePrefix = "#version 430\n";
        mine_static_prog_ = glsl_program::makeProgram(
            "gos_terrain_mine_static",
            "shaders/gos_terrain_mine_static.vert",
            "shaders/gos_terrain_mine_static.frag",
            kMinePrefix);
        if (!mine_static_prog_ || !mine_static_prog_->shp_)
            fprintf(stderr, "[MINE_STATIC] WARNING: failed to compile mine static-bake shader"
                            " — MC2_TERRAIN_INDIRECT_MINE=1 will be a no-op\n");
        else
            printf("[MINE_STATIC] Mine static-bake shader loaded: prog=%u\n",
                   (unsigned)mine_static_prog_->shp_);
        fflush(stdout);
    }

    // B4 Slice Stage 1b — load mask-SOLID program.
    // Pairs the new mask-SOLID VS with the existing terrain frag, reusing all
    // PBR/shadow/atlas logic from gos_terrain.frag.
    {
        ZoneScopedN("gosRenderer::init maskSolidProg");
        std::string kMaskSolidPrefix = "#version 430\n";
        if (terrainNormalArrayEnabled())
            kMaskSolidPrefix += "#define TERRAIN_NORMAL_ARRAY\n";
        mask_solid_prog_ = glsl_program::makeProgram(
            "gos_terrain_mask_solid",
            "shaders/gos_terrain_mask_solid.vert",
            "shaders/gos_terrain.frag",
            kMaskSolidPrefix.c_str());
        if (!mask_solid_prog_ || !mask_solid_prog_->shp_)
            fprintf(stderr, "[MASK_SOLID] WARNING: failed to compile mask-SOLID shader"
                            " — MC2_TERRAIN_MASK_DISPATCH=1 SOLID draw disabled\n");
        else
            printf("[MASK_SOLID] Mask-SOLID shader loaded: prog=%u\n",
                   (unsigned)mask_solid_prog_->shp_);
        fflush(stdout);
    }

    // B4 Slice Stage 1c — load mask-water program.
    // Pairs the mask-water VS (reads water mask + WaterRecipe) with
    // gos_tex_vertex.frag (same frag as the water fast path).
    {
        ZoneScopedN("gosRenderer::init maskWaterProg");
        static const char* kMaskWaterPrefix = "#version 430\n";
        mask_water_prog_ = glsl_program::makeProgram(
            "gos_terrain_mask_water",
            "shaders/gos_terrain_mask_water.vert",
            "shaders/gos_tex_vertex.frag",
            kMaskWaterPrefix);
        if (!mask_water_prog_ || !mask_water_prog_->shp_)
            fprintf(stderr, "[MASK_WATER] WARNING: failed to compile mask-water shader"
                            " — MC2_TERRAIN_MASK_DISPATCH=1 water draw disabled\n");
        else
            printf("[MASK_WATER] Mask-water shader loaded: prog=%u\n",
                   (unsigned)mask_water_prog_->shp_);
        fflush(stdout);
    }

    // Load water fast-path program (Stage 2 of renderWater architectural slice).
    // Pairs the new VS that consumes WaterRecipe + WaterFrame SSBOs with the
    // existing gos_tex_vertex.frag (preserves pixel-stable water visuals).
    {
        ZoneScopedN("gosRenderer::init waterFastProg");
        static const char* kWaterFastPrefix = "#version 430\n";
        water_fast_prog_ = glsl_program::makeProgram(
            "gos_terrain_water_fast",
            "shaders/gos_terrain_water_fast.vert",
            "shaders/gos_tex_vertex.frag",
            kWaterFastPrefix);
        if (!water_fast_prog_ || !water_fast_prog_->shp_)
            fprintf(stderr, "[WATER_FAST] WARNING: failed to compile water-fast shader"
                            " — MC2_RENDER_WATER_FASTPATH=1 will fall back to legacy\n");
        else
            printf("[WATER_FAST] Water-fast shader loaded: prog=%u\n",
                   (unsigned)water_fast_prog_->shp_);
        fflush(stdout);
    }

    // Load world-space overlay shaders and create VAOs/VBOs.
    // Both batches share terrain_overlay.vert; decal uses a different frag.
    // Use glsl_program::makeProgram directly (not gosRenderMaterial::load) because
    // the material loader always pairs [name].vert + [name].frag from the same name.
    {
        ZoneScopedN("gosRenderer::init overlayPrograms");
        // Without a #version prefix NVIDIA defaults to GLSL 1.10 and rejects
        // layout(location=...) and in/out attribute qualifiers, breaking
        // these programs at init on NVIDIA drivers. AMD accepts the same
        // source by silently promoting to a newer GLSL version.
        // TERRAIN-DECAL-SHADOW-CSM: inject the same CSM define the terrain
        // material does (gameos_graphics.cpp:352) so terrain_overlay.frag /
        // decal.frag compile shadow.hglsl's CSM variant of calcDynamicShadow.
        // Without it they used the legacy branch and the C++ CSM uniform binds
        // (setupOverlayShadowsForShp) silently dropped (loc==-1) -> no dynamic
        // shadow on cement overlay or decals. Built locally so unrelated
        // programs aren't recompiled (kShaderPrefix was local to these two).
        // MC2_SHADOW_CSM_MAX must equal terrain's count source so the
        // dynamicCascadeMatrices[] array stride matches the upload count.
        std::string overlayPrefix = "#version 430\n";
        if (mc2ShadowCsmEnabled()) {
            char csmDef[64];
            snprintf(csmDef, sizeof(csmDef),
                     "#define MC2_SHADOW_CSM 1\n#define MC2_SHADOW_CSM_MAX %d\n",
                     mc2ShadowCsmCount());
            overlayPrefix.append(csmDef);
        }
        overlayProg_ = glsl_program::makeProgram("terrain_overlay",
            "shaders/terrain_overlay.vert", "shaders/terrain_overlay.frag",
            overlayPrefix.c_str());
        decalProg_ = glsl_program::makeProgram("decal",
            "shaders/terrain_overlay.vert", "shaders/decal.frag",
            overlayPrefix.c_str());
    }

    if (!overlayProg_)
        fprintf(stderr, "[OverlayBatch] Failed to compile terrain_overlay shader\n");
    if (!decalProg_)
        fprintf(stderr, "[OverlayBatch] Failed to compile decal shader\n");

    // Cache uniform locations for both shaders
    auto cacheOverlayLocs = [](glsl_program* prog, OverlayUniformLocs_& locs) {
        if (!prog) return;
        GLuint shp = prog->shp_;
        locs.terrainMVP      = glGetUniformLocation(shp, "u_worldToClipGL");
        locs.tex1            = glGetUniformLocation(shp, "tex1");
        locs.fog_color       = glGetUniformLocation(shp, "fog_color");
        locs.time            = glGetUniformLocation(shp, "time");
        locs.cameraPos       = glGetUniformLocation(shp, "cameraPos");
        locs.surfaceDebugMode = glGetUniformLocation(shp, "surfaceDebugMode");
        locs.pathTint        = glGetUniformLocation(shp, "u_pathTint");
        locs.terrainLightDir = glGetUniformLocation(shp, "terrainLightDir");
        locs.mapHalfExtent   = glGetUniformLocation(shp, "mapHalfExtent");
        // TERRAIN-DECAL-LIGHTING-1a — populated only on overlayProg_ (the
        // cement transition shader); decalProg_'s decal.frag does not
        // declare these uniforms so the locs stay -1 there. The helper at
        // upload time skips negative locs unconditionally.
        locs.terrainHeightTex                 = glGetUniformLocation(shp, "terrainHeightTex");
        locs.terrainHeightParams              = glGetUniformLocation(shp, "terrainHeightParams");
        locs.useTerrainNormalsFromHeight      = glGetUniformLocation(shp, "useTerrainNormalsFromHeight");
        locs.terrainNormalsFromHeightStrength = glGetUniformLocation(shp, "terrainNormalsFromHeightStrength");
        locs.terrainLightingV1Strength        = glGetUniformLocation(shp, "terrainLightingV1Strength");
        locs.terrainLightingV2ShadowFillFloor = glGetUniformLocation(shp, "terrainLightingV2ShadowFillFloor");
        // ROAD-PBR-ASPHALT-1: asphalt material uniforms (overlayProg_ only).
        locs.asphaltAlbedo   = glGetUniformLocation(shp, "u_asphaltAlbedo");
        locs.matNormalArray  = glGetUniformLocation(shp, "matNormalArray");
        locs.asphaltScale    = glGetUniformLocation(shp, "u_asphaltScale");
        // ROAD-MATERIAL-GRAVEL-1: gravel albedo sampler (overlayProg_ only).
        locs.gravelAlbedo    = glGetUniformLocation(shp, "u_gravelAlbedo");
    };
    { ZoneScopedN("gosRenderer::init overlayUniforms"); cacheOverlayLocs(overlayProg_, overlayLocs_); cacheOverlayLocs(decalProg_, decalLocs_); }
    timeStart_ = timing::get_wall_time_ms();

    // Create VBO/VAO for each batch.
    // WorldOverlayVert layout (stride 28 bytes):
    //   offset  0: vec3 worldPos    → attrib location 0
    //   offset 12: vec2 texcoord    → attrib location 1
    //   offset 20: float fog        → attrib location 2
    //   offset 24: uint8[4] argb    → attrib location 3, GL_UNSIGNED_BYTE, normalized
    auto makeOverlayVAO = [](OverlayBatch_& batch) {
        constexpr int kStride = 28;  // sizeof(WorldOverlayVert)
        glGenBuffers(1, &batch.vbo);
        glGenVertexArrays(1, &batch.vao);
        glBindVertexArray(batch.vao);
        glBindBuffer(GL_ARRAY_BUFFER, batch.vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT,         GL_FALSE, kStride, (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, kStride, (void*)12);
        glVertexAttribPointer(2, 1, GL_FLOAT,         GL_FALSE, kStride, (void*)20);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,  kStride, (void*)24);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    };
    { ZoneScopedN("gosRenderer::init overlayVAOs"); makeOverlayVAO(terrainOverlayBatch_); makeOverlayVAO(decalBatch_); }

    if (!TerrainPatchStream::init()) {
        // init_fail path lives in Task 2; for now this is dead code.
        fprintf(stderr, "[PATCH_STREAM v1] event=init_fail reason=task1_skeleton_returned_false\n");
        fflush(stderr);
    }

    gos_TerrainLodChunk_Init();
}

void gosRenderer::destroy() {
    gos_TerrainLodChunk_Destroy();
    TerrainPatchStream::destroy();

    gosMesh::destroy(quads_);
    gosMesh::destroy(tris_);
    gosMesh::destroy(indexed_tris_);
    gosMesh::destroy(lines_);
    gosMesh::destroy(points_);
    gosMesh::destroy(text_);

    for(size_t i=0; i<fontList_.size(); i++) {
        gosRenderMaterial::destroy(materialList_[i]);
    }
    materialList_.clear();

    // delete fonts before textures, because they refer them
    for(size_t i=0; i<fontList_.size(); i++) {
        while(gosFont::destroy(fontList_[i])) {};
    }
    fontList_.clear();

    for(size_t i=0; i<textureList_.size(); i++) {
        delete textureList_[i];
    }
    textureList_.clear();

    // Terrain tessellation cleanup
    delete[] terrain_extra_data_;
    terrain_extra_data_ = nullptr;
    if (terrain_extra_vb_) { glDeleteBuffers(1, &terrain_extra_vb_); terrain_extra_vb_ = 0; }
    if (terrain_material_) { gosRenderMaterial::destroy(terrain_material_); terrain_material_ = nullptr; }
    if (terrain_normal_array_tex_ != 0) {
        glDeleteTextures(1, &terrain_normal_array_tex_);
        terrain_normal_array_tex_ = 0;
    }

    glDeleteVertexArrays(1, &gVAO);

}

void gosRenderer::initRenderStates() {
    // RENDER_STATES v1: zero-init the entire arrays so the equality check has
    // a defined comparison for states that initRenderStates does not assign
    // explicitly (Terrain, Water, Overlay, IsHUD, Texture3). Without this,
    // uninitialized members at non-explicit indices could differ between
    // curStates_ and renderStates_ on every call and defeat the early-out.
    memset(&curStates_, 0, sizeof(curStates_));
    memset(&renderStates_, 0, sizeof(renderStates_));

	renderStates_[gos_State_Texture] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Texture2] = INVALID_TEXTURE_ID;
    renderStates_[gos_State_Texture3] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Filter] = gos_FilterNone;
	renderStates_[gos_State_ZCompare] = 1; 
    renderStates_[gos_State_ZWrite] = 1;
	renderStates_[gos_State_AlphaTest] = 0;
	renderStates_[gos_State_Perspective] = 1;
	renderStates_[gos_State_Specular] = 0;
	renderStates_[gos_State_Dither] = 0;
	renderStates_[gos_State_Clipping] = 0;	
	renderStates_[gos_State_WireframeMode] = 0;
	renderStates_[gos_State_AlphaMode] = gos_Alpha_OneZero;
	renderStates_[gos_State_TextureAddress] = gos_TextureWrap;
	renderStates_[gos_State_ShadeMode] = gos_ShadeGouraud;
	renderStates_[gos_State_TextureMapBlend] = gos_BlendModulateAlpha;
	renderStates_[gos_State_MipMapBias] = 0;
	renderStates_[gos_State_Fog]= 0;
	renderStates_[gos_State_MonoEnable] = 0;
	renderStates_[gos_State_Culling] = gos_Cull_None;
	renderStates_[gos_State_StencilEnable] = 0;
	renderStates_[gos_State_StencilFunc] = gos_Cmp_Never;
	renderStates_[gos_State_StencilRef] = 0;
	renderStates_[gos_State_StencilMask] = 0xffffffff;
	renderStates_[gos_State_StencilZFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilPass] = gos_Stencil_Keep;
	renderStates_[gos_State_Multitexture] = gos_Multitexture_None;
	renderStates_[gos_State_Ambient] = 0xffffff;
	renderStates_[gos_State_Lighting] = 0;
	renderStates_[gos_State_NormalizeNormals] = 0;
	renderStates_[gos_State_VertexBlend] = 0;

    applyRenderStates();
    renderStatesStackPointer = -1;
}

void gosRenderer::pushRenderStates()
{
    gosASSERT(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1);
    if(!(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1)) {
        return;
    }

    renderStatesStackPointer++;
    memcpy(&statesStack_[renderStatesStackPointer], &renderStates_, sizeof(renderStates_));
}

void gosRenderer::popRenderStates()
{
    gosASSERT(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE);
    
    if(!(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE)) {
        return;
    }

    memcpy(&renderStates_, &statesStack_[renderStatesStackPointer], sizeof(renderStates_));
    renderStatesStackPointer--;
}

// RENDER_STATES v1: kill-switch (default-off → short-circuit path runs).
// MC2_RENDERSTATES_LEGACY=1 forces unconditional apply behavior.
static bool gos_renderStatesLegacyEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_RENDERSTATES_LEGACY");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

void gosRenderer::applyRenderStates() {
    ZoneScopedN("ApplyRenderStates");

    rsCalls_++;

    // RENDER_STATES v1: state-equality early-out.
    // Auto-reset of Terrain/Water flags must run every call regardless (these are
    // single-frame "intent" bits cleared after read). gos_State_Overlay is copied,
    // not auto-reset — managed by renderLists. We therefore split the auto-reset
    // out so it always runs, then test the rest of state for equality.
    const bool legacy = gos_renderStatesLegacyEnabled();
    if (!legacy && stateCacheValid_) {
        bool equal = true;
        int mismatchK = -1;
        uint32_t mismatchCur = 0, mismatchRen = 0;
        // Iterate valid enum values [gos_State_Texture .. gos_MaxState). Index 0
        // is not a valid enum (gos_State_Texture==1) and is never written by
        // initRenderStates(), so its value is indeterminate and may differ
        // between curStates_ / renderStates_ — skip it.
        for (int k = gos_State_Texture; k < gos_MaxState; ++k) {
            if (k == gos_State_Texture || k == gos_State_Texture2 || k == gos_State_Texture3) {
                continue; // texture handles mutate per-frame; checked separately below
            }
            // Terrain/Water are auto-reset bits — renderStates_[k] is cleared after
            // each apply, so curStates_[k] will diverge if a caller set it; that is
            // exactly the signal we want to force a full apply through.
            if (curStates_[k] != renderStates_[k]) {
                equal = false;
                mismatchK = k;
                mismatchCur = curStates_[k];
                mismatchRen = renderStates_[k];
                break;
            }
        }
        // Optional diagnostic: print the first 8 mismatch reasons. Env-gated,
        // off by default per Debug Instrumentation Rule (keep, demote, don't
        // delete). MC2_RENDERSTATES_TRACE=1 to enable.
        static const bool s_rsTrace = (getenv("MC2_RENDERSTATES_TRACE") != nullptr);
        static int s_mismatchPrints = 0;
        if (s_rsTrace && !equal && s_mismatchPrints < 8) {
            s_mismatchPrints++;
            printf("[RENDER_STATES v1] event=mismatch_first idx=%d cur=%u ren=%u\n",
                   mismatchK, mismatchCur, mismatchRen);
            fflush(stdout);
        }
        if (equal) {
            // Resolved-handle parity: check the underlying GL texture id, not the
            // gos handle slot (handles mutate per-frame via tex_resolve).
            uint32_t tex_states[] = { gos_State_Texture, gos_State_Texture2, gos_State_Texture3 };
            for (int i = 0; i < 3 && equal; ++i) {
                DWORD h = renderStates_[tex_states[i]];
                gosTexture* tex = (h == INVALID_TEXTURE_ID) ? nullptr : this->getTexture(h);
                uint32_t glId = tex ? (uint32_t)tex->getTextureId() : 0u;
                if (glId != cachedResolvedTexId_[i]) { equal = false; break; }
                if (curStates_[tex_states[i]] != h) { equal = false; break; }
            }
            if (equal) {
                // Skip GL work. Still run auto-reset bookkeeping so Terrain/Water
                // intent bits don't bleed into the next frame.
                curStates_[gos_State_Terrain] = renderStates_[gos_State_Terrain];
                renderStates_[gos_State_Terrain] = 0;
                curStates_[gos_State_Water]   = renderStates_[gos_State_Water];
                renderStates_[gos_State_Water]   = 0;
                curStates_[gos_State_Overlay] = renderStates_[gos_State_Overlay];
                rsSkipped_++;
                return;
            }
        }
    }

    rsApplied_++;

	////////////////////////////////////////////////////////////////////////////////
	switch (renderStates_[gos_State_Culling]) {
		case gos_Cull_None: glDisable(GL_CULL_FACE); break;
		case gos_Cull_CW:	
		case gos_Cull_CCW:
			glEnable(GL_CULL_FACE);
			// by default in OpenGL front face is CCW (could be changed by glFrontFace)
			glCullFace(renderStates_[gos_State_Culling] == gos_Cull_CW ? GL_BACK : GL_FRONT);
			break;
		default: gosASSERT(0 && "Wrong cull face value");
	}
	curStates_[gos_State_Culling] = renderStates_[gos_State_Culling];

	////////////////////////////////////////////////////////////////////////////////

	////////////////////////////////////////////////////////////////////////////////
	fog_color_ = uint32_to_vec4(renderStates_[gos_State_Fog]);
	curStates_[gos_State_Fog] = renderStates_[gos_State_Fog];

   ////////////////////////////////////////////////////////////////////////////////
   switch(renderStates_[gos_State_ZWrite]) {
       case 0: glDepthMask(GL_FALSE); break;
       case 1: glDepthMask(GL_TRUE); break;
       default: gosASSERT(0 && "Wrong depth write value");
   }
   curStates_[gos_State_ZWrite] = renderStates_[gos_State_ZWrite];

   ////////////////////////////////////////////////////////////////////////////////
   if(0 == renderStates_[gos_State_ZCompare]) {
       glDisable(GL_DEPTH_TEST);
   } else {
       glEnable(GL_DEPTH_TEST);
   }
   // Reverse-Z (U2): gos_State_ZCompare is the SCENE depth-compare bridge.
   // All gos_State_ZCompare writers (incl. txmmgr.cpp:2316/2327 and
   // objmgr.cpp:1826 legacy CPU blob-shadow quads, which depth-test
   // against the SCENE depth buffer) are scene-space; the ortho
   // shadow-MAP passes never route through gos_State_ZCompare (they set
   // glDepthFunc(GL_LESS) via the literal path in gameos_graphics.cpp
   // 4436/4580/4650 + gos_postprocess.cpp 1139/1163 under their own FBO).
   // So the scene remap applies globally here; no shadow carve-out.
   switch(renderStates_[gos_State_ZCompare]) {
       case 0: glDepthFunc(GL_ALWAYS);  break;   // unchanged
       case 1: glDepthFunc(GL_GEQUAL);  break;   // reverse-Z: was GL_LEQUAL
       case 2: glDepthFunc(GL_GREATER); break;   // reverse-Z: was GL_LESS
       default: gosASSERT(0 && "Wrong depth test value");
   }
   curStates_[gos_State_ZCompare] = renderStates_[gos_State_ZCompare];

   ////////////////////////////////////////////////////////////////////////////////
   bool disable_blending = renderStates_[gos_State_AlphaMode] == gos_Alpha_OneZero;
   if(disable_blending) {
       glDisable(GL_BLEND);
   } else {
       glEnable(GL_BLEND);
   }
   switch(renderStates_[gos_State_AlphaMode]) {
       case gos_Alpha_OneZero:          glBlendFunc(GL_ONE, GL_ZERO); break;
       case gos_Alpha_OneOne:           glBlendFunc(GL_ONE, GL_ONE); break;
       case gos_Alpha_AlphaInvAlpha:    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_OneInvAlpha:      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_AlphaOne:         glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
       default: gosASSERT(0 && "Wrong alpha mode value");
   }
   curStates_[gos_State_AlphaMode] = renderStates_[gos_State_AlphaMode];

   ////////////////////////////////////////////////////////////////////////////////
   #if 0 // now in shaders (this is not supported in CORE OpenGL profile
   bool enable_alpha_test = renderStates_[gos_State_AlphaTest] == 1;
   if(enable_alpha_test) {
       glEnable(GL_ALPHA_TEST);
       glAlphaFunc(GL_NOTEQUAL, 0.0f);
   } else {
       glDisable(GL_ALPHA_TEST);
   }
   #endif
   curStates_[gos_State_AlphaTest] = renderStates_[gos_State_AlphaTest];

   ////////////////////////////////////////////////////////////////////////////////
   TexFilterMode filter = TFM_NONE;
   switch(renderStates_[gos_State_Filter]) {
       case gos_FilterNone: filter = TFM_NEAREST; break;
       case gos_FilterBiLinear : filter = TFM_LINEAR; break;
       case gos_FilterTriLinear: filter = TFM_LNEAR_MIPMAP_LINEAR; break;
   }
   // no mips for now, so ensure no invalid filters used
   //gosASSERT(filter == TFM_NEAREST || filter == TFM_LINEAR);
   // i do not know of any mipmaps that we are using
   if(filter == TFM_LNEAR_MIPMAP_LINEAR)
       filter = TFM_LINEAR;

   // in this case does not necessaily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_Filter] = renderStates_[gos_State_Filter];
  
   ////////////////////////////////////////////////////////////////////////////////
   TexAddressMode address_mode = 
       renderStates_[gos_State_TextureAddress] == gos_TextureWrap ? TAM_REPEAT : TAM_CLAMP_TO_EDGE;
   // in this case does not necessarily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_TextureAddress] = renderStates_[gos_State_TextureAddress];

   ////////////////////////////////////////////////////////////////////////////////
   uint32_t tex_states[] = { gos_State_Texture, gos_State_Texture2, gos_State_Texture3 };
   for(int i=0; i<sizeof(tex_states) / sizeof(tex_states[0]); ++i) {
       DWORD gosTextureHandle = renderStates_[tex_states[i]];

       glActiveTexture(GL_TEXTURE0 + i);

       gosTexture* tex = gosTextureHandle == INVALID_TEXTURE_ID ? 0 : this->getTexture(gosTextureHandle);
       if(tex) {
           const uint32_t glId = (uint32_t)tex->getTextureId();
           glBindTexture(GL_TEXTURE_2D, glId);
           setSamplerParams(tex->getTextureType(), address_mode, filter);
           cachedResolvedTexId_[i] = glId;

           gosTextureInfo texinfo;
           tex->getTextureInfo(&texinfo);
           if(renderStates_[gos_State_TextureMapBlend] == gos_BlendDecal && texinfo.format_ == gos_Texture_Alpha)
           {
               PAUSE((""));
           }

       } else {
           glBindTexture(GL_TEXTURE_2D, 0);
           cachedResolvedTexId_[i] = 0u;
       }
       curStates_[tex_states[i]] = gosTextureHandle;
   }

   ////////////////////////////////////////////////////////////////////////////////
   // RENDER_STATES v1: snapshot the full renderStates_ array into curStates_ so
   // the equality check has a coherent "last applied" reference for ALL state
   // dimensions. Many states (Perspective, Specular, ShadeMode, TextureMapBlend,
   // ...) aren't applied to GL at all (commented "now in shaders") but are still
   // mutated by callers per-shape — without this snapshot they'd never match
   // and the early-out would always fail.
   memcpy(&curStates_, &renderStates_, sizeof(curStates_));

   // Terrain tessellation flag — auto-reset to prevent bleed to non-terrain
   // draws. The snapshot above already copied the true value; clear renderStates_
   // AFTER so the next call sees curStates_[Terrain]=value, renderStates_=0 and
   // forces a full apply if a caller re-sets Terrain.
   renderStates_[gos_State_Terrain] = 0;
   renderStates_[gos_State_Water]   = 0;
   // Overlay is NOT auto-reset — managed by renderLists loop. Snapshot above
   // already mirrored its value.

   // RENDER_STATES v1: full-apply complete — cache is now coherent with GL.
   stateCacheValid_ = true;
}

void gosRenderer::beginFrame()
{
    // RENDER_STATES v1: defensive cache invalidate at frame start. SDL window
    // swap, ImGui (if added), or any external GL hook between frames could
    // disturb GL state without the cache knowing. Cost is one redundant full
    // apply per frame at startup; the [RENDER_STATES v1] summary surfaces any
    // unexpected impact. MINOR-1 from the 2026-05-08 adversarial review.
    invalidateRenderStateCache();

    // Frame-boundary hygiene: IsHUD must be cleared by every callsite before the frame ends.
    // If it is still set here, a callsite leaked the bit across the frame boundary.
    if (renderStates_[gos_State_IsHUD] != 0) {
        SPEW(("GRAPHICS", "[HUD] gos_State_IsHUD still set at frame start -- callsite leak\n"));
        renderStates_[gos_State_IsHUD] = 0;
    }
    hudBatch_.clear();
    hudFlushed_ = false;
    glBindVertexArray(gVAO);
    num_draw_calls_ = 0;

    TerrainPatchStream::beginFrame();
    // gos_terrain_indirect::BeginFrame() moved to endFrame() — see comment there.
}

// Lazy-eval gate for the dev-only shader hot-reload sweep. Default OFF in
// shipping; set MC2_SHADER_HOT_RELOAD=1 to opt in for shader iteration.
// Pattern matches gos_terrain_indirect::IsEnabled() — static-init-order safe.
// Diagnostic 2026-05-07: the unconditional sweep showed 1.82 ms self-time
// inside Camera.UpdateRenderers/gos_RendererEndFrame in Tracy because
// last_check_time was never updated, so once 500 ms elapsed checkReload()
// ran every frame over materialList_.
static bool gos_ShaderHotReloadEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_SHADER_HOT_RELOAD");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}

void gosRenderer::endFrame()
{
    // Clear the GPU-terrain arm AFTER renderLists() has consumed it this frame.
    // Must be end-of-frame, not begin-of-frame: DoGameLogic() (which calls
    // Terrain::geometry() → ComputePreflight() → arm) runs BEFORE draw_screen()
    // (which calls beginFrame() then renderLists()). Placing the reset in
    // beginFrame() wiped the arm before renderLists() could see it, causing
    // permanent black terrain. Menu / mech-bay frames never call ComputePreflight(),
    // so they never set the arm; the end-of-frame clear is a no-op for them.
    gos_terrain_indirect::BeginFrame();

    // GPU-BUFFER-WRAPPER-TIER0-HUD-1 (acceptance #4): residency dump of the live
    // HUD rings, throttled. Gated by MC2_GPUBUF_RESIDENCY; no-op otherwise.
    if (gosHudRingResidency()) {
        static uint32_t s_resCountdown = 0;
        if (s_resCountdown == 0) {
            s_resCountdown = 300;
            gosMesh* meshes[] = { quads_, tris_, indexed_tris_, lines_, points_, text_ };
            for (gosMesh* m : meshes) {
                if (m && m->ringIfLive()) m->ringIfLive()->reportResidency();
            }
        }
        --s_resCountdown;
    }

    // RENDER_STATES v1: 600-frame summary line. Always-on counter; gated print.
    rsFrames_++;
    if (rsFrames_ >= 600) {
        printf("[RENDER_STATES v1] event=summary frames=%u calls=%u skipped_count=%u applied_count=%u\n",
               rsFrames_, rsCalls_, rsSkipped_, rsApplied_);
        fflush(stdout);
        rsFrames_ = 0;
        rsCalls_ = 0;
        rsSkipped_ = 0;
        rsApplied_ = 0;
    }

    if (!gos_ShaderHotReloadEnabled())
        return;

    // check for file changes every half second
    static uint64_t last_check_time = timing::get_wall_time_ms();
    const uint64_t now = timing::get_wall_time_ms();
    if(now - last_check_time > 500)
    {
        for(int i=0; i< materialList_.size(); ++i)
        {
            materialList_[i]->checkReload();
        }
        // Originally absent: without reassigning last_check_time the
        // condition stayed true every frame after the first 500 ms,
        // turning a 2 Hz cadence into a per-frame O(materialList_.size())
        // sweep. Diagnosed via [RENDER_STATES v1] / Tracy 2026-05-07.
        last_check_time = now;
    }
}

void gosRenderer::handleEvents()
{
    if(pendingRequest) {
        ZoneScopedN("gosRenderer::handleEvents pendingRequest");

        width_ = reqWidth;
        height_ = reqHeight;

        // x = 1/w; x =2*x - 1;
        // y = 1/h; y= 1- y; y =2*y - 1;
        // z = z;
        projection_ = mat4(2.0f / (float)width_, 0, 0.0f, -1.0f,
                0, -2.0f / (float)height_, 0.0f, 1.0f,
                0, 0, 1.0f, 0.0f,
                0, 0, 0.0f, 1.0f);

        {
        ZoneScopedN("gosRenderer::handleEvents resizeWindow");
        if(graphics::resize_window(win_h_, width_, height_))
		{
            { ZoneScopedN("gosRenderer::handleEvents fullscreen"); graphics::set_window_fullscreen(win_h_, reqGotoFullscreen); }

            Environment.screenWidth = width_;
            Environment.screenHeight = height_;

			{ ZoneScopedN("gosRenderer::handleEvents drawableSize"); graphics::get_drawable_size(win_h_, &Environment.drawableWidth, &Environment.drawableHeight); }

        }
        }
        pendingRequest = false;
    }
}

bool gosRenderer::beforeDrawCall()
{
    num_draw_calls_++;
    if(break_draw_call_num_ == num_draw_calls_ && break_on_draw_call_) {
        PAUSE(("Draw call %d break\n", num_draw_calls_ - 1));
    }

    return (num_draw_calls_ > num_draw_calls_to_draw_) && num_draw_calls_to_draw_ != 0;
}

void gosRenderer::afterDrawCall()
{
}

gosRenderMaterial* gosRenderer::selectBasicRenderMaterial(const RenderState& rs) const
{
	ZoneScopedN("SelectBasicMaterial");
	const auto& sh_var = rs[gos_State_Texture]!=0 ?
		materialDB_.find("gos_tex_vertex")->second :
		materialDB_.find("gos_vertex")->second;
    uint32_t flags = rs[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;
    if (rs[gos_State_Overlay])
        flags |= SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::IS_OVERLAY);

    if(sh_var.count(flags))
        return sh_var.at(flags);
    else
    {
        STOP(("Trying to get variation which does not exist: shader: %s flags: %d\n", "basic", flags));
        return nullptr;
    }
}

gosRenderMaterial* gosRenderer::selectLightedRenderMaterial(const RenderState& rs) const
{
	const auto& sh_var = rs[gos_State_Texture]!=0 ?
		materialDB_.find("gos_tex_vertex_lighted")->second :
		materialDB_.find("gos_vertex_lighted")->second;
    uint32_t flags = rs[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;

    if(sh_var.count(flags))
        return sh_var.at(flags);
    else
    {
        STOP(("Trying to get variation which does not exist: shader: %s flags: %d\n", "lighted", flags));
        return nullptr;
    }
}

void gosRenderer::drawQuads(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawQuads");
    gosASSERT(vertices);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawQuads discarded (after flushHUDBatch)\n"));
            return;
        }
        HudDrawCall call;
        call.kind = kHudQuadBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        call.scaleExempt = s_hud_scale_exempt;
        hudBatch_.push_back(std::move(call));
        return;
    }

    if(beforeDrawCall()) return;

    int num_quads = count / 4;
    int num_vertices = num_quads * 6;

    if(quads_->getNumVertices() + num_vertices > quads_->getVertexCapacity()) {
        applyRenderStates();
        gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
        gosASSERT(mat);

        mat->setTransform(projection_);
        mat->setFogColor(fog_color_);
        quads_->draw(mat);
        quads_->rewind();
    } 

    gosASSERT(quads_->getNumVertices() + num_vertices <= quads_->getVertexCapacity());
    for(int i=0; i<count;i+=4) {

        quads_->addVertices(vertices + 4*i + 0, 1);
        quads_->addVertices(vertices + 4*i + 1, 1);
        quads_->addVertices(vertices + 4*i + 2, 1);

        quads_->addVertices(vertices + 4*i + 0, 1);
        quads_->addVertices(vertices + 4*i + 2, 1);
        quads_->addVertices(vertices + 4*i + 3, 1);
    }

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();

    gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
    gosASSERT(mat);

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    quads_->draw(mat);
    quads_->rewind();

    afterDrawCall();
}

void gosRenderer::drawLines(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawLines");
    gosASSERT(vertices);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawLines discarded (after flushHUDBatch)\n"));
            return;
        }
        HudDrawCall call;
        call.kind = kHudLineBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        call.scaleExempt = s_hud_scale_exempt;
        hudBatch_.push_back(std::move(call));
        return;
    }

    if(beforeDrawCall()) return;

    if(lines_->getNumVertices() + count > lines_->getVertexCapacity()) {
        applyRenderStates();
        basic_material_->setTransform(projection_);
        basic_material_->setFogColor(fog_color_);
        lines_->draw(basic_material_);
        lines_->rewind();
    }

    gosASSERT(lines_->getNumVertices() + count <= lines_->getVertexCapacity());
    lines_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    basic_material_->setTransform(projection_);
    basic_material_->setFogColor(fog_color_);
    lines_->draw(basic_material_);
    lines_->rewind();

    afterDrawCall();
}

void gosRenderer::drawPoints(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawPoints");
    gosASSERT(vertices);

    if(beforeDrawCall()) return;

    if(points_->getNumVertices() + count > points_->getVertexCapacity()) {
        applyRenderStates();
        basic_material_->setTransform(projection_);
		basic_material_->setFogColor(fog_color_);
        points_->draw(basic_material_);
        points_->rewind();
    } 

    gosASSERT(points_->getNumVertices() + count <= points_->getVertexCapacity());
    points_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    points_->draw(basic_material_);
    points_->rewind();

    afterDrawCall();
}

void gosRenderer::drawTris(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawTris");
    gosASSERT(vertices);

    gosASSERT((count % 3) == 0);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawTris discarded (after flushHUDBatch)\n"));
            return;
        }
        HudDrawCall call;
        call.kind = kHudTriBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        call.scaleExempt = s_hud_scale_exempt;
        hudBatch_.push_back(std::move(call));
        return;
    }

    if(beforeDrawCall()) return;


    if(tris_->getNumVertices() + count > tris_->getVertexCapacity()) {
        applyRenderStates();

        gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
        gosASSERT(mat);

        mat->setTransform(projection_);
		mat->setFogColor(fog_color_);
        tris_->draw(mat);
        tris_->rewind();
    } 

    gosASSERT(tris_->getNumVertices() + count <= tris_->getVertexCapacity());
    tris_->addVertices(vertices, count);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();

    gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
    gosASSERT(mat);

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    tris_->draw(mat);
    tris_->rewind();

    afterDrawCall();
}

// ---------------------------------------------------------------------------
// SHADOW-STABILITY-1: explicit shadow-pass GL-state brackets + env-gated trace.
//
// The shadow passes are FORWARD-Z (glClearDepth(1), GL_LESS) bolted onto the
// main REVERSE-Z scene (glClearDepth(0), GL_GEQUAL). The boundary is a manual
// glClearDepth swap with no guard — same state-leak bug class as the
// transparency depth-write fix and the 10.3 terrain-transparency saga. These
// helpers capture the caller's GL state at pass entry, let the pass set what it
// needs, then RESTORE the captured values at exit so nothing leaks into the
// reverse-Z scene pass. They do NOT introduce a GlStateGuard framework — they
// only read/restore via raw GL queries at the four pass boundaries.
//
// (declared in gos_static_prop_batcher.h; forward-declared here to avoid
//  pulling that header into this TU)
void gos_GetStaticBuildingShadowCounts(int& types, int& inst, int& draws);
//
// Trace is gated on MC2_SHADOW_STATE_TRACE=1: default OFF = zero lines, and the
// capture/restore is a handful of glGetIntegerv/glIs* per pass (no hot-loop or
// per-element cost). restored=1 means entry-state == exit-state for every
// tracked field; restored=0 flags a real state leak.
// ---------------------------------------------------------------------------
namespace {
struct ShadowPassGLState {
    GLint    fbo = 0;
    GLint    viewport[4] = {0,0,0,0};
    GLfloat  clearDepth = 0.0f;
    GLboolean depthTest = GL_FALSE;
    GLint    depthFunc = 0;
    GLboolean depthMask = GL_TRUE;
    GLboolean colorMask[4] = {GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
    GLboolean cullEnabled = GL_FALSE;
    GLint    cullFace = 0;
    GLint    frontFace = 0;
    // GLSTATE-SHADOW-CLIP-RESTORE-1: NVIDIA can silently reset glClipControl on FBO
    // switch (reverse-Z scene uses GL_ZERO_TO_ONE; shadow FBO bind can flip it back).
    GLint    clipOrigin = GL_LOWER_LEFT;
    GLint    clipDepth  = GL_ZERO_TO_ONE;
};

// Entry-state captures for the two (non-nesting) shadow passes, held across
// begin->end so the exit can restore + verify against the matching entry.
static ShadowPassGLState s_staticPassEntry;
static ShadowPassGLState s_dynamicPassEntry;

static bool shadowStateTraceEnabled() {
    static int s_on = -1;
    if (s_on < 0) {
        const char* v = std::getenv("MC2_SHADOW_STATE_TRACE");
        s_on = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return s_on != 0;
}

static void captureShadowGLState(ShadowPassGLState& s) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s.clearDepth);
    s.depthTest = glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    s.cullEnabled = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CULL_FACE_MODE, &s.cullFace);
    glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
    glGetIntegerv(GL_CLIP_ORIGIN,     &s.clipOrigin);
    glGetIntegerv(GL_CLIP_DEPTH_MODE, &s.clipDepth);
}

// Restore every tracked field to the captured values. Called at pass exit so
// the reverse-Z scene pass inherits exactly what it had before the shadow pass.
static void restoreShadowGLState(const ShadowPassGLState& s) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s.fbo);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glClearDepth(s.clearDepth);
    if (s.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc((GLenum)s.depthFunc);
    glDepthMask(s.depthMask);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    if (s.cullEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glCullFace((GLenum)s.cullFace);
    glFrontFace((GLenum)s.frontFace);
    // GLSTATE-SHADOW-CLIP-RESTORE-1: explicit re-assert after FBO switch.
    glClipControl((GLenum)s.clipOrigin, (GLenum)s.clipDepth);
}

static const char* depthFuncName(GLint f) {
    switch (f) {
        case GL_NEVER: return "NEVER"; case GL_LESS: return "LESS";
        case GL_EQUAL: return "EQUAL"; case GL_LEQUAL: return "LEQUAL";
        case GL_GREATER: return "GREATER"; case GL_NOTEQUAL: return "NOTEQUAL";
        case GL_GEQUAL: return "GEQUAL"; case GL_ALWAYS: return "ALWAYS";
        default: return "?";
    }
}

static bool sameShadowGLState(const ShadowPassGLState& a, const ShadowPassGLState& b) {
    return a.fbo == b.fbo &&
           a.viewport[0]==b.viewport[0] && a.viewport[1]==b.viewport[1] &&
           a.viewport[2]==b.viewport[2] && a.viewport[3]==b.viewport[3] &&
           a.clearDepth == b.clearDepth &&
           a.depthTest == b.depthTest && a.depthFunc == b.depthFunc &&
           a.depthMask == b.depthMask &&
           a.colorMask[0]==b.colorMask[0] && a.colorMask[1]==b.colorMask[1] &&
           a.colorMask[2]==b.colorMask[2] && a.colorMask[3]==b.colorMask[3] &&
           a.cullEnabled == b.cullEnabled && a.cullFace == b.cullFace &&
           a.frontFace == b.frontFace &&
           a.clipOrigin == b.clipOrigin && a.clipDepth == b.clipDepth;
}

// Emit one [SHADOW_STATE v1] line describing the state the pass ran WITH (the
// `pass` capture) plus the restored-OK check (entry == exit). For the static
// pass, building caster counts are appended (registry-driven, gate-dependent).
static void traceShadowPass(const char* passName,
                            const ShadowPassGLState& pass,
                            const ShadowPassGLState& entry,
                            const ShadowPassGLState& exitState,
                            bool includeBldgCounts) {
    if (!shadowStateTraceEnabled()) return;
    const bool restored = sameShadowGLState(entry, exitState);

    // Rate-limit the per-frame firehose: the dynamic shadow pass fires once per
    // frame (~11k lines over a tier1 run). Emit only the first occurrence of a
    // given pass, then one line per kShadowTraceEvery frames. A state leak
    // (restored==0) is the load-bearing signal and is NEVER throttled -- it
    // always logs. This changes cadence only, not what the line reports.
    static const unsigned kShadowTraceEvery = 60;
    static unsigned long s_passSeen[2] = {0, 0};       // per-pass emit counter
    // includeBldgCounts==true is the static (full-map) pass; false = dynamic.
    const int slot = includeBldgCounts ? 0 : 1;
    const unsigned long seen = s_passSeen[slot]++;
    if (restored) {
        if (seen != 0 && (seen % kShadowTraceEvery) != 0) return;
    }
    char bldg[96] = {0};
    if (includeBldgCounts) {
        int t=0,i=0,d=0;
        gos_GetStaticBuildingShadowCounts(t, i, d);
        snprintf(bldg, sizeof(bldg), " bldg=%d/%d/%d(types/inst/draws)", t, i, d);
    }
    fprintf(stderr,
        "[SHADOW_STATE v1] pass=%s fbo=%d vp=%d,%d,%d,%d clearDepth=%g "
        "depthTest=%d depthFunc=%s depthMask=%d colorMask=%d%d%d%d "
        "cull=%s/%s/%s clipOrigin=0x%X clipDepth=0x%X restored=%d%s\n",
        passName, pass.fbo,
        pass.viewport[0], pass.viewport[1], pass.viewport[2], pass.viewport[3],
        (double)pass.clearDepth,
        pass.depthTest ? 1 : 0, depthFuncName(pass.depthFunc),
        pass.depthMask ? 1 : 0,
        pass.colorMask[0]?1:0, pass.colorMask[1]?1:0,
        pass.colorMask[2]?1:0, pass.colorMask[3]?1:0,
        pass.cullEnabled ? "on" : "off",
        pass.cullFace == GL_FRONT ? "front" : "back",
        pass.frontFace == GL_CW ? "CW" : "CCW",
        (unsigned)pass.clipOrigin, (unsigned)pass.clipDepth,
        restored ? 1 : 0, bldg);
    fflush(stderr);
}
}  // namespace

// --- Shadow pre-pass: renders ALL terrain batches to shadow map before any shading ---
// This eliminates per-batch seams where early batches couldn't see later batches' shadows.

void gosRenderer::beginShadowPrePass(bool clearDepth) {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_ || !shadow_terrain_material_) return;

    ZoneScopedN("Shadow.StaticPrePass");
    TracyGpuZone("Shadow.StaticPrePass");

    // SHADOW-STABILITY-1: capture the caller's full GL state so endShadowPrePass
    // can restore it (forward-Z shadow state must not leak into the reverse-Z
    // scene pass). Captured before we mutate anything below.
    captureShadowGLState(s_staticPassEntry);

    // Save current FBO and viewport
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &shadow_prepass_prev_fbo_);
    glGetIntegerv(GL_VIEWPORT, shadow_prepass_prev_viewport_);

    // Unbind shadow texture (prevent feedback loop — AMD requirement)
    glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitStaticShadow);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    // Disable comparison mode for writing (AMD requirement)
    glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Bind shadow FBO, optionally clear depth, and configure state
    glBindFramebuffer(GL_FRAMEBUFFER, pp->getShadowFBO());
    int smSize = pp->getShadowMapSize();
    glViewport(0, 0, smSize, smSize);
    // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: base fixed-function state from the
    // ShadowTerrain pipeline row (depth test+write, GL_LESS forward-Z, cull none,
    // frontFace ccw, polygon-offset off). Must precede the depth clear so
    // depthMask=GL_TRUE lets the clear write. glProgramName=0 → program bind
    // skipped (the pass binds its own shadow material). colorMask / FBO / viewport
    // / clear / lightSpaceMatrix stay hand-set (not modeled by PipelineDesc).
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowTerrain), "ShadowTerrain");
    // Reverse-Z (U2) state-safe partition: shadow map stays forward-Z;
    // scene set glClearDepth(0), so force 1.0f around this shadow clear.
    if (clearDepth) { glClearDepth(1.0f); glClear(GL_DEPTH_BUFFER_BIT); glClearDepth(0.0f); }
    render_contract::assertPassContract(render_contract::PassIdentity::ShadowCaster,
                                        "gosRenderer::beginShadowPrePass");
    // SHADOW-OBSERVE-2-REVISE: static shadow build is once-per-mission (gos_StaticLightMatrixBuilt-gated),
    // not a per-frame pass — NOT instrumented here. Per-frame dynamic shadow observation
    // (gos_BeginDynamicShadowPass / beginDynamicShadowPass, txmmgr:2728) is the correct future
    // target (SHADOW-OBSERVE-3).

    // Bind shadow shader and upload lightSpaceMatrix
    shadow_terrain_material_->apply();
    GLint lsmLoc = glGetUniformLocation(
        shadow_terrain_material_->getShader()->shp_, "lightSpaceMatrix");
    if (lsmLoc >= 0)
        glUniformMatrix4fv(lsmLoc, 1, GL_FALSE, pp->getLightSpaceMatrix());

    active_light_space_matrix_ = pp->getLightSpaceMatrix();
    shadow_prepass_active_ = true;
}

void gosRenderer::drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount)
{
    if (!shadow_prepass_active_ || numVerts <= 0 || extraCount <= 0) return;

    ZoneScopedN("Shadow.TessBatch");
    TracyGpuZone("Shadow.TessBatch");

    // Upload vertices + indices to indexed_tris_ (same mesh used by normal terrain draw)
    indexed_tris_->rewind();
    indexed_tris_->addVertices(vertices, numVerts);
    indexed_tris_->addIndices(indices, numIndices);
    indexed_tris_->uploadBuffers();

    // Re-activate shadow shader (end() from previous batch deactivates program)
    shadow_terrain_material_->apply();
    GLuint shp = shadow_terrain_material_->getShader()->shp_;
    cacheShadowUniformLocations(shp);
    const auto& sl = shadowLocs_;

    // Upload lightSpaceMatrix (must re-upload per-batch since apply() resets state)
    {
        const float* lsm = active_light_space_matrix_;
        if (!lsm) {
            gosPostProcess* pp = getGosPostProcess();
            if (pp) lsm = pp->getLightSpaceMatrix();
        }
        if (lsm && sl.lightSpaceMatrix >= 0)
            glUniformMatrix4fv(sl.lightSpaceMatrix, 1, GL_FALSE, lsm);
    }

    // Upload tessellation uniforms via direct GL (same pattern as terrainBindUniformsForPatchStream)
    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4] = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4] = { 0.0f, terrain_displace_scale_, 0.0f, 0.0f };  // no Phong in shadow

    if (sl.tessLevel >= 0) glUniform4fv(sl.tessLevel, 1, tessParams);
    if (sl.tessDistanceRange >= 0) glUniform4fv(sl.tessDistanceRange, 1, tessDist);
    if (sl.tessDisplace >= 0) glUniform4fv(sl.tessDisplace, 1, tessDisp);
    if (sl.cameraPos >= 0) glUniform4fv(sl.cameraPos, 1, (const float*)&terrain_camera_pos_);

    // projection_ needed by shadow vert shader (TES overrides gl_Position, but VS needs it for gl_Position passthrough)
    if (sl.mvp >= 0) glUniformMatrix4fv(sl.mvp, 1, GL_TRUE, (const float*)&projection_);

    // Displacement texture for shadow TES (dirt normal / array path)
    if (terrainNormalArrayEnabled()) {
        if (terrain_normal_array_dirty_) buildTerrainNormalArray();
        if (terrain_normal_array_tex_ != 0 && sl.matNormalArray >= 0) {
            glUniform1i(sl.matNormalArray, kTerrainTexUnitNormalArray);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitNormalArray);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
            glActiveTexture(GL_TEXTURE0);
        }
    } else {
        if (sl.matNormal2 >= 0) {
            glUniform1i(sl.matNormal2, kTerrainMatNormalUnits[2]);
            glActiveTexture(GL_TEXTURE0 + kTerrainMatNormalUnits[2]);
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[2]);
            glActiveTexture(GL_TEXTURE0);
        }
    }

    float tiling[4] = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    if (sl.detailNormalTiling >= 0) glUniform4fv(sl.detailNormalTiling, 1, tiling);
    // C1 tactical: push mission-gated material profile to the shadow tese
    // classifier (terrain_common.hglsl). Uniform default is 0 = LEGACY.
    if (sl.terrainMaterialProfile >= 0) glUniform1i(sl.terrainMaterialProfile, g_terrainMaterialProfile);

    // tex1 (colormap) sampler — bound to unit 0 by the gos_SetRenderState texture call
    if (sl.tex1 >= 0) glUniform1i(sl.tex1, 0);

    // Bind main VBO (pos, color, fog, texcoord at locations 0-3)
    glBindBuffer(GL_ARRAY_BUFFER, indexed_tris_->getVB());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexed_tris_->getIB());
    shadow_terrain_material_->applyVertexDeclaration();

    // Bind extras VBO for worldPos (location 4) and worldNorm (location 5)
    updateBuffer(terrain_extra_vb_, GL_ARRAY_BUFFER,
        extras, extraCount * sizeof(gos_TERRAIN_EXTRA), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, terrain_extra_vb_);

    glEnableVertexAttribArray(4);  // worldPos
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)0);
    glEnableVertexAttribArray(5);  // worldNorm
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(gos_TERRAIN_EXTRA), (void*)(3 * sizeof(float)));

    // Draw tessellated patches
    glPatchParameteri(GL_PATCH_VERTICES, 3);
    glDrawElements(GL_PATCHES, numIndices,
        indexed_tris_->getIndexSizeBytes() == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL);

    // Cleanup
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    shadow_terrain_material_->endVertexDeclaration();
    shadow_terrain_material_->end();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    // Leave indexed_tris_ clean so PatchStream (ks=1) non-terrain solid draws
    // don't inherit stale shadow tile vertices. In ks=0 the first terrain
    // drawIndexedTris call rewinds naturally; in ks=1 it never fires.
    indexed_tris_->rewind();
}

void gosRenderer::drawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4)
{
    if (!shadow_prepass_active_ || !vb || !ib || ib->count_ == 0) return;
    if (!shadow_object_material_ || !shadow_object_material_->getShader()) return;

    ZoneScopedN("Shadow.DynObjectDirect");
    TracyGpuZone("Shadow.DynObjectDirect");

    // Direct GPU draw: bypass material system, use shadow_object shader directly.
    // This avoids the glGetBufferSubData readback that was 26% of frame time.
    GLuint shp = shadow_object_material_->getShader()->shp_;
    glUseProgram(shp);

    // Upload uniforms
    GLint lsmLoc = glGetUniformLocation(shp, "lightSpaceMatrix");
    GLint wmLoc = glGetUniformLocation(shp, "worldMatrix");
    GLint loLoc = glGetUniformLocation(shp, "lightOffset");

    if (lsmLoc >= 0)
        glUniformMatrix4fv(lsmLoc, 1, GL_FALSE, active_light_space_matrix_);
    if (wmLoc >= 0)
        glUniformMatrix4fv(wmLoc, 1, GL_TRUE, worldMatrix4x4);  // row-major Stuff matrix
    if (loLoc >= 0) {
        float lo[3] = {
            terrain_light_dir_.x * 30.0f,
            terrain_light_dir_.y * 30.0f,
            terrain_light_dir_.z * 30.0f
        };
        glUniform3fv(loLoc, 1, lo);
    }

    // Force depth write ON — this is the key fix. applyRenderStates would disable it.
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Bind VB with position attribute at location 0 (TG_HWTypeVertex: pos at offset 0, stride 36)
    glBindBuffer(GL_ARRAY_BUFFER, vb->buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void*)0);

    // Bind IB and draw
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->buffer_);
    GLenum indexType = (ib->element_size_ == 2) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    glDrawElements(GL_TRIANGLES, ib->count_, indexType, (void*)0);

    // Cleanup — disable the attribute we enabled
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void gosRenderer::endShadowPrePass() {
    if (!shadow_prepass_active_) return;

    ZoneScopedN("Shadow.PrePassEnd");

    gosPostProcess* pp = getGosPostProcess();

    // Restore comparison mode for sampling
    glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);

    // SHADOW-STABILITY-1: snapshot the state the pass actually ran with (for
    // the trace), then explicitly RESTORE the captured entry state so the
    // forward-Z shadow config (clearDepth/depthFunc/depthMask/colorMask/cull)
    // cannot leak into the reverse-Z scene pass. This supersedes the old
    // FBO+viewport-only restore.
    ShadowPassGLState passState;
    captureShadowGLState(passState);
    restoreShadowGLState(s_staticPassEntry);

    active_light_space_matrix_ = nullptr;
    shadow_prepass_active_ = false;

    // Verify restored-OK (entry == exit) and emit the trace line (gated).
    if (shadowStateTraceEnabled()) {
        ShadowPassGLState exitState;
        captureShadowGLState(exitState);
        traceShadowPass("static", passState, s_staticPassEntry, exitState,
                        /*includeBldgCounts=*/true);
    }

    // RENDER_STATES v1: shadow prepass disturbed program/depth/buffers via direct
    // GL calls. Invalidate cache so next applyRenderStates does a full re-apply.
    invalidateRenderStateCache();

    drainGLErrors("shadow_static");
}

void gosRenderer::beginDynamicShadowPass() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_ || !shadow_terrain_material_ || !pp->getDynamicShadowFBO()) return;

    ZoneScopedN("Shadow.DynBegin");

    // SHADOW-STABILITY-1: capture caller GL state for restore at pass exit.
    captureShadowGLState(s_dynamicPassEntry);

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &shadow_prepass_prev_fbo_);
    glGetIntegerv(GL_VIEWPORT, shadow_prepass_prev_viewport_);

    // Unbind dynamic shadow texture (AMD feedback loop prevention)
    glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynamicShadow);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    // Disable comparison mode for writing (RAW: the engine-owned GL texture; never
    // the VK-produced interop texture, whose compare-mode must stay REF_TO_TEXTURE).
    glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTextureRaw());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // APPLY-STATE-SHADOW-2: when MC2_FRAMEGRAPH_EXECUTOR is ON and the executor applies
    // Shadow's state, dispatch the executor apply HERE — at the body's former FBO-bind position,
    // AFTER the capture/AMD-unbind/compare-flip preamble above (those shadow-TEXTURE mutations
    // MUST precede the FBO bind and stay body-owned). executorApplyShadowState() now applies the
    // FULL render-target mode in order via the EXTEND helper: FBO bind (ShadowDynamicMap) ->
    // viewport (ShadowMap size) -> DepthForwardZ clear, then the ShadowMech base pipeline, and
    // sets shadowStateAppliedByExecutor_. The body's own FBO bind + viewport + applyPipeline +
    // forward-Z clear below are then ALL skipped (one-shot). Gate OFF -> flag stays false ->
    // body binds + sets viewport + applies + clears as before -> byte-identical.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::Shadow) != nullptr) {
        executorApplyShadowState();
    }
    // APPLY-STATE-SHADOW-2: FBO bind + viewport now skipped when the executor applied them.
    if (!shadowStateAppliedByExecutor_) {
        glBindFramebuffer(GL_FRAMEBUFFER, pp->getDynamicShadowFBO());
        glViewport(0, 0, pp->getDynamicShadowMapSize(), pp->getDynamicShadowMapSize());
    }
    // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: base fixed-function state from the
    // ShadowMech pipeline row (depth test+write, GL_LESS forward-Z, cull none,
    // frontFace ccw, polygon-offset off). Before the clear so depthMask=GL_TRUE
    // lets it write. Mech casters inherit this base; dynamic-prop casters re-enable
    // polygon offset via applyPipeline(ShadowStaticProp) at their draw site.
    // APPLY-STATE-SHADOW-1: skip when the executor already applied it (one-shot).
    if (!shadowStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowMech), "ShadowMech");
    }
    // SHADOW-OBSERVE-3: note per-frame dynamic shadow AFTER applyPipeline(ShadowMech)
    // sets GL_LESS + depthWrite=GL_TRUE, so the ambient probe sees ShadowLess.
    // FBO already bound above (ShadowDynamicMap). Fires every frame; distinct from
    // beginShadowPrePass (static, once/mission). ★Stays UNCONDITIONAL in the body — when
    // the executor applied state, the clear ran just before this note (instead of just after),
    // which is inert to what the note samples (FBO binding + depthFunc/depthWrite).
    render_contract::noteRenderPass(render_contract::PassIdentity::ShadowCaster,
                                    "gosRenderer::beginDynamicShadowPass");
    // Reverse-Z (U2) state-safe partition: dynamic shadow stays forward-Z;
    // scene set glClearDepth(0), so force 1.0f around this shadow clear.
    // APPLY-STATE-SHADOW-1: skip when the executor already applied the DepthForwardZ clear
    // (the EXTEND helper performs the identical 1.0 -> glClear(DEPTH) -> 0.0 sequence).
    if (!shadowStateAppliedByExecutor_) {
        glClearDepth(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepth(0.0f);
    }
    // APPLY-STATE-SHADOW-1: one-shot reset AFTER both the pipeline + clear, BEFORE the first
    // shadow draw (material apply / lsm upload / casters follow).
    shadowStateAppliedByExecutor_ = false;

    shadow_terrain_material_->apply();
    GLint lsmLoc = glGetUniformLocation(
        shadow_terrain_material_->getShader()->shp_, "lightSpaceMatrix");
    if (lsmLoc >= 0)
        glUniformMatrix4fv(lsmLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

    active_light_space_matrix_ = pp->getDynamicLightSpaceMatrix();
    shadow_prepass_active_ = true;
}

void gosRenderer::endDynamicShadowPass() {
    if (!shadow_prepass_active_) return;

    ZoneScopedN("Shadow.DynEnd");

    gosPostProcess* pp = getGosPostProcess();

    // Restore comparison mode on dynamic shadow texture (RAW: engine-owned GL
    // texture; the VK interop texture keeps its own compare-mode set at island init).
    glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTextureRaw());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindTexture(GL_TEXTURE_2D, 0);

    // SHADOW-STABILITY-1: snapshot pass state for the trace, then explicitly
    // restore the captured entry state (forward-Z dynamic config must not leak
    // into the reverse-Z scene). Supersedes the FBO+viewport-only restore.
    ShadowPassGLState passState;
    captureShadowGLState(passState);
    restoreShadowGLState(s_dynamicPassEntry);

    active_light_space_matrix_ = nullptr;
    shadow_prepass_active_ = false;

    if (shadowStateTraceEnabled()) {
        ShadowPassGLState exitState;
        captureShadowGLState(exitState);
        traceShadowPass("dynamic", passState, s_dynamicPassEntry, exitState,
                        /*includeBldgCounts=*/false);
    }

    // RENDER_STATES v1: dynamic shadow pass disturbed program/depth/buffers.
    invalidateRenderStateCache();

    drainGLErrors("shadow_dynamic");
}

// TERRAIN-NORMALS-FROM-HEIGHT-1: shared helper called from each of the three
// terrain uniform-upload sites below. Binds the per-mission R32F height
// texture to unit 11, pushes the worldspace mapping params, and reads the
// MC2_TERRAIN_NORMALS_FROM_HEIGHT env gate every frame so toggling it does
// not require a restart. When the texture handle is zero (no mission, or
// pre-upload), the gate is forced OFF so the shader branch stays in its
// no-op path even if the env var is set. Caller is responsible for restoring
// the active texture unit afterwards (existing sites already do glActiveTexture
// (GL_TEXTURE0) immediately after this helper).
static void bindTerrainHeightTexUniforms(GLint heightTexLoc, GLint paramsLoc,
                                         GLint gateLoc, GLint strengthLoc,
                                         float strength,
                                         GLint lightingV1StrengthLoc,
                                         float lightingV1Strength,
                                         GLint lightingV2FloorLoc,
                                         float lightingV2Floor)
{
    const GLuint htex = (GLuint)gos_terrainHeightTexHandle();
    if (htex != 0 && heightTexLoc >= 0) {
        glUniform1i(heightTexLoc, kTerrainTexUnitHeight);
        glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitHeight);
        glBindTexture(GL_TEXTURE_2D, htex);
    }
    if (htex != 0 && paramsLoc >= 0) {
        const float wuPerVert = gos_terrainHeightWorldUnitsPerVertex();
        const float invWu = (wuPerVert > 0.0f) ? (1.0f / wuPerVert) : 0.0f;
        const float p[4] = {
            (float)gos_terrainHeightTexSide(),
            invWu,
            gos_terrainHeightMapTopLeftX(),
            gos_terrainHeightMapTopLeftY(),
        };
        glUniform4fv(paramsLoc, 1, p);
    }
    int gateOn = 0;
    if (htex != 0) {
        if (const char* e = getenv("MC2_TERRAIN_NORMALS_FROM_HEIGHT")) {
            gateOn = (e[0] && e[0] != '0') ? 1 : 0;
        }
    }
    if (gateLoc >= 0) glUniform1i(gateLoc, gateOn);
    // TERRAIN-TUNING-UI-1: per-frame strength multiplier on the additive
    // height-derived normal term. Always uploaded — at strength=1.0 the
    // shader expression collapses to the pre-slice byte-equivalent path.
    if (strengthLoc >= 0) glUniform1f(strengthLoc, strength);
    // TERRAIN-LIGHTING-1: hemisphere ambient strength. Env gate
    // MC2_TERRAIN_LIGHTING_V1 is authoritative — when unset/=0 the
    // uploaded value is force-zeroed and the shader branch short-
    // circuits to a no-op. When ON, the member value (ImGui-tunable)
    // is uploaded.
    float effectiveLightingStrength = 0.0f;
    if (const char* e = getenv("MC2_TERRAIN_LIGHTING_V1")) {
        if (e[0] && e[0] != '0') effectiveLightingStrength = lightingV1Strength;
    }
    if (lightingV1StrengthLoc >= 0) {
        glUniform1f(lightingV1StrengthLoc, effectiveLightingStrength);
    }
    // TERRAIN-LIGHTING-2: shadow-aware hemisphere fill floor. Env gate
    // MC2_TERRAIN_LIGHTING_V2 is authoritative. When OFF we force-upload
    // 1.0 so the shader expression `mix(floor, 1.0, shadow)` collapses to
    // 1.0 (no shadow modulation) → V1 behavior preserved. When ON we
    // upload the member value (default 0.3, ImGui-tunable 0..1). Always
    // bounded so the shader never sees an out-of-range floor.
    float effectiveV2Floor = 1.0f;
    if (const char* e = getenv("MC2_TERRAIN_LIGHTING_V2")) {
        if (e[0] && e[0] != '0') {
            effectiveV2Floor = lightingV2Floor;
            if (effectiveV2Floor < 0.0f) effectiveV2Floor = 0.0f;
            if (effectiveV2Floor > 1.0f) effectiveV2Floor = 1.0f;
        }
    }
    if (lightingV2FloorLoc >= 0) {
        glUniform1f(lightingV2FloorLoc, effectiveV2Floor);
    }
}

// Item 1 CSM: upload the dynamic-shadow uniforms for a terrain-style loc set.
// When CSM is OFF this is byte-identical to the legacy inline block (sets
// dynamicLightSpaceMatrix + dynamicShadowMap + binds the 2D texture). When ON
// it instead uploads dynamicCascadeMatrices[N] + dynamicCsmCount and binds the
// GL_TEXTURE_2D_ARRAY to the same texunit. Template over the loc struct because
// TerrainUniformLocs and ThinTerrainUniformLocs share field names.
template <typename Locs>
static void uploadDynamicShadowUniforms(const Locs& tl, gosPostProcess* pp, GLint texUnit)
{
    if (mc2ShadowCsmEnabled() && pp->getDynamicShadowArrayTexture()) {
        if (tl.dynamicCascadeMatrices >= 0)
            glUniformMatrix4fv(tl.dynamicCascadeMatrices,
                               pp->getDynamicShadowCascadeCount(), GL_FALSE,
                               pp->getDynamicCascadeMatrices());
        if (tl.dynamicCsmCount >= 0)
            glUniform1i(tl.dynamicCsmCount, pp->getDynamicShadowCascadeCount());
        // Stage 3: per-cascade texel-scaled depth bias inputs.
        if (tl.dynamicCascadeTexelWorld >= 0)
            glUniform1fv(tl.dynamicCascadeTexelWorld,
                         pp->getDynamicShadowCascadeCount(),
                         pp->getDynamicCascadeTexelWorld());
        if (tl.csmDepthSpan >= 0)
            glUniform1f(tl.csmDepthSpan, pp->getCsmDepthSpan());
        if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 1);
        if (tl.dynamicShadowArray >= 0) {
            glUniform1i(tl.dynamicShadowArray, texUnit);
            glActiveTexture(GL_TEXTURE0 + texUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, pp->getDynamicShadowArrayTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        // Per-cascade shadow resolution: the LAST cascade samples this separate
        // lower-res 2D depth texture on its own dedicated unit.
        if (tl.dynamicFullMapTexelWorld >= 0)
            glUniform1f(tl.dynamicFullMapTexelWorld, pp->getDynamicFullMapTexelWorld());
        if (tl.dynamicFullMapShadow >= 0) {
            glUniform1i(tl.dynamicFullMapShadow, kTerrainTexUnitDynFullMap);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynFullMap);
            glBindTexture(GL_TEXTURE_2D, pp->getDynamicFullMapTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        return;
    }
    // Legacy single-map path (byte-identical to the prior inline block).
    if (tl.dynamicLightSpaceMatrix >= 0)
        glUniformMatrix4fv(tl.dynamicLightSpaceMatrix, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
    if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 1);
    if (tl.dynamicShadowMap >= 0) {
        glUniform1i(tl.dynamicShadowMap, texUnit);
        glActiveTexture(GL_TEXTURE0 + texUnit);
        glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
        glActiveTexture(GL_TEXTURE0);
    }
}

// LIGHTING-DEBUG-VIEWS: named, render-family-shared lighting debug enum. Maps
// MC2_LIGHTING_DEBUG_VIEW=<name> onto a shared surfaceDebugMode integer space.
// The 40-series is reserved for the unified lighting channels so the SAME name
// means the SAME channel across terrain (1A), static_prop (1B) and mech (1C).
// Returns -1 when the env var is unset OR the name is unknown, so callers keep
// their existing debug mode (default 0 = off = byte-identical legacy render).
// Takes precedence over MC2_TERRAIN_DEBUG_MODE when both are set.
// NON-static: also consumed by gos_static_prop_batcher.cpp (extern decl there).
// Note: not every channel applies to every family — a family ignores ids it
// does not implement (falls through to its normal render), which is safe.
int mc2LightingDebugMode()
{
    const char* v = getenv("MC2_LIGHTING_DEBUG_VIEW");
    if (!v || !*v) return -1;
    if (!strcmp(v, "off") || !strcmp(v, "0")) return 0;   // explicit no-op render
    if (!strcmp(v, "albedo"))     return 40;
    if (!strcmp(v, "normal"))     return 41;   // final per-fragment N as RGB
    if (!strcmp(v, "sun"))        return 42;   // sun N·L diffuse term
    if (!strcmp(v, "ambient"))    return 43;   // hemisphere/ambient/SH fill only
    if (!strcmp(v, "shadow"))     return 44;   // shadow factor (terrain) / no-shadow marker (props)
    if (!strcmp(v, "final"))      return 45;   // == default lit render (falls through)
    if (!strcmp(v, "overbright")) return 46;   // over/under-bright heatmap
    if (!strcmp(v, "lightcount")) return 47;   // dynamic light-count heatmap (props/mech)
    if (!strcmp(v, "lightindex")) return 48;   // baked-static-light-index palette (props/mech)
    return -1;                                 // unknown -> safe fallback (keep existing)
}

// TERRAIN-DETAIL-ANTI-TILE-1: pack distance-fade + macro-noise knobs into the
// unused .yzw of the detailNormalStrength uniform (the shader reads only .x today).
// Gate MC2_TERRAIN_DETAIL_ANTITILE; default OFF => all three return 0 =>
// detailNormalStrength stays {strength,0,0,0} => byte-identical render.
//   .y = fadeStart (world units)   .z = fadeEnd (world units)   .w = macroStrength
// Read once. Returns pointer to a static float[3].
static const float* mc2_detailAntiTileYZW()
{
    static float s_yzw[3] = { 0.0f, 0.0f, 0.0f };
    static bool s_init = false;
    if (!s_init) {
        s_init = true;
        const char* g = getenv("MC2_TERRAIN_DETAIL_ANTITILE");
        if (g && *g && strcmp(g, "0") != 0) {
            auto envF = [](const char* k, float def) -> float {
                const char* v = getenv(k);
                return (v && *v) ? (float)atof(v) : def;
            };
            float start = envF("MC2_TERRAIN_DETAIL_FADE_START", 1000.0f);
            float end   = envF("MC2_TERRAIN_DETAIL_FADE_END",   3000.0f);
            if (end < start + 1.0f) end = start + 1.0f;   // keep fade band positive
            s_yzw[0] = start;
            s_yzw[1] = end;
            s_yzw[2] = envF("MC2_TERRAIN_DETAIL_MACRO_STRENGTH", 0.35f);
        }
    }
    return s_yzw;
}

void gosRenderer::terrainBindUniformsForPatchStream(gosRenderMaterial* material)
{
    if (!material) return;
    material->setTransform(projection_);
    material->setFogColor(fog_color_);
    material->apply();
    material->setSamplerUnit(gosMesh::s_tex1, 0);

    GLuint shp = material->getShader()->shp_;
    cacheTerrainUniformLocations(shp);
    const auto& tl = terrainLocs_;

    float tessParams[4] = { terrain_tess_level_, terrain_tess_level_, 0.0f, 0.0f };
    float tessDist[4]   = { terrain_tess_dist_near_, terrain_tess_dist_far_, 0.0f, 0.0f };
    float tessDisp[4]   = { terrain_phong_alpha_, terrain_displace_scale_, 0.0f, 0.0f };
    if (tl.tessLevel >= 0)         glUniform4fv(tl.tessLevel, 1, tessParams);
    if (tl.tessDistanceRange >= 0) glUniform4fv(tl.tessDistanceRange, 1, tessDist);
    if (tl.tessDisplace >= 0)      glUniform4fv(tl.tessDisplace, 1, tessDisp);
    if (tl.cameraPos >= 0)         glUniform4fv(tl.cameraPos, 1, (const float*)&terrain_camera_pos_);
    float debugMode = terrain_debug_mode_;
    if (const char* envDebug = getenv("MC2_TERRAIN_DEBUG_MODE")) {
        debugMode = (float)atof(envDebug);
    }
    { int lvm = mc2LightingDebugMode(); if (lvm >= 0) debugMode = (float)lvm; }  // LIGHTING-DEBUG-VIEWS-1A
    float tessDebugVec[4] = { debugMode, 0.0f, 0.0f, 0.0f };
    if (tl.tessDebug >= 0)         glUniform4fv(tl.tessDebug, 1, tessDebugVec);
    if (tl.pathTint >= 0)          glUniform1i(tl.pathTint, mc2ShaderPathTint());  // MC2_SHADER_PATH_TINT

    if (tl.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(tl.mapHalfExtent, halfExt);

        // OVERLAY-MAGENTA-TEXTURE-RECON-1 (Source B): the colormap "no-data" magenta
        // texels are hidden by the off-map edge-haze ONLY when mapHalfExtent > 0. If
        // this is <= 0 for a map, the haze guard is skipped and magenta renders raw.
        // Emit the live value (throttled to value changes) so we can tell Source-B
        // guard-fail from a Source-A missing-texture. Gated MC2_OVERLAY_MAGENTA_TRACE.
        {
            static const bool s_magentaTrace = (std::getenv("MC2_OVERLAY_MAGENTA_TRACE") != nullptr);
            static float s_lastHalfExt = -99999.0f;
            if (s_magentaTrace && halfExt != s_lastHalfExt && mc2_diag::tagEnabled("OVERLAY_MAGENTA")) {
                s_lastHalfExt = halfExt;
                char _mg_buf[160];
                snprintf(_mg_buf, sizeof(_mg_buf),
                         "{\"site\":\"map_half_extent\",\"value\":%.1f,\"guard_fail\":%d}",
                         halfExt, (halfExt <= 0.0f) ? 1 : 0);
                mc2_diag::writeEvent("OVERLAY_MAGENTA", 1, 0, _mg_buf);
            }
        }
    }
    if (terrain_mvp_valid_ && tl.terrainMVP >= 0)
        glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, (const float*)&terrain_mvp_);
    // F1 Task 7b: probe-only worldToClipGL flat-uniform fallback path.
    // Kept for non-AMD or future driver fixes; SSBO path (binding 23) is
    // the reliable transport on AMD TES stages (proven: loc=-1 from driver
    // optimizer removing the unused flat uniform once SSBO became primary).
    // When the shader declares u_worldToClipGL as a flat uniform, loc >= 0
    // and this path fires. Currently loc=-1 (shader uses SSBO only).
    if (probeWorldToClipGLValid_ && tl.worldToClipGL >= 0)
        glUniformMatrix4fv(tl.worldToClipGL, 1, GL_FALSE, probeWorldToClipGL_);

    if (tl.terrainLightDir >= 0)        glUniform4fv(tl.terrainLightDir, 1, (const float*)&terrain_light_dir_);
    float tiling[4]      = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    const float* _datYZW = mc2_detailAntiTileYZW();  // TERRAIN-DETAIL-ANTI-TILE-1 (yzw=0 when gate OFF)
    float strength[4]    = { terrain_detail_strength_, _datYZW[0], _datYZW[1], _datYZW[2] };
    float pomP[4]        = { terrain_pom_scale_, 8.0f, 32.0f, 0.0f };
    float worldScaleV[4] = { terrain_world_scale_, 0.0f, 0.0f, 0.0f };
    float cellP[4]       = { terrain_cell_scale_, terrain_cell_jitter_, terrain_cell_rotation_, 0.0f };
    if (tl.detailNormalTiling >= 0)   glUniform4fv(tl.detailNormalTiling, 1, tiling);
    if (tl.detailNormalStrength >= 0) glUniform4fv(tl.detailNormalStrength, 1, strength);
    // C1 tactical: push mission-gated material profile to terrain classifier.
    // Default 0 = LEGACY = exact pre-C1 byte-for-byte rendering.
    if (tl.terrainMaterialProfile >= 0) glUniform1i(tl.terrainMaterialProfile, g_terrainMaterialProfile);
    if (tl.pomParams >= 0)            glUniform4fv(tl.pomParams, 1, pomP);
    if (tl.terrainWorldScale >= 0)    glUniform4fv(tl.terrainWorldScale, 1, worldScaleV);
    if (tl.cellBombParams >= 0)       glUniform4fv(tl.cellBombParams, 1, cellP);
    if (tl.matNormalBoost >= 0)       glUniform4fv(tl.matNormalBoost, 1, terrain_mat_normal_boost_);
    if (tl.matTiling >= 0)            glUniform4fv(tl.matTiling, 1, terrain_mat_tiling_);
    if (tl.matTilingSnow >= 0)        glUniform1f(tl.matTilingSnow, terrain_mat_tiling_snow_);
    if (tl.tintStrengthScale >= 0)    glUniform1f(tl.tintStrengthScale, terrain_tint_strength_scale_);
    if (tl.snowBrightnessDampen >= 0) glUniform1f(tl.snowBrightnessDampen, terrain_snow_brightness_dampen_);
    if (tl.tintRock  >= 0)            glUniform3fv(tl.tintRock,  1, terrain_tint_rock_);
    if (tl.tintGrass >= 0)            glUniform3fv(tl.tintGrass, 1, terrain_tint_grass_);
    if (tl.tintDirt  >= 0)            glUniform3fv(tl.tintDirt,  1, terrain_tint_dirt_);
    if (tl.terrainClassGrass >= 0)    glUniform4fv(tl.terrainClassGrass, 1, terrain_class_grass_);
    if (tl.terrainClassDirt  >= 0)    glUniform4fv(tl.terrainClassDirt,  1, terrain_class_dirt_);
    // TERRAIN-MATERIAL-LIB-1: promoted tints always upload (they replace what
    // were frag literals, so there is no gate-OFF skip for these -- the shader
    // now always reads a uniform, and the default member value is the exact
    // former literal). Roughness/AO + the branch flag ARE gated (byte-identity
    // requires the u_useMaterialLib==0 branch to be taken by default).
    if (tl.tintConcrete >= 0)         glUniform3fv(tl.tintConcrete, 1, terrain_tint_concrete_);
    if (tl.tintSnow     >= 0)         glUniform3fv(tl.tintSnow,     1, terrain_tint_snow_);
    if (tl.matRoughness  >= 0)        glUniform4fv(tl.matRoughness, 1, terrain_mat_roughness_);
    if (tl.matAO         >= 0)        glUniform4fv(tl.matAO,        1, terrain_mat_ao_);
    if (tl.useMaterialLib >= 0)       glUniform1i(tl.useMaterialLib, terrainMaterialLibEnabled() ? 1 : 0);
    // TERRAIN-CONTROLMAP-ALBEDO-1: gate default OFF -> member stays 0.0f ->
    // uploaded verbatim -> frag's mix(x,1.0,0.0)==x (byte-identical).
    if (tl.controlAlbedoStrength >= 0) glUniform1f(tl.controlAlbedoStrength, terrain_control_albedo_strength_);
    if (tl.time >= 0) {
        float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
        glUniform1f(tl.time, elapsed);
    }

    if (terrainNormalArrayEnabled()) {
        if (terrain_normal_array_dirty_) buildTerrainNormalArray();
        if (terrain_normal_array_tex_ != 0 && tl.matNormalArray >= 0) {
            glUniform1i(tl.matNormalArray, kTerrainTexUnitNormalArray);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitNormalArray);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
        }
    } else {
        for (int i = 0; i < 5; i++) {
            if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) {
                glUniform1i(tl.matNormal[i], kTerrainMatNormalUnits[i]);
                glActiveTexture(GL_TEXTURE0 + kTerrainMatNormalUnits[i]);
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
            }
        }
    }
    // TERRAIN-NORMALS-FROM-HEIGHT-1
    bindTerrainHeightTexUniforms(tl.terrainHeightTex, tl.terrainHeightParams,
                                 tl.useTerrainNormalsFromHeight,
                                 tl.terrainNormalsFromHeightStrength,
                                 terrain_nfh_strength_,
                                 tl.terrainLightingV1Strength,
                                 terrain_lighting_v1_strength_,
                                 tl.terrainLightingV2ShadowFillFloor,
                                 terrain_lighting_v2_floor_);
    glActiveTexture(GL_TEXTURE0);

    gosPostProcess* pp = getGosPostProcess();
    if (pp && pp->shadowsEnabled_) {
        if (tl.lightSpaceMatrix >= 0) glUniformMatrix4fv(tl.lightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
        if (tl.enableShadows >= 0)    glUniform1i(tl.enableShadows, 1);
        if (tl.shadowSoftness >= 0)   glUniform1f(tl.shadowSoftness, terrain_shadow_softness_);
        if (tl.shadowMap >= 0) {
            glUniform1i(tl.shadowMap, kTerrainTexUnitStaticShadow);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitStaticShadow);
            glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        if (pp->getDynamicShadowFBO()) {
            uploadDynamicShadowUniforms(tl, pp, kTerrainTexUnitDynamicShadow);
        } else {
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
        }
    } else {
        if (tl.enableShadows >= 0)        glUniform1i(tl.enableShadows, 0);
        if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
    }
}

int gosRenderer::terrainBindThinUniformsForPatchStream(glsl_program* overrideProg)
{
    glsl_program* target = overrideProg ? overrideProg : thin_terrain_prog_;
    if (!target || !target->shp_) return -1;
    GLuint shp = target->shp_;
    glUseProgram(shp);
    cacheThinTerrainUniformLocations(shp);
    const auto& tl = thinTerrainLocs_;

    // VS uniforms: projection chain
    if (terrain_mvp_valid_ && tl.terrainMVP >= 0)
        glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, (const float*)&terrain_mvp_);
    // projection_: row-major Stuff matrix — upload GL_TRUE (column-major interpretation).
    // All other projection_ upload sites (shadow line ~2737, material setTransform) use GL_TRUE.
    // terrainMVP stays GL_FALSE — its D3D chain math cancels the implicit transpose.
    if (tl.mvp >= 0)
        glUniformMatrix4fv(tl.mvp, 1, GL_TRUE, (const float*)&projection_);

    static const bool s_thinDebug   = (getenv("MC2_THIN_DEBUG") != nullptr);
    static bool       s_thinDbgDone = false;
    if (s_thinDebug && !s_thinDbgDone && terrain_mvp_valid_) {
        s_thinDbgDone = true;
        const float* p = (const float*)&projection_;
        const float* m = (const float*)&terrain_mvp_;
        fprintf(stderr,
            "[THIN_DEBUG v1] event=thin_uniforms_bound "
            "proj_row0=[%.5f,%.5f,%.5f,%.5f] proj_row1=[%.5f,%.5f,%.5f,%.5f] "
            "tmvp_diag=[%.5f,%.5f,%.5f,%.5f]\n",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            m[0], m[5], m[10], m[15]);
        fflush(stderr);
    }

    // FS uniforms (same as terrainBindUniformsForPatchStream, minus tess-only params)
    if (tl.cameraPos >= 0)        glUniform4fv(tl.cameraPos, 1, (const float*)&terrain_camera_pos_);
    if (tl.terrainLightDir >= 0)  glUniform4fv(tl.terrainLightDir, 1, (const float*)&terrain_light_dir_);
    // tessDebug: matches non-thin path (line ~4080). The thin path historically
    // omitted this — debug-viz modes 1..8 in gos_terrain.frag therefore could
    // not fire on the indirect/substrate draw, which silently broke the entire
    // shader-debug channel for the path that needs it most.  Env override
    // mirrors the tessellated path so MC2_TERRAIN_DEBUG_MODE works uniformly.
    {
        float debugMode = terrain_debug_mode_;
        if (const char* envDebug = getenv("MC2_TERRAIN_DEBUG_MODE")) {
            debugMode = (float)atof(envDebug);
        }
        { int lvm = mc2LightingDebugMode(); if (lvm >= 0) debugMode = (float)lvm; }  // LIGHTING-DEBUG-VIEWS-1A
        float tessDebugVec[4] = { debugMode, 0.0f, 0.0f, 0.0f };
        if (tl.tessDebug >= 0) glUniform4fv(tl.tessDebug, 1, tessDebugVec);
        if (tl.pathTint >= 0)  glUniform1i(tl.pathTint, mc2ShaderPathTint());  // MC2_SHADER_PATH_TINT
    }
    if (tl.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(tl.mapHalfExtent, halfExt);
    }
    float tiling[4]      = { terrain_detail_tiling_, 0.0f, 0.0f, 0.0f };
    const float* _datYZW = mc2_detailAntiTileYZW();  // TERRAIN-DETAIL-ANTI-TILE-1 (yzw=0 when gate OFF)
    float strength[4]    = { terrain_detail_strength_, _datYZW[0], _datYZW[1], _datYZW[2] };
    float pomP[4]        = { terrain_pom_scale_, 8.0f, 32.0f, 0.0f };
    float worldScaleV[4] = { terrain_world_scale_, 0.0f, 0.0f, 0.0f };
    float cellP[4]       = { terrain_cell_scale_, terrain_cell_jitter_, terrain_cell_rotation_, 0.0f };
    if (tl.detailNormalTiling >= 0)   glUniform4fv(tl.detailNormalTiling, 1, tiling);
    if (tl.detailNormalStrength >= 0) glUniform4fv(tl.detailNormalStrength, 1, strength);
    // C1 tactical: push mission-gated material profile to terrain classifier.
    // Default 0 = LEGACY = exact pre-C1 byte-for-byte rendering.
    if (tl.terrainMaterialProfile >= 0) glUniform1i(tl.terrainMaterialProfile, g_terrainMaterialProfile);
    if (tl.pomParams >= 0)            glUniform4fv(tl.pomParams, 1, pomP);
    if (tl.terrainWorldScale >= 0)    glUniform4fv(tl.terrainWorldScale, 1, worldScaleV);
    if (tl.cellBombParams >= 0)       glUniform4fv(tl.cellBombParams, 1, cellP);
    if (tl.matNormalBoost >= 0)       glUniform4fv(tl.matNormalBoost, 1, terrain_mat_normal_boost_);
    if (tl.matTiling >= 0)            glUniform4fv(tl.matTiling, 1, terrain_mat_tiling_);
    if (tl.matTilingSnow >= 0)        glUniform1f(tl.matTilingSnow, terrain_mat_tiling_snow_);
    if (tl.tintStrengthScale >= 0)    glUniform1f(tl.tintStrengthScale, terrain_tint_strength_scale_);
    if (tl.snowBrightnessDampen >= 0) glUniform1f(tl.snowBrightnessDampen, terrain_snow_brightness_dampen_);
    if (tl.tintRock  >= 0)            glUniform3fv(tl.tintRock,  1, terrain_tint_rock_);
    if (tl.tintGrass >= 0)            glUniform3fv(tl.tintGrass, 1, terrain_tint_grass_);
    if (tl.tintDirt  >= 0)            glUniform3fv(tl.tintDirt,  1, terrain_tint_dirt_);
    if (tl.terrainClassGrass >= 0)    glUniform4fv(tl.terrainClassGrass, 1, terrain_class_grass_);
    if (tl.terrainClassDirt  >= 0)    glUniform4fv(tl.terrainClassDirt,  1, terrain_class_dirt_);
    // TERRAIN-CONTROLMAP-ALBEDO-1: loc stays -1 on this (dead thin/legacy)
    // path today; guarded upload is a no-op until gos_terrain.frag declares
    // the uniform too.
    if (tl.controlAlbedoStrength >= 0) glUniform1f(tl.controlAlbedoStrength, terrain_control_albedo_strength_);
    if (tl.time >= 0) {
        float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
        glUniform1f(tl.time, elapsed);
    }
    if (tl.tex1 >= 0) glUniform1i(tl.tex1, 0);
    if (terrainNormalArrayEnabled()) {
        if (terrain_normal_array_dirty_) buildTerrainNormalArray();
        if (terrain_normal_array_tex_ != 0 && tl.matNormalArray >= 0) {
            glUniform1i(tl.matNormalArray, kTerrainTexUnitNormalArray);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitNormalArray);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
        }
    } else {
        for (int i = 0; i < 5; i++) {
            if (terrain_mat_normal_[i] != 0 && tl.matNormal[i] >= 0) {
                glUniform1i(tl.matNormal[i], kTerrainMatNormalUnits[i]);
                glActiveTexture(GL_TEXTURE0 + kTerrainMatNormalUnits[i]);
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
            }
        }
    }
    // TERRAIN-NORMALS-FROM-HEIGHT-1
    bindTerrainHeightTexUniforms(tl.terrainHeightTex, tl.terrainHeightParams,
                                 tl.useTerrainNormalsFromHeight,
                                 tl.terrainNormalsFromHeightStrength,
                                 terrain_nfh_strength_,
                                 tl.terrainLightingV1Strength,
                                 terrain_lighting_v1_strength_,
                                 tl.terrainLightingV2ShadowFillFloor,
                                 terrain_lighting_v2_floor_);
    glActiveTexture(GL_TEXTURE0);

    gosPostProcess* pp = getGosPostProcess();
    if (pp && pp->shadowsEnabled_) {
        if (tl.lightSpaceMatrix >= 0)
            glUniformMatrix4fv(tl.lightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
        if (tl.enableShadows >= 0)  glUniform1i(tl.enableShadows, 1);
        if (tl.shadowSoftness >= 0) glUniform1f(tl.shadowSoftness, terrain_shadow_softness_);
        if (tl.shadowMap >= 0) {
            glUniform1i(tl.shadowMap, kTerrainTexUnitStaticShadow);
            glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitStaticShadow);
            glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
            glActiveTexture(GL_TEXTURE0);
        }
        if (pp->getDynamicShadowFBO()) {
            uploadDynamicShadowUniforms(tl, pp, kTerrainTexUnitDynamicShadow);
        } else {
            if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
        }
    } else {
        if (tl.enableShadows >= 0)        glUniform1i(tl.enableShadows, 0);
        if (tl.enableDynamicShadows >= 0) glUniform1i(tl.enableDynamicShadows, 0);
    }

    return tl.ssboRecordBase;
}

void gosRenderer::terrainOverrideThinMVP(const float* mvp4x4)
{
    if (!mvp4x4) return;
    const auto& tl = thinTerrainLocs_;
    if (tl.terrainMVP < 0) return;
    // GL_FALSE is correct for terrainMVP (memory/terrain_mvp_gl_false.md) —
    // matches the upload at terrainBindThinUniformsForPatchStream above.
    glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, mvp4x4);
}

// TERRAIN-DEADCODE-DELETE-1: gosRenderer::terrainDrawIndexedPatches deleted.
// The legacy tess-solid terrain funnel is retired — terrain renders via the LOD-chunk path.
// The sole call site at gameos_graphics.cpp (the :7291 if-branch) is now a skip+tripwire.
// Helpers called by this function (cacheTerrainUniformLocations, terrainBindThinUniformsForPatchStream,
// bindTerrainHeightTexUniforms) remain in use by the shadow pre-pass and bridge paths.

void gosRenderer::drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices) {
    ZoneScopedN("DrawIndexedTris.Basic");
    gosASSERT(vertices && indices);

    gosASSERT((num_indices % 3) == 0);

    if(beforeDrawCall()) return;

    bool not_enough_vertices = indexed_tris_->getNumVertices() + num_vertices > indexed_tris_->getVertexCapacity();
    bool not_enough_indices = indexed_tris_->getNumIndices() + num_indices > indexed_tris_->getIndexCapacity();
    if(not_enough_vertices || not_enough_indices){
        applyRenderStates();

        gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
        gosASSERT(mat);

        mat->setTransform(projection_);
		mat->setFogColor(fog_color_);
        indexed_tris_->drawIndexed(mat);
        indexed_tris_->rewind();
    }

    gosASSERT(indexed_tris_->getNumVertices() + num_vertices <= indexed_tris_->getVertexCapacity());
    gosASSERT(indexed_tris_->getNumIndices() + num_indices <= indexed_tris_->getIndexCapacity());
    indexed_tris_->addVertices(vertices, num_vertices);
    indexed_tris_->addIndices(indices, num_indices);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();

    // Terrain tessellation path
    if (curStates_[gos_State_Terrain] && !curStates_[gos_State_Overlay] && terrain_material_ && terrain_batch_extras_count_ > 0 && terrain_draw_enabled_) {
        // TERRAIN-DEADCODE-DELETE-1: the legacy gosRenderer tess-solid terrain draw
        // (terrainDrawIndexedPatches) is retired — all terrain renders via the LOD-chunk
        // path (gos_terrain_lod_chunk.cpp / gamecam.cpp:508). This funnel branch is
        // unreachable post terrain-retire (terrain_path.legacy_mlr==0 confirms). If terrain
        // geometry ever reaches here, SKIP it (must not fall through to the basic renderer,
        // which would mis-draw terrain) and fire the telemetry counter as a regression tripwire.
        RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LegacyMLR);
        indexed_tris_->rewind();
    } else {
        // When tessellation is active, skip SOLID fallback terrain draws (tessellation
        // already rendered base terrain). Overlay/detail draws don't set gos_State_Terrain
        // (it auto-resets after each draw), so they fall through to the basic renderer.
        if (curStates_[gos_State_Terrain] && terrain_material_) {
            indexed_tris_->rewind();
        } else {
            gosRenderMaterial* mat = selectBasicRenderMaterial(curStates_);
            gosASSERT(mat);

            mat->setTransform(projection_);
            mat->setFogColor(fog_color_);

            // Water uniforms: set via deferred system before apply() flushes them
            {
                glsl_program* prog = mat->getShader();
                if (curStates_[gos_State_Water]) {
                    prog->setInt("isWater", 1);
                    static uint64_t water_start = timing::get_wall_time_ms();
                    float elapsed = SmokeMode::fixedTimestepEnabled()
                                        ? (float)SmokeMode::fixedClockSeconds()
                                        : (float)(timing::get_wall_time_ms() - water_start) / 1000.0f;
                    prog->setFloat("time", elapsed);
                } else {
                    prog->setInt("isWater", 0);
                }
            }

            // Overlay.SplitDraw removed — alpha cement and decals now go through
            // gos_DrawTerrainOverlays() / gos_DrawDecals() typed world-space batches.
            {
                ZoneScopedN("BasicDraw.Indexed");
                indexed_tris_->drawIndexed(mat);
            }
            indexed_tris_->rewind();
        }
    }

    afterDrawCall();
}

void gosRenderer::drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp)
{
    ZoneScopedN("DrawIndexedTris.Lighted");
    TracyGpuZone("DrawIndexedTris.Lighted");
    gosASSERT(ib && vb && mvp);
    gosASSERT((ib->count_ % 3) == 0);

    if(beforeDrawCall()) return;

    applyRenderStates();

    gosRenderMaterial* mat = selectLightedRenderMaterial(curStates_);
    gosASSERT(mat);

	mat4 transform(	mvp[0], mvp[1], mvp[2], mvp[3], 
					mvp[4], mvp[5], mvp[6], mvp[7],
					mvp[8], mvp[9], mvp[10], mvp[11],
					mvp[12], mvp[13], mvp[14], mvp[15]);

	vec4 vp = g_gos_renderer->getRenderViewport();

	mat->getShader()->setFloat4("vp", vp);
	mat->getShader()->setMat4("projection_", projection_);

    mat->setTransform(transform);
    //mat->setFogColor(fog_color_);

	// [LIGHTSSBO v1] FORK-2: this legacy lit material's LightsData is now
	// an SSBO; UBO reflection no longer binds it. Bind the storage block
	// for this program (idempotent, per-draw, hot-reload-safe).
	gos_BindLightDataStorageBlock(mat);

	gosMesh::drawIndexed(ib, vb, vdecl, mat);

    afterDrawCall();
}

void gosRenderer::drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
    ZoneScopedN("DrawIndexedTris.Unlighted");
    TracyGpuZone("DrawIndexedTris.Unlighted");
    gosASSERT(ib && vb);
    gosASSERT((ib->count_ % 3) == 0);

    if(beforeDrawCall()) return;

    applyRenderStates();

	// maybe getCurMaterial->set.... to not set it from outer code?
	//vec4 vp = g_gos_renderer->getRenderViewport();
	//mat->getShader()->setFloat4("vp", vp);
	//mat->getShader()->setMat4("projection_", projection_);

	gosMesh::drawIndexed(ib, vb, vdecl);

    afterDrawCall();
}

static int get_next_break(const char* text) {
    const char* start = text;
    do {
        char c = *text;
        if(c==' ' || c=='\n')
            return (int32_t)(text - start);
    } while(*text++);

    return (int32_t)(text - start - 1);
}

int findTextBreak(const char* text, const int count, const gosFont* font, const int region_width, int* out_str_width) {

    int width = 0;
    int pos = 0;

    int space_adv = font->getCharAdvance(' ');

    while(text[pos]) {

        int break_pos = get_next_break(text + pos);

        int cur_width = 0;
        for(int j=0;j<break_pos;++j) {
            cur_width += font->getCharAdvance(text[pos + j]);
        }

        // if next possible break will not fit, then return now
        if(width + cur_width >= region_width) {

            if(pos == 0) { // handle case when only one word in line and it does not fit it, just return whole line
                width = cur_width;
                pos = break_pos;
            }
            break;
        } else {
            width += cur_width;
            pos += break_pos;

            if(text[pos] == '\n') {
                pos++;
                break;
            }

            if(text[pos] == ' ') {
                width += space_adv;
                pos++;
            }
        }
    }

    if(out_str_width)
        *out_str_width = width;
    return pos;
}
// returnes num lines in text which should be wrapped in region_width
int calcTextHeight(const char* text, const int count, const gosFont* font, int region_width)
{
    int pos = 0;
    int num_lines = 0;
    while(pos < count) {

        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, NULL);
        pos += num_chars;
        num_lines++;
    }
    return num_lines;
}

void addCharacter(gosMesh* text_, const float u, const float v, const float u2, const float v2, const float x, const float y, const float x2, const float y2) {

    gos_VERTEX tr, tl, br, bl;

    tl.x = x;
    tl.y = y;
    tl.z = 0;
    tl.u = u;
    tl.v = v;
    tl.argb = 0xffffffff;
    tl.frgb = 0xff000000;

    tr.x = x2;
    tr.y = y;
    tr.z = 0;
    tr.u = u2;
    tr.v = v;
    tr.argb = 0xffffffff;
    tr.frgb = 0xff000000;

    bl.x = x;
    bl.y = y2;
    bl.z = 0;
    bl.u = u;
    bl.v = v2;
    bl.argb = 0xffffffff;
    bl.frgb = 0xff000000;

    br.x = x2;
    br.y = y2;
    br.z = 0;
    br.u = u2;
    br.v = v2;
    br.argb = 0xffffffff;
    br.frgb = 0xff000000;

    text_->addVertices(&tl, 1);
    text_->addVertices(&tr, 1);
    text_->addVertices(&bl, 1);

    text_->addVertices(&tr, 1);
    text_->addVertices(&br, 1);
    text_->addVertices(&bl, 1);

}

void gosRenderer::drawText(const char* text) {
    ZoneScopedN("DrawText");
    gosASSERT(text);

    if(beforeDrawCall()) return;

    const int count = (int)strlen(text);  
/*
    if(text_->getNumVertices() + count > text_->getCapacity()) {
        applyRenderStates();
        gosRenderMaterial* mat = 
            curStates_[gos_State_Texture]!=0 ? basic_tex_material_ : basic_material_;
        text_->draw(mat);
        text_->rewind();
    } 
*/

    // TODO: take text region into account!!!!
    
    gosASSERT(text_->getNumVertices() + 6 * count <= text_->getVertexCapacity());

    int ix, iy;
    getTextPos(ix, iy);
	float x = (float)ix, y = (float)iy;
    const float start_x = x;

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;

    const DWORD tex_id = font->getTextureId();
    const gosTexture* tex = getTexture(tex_id);
    gosTextureInfo ti;
    tex->getTextureInfo(&ti);
    const float oo_tex_width = 1.0f / (float)ti.width_;
    const float oo_tex_height = 1.0f / (float)ti.height_;
    
    const int font_height = font->getMaxCharHeight();
    const int font_ascent = font->getFontAscent();

    const int region_width = getTextRegionWidth();
    const int region_height = getTextRegionHeight();

    const int num_lines = calcTextHeight(text, count, font, region_width);
    if(ta.WrapType == 3) { // center in Y direction as well
        y += (region_height - num_lines * font_height) / 2;
    }
   
    int pos = 0;
    int str_width = 0;
    while(pos < count) {

        x = start_x;    
        int num_chars = findTextBreak(text + pos, count - pos, font, region_width, &str_width);

        // WrapType		- 0=Left aligned, 1=Right aligned, 2=Centered, 3=Centered in region (X and Y)
        switch(ta.WrapType) {
            case 0: break;
            case 1: x += region_width - str_width; break;
            case 2: x += (region_width - str_width) / 2; break;
            case 3: // see vertical centering above
                    x += (region_width - str_width) / 2;
                    break;
        }

        for(int i=0; i<num_chars; ++i) {

            const char c = text[i + pos];

            // Skip non-printable control chars. findTextBreak still
            // includes the trailing '\n' in num_chars to drive line
            // advance via `y += font_height` below; we just must not
            // emit a quad for it. Legacy .glyph files had a zero-width
            // entry at index 10, so the old path drew nothing visible;
            // D3F atlases ship a placeholder glyph there that would
            // appear as a square at end-of-line and on \n\n blank lines.
            if((unsigned char)c < 0x20) {
                continue;
            }

            const gosGlyphMetrics& gm = font->getGlyphMetrics(c);
            int char_off_x = gm.minx;
            int char_off_y = font_ascent - gm.maxy;
            int char_w = gm.maxx - gm.minx;
            int char_h = gm.maxy - gm.miny;

            // u/v are the actual atlas sample origin under the post-load
            // contract — char_off_x/y drive screen quad position only,
            // not the atlas read. The legacy .glyph loader pre-folds its
            // metrics so this path stays pixel-identical for that format.
            uint32_t iu0 = gm.u;
            uint32_t iv0 = gm.v;
            uint32_t iu1 = iu0 + char_w;
            uint32_t iv1 = iv0 + char_h;

            float u0 = (float)iu0 * oo_tex_width;
            float v0 = (float)iv0 * oo_tex_height;
            float u1 = (float)iu1 * oo_tex_width;
            float v1 = (float)iv1 * oo_tex_height;

            addCharacter(text_, u0, v0, u1, v1, (float)(x + char_off_x), (float)(y + char_off_y), (float)(x + char_off_x + char_w), (float)(y + char_off_y + char_h));

            x += font->getCharAdvance(c);
        }
        y += font_height;
        pos += num_chars;
    }
    // T2: if HUD buffering is active, capture pre-expanded glyph geometry and defer
    if (renderStates_[gos_State_IsHUD]) {
        const int n = text_->getNumVertices();
        if (n > 0 && !hudFlushed_) {
            HudDrawCall call;
            call.kind = kHudTextQuadBatch;
            call.vertices.assign(text_->getVertices(), text_->getVertices() + n);
            memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
            call.projection = projection_;
            call.fontTexId = tex_id;
            call.foregroundColor = ta.Foreground;
            call.scaleExempt = s_hud_scale_exempt;
            hudBatch_.push_back(std::move(call));
        } else if (hudFlushed_) {
            SPEW(("GRAPHICS", "[HUD] Late drawText discarded (after flushHUDBatch)\n"));
        }
        text_->rewind();
        return;
    }

    // Save Texture/Filter/TextureAddress so the text draw doesn't leak
    // state to whatever the next caller relies on. Filter was being
    // leaked previously (only Texture was saved); TextureAddress is now
    // forced to clamp because the default wrap mode samples adjacent
    // glyphs at atlas edges with nearest filtering.

    int prev_texture = getRenderState(gos_State_Texture);
    int prev_filter  = getRenderState(gos_State_Filter);
    int prev_addr    = getRenderState(gos_State_TextureAddress);

    setRenderState(gos_State_Texture, tex_id);
    setRenderState(gos_State_Filter, gos_FilterNone);
    setRenderState(gos_State_TextureAddress, gos_TextureClamp);

    // for now draw anyway because no render state saved for draw calls
    applyRenderStates();
    gosRenderMaterial* mat = text_material_;

    //ta.Foreground
    vec4 fg;
    fg.x = (float)((ta.Foreground & 0xFF0000) >> 16);
    fg.y = (float)((ta.Foreground & 0xFF00) >> 8);
    fg.z = (float)(ta.Foreground & 0xFF);
    fg.w = 255.0f;//(ta.Foreground & 0xFF000000) >> 24;
    fg = fg / 255.0f;
    mat->getShader()->setFloat4(s_Foreground, fg);
    //ta.Size
    //ta.WordWrap
    //ta.Proportional
    //ta.Bold
    //ta.Italic
    //ta.WrapType
    //ta.DisableEmbeddedCodes

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    text_->draw(mat);
    text_->rewind();

    setRenderState(gos_State_Texture, prev_texture);
    setRenderState(gos_State_Filter, prev_filter);
    setRenderState(gos_State_TextureAddress, prev_addr);

    afterDrawCall();
}

void gosRenderer::replayTextQuads(const HudDrawCall& call)
{
    if (call.vertices.empty()) return;

    text_->addVertices(const_cast<gos_VERTEX*>(call.vertices.data()),
                       (int)call.vertices.size());

    int prev_texture = getRenderState(gos_State_Texture);
    setRenderState(gos_State_Texture, call.fontTexId);
    setRenderState(gos_State_Filter, gos_FilterNone);

    applyRenderStates();
    gosRenderMaterial* mat = text_material_;

    vec4 fg;
    fg.x = (float)((call.foregroundColor & 0xFF0000) >> 16) / 255.0f;
    fg.y = (float)((call.foregroundColor & 0xFF00)   >>  8) / 255.0f;
    fg.z = (float)( call.foregroundColor & 0xFF)            / 255.0f;
    fg.w = 1.0f;
    mat->getShader()->setFloat4(s_Foreground, fg);

    mat->setTransform(projection_);
    mat->setFogColor(fog_color_);
    text_->draw(mat);
    text_->rewind();

    setRenderState(gos_State_Texture, prev_texture);
}

// UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1: observe-only telemetry entry points, defined in
// mclib/render_contract.cpp (GL-aware TU; reuses the ambient-probe sample helpers).
extern "C" void mc2_ui_pass_sample_entry_ambient();
extern "C" void mc2_ui_pass_note_draw(unsigned kind);
extern "C" void mc2_ui_pass_check_ambient();

static bool vkHudQueueEnabled()
{
    // VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): the game HUD is UI CONTENT, and post-
    // VK-RUNG8-ITEM8 the native VK composite is the sole present-image producer -- the GL 2D
    // HUD draws into the DISCARDED GL backbuffer, so under the umbrella an unarmed VK HUD queue
    // means the HUD VANISHES (the field bug; pre-b0b8ae2b the HUD survived only because present
    // fell back to the GL backbuffer). So arm from the umbrella (subGate): MC2_VK_SHARED_GBUFFER=1
    // queues the HUD into the VK present tail; explicit "1"/"0" still wins. The strict proof armed
    // MC2_VK_HUD=1 and passed. (Representable-draw filtering + the SKIP_GL all-or-nothing guard
    // below still apply -- a non-representable HUD element keeps GL HUD live for parity.)
    static const bool enabled = mc2vkgate::subGate("MC2_VK_HUD");
    return enabled;
}

#ifdef MC2_VULKAN
static bool vkHudSkipGlEnabled()
{
    // VK-FIELD-ENV-CONTENT-STACK-ARM-1: arm from the umbrella too. Skipping the GL HUD is only
    // an optimization under VK-only present (the GL backbuffer is discarded anyway), and it is
    // self-protecting: nativeHudCanSkipGl is AND-reduced over drawRepresentable, so GL HUD stays
    // live whenever any HUD element is not VK-representable. Explicit "1"/"0" still wins.
    static const bool enabled = mc2vkgate::subGate("MC2_VK_HUD_SKIP_GL");
    return enabled;
}

static bool vkHudDrawRepresentable(const mc2vk::VkHudDraw& draw)
{
    // AlphaMode 1 (OneZero/opaque), 3 (AlphaInvAlpha) and 4 (OneInvAlpha, the
    // effect-sprite blend) are all rasterizable by the tail. AlphaMode 4 now binds a
    // dedicated OneInvAlpha textured pipe (ONE, 1-srcA) in engine_present_vk.cpp that
    // matches GL's glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) — the additive-glow
    // effect blend — so beams/tracers/explosions composite at GL brightness.
    if (draw.alphaMode != 1 && draw.alphaMode != 3 && draw.alphaMode != 4)
        return false;
    const bool textured = draw.textureHandle != 0 && draw.textureHandle != INVALID_TEXTURE_ID;
    if (textured) {
        if (draw.glTextureId == 0 || draw.textureWidth == 0 || draw.textureHeight == 0 ||
            draw.textureSerial == 0 || draw.textureRgba.empty())
            return false;
        if (draw.kind == kHudQuadBatch)
            return draw.vertices.size() >= 4 && (draw.vertices.size() % 4) == 0;
        if (draw.kind == kHudTextQuadBatch)
            return draw.vertices.size() >= 4 &&
                   (((draw.vertices.size() % 4) == 0) || ((draw.vertices.size() % 3) == 0));
        if (draw.kind == kHudTriBatch)
            return !draw.vertices.empty() && (draw.vertices.size() % 3) == 0;
        return false;
    }
    if (draw.kind == kHudQuadBatch)
        return draw.vertices.size() >= 4 && (draw.vertices.size() % 4) == 0;
    if (draw.kind == kHudLineBatch)
        return !draw.vertices.empty() && (draw.vertices.size() % 2) == 0;
    if (draw.kind == kHudTriBatch)
        return !draw.vertices.empty() && (draw.vertices.size() % 3) == 0;
    return false;
}
#endif

void gosRenderer::flushHUDBatch()
{
    if (hudBatch_.empty()) {
        hudFlushed_ = true;
        return;
    }

    render_contract::beginPassScope(render_contract::PassIdentity::UI,
                                    "gosRenderer_flushHUDBatch");

    // UI-SAME-ORDER-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical OFF). PIN INVARIANT: additive validate counters only —
    // no GL state change, no reorder, no draw change. The flush body (hudBatch_ replay, the
    // memcpy state save/restore at :7519) runs UNCHANGED between begin and end. Placed after the
    // empty-batch early-return so begin only fires on non-empty frames; the RAII guard's dtor
    // fires end on ALL exit paths. FBO-only: Backbuffer (default FBO 0, bound by pp->endScene
    // pre-flush). Ambient is DO_NOT_MODEL — UI ambient is per-draw legacy gos dynamic state.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::UI,
                                              "gosRenderer_flushHUDBatch");
    struct TopLevelUiGuard_ {
        ~TopLevelUiGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::UI,
                                                    "gosRenderer_flushHUDBatch");
        }
    } _tlUiGuard;

    // HUD scale — shrink only in-game HUD (gated by gos_SetHudScaleActive, set
    // to true by mission->start() and false by mission->destroy()). Menus and
    // modal dialogs run through the same HUD buffer but stay at 100%; we skip
    // any call whose centroid is above 60% of the screen height.
    //
    // Anchor is SINGLE — bottom-center (sw/2, sh). Every bottom-band primitive
    // shrinks toward that one point. Previously we used 9-slice L/C/R anchoring
    // which pulled the tacmap/command clusters apart rather than compressing
    // the HUD strip as a coherent whole. Single-anchor keeps the strip
    // contiguous: tacmap drifts slightly inward-right, force-group stays put,
    // command panel drifts slightly inward-left, all without visible gaps.
    // HUD scale — shrink only in-game HUD (gated by gos_SetHudScaleActive, set
    // true by Mission::start and false by Mission::destroy). Menus and modal
    // dialogs run through the same HUD buffer but stay at 100%.
    //
    // Single bottom-center anchor (sw/2, sh). Every qualifying primitive
    // shrinks toward that one point. Centroid-based gate at 60% of screen
    // height — draws whose centroid is above that threshold are left alone
    // (catches dialogs, menus). This was the variant that looked good in
    // iteration; later attempts at max-Y gating / shrink-in-place broke more
    // than they fixed (touched scene/overlay draws, pulled tall panels out
    // of their corners, etc.).
    const float scale = s_hud_scale;
    if (s_hud_scale_active && scale < 0.999f) {
        const float sw = (float)width_;
        const float sh = (float)height_;
        const float bottomBand = sh * 0.60f;
        const float ax = sw * 0.5f;
        const float ay = sh;
        for (HudDrawCall& call : hudBatch_) {
            if (call.vertices.empty()) continue;
            if (call.scaleExempt) continue;   // cursor + modal dialogs: never shrink
            float cy = 0.0f;
            for (const gos_VERTEX& v : call.vertices) cy += v.y;
            cy /= (float)call.vertices.size();
            if (cy < bottomBand) continue;
            for (gos_VERTEX& v : call.vertices) {
                v.x = ax + (v.x - ax) * scale;
                v.y = ay + (v.y - ay) * scale;
            }
        }
    }

    // pp->endScene() binds FB 0 and sets the full-screen viewport for us,
    // but leaves VAO 0 bound — rebind our VAO so glVertexAttribPointer works.
    glBindVertexArray(gVAO);

    // UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1: observe-only telemetry inside the existing
    // PassIdentity::UI RAII scope. Gate MC2_UI_PASS_MEASURE (default-OFF) — when unset
    // neither the entry sample nor the per-draw note fires, so the HUD replay is
    // byte-identical. Read-only glGet inside mc2_ui_pass_sample_entry_ambient() (samples
    // the same ambient axes the ambient contract tracks: colorMask/depthFunc/blend/
    // depthWrite); NO GL state change. Toward promoting UI DO_NOT_MODEL -> modeled.
    static const bool s_uiPassMeasure = (::getenv("MC2_UI_PASS_MEASURE") != nullptr);
    if (s_uiPassMeasure) {
        // ONE ambient entry sample, taken after the VAO rebind, before the replay loop.
        mc2_ui_pass_sample_entry_ambient();
    }

    if (s_uiPassMeasure) {
        unsigned calls[4] = {};
        unsigned textured = 0;
        unsigned alphaModes[8] = {};
        unsigned totalVertices = 0;
        for (const HudDrawCall& call : hudBatch_) {
            const unsigned kind = (unsigned)call.kind;
            if (kind < 4) ++calls[kind];
            if (call.stateSnapshot[gos_State_Texture] != 0 || call.kind == kHudTextQuadBatch)
                ++textured;
            const unsigned alpha = call.stateSnapshot[gos_State_AlphaMode];
            if (alpha < 8) ++alphaModes[alpha];
            totalVertices += (unsigned)call.vertices.size();
        }

        static unsigned s_hudMeasureFrames = 0;
        ++s_hudMeasureFrames;
        if (s_hudMeasureFrames <= 10 || (s_hudMeasureFrames % 120) == 0) {
            std::fprintf(stderr,
                "[VK_HUD_AUDIT v1] frame=%u calls=%u quads=%u lines=%u tris=%u text=%u "
                "textured=%u vertices=%u alpha0=%u alpha1=%u alpha2=%u alpha3=%u\n",
                s_hudMeasureFrames, (unsigned)hudBatch_.size(),
                calls[0], calls[1], calls[2], calls[3],
                textured, totalVertices,
                alphaModes[0], alphaModes[1], alphaModes[2], alphaModes[3]);
        }
    }

#ifdef MC2_VULKAN
    bool nativeHudQueued = false;
    bool nativeHudCanSkipGl = vkHudSkipGlEnabled();
    if (vkHudQueueEnabled()) {
        std::vector<mc2vk::VkHudDraw> nativeDraws;
        nativeDraws.reserve(hudBatch_.size());
        for (const HudDrawCall& call : hudBatch_) {
            mc2vk::VkHudDraw draw;
            draw.kind = (uint32_t)call.kind;
            draw.textureHandle = (call.kind == kHudTextQuadBatch)
                ? (uint32_t)call.fontTexId
                : (uint32_t)call.stateSnapshot[gos_State_Texture];
            if (draw.textureHandle != 0 && draw.textureHandle != INVALID_TEXTURE_ID) {
                if (gosTexture* tex = getTexture(draw.textureHandle)) {
                    gosTextureInfo ti;
                    tex->getTextureInfo(&ti);
                    draw.glTextureId = tex->getTextureId();
                    draw.textureWidth = (uint32_t)ti.width_;
                    draw.textureHeight = (uint32_t)ti.height_;
                    draw.textureFormat = (uint32_t)ti.format_;
                    draw.textureSerial = tex->getContentSerial();
                    if (draw.textureWidth != 0 && draw.textureHeight != 0 && tex->hasRgbaMirror())
                        draw.textureRgba = tex->getRgbaMirror();
                }
            }
            draw.alphaMode = call.stateSnapshot[gos_State_AlphaMode];
            draw.textureAddress = call.stateSnapshot[gos_State_TextureAddress];
            draw.filter = call.stateSnapshot[gos_State_Filter];
            draw.foregroundColor = call.foregroundColor;
            draw.vertices.reserve(call.vertices.size());
            for (const gos_VERTEX& v : call.vertices) {
                mc2vk::VkHudVertex out;
                out.x = v.x; out.y = v.y; out.z = v.z; out.rhw = v.rhw;
                out.argb = v.argb; out.frgb = v.frgb;
                out.u = v.u; out.v = v.v;
                draw.vertices.push_back(out);
            }
            // VK-FIELD-VFX-3: per-draw self-decline instead of the all-or-nothing veto.
            // The VK tail renderer (recordPendingHudColorProof) ALREADY skips each draw
            // it cannot represent (no texture mirror / unsupported kind) individually
            // (`continue`), so one non-representable draw need NOT force the WHOLE HUD to
            // the DISCARDED GL backbuffer. Under the VFX umbrella we always composite the
            // native tail: the representable subset (brackets/bars/beams/HUD/text + mirror-
            // backed effect sprites) survives, and the rest drops per-draw (it would be
            // lost to the discarded GL HUD anyway). This removes the poisoning landmine
            // where any live textured effect killed the entire HUD composite, and lets the
            // effect classes (carnage/muzzle/artillery) route through IsHUD safely.
            static const bool s_hudPerDrawDecline = mc2vkgate::subGate("MC2_VK_VFX");
            const bool drawRepresentable = vkHudDrawRepresentable(draw);
            if (!drawRepresentable) {
                static unsigned s_nativeHudDeclineLogs = 0;
                if (s_nativeHudDeclineLogs < 16) {
                    std::fprintf(stderr,
                        "[VK_HUD_SKIP_DECLINE v1] kind=%u tex=%u gl=%u wh=%ux%u serial=%u rgba=%zu alpha=%u verts=%zu perDrawDecline=%d\n",
                        draw.kind, draw.textureHandle, draw.glTextureId,
                        draw.textureWidth, draw.textureHeight, draw.textureSerial,
                        draw.textureRgba.size(), draw.alphaMode, draw.vertices.size(),
                        (int)s_hudPerDrawDecline);
                    ++s_nativeHudDeclineLogs;
                }
            }
            if (!s_hudPerDrawDecline)
                nativeHudCanSkipGl = nativeHudCanSkipGl && drawRepresentable;
            // VK-HUD-GLYPH-CORRUPTION diagnostic: dump per-draw texture handle+serial
            // for the first few frames so an intra-frame same-handle/different-serial
            // collision (mirror keyed by handle -> last-upload-wins -> stale glyphs)
            // is directly visible. Gated by MC2_VK_HUD_DRAW_DUMP.
            if (std::getenv("MC2_VK_HUD_DRAW_DUMP")) {
                static unsigned s_hudDumpLines = 0;
                if (s_hudDumpLines < 400 && draw.textureHandle != 0 &&
                    draw.textureHandle != INVALID_TEXTURE_ID) {
                    ++s_hudDumpLines;
                    std::fprintf(stderr, "[VK_HUD_DRAW_DUMP] batchseq=%u kind=%u handle=%u "
                        "serial=%u wh=%ux%u verts=%zu\n", s_hudDumpLines,
                        draw.kind, draw.textureHandle, draw.textureSerial,
                        draw.textureWidth, draw.textureHeight, draw.vertices.size());
                }
            }
            nativeDraws.push_back(std::move(draw));
        }
        // HUD-CANVAS-EXTENT-FIX: pass the logical canvas the HUD vertices were authored
        // in (width_/height_ == Environment.screenWidth/Height) so the VK tail maps them
        // to NDC correctly even when the present image (physical drawable) differs.
        nativeHudQueued = mc2vk::getOrCreateEnginePresentVk().queueHudVulkanFrame(
            std::move(nativeDraws), (uint32_t)width_, (uint32_t)height_);
    }
#endif

    // UI-PASS-MODEL-1: UI is now MODELED (declared ambient row in kPassAmbient[]). Verify
    // the UI entry ambient against that row every frame — OBSERVE-ONLY (own counter,
    // non-fatal), read-only glGet, NO GL state change, so the HUD replay stays byte-identical.
    // This closes the last DO_NOT_MODEL hole: the ambient guard checks UI entry state instead
    // of skipping it. Default-ON (not gated) — the check is cheap glGet + compare.
    mc2_ui_pass_check_ambient();

#ifdef MC2_VULKAN
    if (nativeHudQueued && nativeHudCanSkipGl) {
        static unsigned s_nativeHudSkipFrames = 0;
        ++s_nativeHudSkipFrames;
        if (s_nativeHudSkipFrames <= 10 || (s_nativeHudSkipFrames % 120) == 0) {
            std::fprintf(stderr,
                "[VK_HUD_SKIP v1] frame=%u draws=%u reason=\"native_full_coverage\"\n",
                s_nativeHudSkipFrames, (unsigned)hudBatch_.size());
        }
        hudFlushed_ = true;
        s_hud_scale_exempt = false;
        render_contract::endPassScope(render_contract::PassIdentity::UI,
                                      "gosRenderer_flushHUDBatch");
        return;
    }
#endif

    // Save pre-flush render state and projection
    uint32_t priorState[gos_MaxState];
    memcpy(priorState, renderStates_, sizeof(priorState));
    mat4 priorProjection = projection_;

    for (const HudDrawCall& call : hudBatch_) {
        memcpy(renderStates_, call.stateSnapshot, sizeof(renderStates_));
        renderStates_[gos_State_IsHUD] = 0;   // clear to prevent re-buffering on replay
        projection_ = call.projection;

        switch (call.kind) {
            case kHudQuadBatch:
                if (s_uiPassMeasure) mc2_ui_pass_note_draw(0);
                drawQuads(const_cast<gos_VERTEX*>(call.vertices.data()),
                          (int)call.vertices.size());
                break;
            case kHudLineBatch:
                if (s_uiPassMeasure) mc2_ui_pass_note_draw(1);
                drawLines(const_cast<gos_VERTEX*>(call.vertices.data()),
                          (int)call.vertices.size());
                break;
            case kHudTriBatch:
                if (s_uiPassMeasure) mc2_ui_pass_note_draw(2);
                drawTris(const_cast<gos_VERTEX*>(call.vertices.data()),
                         (int)call.vertices.size());
                break;
            case kHudTextQuadBatch:
                if (s_uiPassMeasure) mc2_ui_pass_note_draw(3);
                replayTextQuads(call);
                break;
        }
    }

    // Restore pre-flush render state and projection
    memcpy(renderStates_, priorState, sizeof(priorState));
    projection_ = priorProjection;
    hudFlushed_ = true;

    // Auto-clear the scale-exemption latch each frame so it can never leak into
    // the next frame's non-exempt HUD recording. Callers (cursor sprite, modal
    // dialogs) set it true around their own draws; this is the single reset.
    s_hud_scale_exempt = false;

    render_contract::endPassScope(render_contract::PassIdentity::UI,
                                  "gosRenderer_flushHUDBatch");
}

void gosRenderer::flush()
{
}

void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {
    ZoneScopedN("gos_CreateRenderer");

    g_gos_renderer = new gosRenderer(ctx_h, win_h, w, h);
    g_gos_renderer->init();

    gosPostProcess* pp = new gosPostProcess();
    pp->init(w, h);

    // Initialize diagnostic JSONL trace. Session id shared with debug_state_dump
    // so MCP can correlate JSONL events with render-state snapshots by session_id.
    mc2_diag::init(mc2_debug_state::getSessionId(), _getpid());

    // BUILD — one-shot build metadata; fires immediately after init().
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"commit\":\"" MC2_BUILD_GIT_SHA "\","
            "\"dirty\":%s,"
            "\"branch\":\"" MC2_BUILD_GIT_BRANCH "\","
            "\"build_time\":\"" MC2_BUILD_TIME_ISO "\","
            "\"config\":\"" MC2_DIAG_BUILD_CONFIG "\","
            "\"platform\":\"windows\"}",
            MC2_BUILD_GIT_DIRTY ? "true" : "false");
        mc2_diag::writeEvent("BUILD", 1, 0, buf);
    }

    // DEVICE — one-shot GL device info; GL context is valid (g_gos_renderer->init() ran above).
    {
        const char* vendor   = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* glsl     = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"gl_vendor\":\"%s\","
            "\"gl_renderer\":\"%s\","
            "\"gl_version\":\"%s\","
            "\"glsl_version\":\"%s\"}",
            vendor   ? vendor   : "<unavailable>",
            renderer ? renderer : "<unavailable>",
            version  ? version  : "<unavailable>",
            glsl     ? glsl     : "<unavailable>");
        mc2_diag::writeEvent("DEVICE", 1, 0, buf);
    }
}

void gos_DestroyRenderer() {

    // Write shutdown state dump and flush diagnostics BEFORE any resource teardown.
    // Both functions read render/postprocess state — must run while it is still valid.
    mc2_debug_state::writeShutdownState();
    mc2_diag::shutdown();

    gosPostProcess* pp = getGosPostProcess();
    if (pp) { pp->destroy(); delete pp; }

    g_gos_renderer->destroy();
    delete g_gos_renderer;
}

void gos_RendererBeginFrame() {
    gosASSERT(g_gos_renderer);
    // F3 CPU projection cost-baseline: idempotent first-call init, then
    // reset per-frame bucket accumulators. No-op when env OFF.
    ::mc2_cpu_proj_cost::init_from_env();
    ::mc2_cpu_proj_cost::frame_begin();
    g_gos_renderer->beginFrame();
}

void gos_RendererEndFrame() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->endFrame();
    // F3 CPU projection cost-baseline: commit per-frame samples; print
    // window stats every 500 frames. No-op when env OFF.
    ::mc2_cpu_proj_cost::frame_end();
    // [B1 C16] gosFX heap + child counter tick; env-gated; no-op when OFF.
    ::gosFX::DiagFrameTick();
}

void gos_RendererFlushHUDBatch() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->flushHUDBatch();
}

// pp->endScene() leaves VAO 0 bound. Any caller that needs to issue
// gos_Draw* calls after endScene (e.g. projectz_overlay_render) must
// rebind gVAO first, same as flushHUDBatch() does internally.
void gos_RendererRebindVAO() {
    glBindVertexArray(gVAO);
}

void gos_RendererHandleEvents() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->handleEvents();
}


gosFont::~gosFont()
{
    if(tex_id_ != INVALID_TEXTURE_ID)
        getGosRenderer()->deleteTexture(tex_id_);

    delete[] gi_.glyphs_;
    delete[] gi_.ink_top_;
    delete[] gi_.ink_bot_;
    delete[] gi_.ink_valid_;
    delete[] font_name_;
    delete[] font_id_;
}

////////////////////////////////////////////////////////////////////////////////
gosFont* gosFont::load(const char* fontFile) {

    char fname[256];
    char dir[256];
    _splitpath(fontFile, NULL, dir, fname, NULL);

    // Retail .d3f wins when present. .bmp + .glyph stays as the
    // permanent fallback for converted fonts and community content.
    {
        const char* d3f_ext = ".d3f";
        const size_t d3fNameSize = strlen(fname) + 1 + strlen(dir) + strlen(d3f_ext) + 1;
        char* d3fName = new char[d3fNameSize];
        memset(d3fName, 0, d3fNameSize);
        uint32_t d3f_len = S_snprintf(d3fName, d3fNameSize, "%s/%s%s", dir, fname, d3f_ext);
        gosASSERT(d3f_len <= d3fNameSize - 1);

        gosGlyphInfo gi;
        gosD3FAtlas atlas;
        if(gos_load_d3f(d3fName, gi, atlas)) {
            // Legacy .glyph sidecar bridge — when a same-basename
            // .glyph exists alongside the .d3f, adopt its line spacing
            // and max-advance globals. UI widgets were authored against
            // those values; D3F's dwFontHeight em-box would pack lines
            // ~2x denser than retail. font_ascent_ is intentionally
            // left at the calibrated value (visible band height) so
            // the per-glyph maxy/miny set by calibrate_vertical stay
            // consistent with the renderer's char_off_y math.
            //
            // .glyph header layout (matches gos_load_glyphs):
            //   u32 num_glyphs, start_glyph, max_advance, ascent, line_skip
            {
                const char* glyph_ext = ".glyph";
                const size_t sidecarNameSize = strlen(fname) + 1 + strlen(dir) + strlen(glyph_ext) + 1;
                char* sidecarName = new char[sidecarNameSize];
                memset(sidecarName, 0, sidecarNameSize);
                S_snprintf(sidecarName, sidecarNameSize, "%s/%s%s", dir, fname, glyph_ext);

                FILE* sidecar = fopen(sidecarName, "rb");
                if(sidecar) {
                    uint32_t legacy_globals[5] = {0};
                    if(fread(legacy_globals, sizeof(uint32_t), 5, sidecar) == 5) {
                        gi.max_advance_    = legacy_globals[2];
                        gi.font_line_skip_ = legacy_globals[4];
                    }
                    fclose(sidecar);
                }
                delete[] sidecarName;
            }

            DWORD tex_id = gos_NewEmptyTexture(gos_Texture_Alpha, d3fName,
                                               RECT_TEX(atlas.width, atlas.height), 0);
            if(tex_id != 0) {
                // Expand 8-bit alpha to RGBA8: fan alpha into R for the
                // gos_text shader's .xxxx sample. Other channels also
                // populated so any future shader change still gets sane data.
                const size_t pixel_count = (size_t)atlas.width * (size_t)atlas.height;
                DWORD* rgba = new DWORD[pixel_count];
                for(size_t i = 0; i < pixel_count; ++i) {
                    uint32_t a = atlas.pixels[i];
                    rgba[i] = (a) | (a << 8) | (a << 16) | (a << 24);
                }
                delete[] atlas.pixels;
                atlas.pixels = NULL;

                GLuint gl_id = gos_GetTextureGLId(tex_id);
                glBindTexture(GL_TEXTURE_2D, gl_id);
                // Save/restore GL_UNPACK_ALIGNMENT — it's global state
                // and a later texture upload may rely on a different
                // value (driver default is 4, but other code paths set
                // it to 1 for tightly-packed sources).
                GLint prev_unpack_alignment = 0;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack_alignment);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                atlas.width, atlas.height,
                                GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack_alignment);
                glBindTexture(GL_TEXTURE_2D, 0);
                if(gosTexture* tex = g_gos_renderer->getTexture(tex_id)) {
                    tex->setRgbaMirror(atlas.width, atlas.height, TF_RGBA8, rgba);
                    tex->markContentMutated();
                }
                delete[] rgba;

                gosFont* font = new gosFont();
                font->gi_ = gi;
                font->font_name_ = new char[strlen(fname) + 1];
                strcpy(font->font_name_, fname);
                font->font_id_ = new char[strlen(fontFile) + 1];
                strcpy(font->font_id_, fontFile);
                font->tex_id_ = tex_id;

                delete[] d3fName;
                return font;
            }
            // Texture allocation failed — release everything
            // calibrate_vertical allocated, including the per-glyph ink
            // bounds arrays, before falling through to the .bmp+.glyph
            // path.
            delete[] gi.glyphs_;
            delete[] gi.ink_top_;
            delete[] gi.ink_bot_;
            delete[] gi.ink_valid_;
            delete[] atlas.pixels;
        }
        delete[] d3fName;
    }

    const char* tex_ext = ".bmp";
    const char* glyph_ext = ".glyph";

	const size_t textureNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(tex_ext) + 1;
    char* textureName = new char[textureNameSize];
	memset(textureName, 0, textureNameSize);

	const size_t glyphNameSize = strlen(fname) + sizeof('/') + strlen(dir) + strlen(glyph_ext) + 1;
    char* glyphName = new char[glyphNameSize];
	memset(glyphName, 0, glyphNameSize);

    uint32_t formatted_len = S_snprintf(textureName, textureNameSize, "%s/%s%s", dir, fname, tex_ext);
	gosASSERT(formatted_len <= textureNameSize - 1);

    formatted_len = S_snprintf(glyphName, glyphNameSize, "%s/%s%s", dir, fname, glyph_ext);
	gosASSERT(formatted_len <= glyphNameSize - 1);

    gosTexture* ptex = new gosTexture(gos_Texture_Alpha, textureName, 0, NULL, 0, false);
    if(!ptex || !ptex->createHardwareTexture()) {
        STOP(("Failed to create font texture: %s\n", textureName));
    }

    DWORD tex_id = getGosRenderer()->addTexture(ptex);

    gosFont* font = new gosFont();
    if(!gos_load_glyphs(glyphName, font->gi_)) {
        delete font;
        STOP(("Failed to load font glyphs: %s\n", glyphName));
        return NULL;
    }

    font->font_name_ = new char[strlen(fname) + 1];
    strcpy(font->font_name_, fname);

    font->font_id_ = new char[strlen(fontFile) + 1];
    strcpy(font->font_id_, fontFile);

    font->tex_id_ = tex_id;

    delete[] textureName;
    delete[] glyphName;

    return font;

}

uint32_t gosFont::destroy(gosFont* font) {
    uint32_t rc = font->decRef();
    if(0 == rc) {
        delete font;
    }

    return rc;
}

void gosFont::getCharUV(int c, uint32_t* u, uint32_t* v) const {

    gosASSERT(u && v);

    int32_t pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_) {
        *u = *v = 0;
        return;
    }

    *u = gi_.glyphs_[pos].u;
    *v = gi_.glyphs_[pos].v;
}

int gosFont::getCharAdvance(int c) const
{
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_) {
        return getMaxCharWidth();
    }

    return gi_.glyphs_[pos].advance;
}

const gosGlyphMetrics& gosFont::getGlyphMetrics(int c) const {
    int pos = c - gi_.start_glyph_;
    if(pos < 0 || pos >= (int)gi_.num_glyphs_)
        pos = 0;

    return gi_.glyphs_[pos];
}




////////////////////////////////////////////////////////////////////////////////
// graphics
//
void _stdcall gos_DrawLines(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawLines(Vertices, NumVertices);
}
void _stdcall gos_DrawPoints(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawPoints(Vertices, NumVertices);
}

bool g_disable_quads = true;
void _stdcall gos_DrawQuads(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(Vertices, NumVertices);
}
void _stdcall gos_DrawTriangles(gos_VERTEX* Vertices, int NumVertices)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawTris(Vertices, NumVertices);
}

void __stdcall gos_GetViewport( float* pViewportMulX, float* pViewportMulY, float* pViewportAddX, float* pViewportAddY )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->getViewportTransform(pViewportMulX, pViewportMulY, pViewportAddX, pViewportAddY);
}

HGOSFONT3D __stdcall gos_LoadFont( const char* FontFile, DWORD StartLine/* = 0*/, int CharCount/* = 256*/, DWORD TextureHandle/*=0*/)
{

    gosFont* font = getGosRenderer()->findFont(FontFile);
    if(!font) {
        font = gosFont::load(FontFile);
        getGosRenderer()->addFont(font);
    } else {
        font->addRef();
    }

    return font;
}

void __stdcall gos_DeleteFont( HGOSFONT3D FontHandle )
{
    gosASSERT(FontHandle);
    gosFont* font = FontHandle;
    getGosRenderer()->deleteFont(font);
}

DWORD __stdcall gos_NewEmptyTexture( gos_TextureFormat Format, const char* Name, DWORD HeightWidth, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    int w = HeightWidth;
    int h = HeightWidth;
    if(HeightWidth&0xffff0000)
    {
        h = HeightWidth >> 16;
        w = HeightWidth & 0xffff;
    }
    gosTexture* ptex = new gosTexture(Format, Hints, w, h, Name);

    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}
DWORD __stdcall gos_NewTextureFromMemory( gos_TextureFormat Format, const char* FileName, BYTE* pBitmap, DWORD Size, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosASSERT(pFunc == 0);

    gosTexture* ptex = new gosTexture(Format, FileName, Hints, pBitmap, Size, true);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }

    return g_gos_renderer->addTexture(ptex);
}

// TEXMGR-COMPRESSED-UPLOAD-1: upload a single-level BC7 (or other block-
// compressed) 2D texture from a pre-loaded block stream and register it as a
// gosTexture handle. Mirrors gos_NewTextureFromMemory's lifecycle (gosTexture
// + addTexture), but uploads via glCompressedTexImage2D instead of decoding a
// TGA. Single level only (MC_TextureManager data/textures load with
// gosHint_DisableMipmap), so GL_TEXTURE_MAX_LEVEL=0 and GL_LINEAR filtering.
// Returns INVALID_TEXTURE_ID on GL failure (caller falls through to the RGBA8
// path). The internal format is passed in by the caller (sRGB BPTC for albedo).
DWORD __stdcall gos_NewCompressedTexture2D( uint32_t glInternalFormat, int w, int h,
                                            const uint8_t* blockData, size_t byteLen,
                                            const char* name )
{
    if(!blockData || byteLen == 0 || w <= 0 || h <= 0)
        return INVALID_TEXTURE_ID;

    while(glGetError() != GL_NO_ERROR) { /* drain */ }

    GLuint texID = 0;
    // TEX-CLASS: asset-pool -- gos_NewCompressedTexture2D content factory (BC7)
    glGenTextures(1, &texID);
    if(texID == 0)
        return INVALID_TEXTURE_ID;
    glBindTexture(GL_TEXTURE_2D, texID);

    // data/textures BC7 sidecars are single-level (DisableMipmap); match the
    // RGBA8 path's filtering (GL_LINEAR/no-mip) and cap the mip ladder.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glCompressedTexImage2D(GL_TEXTURE_2D, 0, (GLenum)glInternalFormat,
                           w, h, 0, (GLsizei)byteLen, blockData);

    GLenum err = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if(err != GL_NO_ERROR) {
        printf("[TEXMGR_BC7] glCompressedTexImage2D failed name=%s %dx%d fmt=0x%x err=%s\n",
               name ? name : "<null>", w, h, glInternalFormat, ogl_get_error_code_str(err));
        glDeleteTextures(1, &texID);
        return INVALID_TEXTURE_ID;
    }

    Texture tex;
    tex.id = texID;
    tex.w = w;
    tex.h = h;
    tex.fmt_ = TF_NONE;          // block-compressed; not one of the RGBA8 TexFormats
    tex.type_ = TT_2D;
    tex.format = (GLenum)glInternalFormat;
    tex.has_mipmaps = false;

    gosTexture* ptex = new gosTexture(tex, gos_Texture_Solid, name);
    return g_gos_renderer->addTexture(ptex);
}

DWORD __stdcall gos_NewTextureFromFile( gos_TextureFormat Format, const char* FileName, DWORD Hints/*=0*/, gos_RebuildFunction pFunc/*=0*/, void *pInstance/*=0*/)
{
    gosTexture* ptex = new gosTexture(Format, FileName, Hints, NULL, 0, false);
    if(!ptex->createHardwareTexture()) {
        STOP(("Failed to create texture\n"));
        return INVALID_TEXTURE_ID;
    }
    return g_gos_renderer->addTexture(ptex);
}
void __stdcall gos_DestroyTexture( DWORD Handle )
{
    g_gos_renderer->deleteTexture(Handle);
}

void __stdcall gos_LockTexture( DWORD Handle, DWORD MipMapSize, bool ReadOnly, TEXTUREPTR* TextureInfo )
{
    // TODO: does not really locks texture
    
    // not implemented yet
    gosASSERT(MipMapSize == 0);
    int mip_level = 0; //func(MipMapSize);

    gosTextureInfo info;
    int pitch = 0;
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->getTextureInfo(&info);
    BYTE* pdata = ptex->Lock(mip_level, ReadOnly, &pitch);

    TextureInfo->pTexture = (DWORD*)pdata;
    TextureInfo->Width = info.width_;
    TextureInfo->Height = info.height_;
    TextureInfo->Pitch = pitch;
    TextureInfo->Type = info.format_;

    //gosASSERT(0 && "Not implemented");
}

void __stdcall gos_UnLockTexture( DWORD Handle )
{
    gosTexture* ptex = g_gos_renderer->getTexture(Handle);
    ptex->Unlock();

    //gosASSERT(0 && "Not implemented");
}

uint32_t __stdcall gos_GetTextureGLId( DWORD Handle )
{
    // gosRenderer::getTexture asserts on INVALID_TEXTURE_ID and
    // out-of-range handles; in release builds gosASSERT is a no-op so
    // the out-of-range case is UB. Pre-validate the obvious invalid
    // cases so the documented "returns 0 for invalid handle" contract
    // holds for callers that may probe with a sentinel.
    if(!g_gos_renderer || Handle == INVALID_TEXTURE_ID)
        return 0;
    gosTexture* tex = g_gos_renderer->getTexture( Handle );
    return tex ? tex->getTextureId() : 0;
}


void __stdcall gos_PushRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->pushRenderStates();
} 

void __stdcall gos_PopRenderStates()
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->popRenderStates();
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(pVertexArray, NumberVertices, lpwIndices, NumberIndices);
}

void __stdcall gos_RenderIndexedArray( gos_VERTEX_2UV* pVertexArray, DWORD NumberVertices, WORD* lpwIndices, DWORD NumberIndices )
{
   gosASSERT(0 && "not implemented");
}

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(ib, vb, vdecl, mvp);
}

void __stdcall gos_RenderIndexedArray(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawIndexedTris(ib, vb, vdecl);
}

void __stdcall gos_SetRenderState( gos_RenderState RenderState, int Value )
{
    gosASSERT(g_gos_renderer);
    // gos_BlendDecal mode is not suported (currently texture color always modulated with vertex color)
    //gosASSERT(RenderState!=gos_State_TextureMapBlend || (Value == gos_BlendDecal));
    g_gos_renderer->setRenderState(RenderState, Value);
}

// [HUD-RES-CLAMP v1] runtime gate. Default ON (game path). The Mission Editor
// disables it via gos_SetHudResClampEnabled(false) so it renders at native
// window resolution; clamping the editor's render base to 800x600 desyncs the
// GL viewport (gos_GetViewport) from the MFC window/mouse space and corrupts
// editor object pick + drag-move projection. Runtime gate (not #ifdef
// MC2_IS_EDITOR) because gameos_graphics.cpp is compiled into the gameos_editor
// library WITHOUT that define (GameOS/gameos/CMakeLists.txt:104).
static bool g_hudResClampEnabled = true;
void __stdcall gos_SetHudResClampEnabled( bool enabled )
{
    g_hudResClampEnabled = enabled;
}

void __stdcall gos_SetScreenMode( DWORD Width, DWORD Height, DWORD bitDepth/*=16*/, DWORD Device/*=0*/, bool disableZBuffer/*=0*/, bool AntiAlias/*=0*/, bool RenderToVram/*=0*/, bool GotoFullScreen/*=0*/, int DirtyRectangle/*=0*/, bool GotoWindowMode/*=0*/, bool EnableStencil/*=0*/, DWORD Renderer/*=0*/)
{
    ZoneScopedN("gos_SetScreenMode");
    gosASSERT(g_gos_renderer);
    gosASSERT((GotoFullScreen && !GotoWindowMode) || (!GotoFullScreen&&GotoWindowMode) || (!GotoFullScreen&&!GotoWindowMode));

    // [HUD-RES-CLAMP v1] The legacy 2D HUD/UI is authored only for the discrete
    // tuned widths {640,800,1024,1280,1600,1920} (ControlGui::swapResolutions).
    // A non-tuned width (e.g. 2560/4096 — what the in-game options menu writes
    // on a hi-dpi desktop, see options_cfg_resolution_drift) lands on the
    // untuned else path and breaks the HUD (pause top-right, loading top-left,
    // comms-video misposition, double-shrunk bottom bar) while the scene stays
    // fine. We render the whole frame at the canonical 800x600 base and let
    // FULLSCREEN_DESKTOP upscale it — identical to the known-good shipped 4K
    // config (options.cfg ResolutionX=800). This makes the HUD immune to
    // whatever the options menu writes. (A native-scene + 800-HUD lane split is
    // deferred to the incoming imgui UI; it would touch camera+input.)
    // Memory: hud_scene_resolution_separation.
    // EDITOR: gated off (gos_SetHudResClampEnabled(false)) — the editor has no
    // legacy 2D HUD (ImGui) and must render at native res so pick/drag align.
    if (g_hudResClampEnabled)
    {
        Width  = 800;
        Height = 600;
    }

    g_gos_renderer->setScreenMode(Width, Height, bitDepth, GotoFullScreen, AntiAlias);
}

// [FORCE-43 v1] Centered 4:3 pillarbox rect. Single source of truth shared by
// the scene composite letterbox, the mouse box-relative remap, and HUD viewport.
bool __stdcall gos_Compute43Box( int w, int h, int* ox, int* oy, int* obw, int* obh )
{
    // Default-OFF gate (matches the launcher toggle model: checked -> MC2_FORCE_43=1,
    // unchecked -> unset). ON only when the var is present and not "0". Unset ->
    // byte-identical legacy (full surface), so smoke without the launcher is a no-op.
    static const bool s_on =
        []{ const char* e = getenv("MC2_FORCE_43"); return e && e[0] != '\0' && e[0] != '0'; }();
    if (!s_on) {                            // gate off -> full surface, no-op
        if (ox) *ox = 0;
        if (oy) *oy = 0;
        if (obw) *obw = w;
        if (obh) *obh = h;
        return false;
    }
    // CAMERA-FRUSTUM-HARNESS-1: pure rect math delegated to camera_frustum_math.h
    // (same arithmetic), exercised game-free by tools/camera_frustum_harness/.
    return camera_frustum_math::compute43BoxRect(w, h, ox, oy, obw, obh);
}

void __stdcall gos_SetupViewport( bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil/*=0*/, DWORD StencilValue/*=0*/)
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setupViewport(FillZ, ZBuffer, FillBG, BGColor, top, left, bottom, right, ClearStencil, StencilValue);
}


void __stdcall gos_SetRenderViewport(float x, float y, float w, float h)
{
    gosASSERT(g_gos_renderer);
	//glViewport(x, y, w, h);
	g_gos_renderer->setRenderViewport(vec4(x, y, w, h));
}

void __stdcall gos_GetRenderViewport(float* x, float* y, float* w, float* h)
{
    gosASSERT(x && y && w && h);
    gosASSERT(g_gos_renderer);
	vec4 vp = g_gos_renderer->getRenderViewport();
	*x = vp.x;
	*y = vp.y;
	*w = vp.z;
	*h = vp.w;
}


void __stdcall gos_TextDraw( const char *Message, ... )
{

	if (!Message || !strlen(Message)) {
        SPEW(("GRAPHICS", "Trying to draw zero legth string\n"));
        return;
    }

	va_list	ap;
    va_start(ap, Message);

    static const int MAX_TEXT_LEN = 4096;
	char text[MAX_TEXT_LEN] = {0};

	vsnprintf(text, MAX_TEXT_LEN - 1, Message, ap);

	size_t len = strlen(text);
	text[len] = '\0';

    va_end(ap);

    gosASSERT(g_gos_renderer);
    g_gos_renderer->drawText(text);
}

void __stdcall gos_TextDrawBackground( int Left, int Top, int Right, int Bottom, DWORD Color )
{
    // TODO: Is it correctly Implemented?
    gosASSERT(g_gos_renderer);

    //PAUSE((""));

    gos_VERTEX v[4];
    v[0].x = (float)Left;
    v[0].y = (float)Top;
    v[0].z = 0;
	v[0].argb = Color;
	v[0].frgb = 0;
	v[0].u = 0;	
	v[0].v = 0;	
    memcpy(&v[1], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[2], &v[0], sizeof(gos_VERTEX));
    memcpy(&v[3], &v[0], sizeof(gos_VERTEX));
    v[1].x = (float)Right;
    v[1].u = 1.0f;

    v[2].x = (float)Right;
    v[2].y = (float)Bottom;
    v[2].u = 1.0f;
    v[2].v = 0.0f;

    v[1].y = (float)Bottom;
    v[1].v = 1.0f;

    if(g_disable_quads == false )
        g_gos_renderer->drawQuads(v, 4);
}

void __stdcall gos_TextSetAttributes( HGOSFONT3D FontHandle, DWORD Foreground, float Size, bool WordWrap, bool Proportional, bool Bold, bool Italic, DWORD WrapType/*=0*/, bool DisableEmbeddedCodes/*=0*/)
{
    gosASSERT(g_gos_renderer);

    gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    ta.FontHandle = FontHandle;
    ta.Foreground = Foreground;
    ta.Size = Size;
    ta.WordWrap = WordWrap;
    ta.Proportional = Proportional;
    ta.Bold = Bold;
    ta.Italic = Italic;
    ta.WrapType = WrapType;
    ta.DisableEmbeddedCodes = DisableEmbeddedCodes;
}

void __stdcall gos_TextSetPosition( int XPosition, int YPosition )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextPos(XPosition, YPosition);
}

void __stdcall gos_TextSetRegion( int Left, int Top, int Right, int Bottom )
{
    gosASSERT(g_gos_renderer);
    g_gos_renderer->setTextRegion(Left, Top, Right, Bottom);
}

void __stdcall gos_TextStringLength( DWORD* Width, DWORD* Height, const char *fmt, ... )
{
    gosASSERT(Width && Height);

    if(!fmt) {
        SPEW(("GRAPHICS", "No text to calculate length!"));
        *Width = 1;
        *Height = 1;
        return;
    }

    const int   MAX_TEXT_LEN = 4096;
	char        text[MAX_TEXT_LEN] = {0};
	va_list	    ap;

    va_start(ap, fmt);
	vsnprintf(text, MAX_TEXT_LEN - 1, fmt, ap);
    va_end(ap);

	size_t len = strlen(text);
    text[len] = '\0';

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    gosASSERT(font);

    int num_newlines = 0;
    int max_width = 0;
    int cur_width = 0;
    const char* txtptr = text;

    while(*txtptr) {
        if(*txtptr == '\n') {
            num_newlines++;
            max_width = max_width > cur_width ? max_width : cur_width;
            cur_width = 0;
        } else {
            const int cw = font->getCharAdvance(*txtptr);
            cur_width += cw;
        }
        txtptr++;
    }
    max_width = max_width > cur_width ? max_width : cur_width;

    *Width = max_width;
    *Height = (num_newlines + 1) * font->getMaxCharHeight();
}

////////////////////////////////////////////////////////////////////////////////
// Visual ink bounds for the active font + text. Width matches
// gos_TextStringLength (max line width across \n). Top/Bottom are signed
// pixel offsets from the gos_TextSetPosition y coordinate to the topmost
// / bottommost inked pixel that would render — independent of line_skip
// padding. Use for vertically centering labels in fixed frames where
// line-skip-based centering biases ALL CAPS text low.
//
// Negative Top is legitimate for extended glyphs (Ä Ö Ü etc.) that sit
// above the ASCII-trimmed band — those would render slightly clipped at
// the top under the current renderer, but the bounds still reflect
// where ink would conceptually appear.
//
// Multi-line: Top is the first line's top, Bottom is
// (num_lines − 1) * font_line_skip + last line's bottom. Single-line
// callers (buttons / list items) get the obvious answer.
//
// Legacy .glyph fonts (no per-glyph ink data) fall back to
// Top=0, Bottom=line-skip-equivalent — centering math degrades to the
// existing line-skip-based behavior, no improvement but no regression.
void __stdcall gos_TextVisualBounds( DWORD* Width, int* Top, int* Bottom, const char *fmt, ... )
{
    gosASSERT(Width && Top && Bottom);

    const int   MAX_TEXT_LEN = 4096;
    char        text[MAX_TEXT_LEN] = {0};
    va_list     ap;

    va_start(ap, fmt);
    vsnprintf(text, MAX_TEXT_LEN - 1, fmt, ap);
    va_end(ap);
    text[MAX_TEXT_LEN - 1] = '\0';

    const gosTextAttribs& ta = g_gos_renderer->getTextAttributes();
    const gosFont* font = ta.FontHandle;
    gosASSERT(font);

    const gosGlyphInfo& gi = font->getGlyphInfo();
    const int line_skip = font->getMaxCharHeight();

    int max_width = 0;
    int cur_width = 0;
    int line_index = 0;
    int min_top = INT_MAX;
    int max_bot = INT_MIN;
    bool any_ink = false;

    // Per-glyph: project each ink position into the multi-line block's
    // y coordinate as `line_index * line_skip + ink_offset`. Tracking
    // min/max globally avoids a leading-empty-lines bug where
    // first-line-ink bookkeeping would otherwise emit a Top that
    // ignores the line offset.
    for(const char* p = text; ; ++p) {
        unsigned char c = (unsigned char)*p;
        if(c == '\n' || c == '\0') {
            if(cur_width > max_width) max_width = cur_width;
            if(c == '\0') break;
            line_index++;
            cur_width = 0;
            continue;
        }
        cur_width += font->getCharAdvance(c);
        if(gi.ink_valid_ && c < gi.num_glyphs_ && gi.ink_valid_[c]) {
            int base = line_index * line_skip;
            int t = base + (int)gi.ink_top_[c];
            int b = base + (int)gi.ink_bot_[c];
            if(t < min_top) min_top = t;
            if(b > max_bot) max_bot = b;
            any_ink = true;
        }
    }

    *Width = (DWORD)max_width;

    if(any_ink) {
        *Top    = min_top;
        *Bottom = max_bot;
    } else if(gi.ink_valid_) {
        // Whitespace-only or empty — degenerate, no ink to center on.
        *Top    = 0;
        *Bottom = 0;
    } else {
        // Legacy .glyph fallback — line-skip-based geometry under the
        // inclusive-bounds convention callers use ((Top+Bottom+1)/2).
        // The `- 1` makes (0 + (lines*ls - 1) + 1)/2 = lines*ls/2,
        // matching gos_TextStringLength's height/2 centering result.
        *Top    = 0;
        *Bottom = (line_index + 1) * line_skip - 1;
    }
}

////////////////////////////////////////////////////////////////////////////////
size_t __stdcall gos_GetMachineInformation( MachineInfo mi, int Param1/*=0*/, int Param2/*=0*/, int Param3/*=0*/, int Param4/*=0*/)
{
    // TODO:
    if(mi == gos_Info_GetDeviceLocalMemory)
        return 1024*1024*1024;
    if(mi == gos_Info_GetDeviceAGPMemory)
        return 512*1024*1024; 
    if (mi == gos_Info_CanMultitextureDetail)
        return true;
    if(mi == gos_Info_NumberDevices)
        return 1;
    if(mi == gos_Info_GetDeviceName)
        return (size_t)glGetString(GL_RENDERER);
    if(mi == gos_Info_ValidMode) {
        int xres = Param2;
        int yres = Param3;
        int bpp = Param4;
        return graphics::is_mode_supported(xres, yres, bpp) ? 1 : 0;
    }
    if(mi == gos_Info_GetIMECaretStatus)
        return 1;

    return 0;
}

int gos_GetWindowDisplayIndex()
{   
    gosASSERT(g_gos_renderer);
    
    return graphics::get_window_display_index(g_gos_renderer->getRenderContextHandle());
}

int gos_GetNumDisplayModes(int DisplayIndex)
{
    return graphics::get_num_display_modes(DisplayIndex);
}

bool gos_GetDisplayModeByIndex(int DisplayIndex, int ModeIndex, int* XRes, int* YRes, int* BitDepth)
{
    return graphics::get_display_mode_by_index(DisplayIndex, ModeIndex, XRes, YRes, BitDepth);
}


////////////////////////////////////////////////////////////////////////////////
// GPU Buffers management code
////////////////////////////////////////////////////////////////////////////////

GLenum getGLBufferType(gosBUFFER_TYPE type)
{
	GLenum t = -1;
	switch (type)
	{
		case gosBUFFER_TYPE::VERTEX: t = GL_ARRAY_BUFFER; break;
		case gosBUFFER_TYPE::INDEX: t = GL_ELEMENT_ARRAY_BUFFER; break;
		case gosBUFFER_TYPE::UNIFORM: t = GL_UNIFORM_BUFFER; break;

		default:
			gosASSERT(0 && "unknows buffer type");
	}
	return t;
}

GLenum getGLBufferUsage(gosBUFFER_USAGE usage)
{
	GLenum u = -1;
	switch (usage)
	{
		case gosBUFFER_USAGE::STREAM_DRAW: u = GL_STREAM_DRAW; break;
		case gosBUFFER_USAGE::STATIC_DRAW: u = GL_STATIC_DRAW; break;
		case gosBUFFER_USAGE::DYNAMIC_DRAW: u = GL_DYNAMIC_DRAW; break;

		default:
			gosASSERT(0 && "unknows buffer usage");
	}
	return u;
}



gosBuffer* __stdcall gos_CreateBuffer(gosBUFFER_TYPE type, gosBUFFER_USAGE usage, int element_size, uint32_t count, void* buffer_data)
{
	GLenum gl_target = getGLBufferType(type);
	GLenum gl_usage = getGLBufferUsage(usage);

	size_t buffer_size = element_size * count;
	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(gl_target, buffer);
	glBufferData(gl_target, buffer_size, buffer_data, gl_usage);
	glBindBuffer(gl_target, 0);

	gosBuffer* pbuffer = new gosBuffer();
	pbuffer->buffer_ = buffer;
	pbuffer->element_size_ = element_size;
	pbuffer->count_ = count;
	pbuffer->type_ = type;
	pbuffer->usage_ = usage;

    gosASSERT(g_gos_renderer);
	g_gos_renderer->addBuffer(pbuffer);

	return pbuffer;
}

uint32_t gos_GetBufferSizeBytes(HGOSBUFFER buffer)
{
	gosASSERT(buffer);
    return buffer->element_size_ * buffer->count_;
}

void __stdcall gos_DestroyBuffer(gosBuffer* buffer)
{
	gosASSERT(buffer);
    gosASSERT(g_gos_renderer);
	bool rv = g_gos_renderer->deleteBuffer(buffer);
    (void)rv;
	delete buffer;
}

void __stdcall gos_BindBufferBase(gosBuffer* buffer, uint32_t slot)
{
	gosASSERT(buffer);

	GLenum gl_target = getGLBufferType(buffer->type_);
	glBindBufferBase(gl_target, slot, buffer->buffer_);
}

void __stdcall gos_UpdateBuffer(HGOSBUFFER buffer, void* data, size_t offset, size_t num_bytes)
{
	gosASSERT(buffer);
    gosASSERT(buffer->element_size_ * buffer->count_ >= num_bytes);
	GLenum gl_target = getGLBufferType(buffer->type_);
    glBindBuffer(gl_target, buffer->buffer_);
	// NOTE (unchanged behavior): this full glBufferData re-spec IGNORES the
	// `offset` arg — it always orphans the whole buffer from byte 0. Left as-is
	// by GPU-UPDATE-BUFFER-COUNTER-1 (measurement-only); the macro just tallies
	// this orphan-on-write under owner GosUpdateBuffer when the gate is on.
	MC2_GL_BufferData_Owner(gl_target, num_bytes, data, GL_DYNAMIC_DRAW, GosUpdateBuffer);
    glBindBuffer(gl_target, 0);
}

HGOSVERTEXDECLARATION __stdcall gos_CreateVertexDeclaration(gosVERTEX_FORMAT_RECORD* records, int count)
{
	gosASSERT(records && count > 0);
    gosASSERT(g_gos_renderer);
	gosVertexDeclaration* vdecl = gosVertexDeclaration::create(records, count);
	g_gos_renderer->addVertexDeclaration(vdecl);
	return vdecl;
}

void __stdcall gos_DestroyVertexDeclaration(HGOSVERTEXDECLARATION vdecl)
{
	gosASSERT(vdecl);
    gosASSERT(g_gos_renderer);
	bool rv = g_gos_renderer->deleteVertexDeclaration(vdecl);
    (void)rv;
	gosVertexDeclaration::destroy(vdecl);

}

HGOSRENDERMATERIAL __stdcall gos_getRenderMaterial(const char* material)
{
	gosASSERT(material);
	gosASSERT(g_gos_renderer);
	return g_gos_renderer->getRenderMaterial(material);
}

void __stdcall gos_ApplyRenderMaterial(HGOSRENDERMATERIAL material)
{
	gosASSERT(material);

	//setup commoin stuff
	gos_SetCommonMaterialParameters(material);

	material->apply();
	material->setSamplerUnit(gosMesh::s_tex1, 0);
	material->setUniformBlock("lights_data", 0);
}

void __stdcall gos_SetRenderMaterialParameterFloat4(HGOSRENDERMATERIAL material, const char* name, const float* v)
{
	gosASSERT(material);
	gosASSERT(v);
	material->getShader()->setFloat4(name, v);
}

void __stdcall gos_SetRenderMaterialParameterMat4(HGOSRENDERMATERIAL material, const char* name, const float* m)
{
	gosASSERT(material);
	gosASSERT(m);
	material->getShader()->setMat4(name, m);
}

void __stdcall gos_SetRenderMaterialParameterInt(HGOSRENDERMATERIAL material, const char* name, int v)
{
	gosASSERT(material);
	material->getShader()->setInt(name, v);
}

void __stdcall gos_SetRenderMaterialSamplerUnit(HGOSRENDERMATERIAL material, const char* name, uint32_t unit)
{
	gosASSERT(material);
	gosASSERT(name);
	material->setSamplerUnit(name, unit);
}

// ===================================================================
// [LIGHTSSBO v1] LightsData SSBO. Was a std140 UBO (ObjectLights
// light[64]) at LIGHT_DATA_ATTACHMENT_SLOT; converted to an unbounded
// std430 SSBO at LIGHT_DATA_SSBO_BINDING to remove the 64-slot ceiling
// (mc2_17 was 57/64 combined mech+static — one dense mission from silent
// corruption). The gos buffer API has no STORAGE type, so this is raw
// GL, mirroring the s_perCmdSsbo pattern. Named device-mediated helper
// (vulkan-prep): callers do NOT touch GL directly.
// See docs/superpowers/plans/2026-05-17-lightsdata-ubo-to-ssbo.md
// ===================================================================
// LIGHTDATA-SSBO-OWNER-1: the live default-path light-data SSBO is narrowed behind
// a GpuBufferOwner identity record (logical id + lifetime + debug name + backend
// handle). Every gen/bind/bufferData/subdata/delete/guard site reaches the raw
// handle ONLY via s_lightDataOwner.backendHandle; GL args/binding-slot/flags/order are
// unchanged. Persistent lifetime: lazy-created on first upload, destroyed in
// gos_LightDataSsbo_Destroy (txmmgr.cpp mcTextureManager teardown). Grow-once:
// on grow the handle is deleted+regenerated, so the owner is RE-REGISTERED on the
// new handle (registerLightDataSsbo helper) and invalidated on destroy.
// L6.1 backendHandle widen: the alias below is a READ-ONLY GLuint view of the
// 64-bit owner handle; gen/delete/reset sites write the owner field via a
// GLuint local (the standard owner idiom), never through the macro.
static RenderCore::GpuBufferOwner s_lightDataOwner{
    RenderCore::RenderResourceId::LightDataSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "LightDataSsbo",
    0u};
#define s_lightDataSsbo (static_cast<GLuint>(s_lightDataOwner.backendHandle))
static GLsizeiptr s_lightDataSsboBytes = 0;

// LIGHT-GROW-ONCE-PREFIX-SKIP-1: number of immutable-prefix bytes currently known
// resident in the persistent grow-once store. The static light prefix [0..S) is
// proven permanent (STATICPROP-PERMANENT-INSTANCE-LIGHTS-1); once uploaded into the
// persistent store it stays valid across frames (grow-once never orphans in steady
// state). We re-ship the prefix ONLY when it changes (prefixDirty), when its extent
// changes (prefixBytes != resident), or when the backing store was (re)created by a
// create/grow — in all of which cases the resident prefix is stale/gone. Otherwise
// the steady-state frame uploads the dynamic SUFFIX only, and the immutable prefix
// is not re-shipped. Reset to 0 on destroy / create / grow so the next call re-ships.
static GLsizeiptr s_lightPrefixResidentBytes = 0;

// LIGHT-GROW-ONCE-PREFIX-SKIP-1: per-frame light-upload byte accounting (deterministic
// win metric — quantifies the immutable-prefix bytes NOT re-shipped once the prefix is
// resident). Behind MC2_LIGHT_UPLOAD_STATS so it is zero-cost when off. Mirrors the
// #1 MC2_VFX_TUBE_UPLOAD_STATS pattern. baselineBytes = what the pre-fix grow-once
// path WOULD have uploaded (the full live range every frame).
struct LightUploadStats {
	unsigned long long uploadBytes   = 0;  // glBufferSubData payload bytes this frame (post-fix)
	unsigned long long baselineBytes = 0;  // full-range bytes the pre-fix path would ship
	unsigned long long prefixSkipped = 0;  // immutable-prefix bytes NOT re-shipped this frame
	unsigned int       frames        = 0;  // grow-once upload calls accounted this frame window
};
static LightUploadStats s_lightUploadStats;
static bool lightUploadStatsEnabled() {
	static int cached = -1;
	if (cached < 0) {
		const char* v = std::getenv("MC2_LIGHT_UPLOAD_STATS");
		cached = (v && v[0] && v[0] != '0') ? 1 : 0;
	}
	return cached != 0;
}

// LIGHTDATA-SSBO-OWNER-1: (re)register the owner in the resource registry at every
// create/grow site (observe-only metadata; never read by the draw/upload path).
static void registerLightDataSsbo(GLsizeiptr bytes) {
    RenderCore::RenderResourceDesc d;
    d.id        = RenderCore::RenderResourceId::LightDataSsbo;
    d.kind      = RenderCore::RenderResourceKind::Buffer;
    d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;
    d.format    = RenderCore::RenderResourceFormat::BufferRaw;
    d.debugName = "LightDataSsbo";
    d.glName    = static_cast<GLuint>(s_lightDataOwner.backendHandle);
    d.sizeBytes = static_cast<uint64_t>(bytes);
    d.valid     = true;
    RenderCore::registerOrUpdateRenderResource(d);
}
static void invalidateLightDataSsbo() {
    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::LightDataSsbo;
    RenderCore::registerOrUpdateRenderResource(invalid);
}
static const bool s_lightSsboTrace =
	(getenv("MC2_LIGHTSSBO_TRACE") != nullptr);

// LIGHT-GROW-ONCE-SUBDATA-1: per-record stride for headroom sizing.
// Lockstep with mclib/tgl.h `static_assert(sizeof(TG_HWLightsData) == 3600)`.
// gameos_graphics.cpp does NOT include tgl.h, so the value is mirrored here
// (same convention as LIGHT_DATA_SSBO_BINDING being a hardcoded #define).
// LIGHT-ABI-WIDEN-STAGE0-1: widened 1808->3600 (per-object cap N=16->32). This
// hand-copied literal is the sneakiest drift site — scripts/check-light-abi-lockstep.py
// fails CI if it disagrees with the other 4 lockstep sites.
// Headroom of +128 records matches the CPU backing grow step
// (mclib/txmmgr.cpp addLightDataStructure, `lightDataStructuresCapacity + 128`)
// so the GL grow cadence equals the CPU grow cadence — both amortized, rare.
static constexpr GLsizeiptr kLightRecordStride = 3600;
static constexpr GLsizeiptr kLightGrowHeadroomRecords = 128;

// LIGHT-GROW-ONCE-SUBDATA-1: when MC2_GPUBUF_LIGHT_GROWONCE is ON, upload only
// the live used bytes into the single persistent slot-20 SSBO via
// glBufferSubData (no per-frame full glBufferData orphan re-spec). Grow ONLY
// when used bytes exceed current GL capacity, sized with +128-record headroom
// so grow is rare. Returns true if it handled the upload (ON path); false to
// fall through to the unchanged OFF (orphan) path.
//
// CRITICAL NVIDIA CAVEAT (documented, NOT solved here — see slice + recon §3/§4):
// glBufferSubData into the single live buffer that is read all-frame by every
// lit draw (and cross-phase by the mech/static-prop batchers, txmmgr.cpp:485-492)
// has a CROSS-FRAME in-flight-write hazard. On NVIDIA, SubData into a buffer the
// GPU is still reading from the prior frame's draws STALLS the CPU until the GPU
// finishes — exactly the ~80ms stall the LIGHTSSBO-ORPHAN-1 orphan path dodges.
// On AMD this is tolerated (no stall), which is WHY this gate is default-OFF and
// NVIDIA is a HARD BLOCKER before any default-on. Do NOT add an N-buffer rotation
// to fix it — that is the full GpuStorageRing ring (explicitly out of scope here).
// LIGHT-GROW-ONCE-PREFIX-SKIP-1: prefixBytes/prefixDirty let the persistent grow-once
// store honor the immutable static-prefix skip that the orphan split path CANNOT (an
// orphan discards the store). Contract: bytes[0..prefixBytes) is the permanent static
// light table; bytes[prefixBytes..bytes) is the dynamic per-frame tail. On create/grow
// the WHOLE range is uploaded (prefix included) so the GPU is correct, and the resident
// prefix is (re)established. In steady state, the prefix is re-shipped ONLY if dirty or
// its extent changed; otherwise only the dynamic suffix is uploaded. Callers with no
// static prefix (gos_LightDataSsbo_Upload legacy whole-buffer) pass prefixBytes=0 /
// prefixDirty=true, which degenerates to "upload the whole range" — byte-identical to
// the prior behavior.
static bool gos_LightDataSsbo_UploadGrowOnce(const void* data, size_t bytes,
                                             size_t prefixBytes = 0, bool prefixDirty = true)
{
	const GLsizeiptr want = (GLsizeiptr)bytes;
	if ((GLsizeiptr)prefixBytes > want) prefixBytes = (size_t)want;  // clamp
	const char* base = static_cast<const char*>(data);
	const bool statsOn = lightUploadStatsEnabled();
	if (statsOn) {
		s_lightUploadStats.baselineBytes += (unsigned long long)want;  // pre-fix would ship full range
		s_lightUploadStats.frames        += 1;
		// Periodic window dump (every 300 accounted calls ≈ 300 frames). savedBytes =
		// prefixSkipped = the immutable-prefix bytes NOT re-shipped this window.
		if (s_lightUploadStats.frames >= 300) {
			std::fprintf(stderr,
			    "[LIGHT_UPLOAD_STATS] frames=%u uploadBytes=%llu baselineBytes=%llu savedBytes=%llu\n",
			    s_lightUploadStats.frames, s_lightUploadStats.uploadBytes,
			    s_lightUploadStats.baselineBytes, s_lightUploadStats.prefixSkipped);
			std::fflush(stderr);
			s_lightUploadStats = LightUploadStats{};  // reset window
		}
	}
	if (s_lightDataSsbo == 0) {
		// First create: allocate at requested size + headroom, no data copy
		// of trailing slack. Upload the live bytes via SubData.
		const GLsizeiptr cap = want + kLightGrowHeadroomRecords * kLightRecordStride;
		GLuint local = 0;
		glGenBuffers(1, &local);
		s_lightDataOwner.backendHandle = local;  // LIGHTDATA-SSBO-OWNER-1 / L6.1 owner idiom
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
		// Allocate uninitialized capacity (nullptr) ONCE, then fill live bytes.
		// GL_DYNAMIC_DRAW: rewritten-often, drawn-often (read every frame).
		glBufferData(GL_SHADER_STORAGE_BUFFER, cap, nullptr, GL_DYNAMIC_DRAW);
		s_lightDataSsboBytes = cap;
		MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, want, data);  // full range incl. prefix
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: register at create
		s_lightPrefixResidentBytes = (GLsizeiptr)prefixBytes;  // prefix now resident in the fresh store
		if (statsOn) s_lightUploadStats.uploadBytes += (unsigned long long)want;
		if (s_lightSsboTrace) {
			std::fprintf(stderr,
			    "[LIGHTSSBO v1] event=growonce_create binding=%d capBytes=%td liveBytes=%zu prefix=%zu\n",
			    LIGHT_DATA_SSBO_BINDING, (ptrdiff_t)cap, bytes, prefixBytes);
			std::fflush(stderr);
		}
		return true;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
	if (want > s_lightDataSsboBytes) {
		// RARE grow. The old buffer may be read in-flight by the prior frame's
		// lit draws; a stall on the (rare) grow is acceptable, so DRAIN with
		// glFinish before deleting it. Recreate at new capacity WITH +128-record
		// headroom so growth amortizes, rebind to slot 20, log ONCE per grow.
		// RF2 invariant preserved: glBindBufferBase (buffer->binding-point) is
		// CONTEXT state and must re-follow the new storage; the program block
		// binding (gos_BindLightDataStorageBlock) is PROGRAM state and is NOT
		// re-issued here.
		const GLsizeiptr newCap = want + kLightGrowHeadroomRecords * kLightRecordStride;
		const GLsizeiptr oldCap = s_lightDataSsboBytes;
		glFinish();  // drain any in-flight reads of the old store before delete
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		GLuint local = s_lightDataSsbo;
		glDeleteBuffers(1, &local);
		local = 0;
		glGenBuffers(1, &local);
		s_lightDataOwner.backendHandle = local;  // LIGHTDATA-SSBO-OWNER-1 / L6.1 owner idiom
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
		glBufferData(GL_SHADER_STORAGE_BUFFER, newCap, nullptr, GL_DYNAMIC_DRAW);
		s_lightDataSsboBytes = newCap;
		MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, want, data);  // full range incl. prefix into fresh store
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: handle recreated on grow -> re-register
		s_lightPrefixResidentBytes = (GLsizeiptr)prefixBytes;  // store recreated -> prefix re-established
		if (statsOn) s_lightUploadStats.uploadBytes += (unsigned long long)want;
		if (s_lightSsboTrace) {
			std::fprintf(stderr,
			    "[LIGHTSSBO v1] event=growonce_grow oldCap=%td newCap=%td liveBytes=%zu prefix=%zu\n",
			    (ptrdiff_t)oldCap, (ptrdiff_t)newCap, bytes, prefixBytes);
			std::fflush(stderr);
		}
		return true;
	}
	// Steady state: in-place partial update of the live bytes ONLY. No orphan,
	// no full re-spec. This is the per-frame win — [GPUBUF v1] light owner
	// orphan bytes drop to ~0 (routed through MC2_GL_BufferSubData, NOT the
	// owner glBufferData macro, so the light orphan tally is not incremented).
	//
	// LIGHT-GROW-ONCE-PREFIX-SKIP-1: the immutable static prefix [0..prefixBytes)
	// was uploaded on the last create/grow/dirty frame and is still resident in the
	// persistent (never-orphaned) store. Re-ship it ONLY when it is dirty or its
	// extent changed; otherwise upload just the dynamic suffix. GPU contents are
	// byte-identical to shipping the full range — the skipped prefix bytes are
	// already present and unchanged.
	const bool prefixResident = (s_lightPrefixResidentBytes == (GLsizeiptr)prefixBytes);
	const GLsizeiptr skipStart = (prefixDirty || !prefixResident) ? (GLsizeiptr)0
	                                                               : (GLsizeiptr)prefixBytes;
	if (skipStart < want) {
		MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, skipStart, want - skipStart,
		                     base + skipStart);
	}
	s_lightPrefixResidentBytes = (GLsizeiptr)prefixBytes;  // prefix confirmed resident this frame
	if (statsOn) {
		s_lightUploadStats.uploadBytes   += (unsigned long long)(want - skipStart);
		s_lightUploadStats.prefixSkipped += (unsigned long long)skipStart;
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	return true;
}

void __stdcall gos_LightDataSsbo_Upload(const void* data, size_t bytes)
{
	if (bytes == 0) return;
	// LIGHT-GROW-ONCE-SUBDATA-1: ON path takes over entirely (grow-once +
	// per-frame SubData). When OFF, fall through to the byte-identical legacy
	// orphan path below — nothing in that path changes.
	if (gosLightGrowOnceEnabled()) {
		if (gos_LightDataSsbo_UploadGrowOnce(data, bytes)) return;
	}
	if (s_lightDataSsbo == 0) {
		GLuint local = 0;
		glGenBuffers(1, &local);
		s_lightDataOwner.backendHandle = local;  // LIGHTDATA-SSBO-OWNER-1 / L6.1 owner idiom
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
		MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, data, GL_DYNAMIC_DRAW, LightSsbo);
		s_lightDataSsboBytes = (GLsizeiptr)bytes;
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: register at create (legacy path)
		if (s_lightSsboTrace) {
			std::fprintf(stderr, "[LIGHTSSBO v1] event=enabled binding=%d bytes=%zu\n",
			             LIGHT_DATA_SSBO_BINDING, bytes);
			std::fflush(stderr);
		}
		return;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
	if ((GLsizeiptr)bytes > s_lightDataSsboBytes) {
		// Grow: reallocate storage. RF2 — the buffer->binding-point
		// (glBindBufferBase below) is CONTEXT state and must follow the
		// new storage; the program block->binding
		// (glShaderStorageBlockBinding, gos_BindLightDataStorageBlock) is
		// PROGRAM state and is UNAFFECTED by buffer reallocation — do NOT
		// re-issue it here.
		MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, data, GL_DYNAMIC_DRAW, LightSsbo);
		if (s_lightSsboTrace) {
			std::fprintf(stderr, "[LIGHTSSBO v1] event=buffer_grow old=%td new=%zu\n",
			             (ptrdiff_t)s_lightDataSsboBytes, bytes);
			std::fflush(stderr);
		}
		s_lightDataSsboBytes = (GLsizeiptr)bytes;
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: storage realloc'd (same handle) -> refresh size
	} else {
		// LIGHTSSBO-ORPHAN-1: buffer orphaning eliminates the implicit GPU pipeline
		// sync stall on NVIDIA. glBufferSubData on a buffer that the GPU is still
		// reading (from the prior frame's draw calls) forces the NVIDIA driver to
		// block the CPU until the GPU finishes — observed as ~80ms in the
		// RenderLists.LightDataUpload Tracy zone on a 1050 Ti. AMD tolerates it
		// silently. glBufferData(nullptr) discards the old backing store immediately;
		// the driver retires it asynchronously once the GPU finishes, and hands the
		// CPU a fresh store with no sync stall. GL_STREAM_DRAW is the correct hint
		// for write-once-per-frame data (vs GL_DYNAMIC_DRAW which NVIDIA can place
		// in VRAM, making the subsequent write go through PCI-E with sync).
		MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, s_lightDataSsboBytes, nullptr, GL_STREAM_DRAW, LightSsbo);
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, data);
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void __stdcall gos_LightDataSsbo_UploadSplit(const void* data, size_t prefixBytes,
                                             size_t totalBytes, bool prefixDirty)
{
	if (totalBytes == 0) return;
	if (prefixBytes > totalBytes) prefixBytes = totalBytes;  // clamp (S floored by count)
	const char* base = static_cast<const char*>(data);

	// LIGHT-GROW-ONCE-SUBDATA-1: when ON, the split (prefix/suffix) optimization
	// is moot — the grow-once path keeps a persistent store, so SubData of the
	// full live range is correct and cheap (no orphan to defeat prefixDirty, no
	// full re-spec). Route the whole upload through the grow-once helper. The
	// per-frame cost is one glBufferSubData of usedBytes (the win). See the
	// NVIDIA in-flight caveat on gos_LightDataSsbo_UploadGrowOnce.
	if (gosLightGrowOnceEnabled()) {
		// LIGHT-GROW-ONCE-PREFIX-SKIP-1: forward the prefix so the persistent
		// grow-once store honors the immutable-prefix skip (the split's whole point).
		// Prior behavior re-shipped the full range every steady-state frame here.
		gos_LightDataSsbo_UploadGrowOnce(data, totalBytes, prefixBytes, prefixDirty);
		if (s_lightSsboTrace) {
			std::fprintf(stderr,
			    "[LIGHTSSBO v2] event=growonce_split_prefixaware total=%zu prefix=%zu dirty=%d\n",
			    totalBytes, prefixBytes, prefixDirty ? 1 : 0);
			std::fflush(stderr);
		}
		return;
	}

	// Create or grow → full upload (prefix necessarily included; dirty cleared
	// implicitly since the whole buffer is now fresh).
	if (s_lightDataSsbo == 0 || (GLsizeiptr)totalBytes > s_lightDataSsboBytes) {
		gos_LightDataSsbo_Upload(data, totalBytes);  // reuses create/grow + binding
		if (s_lightSsboTrace) {
			std::fprintf(stderr, "[LIGHTSSBO v2] event=full_on_grow total=%zu prefix=%zu\n",
			             totalBytes, prefixBytes);
			std::fflush(stderr);
		}
		return;
	}

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
	// LIGHTSSBO-ORPHAN-1: orphan before any write to avoid implicit GPU sync stall
	// on NVIDIA (same root cause as the non-split path above). After orphaning, the
	// old data store is gone, so we must re-upload the prefix unconditionally —
	// the prefixDirty skip is disabled. On AMD the orphan is equally fast (~1us)
	// and eliminates the latent stall if the GPU falls behind the CPU.
	MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, s_lightDataSsboBytes, nullptr, GL_STREAM_DRAW, LightSsbo);
	if (prefixBytes > 0) {
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)prefixBytes, base);
	}
	if (totalBytes > prefixBytes) {
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)prefixBytes,
		                (GLsizeiptr)(totalBytes - prefixBytes), base + prefixBytes);
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	if (s_lightSsboTrace) {
		// prefixDirty is always treated as true post-orphan; log what was requested
		std::fprintf(stderr, "[LIGHTSSBO v2] event=split_orphan prefixDirty=%d prefix=%zu suffix=%zu\n",
		             prefixDirty ? 1 : 0, prefixBytes, totalBytes - prefixBytes);
		std::fflush(stderr);
	}
}

void __stdcall gos_LightDataSsbo_Destroy()
{
	if (s_lightDataSsbo) {
		GLuint local = s_lightDataSsbo;
		glDeleteBuffers(1, &local);
		s_lightDataOwner.backendHandle = 0;  // L6.1 owner idiom: reset through the owner
		s_lightDataSsboBytes = 0;
		s_lightPrefixResidentBytes = 0;  // LIGHT-GROW-ONCE-PREFIX-SKIP-1: store gone -> prefix not resident
		invalidateLightDataSsbo();  // LIGHTDATA-SSBO-OWNER-1: mark registry slot unavailable on teardown
	}
}

#ifdef MC2_VULKAN_ISLAND
// VK-MECH-MATERIAL-LIGHTING-1: expose the live LightsData GL SSBO name + byte size so
// the VK mech island can copy it (glGetBufferSubData) into a VK SSBO for its calc_light
// port. Read-only view of the owner handle; matches gos_LightDataSsbo_Upload lifetime.
extern "C" void gos_LightDataSsbo_GetInterop(unsigned int* glName, unsigned long long* bytes)
{
	if (glName) *glName = static_cast<unsigned int>(s_lightDataSsbo);
	if (bytes)  *bytes  = static_cast<unsigned long long>(s_lightDataSsboBytes);
}
#endif

// RF1: bind the LightsData SSBO block for a lit material's program.
// Unconditional per-draw (idempotent, ~free, and immune to the CLAUDE.md
// shader-hot-reload relink which resets program block bindings to 0 — a
// cached guard would silently revert). Called from BOTH the legacy lit
// paths (ShapeRenderer::render in txmmgr.cpp, gosRenderer::drawIndexedTris
// here). Replaces the now-silent UBO-reflection
// gos_SetRenderMaterialUniformBlockBindingPoint(mat,"LightsData",...).
void __stdcall gos_BindLightDataStorageBlock(HGOSRENDERMATERIAL material)
{
	if (!material || !material->getShader() || !material->getShader()->shp_)
		return;
	GLuint shp = material->getShader()->shp_;
	GLuint idx = glGetProgramResourceIndex(shp, GL_SHADER_STORAGE_BLOCK, "LightsData");
	if (idx != GL_INVALID_INDEX)
		glShaderStorageBlockBinding(shp, idx, LIGHT_DATA_SSBO_BINDING);
}

void __stdcall gos_SetRenderMaterialUniformBlockBindingPoint(HGOSRENDERMATERIAL material, const char* name, uint32_t slot)
{
	gosASSERT(material && name);
	material->setUniformBlock(name, slot);
}

void __stdcall gos_SetupObjectShadows(HGOSRENDERMATERIAL material)
{
	ZoneScopedN("SetupObjectShadows");
	gosASSERT(material);
	gosASSERT(g_gos_renderer);

	gosPostProcess* pp = getGosPostProcess();
	if (!pp) return;

	GLuint shp = material->getShader()->shp_;

	// Upload terrainMVP for GPU projection (MC2 world → clip space).
	// OBJECT-DECAL-MATRIX-SHARE-1: on solid-armed frames terrain + the cement/road
	// decals (drawDecalStaticBatch) project through the BAKED dispatch matrix
	// (getDispatchMvp16). Objects (buildings/turrets/generators + legacy shapes via
	// this path) used the LIVE camera matrix, which lags the baked snapshot as the
	// camera zooms — so a turret base and the coplanar cement decal drifted apart
	// and the turret sank at some zooms. Project objects through the SAME baked
	// matrix when armed so the coplanar ordering is fixed (OBJECT_DEPTH_BIAS then
	// cleanly decides). Fall back to live pre-arm / first frame.
	if (g_gos_renderer->isTerrainMVPValid()) {
		GLint mvpLoc = glGetUniformLocation(shp, "u_worldToClipGL");
		if (mvpLoc >= 0) {
			// OBJECT-SHADOW-MVP-CURRENCY-1: this projects OBJECT geometry
			// (buildings/turrets/generators + legacy shapes), so a stale-view
			// dispatch snapshot drifts/sinks them on zoom/rotate — the same class
			// as RENDER-VIEW-CURRENCY-1, previously hidden in this terrain-phase TU.
			// Route through the view-epoch-checked accessor: snapshot only when its
			// epoch is current, else live MVP. Killswitch shares the object family's
			// MC2_PROP_FIXB_MVP gate.
			static const bool s_objShadowFixB = []{
				const char* v = std::getenv("MC2_PROP_FIXB_MVP");
				return !(v && std::atoi(v) == 0);   // default ON; only =0 reverts
			}();
			const float* mvp = gos_GetObjectDrawMVP(s_objShadowFixB);
			if (!mvp) mvp = (const float*)&g_gos_renderer->getTerrainMVP();
			glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp);
		}
	}

	// Terrain light direction (needed for shadow bias calculations)
	GLint loc = glGetUniformLocation(shp, "terrainLightDir");
	if (loc >= 0)
		glUniform4fv(loc, 1, (const float*)&g_gos_renderer->getTerrainLightDir());

	if (!pp->shadowsEnabled_) {
		loc = glGetUniformLocation(shp, "enableShadows");
		if (loc >= 0) glUniform1i(loc, 0);
		loc = glGetUniformLocation(shp, "enableDynamicShadows");
		if (loc >= 0) glUniform1i(loc, 0);
		return;
	}

	// Static shadow map (texture unit 9)
	loc = glGetUniformLocation(shp, "lightSpaceMatrix");
	if (loc >= 0)
		glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getLightSpaceMatrix());

	loc = glGetUniformLocation(shp, "enableShadows");
	if (loc >= 0) glUniform1i(loc, 1);

	loc = glGetUniformLocation(shp, "shadowSoftness");
	if (loc >= 0) glUniform1f(loc, g_gos_renderer->getTerrainShadowSoftness());

	loc = glGetUniformLocation(shp, "shadowMap");
	if (loc >= 0) {
		glUniform1i(loc, kTerrainTexUnitStaticShadow);
		glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitStaticShadow);
		glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
	}

	// Dynamic shadow map (unit kTerrainTexUnitDynamicShadow)
	if (pp->getDynamicShadowFBO()) {
		loc = glGetUniformLocation(shp, "enableDynamicShadows");
		if (loc >= 0) glUniform1i(loc, 1);

		if (mc2ShadowCsmEnabled() && pp->getDynamicShadowArrayTexture()) {
			// Item 1 CSM: array-sampler variant.
			loc = glGetUniformLocation(shp, "dynamicCascadeMatrices");
			if (loc >= 0)
				glUniformMatrix4fv(loc, pp->getDynamicShadowCascadeCount(), GL_FALSE,
				                   pp->getDynamicCascadeMatrices());
			loc = glGetUniformLocation(shp, "dynamicCsmCount");
			if (loc >= 0) glUniform1i(loc, pp->getDynamicShadowCascadeCount());
			// Stage 3: per-cascade texel-scaled depth bias inputs.
			loc = glGetUniformLocation(shp, "dynamicCascadeTexelWorld");
			if (loc >= 0) glUniform1fv(loc, pp->getDynamicShadowCascadeCount(),
			                           pp->getDynamicCascadeTexelWorld());
			loc = glGetUniformLocation(shp, "csmDepthSpan");
			if (loc >= 0) glUniform1f(loc, pp->getCsmDepthSpan());
			loc = glGetUniformLocation(shp, "dynamicShadowArray");
			if (loc >= 0) {
				glUniform1i(loc, kTerrainTexUnitDynamicShadow);
				glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynamicShadow);
				glBindTexture(GL_TEXTURE_2D_ARRAY, pp->getDynamicShadowArrayTexture());
			}
			// Per-cascade shadow resolution: separate full-map (last) cascade.
			loc = glGetUniformLocation(shp, "dynamicFullMapTexelWorld");
			if (loc >= 0) glUniform1f(loc, pp->getDynamicFullMapTexelWorld());
			loc = glGetUniformLocation(shp, "dynamicFullMapShadow");
			if (loc >= 0) {
				glUniform1i(loc, kTerrainTexUnitDynFullMap);
				glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynFullMap);
				glBindTexture(GL_TEXTURE_2D, pp->getDynamicFullMapTexture());
				glActiveTexture(GL_TEXTURE0);
			}
		} else {
			loc = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
			if (loc >= 0)
				glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

			loc = glGetUniformLocation(shp, "dynamicShadowMap");
			if (loc >= 0) {
				glUniform1i(loc, kTerrainTexUnitDynamicShadow);
				glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynamicShadow);
				glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
			}
		}
	} else {
		loc = glGetUniformLocation(shp, "enableDynamicShadows");
		if (loc >= 0) glUniform1i(loc, 0);
	}

	glActiveTexture(GL_TEXTURE0);
}

void __stdcall gos_SetCommonMaterialParameters(HGOSRENDERMATERIAL material)
{
	gosASSERT(material);
	gosASSERT(g_gos_renderer);

	const mat4& projection = getGosRenderer()->getProj2Screen();
	const vec4& vp = getGosRenderer()->getRenderViewport();

	// TODO: make typed parameters !!!!!!!!!!!!!!! not just float* pointers, helps track errors
	gos_SetRenderMaterialParameterMat4(material, "projection_", projection);
	gos_SetRenderMaterialParameterFloat4(material, "vp", vp);
}


void __stdcall gos_ForceApplyRenderStates() {
    // Force-apply by directly setting GL state to match the requested render states.
    // applyRenderStates() short-circuits if curStates_ == renderStates_ (RENDER_STATES v1),
    // so invalidate the cache first to force the full body to run.
    if (!g_gos_renderer) return;
    g_gos_renderer->invalidateRenderStateCache();
    // Just set GL directly for the critical states that renderLists() dirties
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL (force-apply scene)
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    // Then sync applyRenderStates tracking
    g_gos_renderer->applyRenderStates();
}

// RENDER_STATES v1: invalidation hook for fast paths that disturb GL state outside
// of applyRenderStates() (terrain bridges, water fast path, post-process composite,
// shadow direct draws, indirect terrain). Callers MUST call this AFTER finishing
// their direct-GL work so the next applyRenderStates() does a full re-apply.
void __stdcall gos_InvalidateRenderStateCache() {
    if (!g_gos_renderer) return;
    g_gos_renderer->invalidateRenderStateCache();
}

// Terrain tessellation API
void __stdcall gos_SetTerrainTessParams(float level, float near_dist, float far_dist) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTessParams(level, near_dist, far_dist);
}
void __stdcall gos_SetTerrainPhongAlpha(float a) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPhongAlpha(a);
}
void __stdcall gos_SetTerrainDisplaceScale(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDisplaceScale(s);
}
float gos_GetTerrainPhongAlpha() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPhongAlpha() : 0.0f;
}
float gos_GetTerrainDisplaceScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDisplaceScale() : 0.0f;
}
float gos_GetTerrainDetailTiling() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailTiling() : 1.0f;
}
float gos_GetTerrainDetailStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDetailStrength() : 4.0f;
}
float gos_GetTerrainPOMScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainPOMScale() : 0.0f;
}
void __stdcall gos_SetTerrainWireframe(bool w) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWireframe(w);
}
void __stdcall gos_SetTerrainDebugMode(float mode) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDebugMode(mode);
}
float __stdcall gos_GetTerrainDebugMode() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDebugMode() : 0.0f;
}
void __stdcall gos_TerrainExtraReset() {
    if (g_gos_renderer) g_gos_renderer->terrainExtraReset();
}
void __stdcall gos_TerrainExtraAdd(const gos_TERRAIN_EXTRA* data, int count) {
    if (g_gos_renderer) g_gos_renderer->terrainExtraAdd(data, count);
}
void __stdcall gos_SetTerrainBatchExtras(const gos_TERRAIN_EXTRA* extras, int count) {
    if (g_gos_renderer) g_gos_renderer->setTerrainBatchExtras(extras, count);
}
bool __stdcall gos_IsTerrainTessellationActive() {
    return g_gos_renderer && g_gos_renderer->getTerrainMaterial() != nullptr;
}

void gos_SetShadowMode(bool enable) {
    if (g_gos_renderer) g_gos_renderer->setShadowMode(enable);
}

void gos_GetTerrainCameraPos(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& cp = g_gos_renderer->getTerrainCameraPos();
        if (x) *x = cp.x;
        if (y) *y = cp.y;
        if (z) *z = cp.z;
    }
}
// F1 Stage A unified-projection production setter.
// Repackages column-major Stuff::Matrix4D -> row-major M (matching
// the legacy upload convention so all consumers see the same
// shader-visible matrix orientation). Writes the terrain_mvp_ cache
// for 15+ existing gos_GetTerrainMVPMat4() callers (CullUBO at
// gpu_cull_compute.cpp:831, mech-batcher, static-prop-batcher,
// etc.) to inherit transparently. The 10 CPU bind sites (Task 15)
// each upload the cached matrix to their respective programs via
// glUniformMatrix4fv(loc, 1, GL_FALSE, terrain_mvp_).
//
// Signature is parameter-light — no explicit program handle —
// because per-program upload happens at the 10 bind sites via the
// cache, not here. R-clipw polarity is baked into the matrix
// itself via kAxisSwapMC2toGL (Task 7g); shader-visible clip.w > 0
// for in-front MC2 verts.
// [MVP_DIAG v1] S2.7 — shared frame counter incremented at the
// canonical world-to-clip set site. Read by gos_GetTerrainMVPMat4
// and the static-prop coalesce uploader (extern in
// gos_static_prop_batcher.cpp). Kept past S2.7 because every probe
// is throttled (frames {1,5,30,120}) — silent at steady state.
long g_mvpDiagFrame = 0;

// RENDER-VIEW-CURRENCY-1 (M2): object/mech stale-snapshot-read telemetry. Bumped
// by gos_GetObjectDrawMVP() (gos_object_draw_mvp.h) whenever an armed object draw
// requested the dispatch snapshot but its view epoch did not match the current
// view (it fell back to the live MVP instead). Should be 0 in steady state; a
// rising count localizes a future re-introduction of the BUG1/3/5 staleness. The
// counter is read by the debug-state dump. MC2_OBJ_MVP_STALE_FATAL=1 aborts so CI
// catches it instead of a player on NVIDIA three days later.
static unsigned long g_objMvpStaleCount = 0;
unsigned long gos_object_mvp_stale_count() { return g_objMvpStaleCount; }
// VIEW-EPOCH-DEDUPE-1: counts the snapshot-USED path (object draw projected through
// the depth-matched dispatch snapshot = the FixB z-fight fix active). Paired with
// the stale counter it proves the fix is live, not merely "fewer fallbacks".
static unsigned long g_objMvpUsedCount = 0;
unsigned long gos_object_mvp_used_count() { return g_objMvpUsedCount; }
void gos_object_mvp_note_used() { ++g_objMvpUsedCount; }

// RENDER-FRAME-CONTEXT-1 (M-guard): counts divergences between the read-only
// gos_FrameCtx() mirror and the authoritative globals. No-op by construction in
// the additive slice (the mirror reads those globals); becomes load-bearing when
// the authority-inversion slice makes the globals shims. MC2_FRAMECTX_MISMATCH_FATAL=1
// aborts so CI catches a desync instead of it going silent.
static unsigned long g_framectxMismatchCount = 0;
unsigned long gos_framectx_mismatch_count() { return g_framectxMismatchCount; }
void gos_framectx_note_mismatch(const char* what) {
    ++g_framectxMismatchCount;
    static const bool s_fatal = (std::getenv("MC2_FRAMECTX_MISMATCH_FATAL") != nullptr);
    if (s_fatal) {
        fprintf(stderr, "[FRAMECTX] FATAL: frame-context mirror diverged from "
                        "authoritative global (%s); count=%lu\n",
                        what ? what : "?", g_framectxMismatchCount);
        fflush(stderr);
        abort();
    }
}
void gos_object_mvp_note_stale() {
    ++g_objMvpStaleCount;
    static const bool s_fatal = (std::getenv("MC2_OBJ_MVP_STALE_FATAL") != nullptr);
    if (s_fatal) {
        fprintf(stderr, "[OBJ_MVP] FATAL: object draw read a stale-view dispatch "
                        "MVP snapshot (view-epoch mismatch); count=%lu\n",
                        g_objMvpStaleCount);
        fflush(stderr);
        abort();
    }
}

// [MVP_EARLY v1] MVP-PUBLISH-EARLY-HOIST. Records the g_mvpDiagFrame counter
// value observed at the gated early world-to-clip publish in Mission::update
// (code/mission.cpp, right after eye->update()). ComputeDispatch
// (gos_terrain_indirect.cpp) re-reads g_mvpDiagFrame at its own MVP read and
// compares against this to prove same-frame currency: with the hoist ON the
// two are equal (snapshot is current-frame); with it OFF the dispatch read
// trails by one (the stale previous-frame camera). -1 = no early publish yet
// this process (gate OFF or pre-mission). Set ONLY by the early publish path.
long g_mvpEarlyPublishSeq = -1;

// VIEW-EPOCH-DEDUPE-1: semantic VIEW-CONTENT epoch. g_mvpDiagFrame is a raw publish
// counter that bumps on EVERY gos_SetWorldToClipGL (~2x/frame: the early-publish at
// mission.cpp + the gamecam publish, usually the SAME camera). The object/mech MVP
// currency check must NOT treat a redundant same-camera republish as a view change,
// or it false-stales the dispatch snapshot every frame and silently disables the
// FixB z-fight fix (telemetry: stale_mvp_reads ~= frame count). This epoch bumps
// ONLY when the published world-to-clip CONTENT actually changes; real camera motion
// still advances it (so vanish/flicker stays fixed). Consumed via gos_object_draw_mvp.h.
long g_viewContentEpoch = 0;
static float g_prevPublishedWtc[16] = { 0 };
static bool  g_havePrevPublishedWtc = false;

void __stdcall gos_SetWorldToClipGL(const Stuff::Matrix4D& mat)
{
    if (!g_gos_renderer) return;
    const float* col = (const float*)&mat;
    #define WTC(r,c) col[(c)*4 + (r)]
    // Repackage column-major Stuff -> row-major M (same convention as
    // the legacy terrain upload; consumers see GL_FALSE-interpretation
    // transpose, R-clipw polarity baked into kAxisSwapMC2toGL).
    float M[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            M[i*4 + j] = WTC(i, j);
    #undef WTC
    g_gos_renderer->setTerrainMVP(M);

    // [MVP_DIAG v1] S2.7 — log set_mvp entry. Throttled to frames
    // {1,5,30,120}. row0 is M[0..3] AFTER the column->row repack.
    ++g_mvpDiagFrame;

    // VIEW-EPOCH-DEDUPE-1: advance the semantic view-content epoch only on a real
    // matrix change. Identical same-camera republishes (early-publish then gamecam)
    // leave it unchanged, so the dispatch snapshot is not false-staled and object draw
    // keeps the depth-matched snapshot. Logic lives in the pure kernel so the offline
    // unit harness (tests/unit/test_view_currency.cpp) verifies the SAME code.
    {
        static const float kViewEpsilon = 1e-6f;  // ignore redundant republish, not real motion
        if (mc2::view_currency::viewContentChanged(g_prevPublishedWtc, M,
                                                   g_havePrevPublishedWtc, kViewEpsilon))
            ++g_viewContentEpoch;
        for (int i = 0; i < 16; ++i) g_prevPublishedWtc[i] = M[i];
        g_havePrevPublishedWtc = true;
    }
    if (g_mvpDiagFrame == 1 || g_mvpDiagFrame == 5 ||
        g_mvpDiagFrame == 30 || g_mvpDiagFrame == 120) {
        fprintf(stderr,
                "[MVP_DIAG v1] event=set_mvp frame=%ld renderer=%p row0=[%g %g %g %g]\n",
                g_mvpDiagFrame, (void*)g_gos_renderer,
                M[0], M[1], M[2], M[3]);
        fflush(stderr);
    }
}
// gos_SetTerrainMVP / gos_SetTerrainViewport: raw-float counterparts to
// gos_SetWorldToClipGL. Used by EditorCamera.h which builds the matrix
// via axis-swap math rather than going through Stuff::Matrix4D. Both APIs
// write the same terrain_mvp_ cache in the renderer.
void __stdcall gos_SetTerrainMVP(const float* matrix16) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMVP(matrix16);
}
void __stdcall gos_SetTerrainViewport(float vmx, float vmy, float vax, float vay) {
    if (g_gos_renderer) g_gos_renderer->setTerrainViewport(vmx, vmy, vax, vay);
}
void __stdcall gos_SetTerrainCameraPos(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCameraPos(x, y, z);
}
// Static shadow API — world-fixed shadow map rendered once at map load
void gos_SetMapHalfExtent(float halfExtent) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->setMapHalfExtent(halfExtent);
}
void gos_SetWaterElevation(float elevation) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->setWaterElevation(elevation);
}
bool gos_StaticLightMatrixBuilt() {
    gosPostProcess* pp = getGosPostProcess();
    return pp && pp->staticLightMatrixBuilt();
}
void gos_BuildStaticLightMatrix() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_) return;
    float lx = 0, ly = 0, lz = 0;
    gos_GetTerrainLightDir(&lx, &ly, &lz);
    // Negate: lightDir points scene→sun, but matrix needs light→scene
    pp->buildStaticLightMatrix(-lx, -ly, -lz, pp->getMapHalfExtent());
}
void gos_MarkStaticLightMatrixBuilt() {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->markStaticLightMatrixBuilt();
}
// VK-SHADOW-COPY-ONCE-1: generation counter bumped each time the static
// shadow map is (re)built (mission load / re-arm). 0 while never built.
uint64_t gos_GetStaticShadowGeneration() {
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->staticShadowGeneration() : 0;
}
bool gos_BeginShadowPrePass(bool clearDepth) {
    // Report whether the prepass actually binds the shadow FBO + sets state.
    // beginShadowPrePass early-returns when shadows are disabled at runtime
    // (pp->shadowsEnabled_); shadow_terrain_material_ is non-null post-init.
    // SHADOW-STATIC-BUILDINGS-2 relies on this so it doesn't append building
    // casters into the scene FBO when the prepass no-ops.
    gosPostProcess* pp = getGosPostProcess();
    const bool willActivate = (g_gos_renderer != nullptr && pp != nullptr &&
                               pp->shadowsEnabled_);
    if (g_gos_renderer) g_gos_renderer->beginShadowPrePass(clearDepth);
    return willActivate;
}
void gos_EndShadowPrePass() {
    if (g_gos_renderer) g_gos_renderer->endShadowPrePass();
}
// VPL-#shadow C-1: per-mission re-arm of the one-shot full-map static
// shadow build (called from Terrain::destroy so mission 2+ rebuilds
// against fresh blocks[] instead of freezing mission 1's shadow).
void gos_ResetStaticLightMatrix() {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->resetStaticLightMatrix();
}
// VPL-#shadow Phase 1: the gos_*ShadowRebuild* one-shot-flag API
// (s_shadowRebuildPending + Request/Pending/Clear) is RETIRED. Its only
// caller was the txmmgr camera-windowed-accumulate prime block, deleted
// with the move to the build-once full-map static shadow. Do not
// reintroduce a camera-motion shadow trigger.
void gos_DrawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount) {
    if (g_gos_renderer) g_gos_renderer->drawShadowBatchTessellated(
        vertices, numVerts, indices, numIndices, extras, extraCount);
}
void gos_DrawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4) {
    if (g_gos_renderer) g_gos_renderer->drawShadowObjectBatch(vb, ib, vdecl, worldMatrix4x4);
}
// CP-2: static-context shadow submit. Binds the world-fixed static shadow
// map (shadowFBO_ / staticLightSpaceMatrix_) which is already set by
// gos_BeginShadowPrePass via active_light_space_matrix_. The body is
// identical to gos_DrawShadowObjectBatch because gosRenderer::
// drawShadowObjectBatch routes through active_light_space_matrix_ which
// is already staticLightSpaceMatrix_ when called within
// gos_BeginShadowPrePass ... gos_EndShadowPrePass. This entry point exists
// as a named, separately-callable symbol for Plan 2C's decorative-mesh
// static shadow wiring.
void gos_DrawShadowObjectBatchStatic(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4) {
    if (g_gos_renderer) g_gos_renderer->drawShadowObjectBatch(vb, ib, vdecl, worldMatrix4x4);
}

// Dynamic object shadow pass API
void gos_BeginDynamicShadowPass() {
    if (g_gos_renderer) g_gos_renderer->beginDynamicShadowPass();
}
void gos_EndDynamicShadowPass() {
    if (g_gos_renderer) g_gos_renderer->endDynamicShadowPass();
}
void gos_BuildDynamicLightMatrix(float sx, float sy, float sz,
                                  const float camFitCornersMC2[8][3],
                                  const float shadowCenterXYZ[3], bool shadowCenterValid) {
    gosPostProcess* pp = getGosPostProcess();
    if (pp) pp->buildDynamicLightMatrix(sx, sy, sz, camFitCornersMC2,
                                        shadowCenterXYZ, shadowCenterValid);
}

void gos_GetTerrainLightDir(float* x, float* y, float* z) {
    if (g_gos_renderer) {
        const vec4& ld = g_gos_renderer->getTerrainLightDir();
        if (x) *x = ld.x;
        if (y) *y = ld.y;
        if (z) *z = ld.z;
    }
}
void gos_SetTerrainLightDir(float x, float y, float z) {
    if (g_gos_renderer) g_gos_renderer->setTerrainLightDir(x, y, z);
}
// VK-TERRAIN-LODFADE: reuses the existing gos_GetTerrainCameraPos free function
// (defined above at the gos_SetShadowMode block) for the camera-distance fade port --
// no new accessor needed.
void gos_SetTerrainDetailParams(float tiling, float strength) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDetailParams(tiling, strength);
}
void gos_SetTerrainMaterialNormal(int index, unsigned int glTexId) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMaterialNormal(index, glTexId);
}
void gos_SetTerrainWorldScale(float scale) {
    if (g_gos_renderer) g_gos_renderer->setTerrainWorldScale(scale);
}
void gos_SetTerrainCellBombParams(float scale, float jitter, float rotation) {
    if (g_gos_renderer) g_gos_renderer->setTerrainCellBombParams(scale, jitter, rotation);
}
void gos_SetTerrainPOMParams(float scale, float minLayers, float maxLayers) {
    if (g_gos_renderer) g_gos_renderer->setTerrainPOMParams(scale, minLayers, maxLayers);
}
void gos_SetTerrainDetailNormalTexture(unsigned int glTexId) {
    // Legacy single-texture path — not used with per-material splatting, but terrtxm2 still calls it
}
void gos_SetTerrainDisplacementTexture(unsigned int glTexId) {
    // Legacy single displacement — not used with per-material splatting
}
void gos_SetTerrainViewDir(float x, float y, float z) {
    // View direction for POM — stored but not critical for tessellation path
}
unsigned int gos_CreateTerrainNormalTexture(const unsigned char* rgbaData, int width) {
    GLuint texId = 0;
    // TEX-CLASS: asset-pool -- terrain normal content texture factory
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, width, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    printf("[TESS] Created terrain normal texture: id=%u size=%d\n", texId, width);
    return texId;
}

// ROAD-PBR-ASPHALT-1: create the asphalt albedo GL_TEXTURE_2D from a TGA-loaded
// BGRA byte buffer (TGA stores BGR; terrtxm2 hands us bytes in B,G,R,0xff order).
// Upload as GL_BGRA so the texture samples back as correct RGB (no channel swap),
// with mipmaps + REPEAT for high-res tiling. Returns the GL id (0 on failure).
unsigned int gos_CreateAsphaltAlbedoTexture(const unsigned char* bgraData, int width) {
    if (!bgraData || width <= 0) return 0;
    GLuint texId = 0;
    // TEX-CLASS: asset-pool -- asphalt albedo content texture factory
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, width, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgraData);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    printf("[ROAD-PBR-ASPHALT-1] created asphalt albedo texture: id=%u size=%d\n", texId, width);
    return texId;
}

void gos_SetTerrainAsphaltAlbedoTexture(unsigned int glTexId) {
    if (g_gos_renderer) g_gos_renderer->setTerrainAsphaltAlbedoTexture(glTexId);
}

// ROAD-MATERIAL-GRAVEL-1: create the gravel albedo GL_TEXTURE_2D from a TGA-loaded
// BGRA byte buffer (B,G,R,0xff order). Upload as GL_BGRA (samples back as RGB), with
// mipmaps + REPEAT for high-res tiling. Returns the GL id (0 on failure). Mirrors
// gos_CreateAsphaltAlbedoTexture exactly.
unsigned int gos_CreateGravelAlbedoTexture(const unsigned char* bgraData, int width) {
    if (!bgraData || width <= 0) return 0;
    GLuint texId = 0;
    // TEX-CLASS: asset-pool -- gravel albedo content texture factory
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, width, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgraData);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    printf("[ROAD-MATERIAL-GRAVEL-1] created gravel albedo texture: id=%u size=%d\n", texId, width);
    return texId;
}

void gos_SetTerrainGravelAlbedoTexture(unsigned int glTexId) {
    if (g_gos_renderer) g_gos_renderer->setTerrainGravelAlbedoTexture(glTexId);
}

void gos_SetTerrainShadowSoftness(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainShadowSoftness(s);
}
float gos_GetTerrainShadowSoftness() {
    return g_gos_renderer ? g_gos_renderer->getTerrainShadowSoftness() : 2.5f;
}

void gos_SetTerrainMatNormalBoost(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatNormalBoost(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatNormalBoost(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatNormalBoost(rock, grass, dirt, concrete);
    else { *rock = 0.9f; *grass = 1.1f; *dirt = 1.1f; *concrete = 2.5f; }
}
void gos_SetTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatTiling(rock, grass, dirt, concrete, snow);
}
void gos_GetTerrainMatTiling(float* rock, float* grass, float* dirt, float* concrete, float* snow) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatTiling(rock, grass, dirt, concrete, snow);
    else { *rock = 3.0f; *grass = 2.0f; *dirt = 1.0f; *concrete = 6.0f; *snow = 1.0f; }
}
// TERRAIN-CLASSIFY-TUNING-1
void gos_SetTerrainClassGrass(float gMinusRLo, float gMinusRHi, float gBrightLo, float gBrightHi) {
    if (!g_gos_renderer) return;
    const float v[4] = { gMinusRLo, gMinusRHi, gBrightLo, gBrightHi };
    g_gos_renderer->setTerrainClassGrass(v);
}
void gos_GetTerrainClassGrass(float* gMinusRLo, float* gMinusRHi, float* gBrightLo, float* gBrightHi) {
    if (g_gos_renderer) {
        float v[4]; g_gos_renderer->getTerrainClassGrass(v);
        *gMinusRLo = v[0]; *gMinusRHi = v[1]; *gBrightLo = v[2]; *gBrightHi = v[3];
    } else { *gMinusRLo = -0.02f; *gMinusRHi = 0.06f; *gBrightLo = 0.22f; *gBrightHi = 0.40f; }
}
void gos_SetTerrainClassDirt(float rMinusGLo, float rMinusGHi, float rBrightLo, float rBrightHi) {
    if (!g_gos_renderer) return;
    const float v[4] = { rMinusGLo, rMinusGHi, rBrightLo, rBrightHi };
    g_gos_renderer->setTerrainClassDirt(v);
}
void gos_GetTerrainClassDirt(float* rMinusGLo, float* rMinusGHi, float* rBrightLo, float* rBrightHi) {
    if (g_gos_renderer) {
        float v[4]; g_gos_renderer->getTerrainClassDirt(v);
        *rMinusGLo = v[0]; *rMinusGHi = v[1]; *rBrightLo = v[2]; *rBrightHi = v[3];
    } else { *rMinusGLo = -0.02f; *rMinusGHi = 0.06f; *rBrightLo = 0.22f; *rBrightHi = 0.45f; }
}
void gos_SetTerrainTintStrengthScale(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintStrengthScale(s);
}
float gos_GetTerrainTintStrengthScale() {
    return g_gos_renderer ? g_gos_renderer->getTerrainTintStrengthScale() : 1.0f;
}
// TERRAIN-CONTROLMAP-ALBEDO-1
void gos_SetTerrainControlAlbedoStrength(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainControlAlbedoStrength(s);
}
float gos_GetTerrainControlAlbedoStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainControlAlbedoStrength() : 0.0f;
}
// TERRAIN-MATERIAL-LIB-1
void gos_SetTerrainSnowBrightnessDampen(float v) {
    if (g_gos_renderer) g_gos_renderer->setTerrainSnowBrightnessDampen(v);
}
float gos_GetTerrainSnowBrightnessDampen() {
    return g_gos_renderer ? g_gos_renderer->getTerrainSnowBrightnessDampen() : 0.78f;
}
// TERRAIN-TINT-UI-1
void gos_SetTerrainTintRock(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintRock(r, g, b);
}
void gos_GetTerrainTintRock(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintRock(r, g, b);
    else { *r = 0.36f; *g = 0.37f; *b = 0.40f; }
}
void gos_SetTerrainTintGrass(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintGrass(r, g, b);
}
void gos_GetTerrainTintGrass(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintGrass(r, g, b);
    else { *r = 0.35f; *g = 0.42f; *b = 0.25f; }
}
void gos_SetTerrainTintDirt(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintDirt(r, g, b);
}
void gos_GetTerrainTintDirt(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintDirt(r, g, b);
    else { *r = 0.48f; *g = 0.42f; *b = 0.33f; }
}
// TERRAIN-MATERIAL-LIB-1: promoted frag-literal tints + roughness/AO scalars.
void gos_SetTerrainTintConcrete(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintConcrete(r, g, b);
}
void gos_GetTerrainTintConcrete(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintConcrete(r, g, b);
    else { *r = 0.55f; *g = 0.53f; *b = 0.50f; }
}
void gos_SetTerrainTintSnow(float r, float g, float b) {
    if (g_gos_renderer) g_gos_renderer->setTerrainTintSnow(r, g, b);
}
void gos_GetTerrainTintSnow(float* r, float* g, float* b) {
    if (g_gos_renderer) g_gos_renderer->getTerrainTintSnow(r, g, b);
    else { *r = 0.75f; *g = 0.78f; *b = 0.84f; }
}
void gos_SetTerrainMatRoughness(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatRoughness(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatRoughness(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatRoughness(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
void gos_SetTerrainMatAO(float rock, float grass, float dirt, float concrete) {
    if (g_gos_renderer) g_gos_renderer->setTerrainMatAO(rock, grass, dirt, concrete);
}
void gos_GetTerrainMatAO(float* rock, float* grass, float* dirt, float* concrete) {
    if (g_gos_renderer) g_gos_renderer->getTerrainMatAO(rock, grass, dirt, concrete);
    else { *rock = 1.0f; *grass = 1.0f; *dirt = 1.0f; *concrete = 1.0f; }
}
// TERRAIN-MATERIAL-LIB-1: public wrapper -- terrainMaterialLibEnabled() above is
// file-static (TU-local); the live chunk binder (gos_terrain_lod_chunk.cpp) is a
// separate TU and needs the same MC2_TERRAIN_MATERIAL_LIB gate value to drive its
// own u_useMaterialLib upload. Same cached-static evaluation as the bridge path.
bool gos_TerrainMaterialLibEnabled() {
    return terrainMaterialLibEnabled();
}
// TERRAIN-TUNING-UI-1
void gos_SetTerrainNormalsFromHeightStrength(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainNormalsFromHeightStrength(s);
}
float gos_GetTerrainNormalsFromHeightStrength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainNormalsFromHeightStrength() : 1.0f;
}
// TERRAIN-LIGHTING-1
void gos_SetTerrainLightingV1Strength(float s) {
    if (g_gos_renderer) g_gos_renderer->setTerrainLightingV1Strength(s);
}
float gos_GetTerrainLightingV1Strength() {
    return g_gos_renderer ? g_gos_renderer->getTerrainLightingV1Strength() : 1.0f;
}
// TERRAIN-LIGHTING-2
void gos_SetTerrainLightingV2Floor(float f) {
    if (g_gos_renderer) g_gos_renderer->setTerrainLightingV2Floor(f);
}
float gos_GetTerrainLightingV2Floor() {
    return g_gos_renderer ? g_gos_renderer->getTerrainLightingV2Floor() : 0.3f;
}

void gos_SetTerrainDrawEnabled(bool e) {
    if (g_gos_renderer) g_gos_renderer->setTerrainDrawEnabled(e);
}
bool gos_GetTerrainDrawEnabled() {
    return g_gos_renderer ? g_gos_renderer->getTerrainDrawEnabled() : true;
}

// HUD scale — clamped to [0.5, 1.0]. 1.0 disables the transform entirely.
// s_hud_scale itself is defined near the top of this file so flushHUDBatch()
// can reference it directly without a forward declaration dance.
void gos_SetHudScale(float s) {
    if (s < 0.5f) s = 0.5f;
    if (s > 1.0f) s = 1.0f;
    s_hud_scale = s;
}
float gos_GetHudScale() { return s_hud_scale; }

void gos_SetHudScaleActive(bool on) { s_hud_scale_active = on; }
bool gos_GetHudScaleActive()        { return s_hud_scale_active; }

// HUD-scale exemption: while set, HUD draw calls recorded are tagged scaleExempt
// and skip the bottom-band shrink in flushHUDBatch. Bracket the cursor sprite
// and modal dialogs so the HUD-fit shrink never moves the pointer or a popup.
void gos_SetHudScaleExempt(bool on) { s_hud_scale_exempt = on; }
bool gos_GetHudScaleExempt()        { return s_hud_scale_exempt; }

void gos_HudInverseMousePoint(float& x, float& y) {
    // Inverse of the single-anchor bottom-center HUD transform. Must stay in
    // sync with gosRenderer::flushHUDBatch() above.
    const float scale = s_hud_scale;
    if (!s_hud_scale_active || scale > 0.999f || !g_gos_renderer) return;
    const float sw = (float)g_gos_renderer->getWidth();
    const float sh = (float)g_gos_renderer->getHeight();
    const float bottomBand = sh * 0.60f;
    const float renderedBandTop = sh + (bottomBand - sh) * scale;
    if (y < renderedBandTop) return;
    const float ax = sw * 0.5f;
    const float ay = sh;
    x = ax + (x - ax) / scale;
    y = ay + (y - ay) / scale;
}

// ── World-space overlay batch API ────────────────────────────────────────────

// Private helper: push one triangle into a batch, grouping consecutive same-texture
// calls into a single draw entry.  Batches are cleared at the END of the draw
// functions (called exactly once per frame from renderLists), not on push.
void gosRenderer::pushToOverlayBatch_(OverlayBatch_& b,
                                      const WorldOverlayVert* v3,
                                      unsigned int texHandle)
{
    if (!b.draws.empty() && b.draws.back().texHandle == texHandle) {
        b.draws.back().vertCount += 3;
    } else {
        OverlayBatchEntry_ entry;
        entry.texHandle = texHandle;
        entry.firstVert = (unsigned int)b.verts.size();
        entry.vertCount = 3;
        b.draws.push_back(entry);
    }
    b.verts.push_back(v3[0]);
    b.verts.push_back(v3[1]);
    b.verts.push_back(v3[2]);
}

void gosRenderer::pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(terrainOverlayBatch_, verts3, texHandle);
}

void gosRenderer::pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle)
{
    pushToOverlayBatch_(decalBatch_, verts3, texHandle);
}

// Upload shadow uniforms for a standalone shader program.
// Kept as a static helper because it only touches public getters — no private types.
static void setupOverlayShadowsForShp(GLuint shp)
{
    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    GLint loc;
    loc = glGetUniformLocation(shp, "terrainLightDir");
    if (loc >= 0) glUniform4fv(loc, 1, (const float*)&g_gos_renderer->getTerrainLightDir());

    if (!pp->shadowsEnabled_) {
        loc = glGetUniformLocation(shp, "enableShadows");
        if (loc >= 0) glUniform1i(loc, 0);
        loc = glGetUniformLocation(shp, "enableDynamicShadows");
        if (loc >= 0) glUniform1i(loc, 0);
        return;
    }

    loc = glGetUniformLocation(shp, "lightSpaceMatrix");
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getLightSpaceMatrix());
    loc = glGetUniformLocation(shp, "enableShadows");
    if (loc >= 0) glUniform1i(loc, 1);
    loc = glGetUniformLocation(shp, "shadowSoftness");
    if (loc >= 0) glUniform1f(loc, g_gos_renderer->getTerrainShadowSoftness());
    loc = glGetUniformLocation(shp, "shadowMap");
    if (loc >= 0) {
        glUniform1i(loc, kTerrainTexUnitStaticShadow);
        glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitStaticShadow);
        glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
    }

    if (pp->getDynamicShadowFBO()) {
        loc = glGetUniformLocation(shp, "enableDynamicShadows");
        if (loc >= 0) glUniform1i(loc, 1);
        if (mc2ShadowCsmEnabled() && pp->getDynamicShadowArrayTexture()) {
            // Item 1 CSM: array-sampler variant.
            loc = glGetUniformLocation(shp, "dynamicCascadeMatrices");
            if (loc >= 0)
                glUniformMatrix4fv(loc, pp->getDynamicShadowCascadeCount(), GL_FALSE,
                                   pp->getDynamicCascadeMatrices());
            loc = glGetUniformLocation(shp, "dynamicCsmCount");
            if (loc >= 0) glUniform1i(loc, pp->getDynamicShadowCascadeCount());
            // Stage 3: per-cascade texel-scaled depth bias inputs.
            loc = glGetUniformLocation(shp, "dynamicCascadeTexelWorld");
            if (loc >= 0) glUniform1fv(loc, pp->getDynamicShadowCascadeCount(),
                                       pp->getDynamicCascadeTexelWorld());
            loc = glGetUniformLocation(shp, "csmDepthSpan");
            if (loc >= 0) glUniform1f(loc, pp->getCsmDepthSpan());
            loc = glGetUniformLocation(shp, "dynamicShadowArray");
            if (loc >= 0) {
                glUniform1i(loc, kTerrainTexUnitDynamicShadow);
                glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynamicShadow);
                glBindTexture(GL_TEXTURE_2D_ARRAY, pp->getDynamicShadowArrayTexture());
            }
            // Per-cascade shadow resolution: separate full-map (last) cascade.
            loc = glGetUniformLocation(shp, "dynamicFullMapTexelWorld");
            if (loc >= 0) glUniform1f(loc, pp->getDynamicFullMapTexelWorld());
            loc = glGetUniformLocation(shp, "dynamicFullMapShadow");
            if (loc >= 0) {
                glUniform1i(loc, kTerrainTexUnitDynFullMap);
                glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynFullMap);
                glBindTexture(GL_TEXTURE_2D, pp->getDynamicFullMapTexture());
                glActiveTexture(GL_TEXTURE0);
            }
        } else {
            loc = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
            loc = glGetUniformLocation(shp, "dynamicShadowMap");
            if (loc >= 0) {
                glUniform1i(loc, kTerrainTexUnitDynamicShadow);
                glActiveTexture(GL_TEXTURE0 + kTerrainTexUnitDynamicShadow);
                glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
            }
        }
    } else {
        loc = glGetUniformLocation(shp, "enableDynamicShadows");
        if (loc >= 0) glUniform1i(loc, 0);
    }

    glActiveTexture(GL_TEXTURE0);
}

// Private member: common uniform upload for both draw paths.
void gosRenderer::uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed, const float* terrainMvpOverride)
{
    if (L.terrainMVP >= 0) {
        const float* tmvp = terrainMvpOverride
                                ? terrainMvpOverride
                                : (const float*)&getTerrainMVP();
        glUniformMatrix4fv(L.terrainMVP, 1, GL_FALSE, tmvp);
    }
    // projection_: row-major Stuff matrix — upload GL_TRUE (column-major interpretation)
    if (L.mvp >= 0)
        glUniformMatrix4fv(L.mvp, 1, GL_TRUE, (const float*)&getProj2Screen());
    if (L.fog_color >= 0)
        glUniform4fv(L.fog_color, 1, (const float*)&getFogColor());
    if (L.time >= 0)
        glUniform1f(L.time, elapsed);
    if (L.cameraPos >= 0)
        glUniform4fv(L.cameraPos, 1, (const float*)&getTerrainCameraPos());
    if (L.surfaceDebugMode >= 0)
        glUniform1i(L.surfaceDebugMode, (GLint)terrain_debug_mode_);
    if (L.pathTint >= 0)
        glUniform1i(L.pathTint, mc2ShaderPathTint());  // MC2_SHADER_PATH_TINT
    if (L.mapHalfExtent >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        float halfExt = pp ? pp->getMapHalfExtent() : 0.0f;
        glUniform1f(L.mapHalfExtent, halfExt);
    }

    setupOverlayShadowsForShp(shp);

    // TERRAIN-DECAL-LIGHTING-1a: extend the same terrain lighting stack
    // (NFH height tex, V1 hemi, V2 shadow-fill floor) to the cement
    // overlay program so transitions no longer form a lighting seam
    // against lit terrain. Helper handles env-gate force-zero semantics
    // identically to the main terrain path; locs that are -1 (decal
    // program, no uniforms declared) get skipped. After binding we
    // restore active texture unit 0 because the helper leaves unit 11
    // bound — overlay per-draw glBindTexture targets unit 0.
    bindTerrainHeightTexUniforms(L.terrainHeightTex,
                                 L.terrainHeightParams,
                                 L.useTerrainNormalsFromHeight,
                                 L.terrainNormalsFromHeightStrength,
                                 terrain_nfh_strength_,
                                 L.terrainLightingV1Strength,
                                 terrain_lighting_v1_strength_,
                                 L.terrainLightingV2ShadowFillFloor,
                                 terrain_lighting_v2_floor_);

    // ROAD-PBR-ASPHALT-1: bind the asphalt material samplers for the asphalt
    // branch in terrain_overlay.frag (v_matId==1). Only touches the program that
    // declares these uniforms — decalProg_ (decal.frag) leaves all three locs -1
    // so this whole block is skipped there (byte-identical decal path).
    if (L.asphaltScale >= 0)
        glUniform1f(L.asphaltScale, terrain_asphalt_scale_);
    if (L.asphaltAlbedo >= 0) {
        // Lazy-load the asphalt albedo TGA-backed GL texture handle on first use.
        // terrtxm2 normally pre-loads it at material init via
        // gos_SetTerrainAsphaltAlbedoTexture; if that didn't run (e.g. asset
        // missing) terrain_asphalt_albedo_tex_ stays 0 and we bind 0 — the frag's
        // asphalt branch then samples black albedo but still keeps markings/alpha,
        // so the road is still drawn (degraded, not broken).
        glUniform1i(L.asphaltAlbedo, kOverlayTexUnitAsphaltAlbedo);
        glActiveTexture(GL_TEXTURE0 + kOverlayTexUnitAsphaltAlbedo);
        glBindTexture(GL_TEXTURE_2D, terrain_asphalt_albedo_tex_);
    }
    // ROAD-MATERIAL-GRAVEL-1: bind the gravel albedo sampler for the gravel branch
    // in terrain_overlay.frag (v_matId==2). Gated on loc>=0 so decalProg_
    // (decal.frag), which has no u_gravelAlbedo, is byte-identical (skipped).
    if (L.gravelAlbedo >= 0) {
        // Same lazy-load contract as asphalt: terrtxm2 pre-loads via
        // gos_SetTerrainGravelAlbedoTexture; a missing asset leaves
        // terrain_gravel_albedo_tex_ at 0 (binds 0 → black albedo, road still
        // drawn via markings/alpha — degraded, not broken).
        glUniform1i(L.gravelAlbedo, kOverlayTexUnitGravelAlbedo);
        glActiveTexture(GL_TEXTURE0 + kOverlayTexUnitGravelAlbedo);
        glBindTexture(GL_TEXTURE_2D, terrain_gravel_albedo_tex_);
    }
    if (L.matNormalArray >= 0) {
        // Reuse the shared terrain normal array (layer MAT_LAYER_ASPHALT=7).
        // EnsureBuilt builds lazily from the per-material normals (which terrtxm2
        // always loads) and returns 0 only if the required slots 0-4 are absent.
        GLuint narr = getTerrainNormalArrayTexEnsureBuilt();
        glUniform1i(L.matNormalArray, kOverlayTexUnitMatNormalArr);
        glActiveTexture(GL_TEXTURE0 + kOverlayTexUnitMatNormalArr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, narr);
    }

    glActiveTexture(GL_TEXTURE0);
}

// Draw the terrain overlay batch (alpha cement perimeter tiles).
// Render state: opaque, depth-write ON, depth-test LEQUAL (same as solid terrain).
// MRT: terrain_overlay.frag writes GBuffer1.alpha=1 → shadow_screen skips these pixels.
// Batch cleared after draw.
// [RENDER_CONTRACT:Pass=TerrainOverlay id=gosRenderer_drawTerrainOverlays]
void gosRenderer::drawTerrainOverlays()
{
    if (terrainOverlayBatch_.draws.empty() || !overlayProg_) {
        terrainOverlayBatch_.verts.clear();
        terrainOverlayBatch_.draws.clear();
        return;
    }

    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                              "gosRenderer_drawTerrainOverlays");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                                    "gosRenderer_drawTerrainOverlays");
        }
    } _tlGuard;

    // APPLY-STATE-TERRAINOVERLAY-1: second top-level scene-pass apply-state. Gated by the
    // SAME predicate executorOwnBeginTopLevel uses internally (MC2_FRAMEGRAPH_EXECUTOR).
    // When ON and the executor applies state for this pass, executorApplyTerrainOverlayState()
    // pre-applies the TerrainOverlay pipeline and sets overlayStateAppliedByExecutor_; the body
    // applyPipeline below is then skipped (one-shot). The VBO upload that follows touches no FF
    // pipeline state, so this apply is order-equivalent to the body's. Idempotent ON.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::TerrainOverlay) != nullptr) {
        executorApplyTerrainOverlayState();
    }

    render_contract::noteRenderPass(render_contract::PassIdentity::TerrainOverlay,
                                    "gosRenderer_drawTerrainOverlays");
    render_contract::beginPassScope(render_contract::PassIdentity::TerrainOverlay,
                                    "gosRenderer_drawTerrainOverlays");

    glBindBuffer(GL_ARRAY_BUFFER, terrainOverlayBatch_.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(terrainOverlayBatch_.verts.size() * sizeof(WorldOverlayVert)),
        terrainOverlayBatch_.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // TERRAIN-OVERLAY-DECAL-APPLYPIPELINE-ROUTING-1: TerrainOverlay row (opaque,
    // depth test+write, GEQUAL, cull none) — byte-identical to the old hand-set
    // state. program(0)=skip keeps the manual overlay program bind below.
    // APPLY-STATE-TERRAINOVERLAY-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Mirrors decal.
    // Gate OFF -> flag false -> body applies -> byte-identical.
    if (!overlayStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainOverlay), "TerrainOverlay");
    }
    overlayStateAppliedByExecutor_ = false;

    glUseProgram(overlayProg_->shp_);
    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    const float* fixBMvpOverlay =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!fixBMvpOverlay) fixBMvpOverlay = gos_GetTerrainMVPMat4();
    uploadOverlayUniforms_(overlayProg_->shp_, overlayLocs_, elapsed, fixBMvpOverlay);

    {
        mc2gl::GlScopedTextureUnit texGuard(0);  // GLSTATE-TEXTURE-UNIT0-RESTORE-1
        GLint prevVao = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glBindVertexArray(terrainOverlayBatch_.vao);
        GLuint canary = vkScopedGlCanaryBegin("MC2_VK_TERRAIN_OVERLAY_GL_CANARY");
        int canaryDraws = 0;
        for (const auto& entry : terrainOverlayBatch_.draws) {
            if (overlayLocs_.tex1 >= 0)
                glUniform1i(overlayLocs_.tex1, 0);
            glActiveTexture(GL_TEXTURE0);
            gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "terrainOverlayBatch");
            bindBatchTextureOrFallback(t, entry.texHandle, "terrainOverlayBatch");  // OMT-1: no bind-0 on resolve-fail
            glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
            ++canaryDraws;
        }
        vkScopedGlCanaryEnd(canary, "terrain_overlay_batch", canaryDraws);
        glBindVertexArray((GLuint)prevVao);
    }  // texGuard: restores unit-0 binding + active texture unit before invalidate

    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    terrainOverlayBatch_.verts.clear();
    terrainOverlayBatch_.draws.clear();

    gos_InvalidateRenderStateCache();

    overlayTexTraceSummary();  // OMT-1 counter summary (rate-limited, gated)

    render_contract::endPassScope(render_contract::PassIdentity::TerrainOverlay,
                                  "gosRenderer_drawTerrainOverlays");
}

// Slice A — draw the mission-static cement-overlay bake.
//
// Byte-for-byte mirror of gosRenderer::drawTerrainOverlays()'s render-state
// block + program + uniform upload + per-draw texture bind + glDrawArrays,
// EXCEPT: (1) it draws the caller-owned static VBO via a dedicated persistent
// VAO (the per-frame terrainOverlayBatch_.vao captured the per-frame VBO at
// makeOverlayVAO time, so it cannot be reused for a different buffer);
// (2) draw ranges are supplied by the caller (gos_terrain_indirect's static
// bake) rather than read from terrainOverlayBatch_.draws; (3) it does NOT
// clear anything (mirrors DrawMineStatic — the static buffer persists across
// frames). The vertex layout (WorldOverlayVert, 28-byte stride, attribs
// 0..3) is identical to makeOverlayVAO so the same overlay shader binds.
bool gosRenderer::drawDecalStaticBatch(unsigned int vboGL,
                                       const struct GosDecalStaticDraw* draws,
                                       int drawCount)
{
    if (!overlayProg_ || vboGL == 0 || !draws || drawCount <= 0)
        return false;

    // APPLY-STATE-TERRAINOVERLAY-SITE-FIX-1: TerrainOverlay executor validate+apply
    // hooks live HERE (the LIVE overlay draw on cement/road maps), not in the dormant
    // drawTerrainOverlays(). When MC2_TERRAIN_INDIRECT_OVERLAY is armed (default-ON since
    // 2026-05-17) the per-quad producer is skipped, so drawTerrainOverlays() flushes an
    // empty batch and early-returns BEFORE its own validate wrapper — only THIS function
    // actually draws overlay content. The two sites are mutually exclusive per frame (the
    // indirect-overlay gate makes exactly one draw), so both can own PassIdentity::
    // TerrainOverlay without ever double-owning in a single frame. Placed AFTER the
    // early-returns above so we only own/apply on a real draw. PIN: additive only — no GL
    // state change, no reorder; the body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                              "gosRenderer_drawDecalStaticBatch");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainOverlay,
                                                    "gosRenderer_drawDecalStaticBatch");
        }
    } _tlGuard;

    // [TEMP DECAL_GLPROBE] eager-drain probe — env MC2_DECAL_GLPROBE=1.
    // Removed once root cause is pinned. Attributes GL_INVALID_OPERATION to
    // the exact call instead of the deferred CHECK_GL_ERROR drain site.
    static const bool s_decalGlProbe =
        (getenv("MC2_DECAL_GLPROBE") && getenv("MC2_DECAL_GLPROBE")[0] == '1');
    #define DECAL_GLPROBE(tag) do { if (s_decalGlProbe) { \
        GLenum e; while ((e = glGetError()) != GL_NO_ERROR) \
            printf("[DECAL_GLPROBE] at=%s err=0x%x\n", tag, (unsigned)e); \
        fflush(stdout); } } while(0)
    DECAL_GLPROBE("entry");

    // Capture the caller's VAO binding ONCE, before any VAO mutation. The
    // lazy-init block below clobbers the binding; if prevVao were captured
    // after it (as drawTerrainOverlays captures it — that path has no lazy
    // init), the first armed draw would record VAO 0 and this function would
    // return with VAO 0 bound, breaking the next generic-mesh draw on AMD
    // (trap #4, gpu_direct_renderer_bringup_checklist.md — VAO 0 left bound
    // raises GL_INVALID_OPERATION at the next applyVertexDeclaration/draw).
    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);

    // Persistent VAO bound to whatever static VBO name the bake hands us.
    // Created once; the bake keeps a single GL_STATIC_DRAW buffer for the
    // process lifetime (mirror MineStaticVBO), so the name is stable.
    static GLuint s_decalStaticVAO = 0;
    static unsigned int s_decalStaticVAOBoundVBO = 0;
    if (s_decalStaticVAO == 0 || s_decalStaticVAOBoundVBO != vboGL) {
        if (s_decalStaticVAO == 0)
            glGenVertexArrays(1, &s_decalStaticVAO);
        constexpr int kStride = 28;  // sizeof(WorldOverlayVert) — mirror makeOverlayVAO
        glBindVertexArray(s_decalStaticVAO);
        glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vboGL);
        glVertexAttribPointer(0, 3, GL_FLOAT,         GL_FALSE, kStride, (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT,         GL_FALSE, kStride, (void*)12);
        glVertexAttribPointer(2, 1, GL_FLOAT,         GL_FALSE, kStride, (void*)20);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE,  kStride, (void*)24);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        // Restore the caller's VAO (NOT 0) so a draw issued between this
        // init and the bind below still sees a valid VAO on AMD.
        glBindVertexArray((GLuint)prevVao);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        s_decalStaticVAOBoundVBO = vboGL;
        DECAL_GLPROBE("after_vao_init");
    }
    DECAL_GLPROBE("after_vao_block");

    // ---- State block: identical to drawTerrainOverlays() -------------------
    // TERRAIN-OVERLAY-DECAL-APPLYPIPELINE-ROUTING-1: drive FF state from the
    // TerrainOverlay row (opaque, depth test+write, reverse-Z GEQUAL, cull none).
    // Byte-identical to the old hand-set state (+ no-op glFrontFace(CCW)/blendFunc/
    // offset-disable). glProgramName=0 -> applyPipeline SKIPs program; the manual
    // glUseProgram(overlayProg_) below stays.
    // APPLY-STATE-TERRAINOVERLAY-SITE-FIX-1: relocated apply-state dispatch. When
    // MC2_FRAMEGRAPH_EXECUTOR is ON and the executor applies state for this pass,
    // executorApplyTerrainOverlayState() pre-applies the TerrainOverlay pipeline and sets
    // overlayStateAppliedByExecutor_; the body applyPipeline below is then skipped (one-shot).
    // Gate OFF -> flag false -> body applies -> byte-identical. Mirrors drawDecals().
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::TerrainOverlay) != nullptr) {
        executorApplyTerrainOverlayState();
    }
    if (!overlayStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainOverlay), "TerrainOverlay");
    }
    overlayStateAppliedByExecutor_ = false;

    DECAL_GLPROBE("after_state_block");
    glUseProgram(overlayProg_->shp_);
    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    const float* fixBMvpDecalStatic =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!fixBMvpDecalStatic) fixBMvpDecalStatic = gos_GetTerrainMVPMat4();
    uploadOverlayUniforms_(overlayProg_->shp_, overlayLocs_, elapsed, fixBMvpDecalStatic);
    DECAL_GLPROBE("after_uniform_upload");

    {
        mc2gl::GlScopedTextureUnit texGuard(0);  // GLSTATE-TEXTURE-UNIT0-RESTORE-1
        glBindVertexArray(s_decalStaticVAO);
        GLuint canary = vkScopedGlCanaryBegin("MC2_VK_DECAL_STATIC_GL_CANARY");
        int canaryDraws = 0;
        for (int i = 0; i < drawCount; ++i) {
            const struct GosDecalStaticDraw& entry = draws[i];
            if (overlayLocs_.tex1 >= 0)
                glUniform1i(overlayLocs_.tex1, 0);
            glActiveTexture(GL_TEXTURE0);
            gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "decalStaticBatch");
            bindBatchTextureOrFallback(t, entry.texHandle, "decalStaticBatch");  // OMT-1: no bind-0 on resolve-fail
            glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
            ++canaryDraws;
            if (i == 0) DECAL_GLPROBE("after_first_drawarrays");
        }
        vkScopedGlCanaryEnd(canary, "decal_static_batch", canaryDraws);
        DECAL_GLPROBE("after_all_drawarrays");
        glBindVertexArray((GLuint)prevVao);
        DECAL_GLPROBE("after_vao_restore");
    }  // texGuard: restores unit-0 binding + active texture unit before invalidate

    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    // NO batch clear — the static bake persists across frames (mirror
    // DrawMineStatic; the per-frame drawTerrainOverlays clears here).

    gos_InvalidateRenderStateCache();
    DECAL_GLPROBE("exit");
    #undef DECAL_GLPROBE
    return true;
}

// Draw the decal batch (bomb craters + mech footprints).
// Render state: alpha blend, depth-write OFF, depth-test LEQUAL.
// Batch cleared after draw.
// [RENDER_CONTRACT:Pass=TerrainDecal id=gosRenderer_drawDecals]
void gosRenderer::drawDecals()
{
    if (decalBatch_.draws.empty() || !decalProg_) {
        decalBatch_.verts.clear();
        decalBatch_.draws.clear();
        return;
    }

    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainDecal,
                                              "gosRenderer_drawDecals");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainDecal,
                                                    "gosRenderer_drawDecals");
        }
    } _tlGuard;

    // APPLY-STATE-TERRAINDECAL-1: first top-level scene-pass apply-state. Gated by the
    // SAME predicate executorOwnBeginTopLevel uses internally (MC2_FRAMEGRAPH_EXECUTOR).
    // When ON and the executor applies state for this pass, executorApplyTerrainDecalState()
    // pre-applies the TerrainDecal pipeline and sets decalStateAppliedByExecutor_; the body
    // applyPipeline below is then skipped (one-shot). The VBO upload that follows touches no
    // FF pipeline state, so this apply is order-equivalent to the body's. Idempotent ON.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::TerrainDecal) != nullptr) {
        executorApplyTerrainDecalState();
    }

    render_contract::noteRenderPass(render_contract::PassIdentity::TerrainDecal,
                                    "gosRenderer_drawDecals");
    render_contract::beginPassScope(render_contract::PassIdentity::TerrainDecal,
                                    "gosRenderer_drawDecals");

    glBindBuffer(GL_ARRAY_BUFFER, decalBatch_.vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)(decalBatch_.verts.size() * sizeof(WorldOverlayVert)),
        decalBatch_.verts.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // TERRAIN-OVERLAY-DECAL-APPLYPIPELINE-ROUTING-1: TerrainDecal row (AlphaBlend
    // SRC_ALPHA/ONE_MINUS_SRC_ALPHA, depth-test but depth-WRITE OFF, GEQUAL, cull
    // none) — byte-identical to the old hand-set state. program(0)=skip keeps the
    // manual decalProg_ bind below.
    // APPLY-STATE-TERRAINDECAL-1: skip when the executor already applied the pipeline
    // (MC2_FRAMEGRAPH_EXECUTOR ON). Reset one-shot before draw. Mirrors EdgeFog
    // REMOVE-1 / SCREENSHADOW-1. Gate OFF -> flag false -> body applies -> byte-identical.
    if (!decalStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainDecal), "TerrainDecal");
    }
    decalStateAppliedByExecutor_ = false;

    glUseProgram(decalProg_->shp_);
    float elapsed = SmokeMode::fixedTimestepEnabled()
                        ? (float)SmokeMode::fixedClockSeconds()
                        : (float)(timing::get_wall_time_ms() - timeStart_) / 1000.0f;
    const float* fixBMvpDecals =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!fixBMvpDecals) fixBMvpDecals = gos_GetTerrainMVPMat4();
    uploadOverlayUniforms_(decalProg_->shp_, decalLocs_, elapsed, fixBMvpDecals);

    {
        mc2gl::GlScopedTextureUnit texGuard(0);  // GLSTATE-TEXTURE-UNIT0-RESTORE-1
        GLint prevVao = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glBindVertexArray(decalBatch_.vao);
        GLuint canary = vkScopedGlCanaryBegin("MC2_VK_TERRAIN_DECAL_GL_CANARY");
        int canaryDraws = 0;
        for (const auto& entry : decalBatch_.draws) {
            if (decalLocs_.tex1 >= 0)
                glUniform1i(decalLocs_.tex1, 0);
            glActiveTexture(GL_TEXTURE0);
            gosTexture* t = lookupBatchTextureOrWarn(textureList_, entry.texHandle, "decalBatch");
            bindBatchTextureOrFallback(t, entry.texHandle, "decalBatch");  // OMT-1: no bind-0 on resolve-fail
            glDrawArrays(GL_TRIANGLES, (GLint)entry.firstVert, (GLsizei)entry.vertCount);
            ++canaryDraws;
        }
        vkScopedGlCanaryEnd(canary, "terrain_decal_batch", canaryDraws);
        glBindVertexArray((GLuint)prevVao);
    }  // texGuard: restores unit-0 binding + active texture unit before invalidate

    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glUseProgram(0);

    decalBatch_.verts.clear();
    decalBatch_.draws.clear();

    gos_InvalidateRenderStateCache();

    render_contract::endPassScope(render_contract::PassIdentity::TerrainDecal,
                                  "gosRenderer_drawDecals");
}

// APPLY-STATE-TERRAINDECAL-1: forward-decl of the cross-TU apply-state counter bump
// (defined in gos_postprocess.cpp). Bumps the SAME g_applyStatePasses that the
// PostProcess apply-state islands use, so executor_apply_state_passes aggregates
// top-level + sub-stage applies without a separate gauge.
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);

// APPLY-STATE-TERRAINDECAL-1: pre-apply the declared TerrainDecal pipeline. This is
// the top-level analog of gosPostProcess::executorApply<X>State() — it performs the
// SOLE entry render-state of drawDecals (applyPipeline(TerrainDecal)), bumps the
// apply-state counter, and flags the body to skip its own applyPipeline (one-shot).
// Only the pipeline is lifted this slice; FBO/drawBuffers/viewport are inherited from
// the preceding Terrain pass and are NOT touched here (kTopLevelStateDesc records them
// as Unknown/Inherit). Idempotent: the body makes the identical call when the flag is
// false. Only reached when MC2_FRAMEGRAPH_EXECUTOR is ON and the pass runs.
void gosRenderer::executorApplyTerrainDecalState()
{
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainDecal), "TerrainDecal");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::TerrainDecal);
    decalStateAppliedByExecutor_ = true;
}

// APPLY-STATE-TERRAINOVERLAY-1: pre-apply the declared TerrainOverlay pipeline. Second
// consumer of the top-level apply-state machinery, mirroring executorApplyTerrainDecalState().
// Performs the SOLE entry render-state of drawTerrainOverlays (applyPipeline(TerrainOverlay)
// — opaque, depth-write ON), bumps the apply-state counter, and flags the body to skip its
// own applyPipeline (one-shot). Only the pipeline is lifted; FBO/drawBuffers/viewport are
// inherited from the preceding Terrain pass and NOT touched here (kTopLevelStateDesc records
// them Unknown/Inherit). Idempotent: the body makes the identical call when the flag is false.
// Only reached when MC2_FRAMEGRAPH_EXECUTOR is ON and the pass runs.
void gosRenderer::executorApplyTerrainOverlayState()
{
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainOverlay), "TerrainOverlay");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::TerrainOverlay);
    overlayStateAppliedByExecutor_ = true;
}

// APPLY-STATE-WATER-1: pre-apply the declared WaterArmed pipeline. Fifth consumer of the
// top-level apply-state machinery, mirroring executorApplyTerrainOverlayState(). ★Unlike
// decal/overlay/staticprop/mech (whose apply is dispatched at the begin seam), this is
// dispatched at the BODY draw site (renderWaterFastPath, immediately before the :3276
// applyPipeline) because a mid-body COMPUTE dispatch (ComputeDispatchAndBindThinRecords ->
// glUseProgram(compute)) runs between the begin seam and the applyPipeline, so a seam
// pre-apply would be clobbered. Performs the SOLE liftable entry render-state
// (applyPipeline(WaterArmed)), bumps the apply-state counter, and flags the body to skip its
// own applyPipeline (one-shot). Only the pipeline is lifted; FBO/drawBuffers/viewport are
// inherited from the caller (scene/MainColor FBO) and NOT touched here (kTopLevelStateDesc
// records them Unknown/Inherit). The debug-gated glDepthMask (MC2_WATER_NO_DEPTH_WRITE) that
// follows in the body is NOT lifted. Idempotent: the body makes the identical call when the
// flag is false. Only reached when MC2_FRAMEGRAPH_EXECUTOR is ON and the pass runs.
void gosRenderer::executorApplyWaterState()
{
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::WaterArmed), "WaterArmed");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::Water);
    waterStateAppliedByExecutor_ = true;
}

// APPLY-STATE-SHADOW-1: pre-apply the declared Shadow render-target MODE at the dynamic
// shadow seam. ★FIRST consumer that lifts a CLEAR (DepthForwardZ) in addition to the
// pipeline — Shadow owns a render-target mode, not just a pipeline. The EXTEND generic-axes
// helper is called with the Shadow row whose fboTarget=Unknown / viewport=Inherit, so it
// applies ONLY the DepthForwardZ depth clear (1.0 -> glClear(DEPTH) -> 0.0; the trailing
// reset to 0.0 protects the reverse-Z scene). FBO/viewport are passed as 0 and are NOT
// touched by the helper (skip-sentinels) — the body keeps its glBindFramebuffer
// (ShadowDynamicMap) + glViewport (ShadowMap size); lifting those is SHADOW-2.
// Then applies the ShadowMech BASE pipeline (mech casters inherit it; dynamic-prop casters
// re-apply ShadowStaticProp at their draw sites — body-owned), bumps the apply counter, and
// flags the body to skip its own applyPipeline + clear (one-shot). Order note: the helper does
// the clear, and the pipeline is applied here, so when ON the clear runs immediately before
// applyPipeline(ShadowMech) and BEFORE the unconditional noteRenderPass(ShadowCaster) in the
// body. That moves the clear ~6 lines earlier than the OFF path; it is provably inert to what
// the note samples (FBO binding + depthFunc/depthWrite — a depth-buffer clear changes neither)
// and no draw occurs in the gap, so the rendered shadow is byte-identical. Only reached when
// MC2_FRAMEGRAPH_EXECUTOR is ON and the dynamic shadow pass runs.
void gosRenderer::executorApplyShadowState()
{
    const RenderCore::framegraph::TopLevelStateDesc* desc =
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::Shadow);
    if (desc) {
        // APPLY-STATE-SHADOW-2: resolve the dynamic shadow FBO + shadow-map size and pass them
        // so the EXTEND helper binds fboTarget=ShadowDynamicMap, sets viewport=ShadowMap (the
        // shadow-map size), then does the DepthForwardZ clear — in that order, AFTER the body's
        // AMD-unbind + GL_TEXTURE_COMPARE_MODE preamble (the dispatch sits at the body's former
        // FBO-bind position). Byte-identical to the body's FBO->viewport->clear sequence.
        gosPostProcess* pp = getGosPostProcess();
        const unsigned fbo = pp ? pp->getDynamicShadowFBO() : 0u;
        const int      sz  = pp ? pp->getDynamicShadowMapSize() : 0;
        RenderCore::framegraph::applyTopLevelGenericAxes(*desc, fbo, sz, sz);
    }
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowMech), "ShadowMech");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::Shadow);
    shadowStateAppliedByExecutor_ = true;
}

// FRAMEGRAPH-APPLY-STATE-EXTEND-1: shared GENERIC-AXES apply helper (declared in
// RenderCore/top_level_apply_axes.h). Applies the OPTIONAL non-pipeline axes a
// TopLevelStateDesc declares, each SKIPPED on its sentinel so the 4 unchanged consumers
// (fboTarget=Unknown / viewport=Inherit / clear=None) are byte-identical no-ops. The
// pipeline is applied by the per-pass fn AFTER this returns. Cross-TU shared so the
// upcoming Shadow slice reuses it (FBO + ShadowMap viewport + DepthForwardZ clear).
namespace RenderCore { namespace framegraph {
void applyTopLevelGenericAxes(const TopLevelStateDesc& d, unsigned fbo, int vpW, int vpH) {
    // 1. FBO bind — skip on Unknown (body inherits the bound FBO).
    if (d.fboTarget != RenderResourceId::Unknown) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    }
    // 2. Viewport — skip on Inherit (body inherits the active viewport).
    if (d.viewport != ViewportKind::Inherit) {
        glViewport(0, 0, vpW, vpH);
    }
    // 3. Depth clear — skip on None. DepthForwardZ = forward-Z clear; the scene runs reverse-Z
    //    (clear-to-0) by default, so we clear to 1 then RESTORE clear-depth to 0 to leave global
    //    GL clear-depth state unchanged. NOT used this slice (StaticProp.clear == None); present
    //    for the Shadow slice (forward-Z shadow map).
    if (d.clear == ClearSpec::DepthForwardZ) {
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glClearDepth(0.0);
    }
}
}} // namespace RenderCore::framegraph

// ── Thin exported wrappers ────────────────────────────────────────────────────

void __stdcall gos_PushTerrainOverlay(const WorldOverlayVert* verts3, unsigned int texHandle) {
    // PR2b Stage 0b probe — counts every entry into the producer regardless
    // of which call site invoked. Diagnoses whether the overlay-via-quad-walk
    // pipeline is live anywhere in tier1.
    gos_terrain_indirect::Counters_AddGosPushOverlayCall();
    if (g_gos_renderer && verts3) g_gos_renderer->pushTerrainOverlayTri(verts3, texHandle);
}

void __stdcall gos_PushDecal(const WorldOverlayVert* verts3, unsigned int texHandle) {
    if (g_gos_renderer && verts3) g_gos_renderer->pushDecalTri(verts3, texHandle);
}

void __stdcall gos_DrawTerrainOverlays() {
    if (g_gos_renderer) g_gos_renderer->drawTerrainOverlays();
}

void __stdcall gos_DrawDecals() {
    if (g_gos_renderer) g_gos_renderer->drawDecals();
}

// ── End world-space overlay batch API ─────────────────────────────────────────

// Resolve a gosTextureHandle to the underlying raw GL texture name for
// consumers outside this TU (gos_static_prop_batcher). Returns 0 on
// invalid handle — caller should treat 0 as "unbind / default white".
uint32_t gos_GetGLTextureId(uint32_t gosHandle) {
    if (gosHandle == INVALID_TEXTURE_ID || !g_gos_renderer) return 0;
    // Reject uninitialized / sentinel handles (seen in the wild on
    // TG_TinyTexture entries that were never loaded: 0xFFFFFFFF).
    // Bound by the current texture list size to avoid the hard assert
    // inside gosRenderer::getTexture(texture_id >= list size).
    if (gosHandle >= g_gos_renderer->getTextureListSize()) return 0;
    gosTexture* tex = g_gos_renderer->getTexture(gosHandle);
    return tex ? tex->getTextureId() : 0;
}

// P1-1: FX-GPU-1 Phase 1 — narrow GL-name resolver for the GPU particle
// bridge. Declared in GameOS/include/gameos.hpp and called at flush time.
// Delegates to gos_GetGLTextureId; the DWORD parameter matches the GOS
// texture handle convention used by gosFX/MLR.
unsigned int gos_GetGLTextureName(DWORD handle) {
    return gos_GetGLTextureId(static_cast<uint32_t>(handle));
}

const float* gos_GetProj2ScreenMat4() {
    if (!g_gos_renderer) return nullptr;
    return (const float*)&g_gos_renderer->getProj2Screen();
}

const float* gos_GetTerrainMVPMat4() {
    const bool rendererOk = (g_gos_renderer != nullptr);
    const bool valid      = rendererOk && g_gos_renderer->isTerrainMVPValid();
    const float* p        = valid ? (const float*)&g_gos_renderer->getTerrainMVP() : nullptr;

    // [MVP_DIAG v1] S2.7 — log get_mvp entry, throttled. If valid,
    // also dump row0 from the cache to compare against set_mvp row0.
    if (g_mvpDiagFrame == 1 || g_mvpDiagFrame == 5 ||
        g_mvpDiagFrame == 30 || g_mvpDiagFrame == 120) {
        static long s_lastLogged = -1;
        if (s_lastLogged != g_mvpDiagFrame) {
            s_lastLogged = g_mvpDiagFrame;
            fprintf(stderr,
                    "[MVP_DIAG v1] event=get_mvp frame=%ld renderer=%p valid=%d ptr=%p\n",
                    g_mvpDiagFrame, (void*)g_gos_renderer, valid ? 1 : 0, (void*)p);
            if (p) {
                fprintf(stderr,
                        "[MVP_DIAG v1] event=get_mvp_row0 frame=%ld row0=[%g %g %g %g]\n",
                        g_mvpDiagFrame, p[0], p[1], p[2], p[3]);
            }
            fflush(stderr);
        }
    }
    return p;
}

// TERRAIN-SHORELINE-MASK-1: shared render-shader clock, same computation as
// the water fast-path's "time" uniform (SmokeMode fixed-timestep override for
// deterministic capture, else wall-time since renderer init). Consumed by the
// chunk terrain frag's shoreline wet/foam band (f(worldPos,time) ONLY — no
// camera dependence, matching the water FS's camera-independence ruling).
float gos_GetShaderClockSeconds() {
    return g_gos_renderer ? g_gos_renderer->getShaderClockSeconds() : 0.0f;
}

// gos_GetTerrainTeseProgram removed in Task 7b (UBO pivot).
// Per-program upload approach retired; UnifiedProjectionUBO at binding=0
// reaches all material-variant programs by binding point.

#include "gameos_graphics_debug.cpp"
