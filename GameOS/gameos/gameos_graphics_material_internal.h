// Shared surface of the gameos_graphics split pieces (gameos_graphics_{draw,water,bridge,shadow,tparams,hud,capi,overlay}.cpp).
//
// gameos_graphics.cpp keeps the renderer lifecycle (init/destroy), the gate readers and the accessors; the rest of the
// renderer is split by responsibility (see DOC in each piece). This header holds the material, mesh and buffer classes,
// the constants and declarations every piece needs, and includes the gosRenderer class header.
// gameos_graphics_internal.h (the earlier extraction shim header) is included below, unchanged.
#ifndef GAMEOS_GRAPHICS_MATERIAL_INTERNAL_H
#define GAMEOS_GRAPHICS_MATERIAL_INTERNAL_H

#include <vector>
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "utils/mc2log.h"
#include "mc2fmt.h"
#include <memory>   // VK-HUD-MIRRORCOPY-1: shared_ptr rgba mirror
#include <map>
#include <algorithm>
#include <cstdint>
#include <string>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <cctype>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

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

#ifdef MC2_IMGUI
#include "../../GuiRuntime/GuiRuntime.h"
#endif
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
#include "gos_validate.h"  // validation entry points
#include "gl_state_guard.h"  // GlScopedTextureUnit (GLSTATE-TEXTURE-UNIT0-RESTORE-1)
#include "gos_visual_capture.h"   // VISUAL-CAPTURE-CLEAN-1
#include "gos_smoke.h"     // S9E: SmokeMode fixed deterministic render-shader clock
#include "../../mclib/cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline
#include "../../mclib/camera_frustum_math.h"  // CAMERA-FRUSTUM-HARNESS-1 pure rect/frustum math
#include "../../mclib/render_contract.h"      // [RENDER_PASS v1] noteRenderPass
#include "../../mclib/mc2_audit.h"            // META-FIX INFRA-1 gated audit emitter
#include "RenderCore/top_level_pass_executor.h" // APPLY-STATE-TERRAINDECAL-1: findTopLevelStateDesc
#include "RenderCore/top_level_apply_axes.h"   // FRAMEGRAPH-APPLY-STATE-EXTEND-1: applyTopLevelGenericAxes
#include "RenderCore/ResolutionContract.h"     // META-FIX design D: MC2_RES_ASSERT audit
#include "../../RenderCore/frame_executor.h"     // PER-PASS-APPLY-COUNTERS-1: ApplyPassId
#include "Stuff/Stuff.hpp"                     // Stuff::Matrix4D for gos_SetWorldToClipGL (full chain required; matrix.hpp alone creates circular include ordering)

// [B1 C16] (diagnostic) gosFX heap + child accumulation counters; env-gated on
// MC2_GPU_PARTICLES=1. Forward-decl to avoid Stuff/gosfx header chain here.
namespace gosFX { void DiagFrameTick(); }
#include "debug_state_dump.h"    // mc2_debug_state::getSessionId, writeShutdownState
#include "diagnostic_trace.h"   // mc2_diag::init, shutdown
#include "build_fingerprint.h"  // MC2_BUILD_GIT_SHA/BRANCH/DIRTY/TIME_ISO
#include <process.h>             // _getpid()

#if defined(_DEBUG)
#  define MC2_DIAG_BUILD_CONFIG "Debug"
#elif defined(NDEBUG)
#  define MC2_DIAG_BUILD_CONFIG "Release"
#else
#  define MC2_DIAG_BUILD_CONFIG "RelWithDebInfo"
#endif
#include "gos_terrain_bridge.h"
#include "gos_terrain_lod_chunk.h"
#include "gos_terrain_lighting.h"                // NIGHT-LIGHT-POOLS-2b: pool SSBO accessors
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
#include "Gates/gate_cold.h"   // typed gate registry: get<>(), str<>()
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_terrain_island.h"    // VK-PARITY-OVERLAY-1: overlay/decal VK owner queue
#endif

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
#include "../../RenderCore/axis_convert.h"             // mc2ToRender: the one spelling of (x,y,z)->(-x,z,y)

// Declarations, macros, constants and classes the split pieces share.
bool terrainNormalArrayEnabled();   // defined below; the classes in this header call it
bool mc2ShadowStaticBandFixEnabled();   // defined below; the classes in this header call it
bool mc2ShadowStaticBandFixV2Enabled();   // defined below; the classes in this header call it
GLenum getGLVertexAttribType(gosVERTEX_ATTRIB_TYPE type);   // defined below; the classes in this header call it

