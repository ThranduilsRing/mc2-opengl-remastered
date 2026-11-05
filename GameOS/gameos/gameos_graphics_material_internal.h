// Shared surface of the gameos_graphics split pieces (gameos_graphics_{draw,water,bridge,shadow,tparams,hud,capi,overlay}.cpp).
//
// gameos_graphics.cpp keeps the renderer lifecycle (init/destroy), the gate readers and the accessors; the rest of the
// renderer is split by responsibility (see DOC in each piece). This header holds the material, mesh and buffer classes,
// the constants and declarations every piece needs, and includes the gosRenderer class header.
// gameos_graphics_internal.h (the earlier extraction shim header) is included below, unchanged.
#ifndef GAMEOS_GRAPHICS_MATERIAL_INTERNAL_H
#define GAMEOS_GRAPHICS_MATERIAL_INTERNAL_H
#include <string_view>

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

#include "utils/color_utils.h"

#ifdef MC2_IMGUI
#include "../../GuiRuntime/GuiRuntime.h"
#endif
#include "utils/Image.h"
#include "utils/vec.h"
#include "utils/string_utils.h"
#include "utils/timing.h"
#include "gos_render.h"
#include "../../RenderCore/PipelineRegistry.h"   // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1
#include "render_frame_plan.h"                    // RENDER-FRAME-PLAN-SCAFFOLD-1
#include "mdi_submit.h"                            // MDI-SUBMISSION-SCAFFOLD-1
#include "gos_postprocess.h"
#include "gos_profiler.h"
#include "gos_validate.h"  // validation entry points
#include "gos_visual_capture.h"   // VISUAL-CAPTURE-CLEAN-1
#include "gos_smoke.h"     // S9E: SmokeMode fixed deterministic render-shader clock
#include "../../mclib/cpu_proj_cost_split.h"  // F3 CPU projection cost-baseline
#include "../../mclib/camera_frustum_math.h"  // CAMERA-FRUSTUM-HARNESS-1 pure rect/frustum math
#include "../../mclib/render_contract.h"      // [RENDER_PASS v1] noteRenderPass
#include "../../mclib/mc2_audit.h"            // META-FIX INFRA-1 gated audit emitter
#include "RenderCore/top_level_pass_executor.h" // APPLY-STATE-TERRAINDECAL-1: findTopLevelStateDesc
#include "RenderCore/ResolutionContract.h"     // META-FIX design D: MC2_RES_ASSERT audit
#include "../../RenderCore/frame_executor.h"     // PER-PASS-APPLY-COUNTERS-1: ApplyPassId
#include "stuff/stuff.hpp"                     // Stuff::Matrix4D for gos_SetWorldToClipGL (full chain required; matrix.hpp alone creates circular include ordering)

// [B1 C16] (diagnostic) gosFX heap + child accumulation counters; env-gated on
// MC2_GPU_PARTICLES=1. Forward-decl to avoid Stuff/gosfx header chain here.
namespace gosFX { void DiagFrameTick(); }
#include "debug_state_dump.h"    // mc2_debug_state::getSessionId, writeShutdownState
#include "diagnostic_trace.h"   // mc2_diag::init, shutdown
#include "build_identity.h"  // MC2_BUILD_GIT_SHA/BRANCH/DIRTY/TIME_ISO
#ifdef _WIN32
#include <process.h>             // _getpid()
#else
#include "platform_io.h"
#endif

// Build-type name written into the BUILD diag event (gameos_graphics.cpp).
#if defined(_DEBUG)
inline constexpr const char* kDiagBuildConfig = "Debug";
#elif defined(NDEBUG)
inline constexpr const char* kDiagBuildConfig = "Release";
#else
inline constexpr const char* kDiagBuildConfig = "RelWithDebInfo";
#endif
#include "gos_terrain_bridge.h"
#include "gos_terrain_lod_chunk.h"
#include "gos_terrain_lighting.h"                // NIGHT-LIGHT-POOLS-2b: pool SSBO accessors
#include "gos_terrain_patch_stream.h"
#include "gos_terrain_indirect.h"
#include "gos_terrain_water_stream.h"
#include "gpu_driven_common.h"
#include "mc2_hitch_trace.h"  // GPU-UPDATE-BUFFER-COUNTER-1: MC2_GL_BufferData_Owner (include LAST, after all GL headers)
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
#include "../../RenderCore/axis_convert.h"             // mc2ToRender: the one spelling of (x,y,z)->(-x,z,y)

// Declarations, macros, constants and classes the split pieces share.
bool terrainNormalArrayEnabled();   // defined below; the classes in this header call it
bool mc2ShadowStaticBandFixEnabled();   // defined below; the classes in this header call it
bool mc2ShadowStaticBandFixV2Enabled();   // defined below; the classes in this header call it

extern const float*     gos_GetTerrainMVPMat4();


static const DWORD INVALID_TEXTURE_ID = 0;


class gosBuffer {
	friend class gosRenderer;
public:
	std::vector<uint8_t> bytes_;
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


};


// CPU glyph staging for native HUD batches. No GL buffers or draw ownership.
class gosMesh {
public:
    static gosMesh* makeMesh(int capacity) { return new gosMesh(capacity); }
    static void destroy(gosMesh* mesh) { delete mesh; }
    ~gosMesh() { delete[] vertices_; }

    bool addVertices(gos_VERTEX* vertices, int count) {
        if (size_ + count > capacity_) return false;
        memcpy(vertices_ + size_, vertices, sizeof(gos_VERTEX) * count);
        size_ += count;
        return true;
    }
    int getVertexCapacity() const { return capacity_; }
    int getNumVertices() const { return size_; }
    const gos_VERTEX* getVertices() const { return vertices_; }
    void rewind() { size_ = 0; }



private:
    explicit gosMesh(int capacity)
        : capacity_(capacity), vertices_(new gos_VERTEX[capacity]) {}
    int capacity_;
    int size_ = 0;
    gos_VERTEX* vertices_;
};




#include "gameos_graphics_internal.h"


struct gosTextAttribs {
    HGOSFONT3D FontHandle = nullptr;   // must start null: GameDebugWindow::setFont queries length before any font is set
    DWORD Foreground = 0xffffffff;
    float Size = 1.0f;
    bool WordWrap = false;
    bool Proportional = true;
    bool Bold = false;
    bool Italic = false;
    DWORD WrapType = 0;
    bool DisableEmbeddedCodes = false;
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
extern "C" void mc2_ui_pass_begin_measure();

extern "C" void mc2_ui_pass_note_draw(unsigned kind);



// APPLY-STATE-TERRAINDECAL-1: forward-decl of the cross-TU apply-state counter bump
// (defined in gos_postprocess.cpp). Bumps the SAME g_applyStatePasses that the
// PostProcess apply-state islands use, so executor_apply_state_passes aggregates
// top-level + sub-stage applies without a separate gauge.
extern "C" void mc2_framegraph_executor_bump_apply_state(unsigned id);


extern gosRenderer* g_gos_renderer;
gosRenderer* getGosRenderer();
extern bool g_disable_quads;
extern long g_mvpDiagFrame;

// Second shared header: the class definitions that no other piece may redefine.
#include "gameos_graphics_renderer_internal.h"

#endif // GAMEOS_GRAPHICS_MATERIAL_INTERNAL_H