extern const float*     gos_GetTerrainMVPMat4();


static const DWORD INVALID_TEXTURE_ID = 0;


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

static_assert(kTerrainMatNormalUnits[4] != kTerrainTexUnitStaticShadow,
              "matNormal4 collides with static shadow map unit");

static_assert(kTerrainMatNormalUnits[4] != kTerrainTexUnitDynamicShadow,
              "matNormal4 collides with dynamic shadow map unit");

static_assert(kTerrainMatNormalUnits[4] != kTerrainTexUnitHeight,
              "matNormal4 collides with height texture unit");

static_assert(kTerrainMatNormalUnits[4] < 16,
              "matNormal4 exceeds OpenGL min-spec 16-unit floor");


// Public helper for consumers outside this TU (e.g. gos_static_prop_batcher)
// that need to turn a gosTextureHandle into a raw GL texture name. Declared
// here so local code can reference it; defined at end of this file after
// the gosRenderer / gosTexture class bodies are in scope.
uint32_t gos_GetGLTextureId(uint32_t gosHandle);



class gosBuffer {
	friend class gosRenderer;
public:
	GLuint buffer_;
	int element_size_;
	uint32_t count_;
	gosBUFFER_TYPE type_;
	gosBUFFER_USAGE usage_;
};


class gosVertexDeclaration {
	friend class gosRenderer;

	gosVERTEX_FORMAT_RECORD* vf_;
	uint32_t count_;

	gosVertexDeclaration() :vf_(nullptr), count_(0) {}
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
            if (mc2ShadowStaticBandFixEnabled())
                defines_str.append("#define MC2_SHADOW_STATIC_BANDFIX 1\n");
            if (mc2ShadowStaticBandFixV2Enabled())
                defines_str.append("#define MC2_SHADOW_STATIC_BANDFIX_V2 1\n");
            defines_str.append(mc2TerrainShadowSunDefine());   // TERRAIN-SHADOW-GUARD-1
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
            // SHADOW-DIST-FADE-1: distance-scaled dynamic-shadow penumbra +
            // fade (shadow.hglsl). =0 (killswitch) -> no define -> block
            // folds away, current behavior.
            if (mc2ShadowDistFadeWu() > 0.0f) {
                char fadeDef[240];
                snprintf(fadeDef, sizeof(fadeDef),
                         "#define MC2_SHADOW_DIST_FADE_WU %.1f\n"
                         "#define MC2_SHADOW_DIST_FADE_MAX_F %.2f\n"
                         "#define MC2_SHADOW_FADE_CONT_F %d\n"
                         "#define MC2_SHADOW_FADE_RAMP0_F %d\n"
                         "#define MC2_SHADOW_ALT_SCALE_F %d\n",
                         mc2ShadowDistFadeWu(), mc2ShadowDistFadeMax(),
                     mc2ShadowFadeCont() ? 1 : 0,
                     mc2ShadowFadeRamp0() ? 1 : 0,
                     mc2ShadowAltScale() ? 1 : 0);
                defines_str.append(fadeDef);
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
                MC2_LOG(Shader, Debug, "Failed to create {} material", mc2::fmt::safe(shader));
                printf("[TESS] FAILED to create shader: %s\n", shader);
                delete pmat;
                return nullptr;
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
                pmat->program_ = nullptr;
            }

            delete[] pmat->name_;
            pmat->name_ = nullptr;
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
            

            gosASSERT(pos_loc >= 0);
            glEnableVertexAttribArray(pos_loc);
            glVertexAttribPointer(pos_loc, 4, GL_FLOAT, GL_FALSE, stride, (void*)nullptr);

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
            program_(nullptr)
            , name_(nullptr)
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
// SPLIT-1 slice 4: gosLightGrowOnceEnabled moved to gameos_graphics_light_ssbo.cpp.

class gosMesh {
    public:
        using INDEX_TYPE = WORD;

        static gosMesh* makeMesh(gosPRIMITIVETYPE prim_type, int vertex_capacity, int index_capacity = 0) {
            GLuint vb = makeBuffer(GL_ARRAY_BUFFER, nullptr, sizeof(gos_VERTEX)*vertex_capacity, GL_DYNAMIC_DRAW);
            if(!vb)
                return nullptr;

            GLuint ib = 0;
            if(index_capacity > 0) {
                ib = makeBuffer(GL_ELEMENT_ARRAY_BUFFER, nullptr, sizeof(INDEX_TYPE)*index_capacity, GL_DYNAMIC_DRAW);
                if(!ib)
                    return nullptr;
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
            , pvertex_data_(nullptr)    
            , pindex_data_(nullptr)    
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




#include "gameos_graphics_internal.h"


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




enum HudDrawKind { kHudQuadBatch, kHudLineBatch, kHudTriBatch, kHudTextQuadBatch };


struct HudDrawCall {
    HudDrawKind              kind;
    std::vector<gos_VERTEX>  vertices;
    uint32_t                 stateSnapshot[gos_MaxState];
    mat4                     projection;
    DWORD                    fontTexId;       // kHudTextQuadBatch only
    DWORD                    foregroundColor; // kHudTextQuadBatch only
    bool                     scaleExempt = false; // skip s_hud_scale shrink (cursor, modal dialogs)
    bool                     canvasExempt = false; // skip 16:9 HUD-canvas remap (cursor, world-anchored overlays, dialogs)
};


// Probe 8: forward decls for cross-TU MVP fingerprint accessors (defined in
// gos_terrain_indirect.cpp inside its extern "C" block).
extern "C" uint32_t gos_terrain_indirect_getDispatchMvpFp();

extern "C" uint64_t gos_terrain_indirect_getDispatchMvpFrameIdx();

extern "C" void     gos_terrain_indirect_getDispatchMvpFloats4(float out[4]);


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


// ---------------------------------------------------------------------------
// DEBUG-VIEW-SWEEP-1: one global debug-view selector fanned out to every lane's
// existing per-lane debug channel. Deterministic full-scene diagnostic views
// for pixel-comparable agent sweeps (tools/debug_view_sweep.py).
//   0 = off (all lanes restored to their default channel; env values resume)
//   1 = shadow factor    terrain 44 (raw shadow term grayscale); props 44
//                        (constant no-shadow marker — props don't self-shadow
//                        in-shader); mechs 3 (vertex light-only)
//   2 = normalized depth 50-series: gl_FragCoord.z remapped over the 0.85..1.0
//                        band the scene occupies, grayscale, all main lanes
//   3 = unlit albedo     terrain/props 40, mechs 2, vfx 1 (raw texture color)
//   4 = overdraw proxy   opaque lanes flat 0.12 gray (mode 51); VFX mode 4
//                        (real additive accumulation on top)
//   5 = world normals    terrain/props 41, mechs 4 (N*0.5+0.5 as RGB)
//   6 = chunk/LOD tint   terrain 52 (hashed thin-record palette = real
//                        GPU-indirect chunking); props materialIdx palette
//                        (debugMaterialMode 2); mechs 52 (black; unsupported);
//                        vfx 3 (per-kind palette)
// Lanes NOT covered (render their normal output in every view): water, sky,
// grass/vegetation cards, decals/overlays, legacy gos_vertex* draws, HUD.
// Registered MC2_DEBUG_VIEW (boot seed); runtime via `debugview N` verb.
void batcher_setDebugMaterialMode(int shaderMode);      // gos_static_prop_batcher.h


extern "C" void gos_vfx_setDebugMode(int mode);             // gos_particle_bridge.h


// UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1: observe-only telemetry entry points, defined in
// mclib/render_contract.cpp (GL-aware TU; reuses the ambient-probe sample helpers).
extern "C" void mc2_ui_pass_sample_entry_ambient();

extern "C" void mc2_ui_pass_note_draw(unsigned kind);

extern "C" void mc2_ui_pass_check_ambient();


// APPLY-STATE-TERRAINDECAL-1: forward-decl of the cross-TU apply-state counter bump
// (defined in gos_postprocess.cpp). Bumps the SAME g_applyStatePasses that the
// PostProcess apply-state islands use, so executor_apply_state_passes aggregates
// top-level + sub-stage applies without a separate gauge.
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);


extern gosRenderer* g_gos_renderer;
gosRenderer* getGosRenderer();
extern GLuint gVAO;
void bindTerrainHeightTexUniforms(GLint heightTexLoc, GLint paramsLoc, GLint cliffShadowFloorLoc, float cliffShadowFloor);
void gos_RendererRebindVAO();
extern bool g_disable_quads;
extern long g_mvpDiagFrame;

// Second shared header: the class definitions that no other piece may redefine.
#include "gameos_graphics_renderer_internal.h"

#endif // GAMEOS_GRAPHICS_MATERIAL_INTERNAL_H
