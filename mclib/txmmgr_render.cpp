// MC_TextureManager: renderLists and the ShapeRenderer helper, with the RENDERLISTS cost spans and the shadow-shape list.
// Declared in the class headers; shared names in txmmgr_internal.h.
#include "txmmgr_internal.h"


// ---------------------------------------------------------------------------
// [RENDERLISTS_COST v1] — TXMMGR-PERF-EASYWINS-1 coarse per-phase CPU cost
// split of MC_TextureManager::renderLists(). Smoke-visible complement of the
// Tracy zones (smoke runs cannot take user-driven Tracy captures). Pattern =
// SPFLUSH_COST_SPLIT: env-gated, window-averaged, default OFF (zero overhead
// beyond one cached-bool test per phase when unset; ~2 steady_clock reads per
// phase per frame when set — coarse spans only, per the 100ns-floor rule).
// Enable: MC2_RENDERLISTS_COST_SPLIT=1 -> one [RENDERLISTS_COST v1] stderr
// summary every 60 frames: per-frame mean µs per phase + node-table sizes.
// self_us = total minus the sum of instrumented phases (unattributed gaps).
// ---------------------------------------------------------------------------
namespace rlcost {
static const bool s_enabled = mc2::gate::get<"MC2_RENDERLISTS_COST_SPLIT">();
enum Phase {
    kPreamble = 0, kLightUpload, kSceneData, kObj3d, kStateRestore,
    kStaticShadowBuild, kSpRegistryFlush, kDynShadow, kTerrainSolid,
    kGpuSpPrep, kSpBatcherFlush, kMechFlush, kOverlays, kWaterLoops,
    kNoUnderlayer, kShadowBlobs, kAlphaLoops, kVfxHud, kTotal, kPhaseCount
};
static const char* kName[kPhaseCount] = {
    "preamble", "light_upload", "scene_data", "obj3d", "state_restore",
    "static_shadow_build", "sp_registry_flush", "dyn_shadow", "terrain_solid",
    "gpusp_prep_cull", "sp_batcher_flush", "mech_flush", "overlays",
    "water_loops", "no_underlayer", "shadow_blobs", "alpha_loops",
    "vfx_hud", "total"
};
static unsigned long long s_ns[kPhaseCount] = {};
static int s_frames = 0;

static inline unsigned long long nowNs() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Lexical phase span. Zero-cost (two branch-predicted tests) when disabled.
struct Span {
    int p; unsigned long long t0; GosGlLostDrawsScope lost_;
    explicit Span(int phase) : p(phase), t0(s_enabled ? nowNs() : 0ULL), lost_(kName[phase]) {}
    ~Span() { if (s_enabled) s_ns[p] += nowNs() - t0; }
};

// Whole-function span + window emit. Construct FIRST in renderLists() so the
// destructor runs LAST: accumulates total, then emits every 60 frames.
struct TotalSpan {
    unsigned long long t0; long nodes_; long hwNodes_;
    TotalSpan(long nodes, long hwNodes)
        : t0(s_enabled ? nowNs() : 0ULL), nodes_(nodes), hwNodes_(hwNodes) {}
    ~TotalSpan() {
        if (!s_enabled) return;
        s_ns[kTotal] += nowNs() - t0;
        if (++s_frames < 60) return;
        const double wf = static_cast<double>(s_frames);
        double phaseSumUs = 0.0;
        char line[1400]; int off = 0;
        off += snprintf(line + off, sizeof(line) - off,
            "[RENDERLISTS_COST v1] event=summary frames=%d nodes=%ld hw_nodes=%ld",
            s_frames, nodes_, hwNodes_);
        for (int p = 0; p < kPhaseCount; ++p) {
            const double us = static_cast<double>(s_ns[p]) / 1000.0 / wf;
            if (p != kTotal) phaseSumUs += us;
            if (off < static_cast<int>(sizeof(line)))
                off += snprintf(line + off, sizeof(line) - off,
                                " %s_us=%.1f", kName[p], us);
        }
        const double selfUs =
            static_cast<double>(s_ns[kTotal]) / 1000.0 / wf - phaseSumUs;
        if (off < static_cast<int>(sizeof(line)))
            snprintf(line + off, sizeof(line) - off, " self_us=%.1f", selfUs);
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
        memset(s_ns, 0, sizeof(s_ns));
        s_frames = 0;
    }
};
}

// --- Shadow shape collection (file-scope global, no struct layout impact) ---
struct ShadowShapeEntry {
	HGOSBUFFER vb;
	HGOSBUFFER ib;
	HGOSVERTEXDECLARATION vdecl;
	float worldMatrix[16];
};
static const int MAX_SHADOW_SHAPES = 512;
static ShadowShapeEntry g_shadowShapes[MAX_SHADOW_SHAPES];

// FRAME-JOBS-2 precondition: atomic so worker threads can reserve shadow
// shape slots without a mutex. ShadowShapeEntry fields are fully independent
// per-slot (vb/ib/vdecl/worldMatrix — no aliased pointers), so fetch_add
// reservation + per-slot write is race-free between different callers.
static std::atomic<int> g_numShadowShapes{0};

static bool isAllConcreteTerrainBatch(const gos_VERTEX* vertices, DWORD totalVertices)
{
	if (!vertices || totalVertices == 0)
		return false;

	for (DWORD vi = 0; vi < totalVertices; ++vi)
	{
		if ((vertices[vi].frgb & 0x000000ff) != 3)
			return false;
	}

	return true;
}

void addShadowShape(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, const float* worldEntries16) {
	// Atomic reservation: fetch_add reserves an exclusive slot index.
	// ShadowShapeEntry fields are per-slot independent, so two threads
	// writing different indices never alias.
	// Do NOT store MAX_SHADOW_SHAPES back on overflow — that store is racy
	// (concurrent fetch_adds can push count past MAX before the store lands,
	// and the store itself races with other fetch_adds). Instead let the
	// counter freely overflow and clamp at all read sites via
	//   std::min(g_numShadowShapes.load(), MAX_SHADOW_SHAPES).
	int idx = g_numShadowShapes.fetch_add(1, std::memory_order_relaxed);
	if (idx >= MAX_SHADOW_SHAPES) {
		return;
	}
	ShadowShapeEntry& ss = g_shadowShapes[idx];
	ss.vb = vb;
	ss.ib = ib;
	ss.vdecl = vdecl;
	memcpy(ss.worldMatrix, worldEntries16, 16 * sizeof(float));
}

void clearShadowShapes() {
	g_numShadowShapes.store(0, std::memory_order_relaxed);
}

#define MAX_SENDDOWN		10002

mat4 gos2my(Stuff::Matrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		m.entries[12], m.entries[13], m.entries[14], m.entries[15]);
	return m2;
}

mat4 gos2my(Stuff::LinearMatrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		0.0f, 0.0f, 0.0f, 1.0f);
	return m2;
}


////////////////////////////////////////////////////////////////////////////////
class ShapeRenderer {

	mat4* world_;
	mat4* view_;
	mat4* wvp_;
	float* viewport_;
	HGOSBUFFER lights_data_;

public:

	void setup(mat4* world, mat4* view, mat4* wvp, float* viewport)
	{
		gosASSERT(world && view && wvp && viewport);
		world_ = world;
		view_ = view;
		wvp_ = wvp;
		viewport_ = viewport;
	}

	void set_lights_data(const HGOSBUFFER lights_data)
	{
		lights_data_ = lights_data;
	}

	void render(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, DWORD texture_id, int light_index, bool isHudElement = false, const TG_RenderShape* renderShape = NULL)
	{
		gos_SetRenderState(gos_State_Texture, texture_id);
		gos_SetRenderViewport(viewport_[2], viewport_[3], viewport_[0], viewport_[1]);

		HGOSRENDERMATERIAL mat;
		const bool usePbrOverride = renderShape &&
			renderShape->programOverride_ &&
			renderShape->pbrNormalTexture_ &&
			renderShape->pbrOrmTexture_ &&
			renderShape->pbrMaterialSsbo_;
		if (usePbrOverride) {
			mat = renderShape->programOverride_;
		} else if (texture_id == 0) {
			static const HGOSRENDERMATERIAL s_matVertexLighted = gos_getRenderMaterial("gos_vertex_lighted");
			mat = s_matVertexLighted;
		} else {
			static const HGOSRENDERMATERIAL s_matTexVertexLighted = gos_getRenderMaterial("gos_tex_vertex_lighted");
			mat = s_matTexVertexLighted;
		}

		gos_SetRenderMaterialParameterMat4(mat, "world_", (const float*)*world_);
		//gos_SetRenderMaterialParameterMat4(mat, "view_", (const float*)*view_);
		gos_SetRenderMaterialParameterMat4(mat, "wvp_", (const float*)*wvp_);

        float ld[4] = { (float)light_index, 0.0f, 0.0f, 0.0f};
		gos_SetRenderMaterialParameterFloat4(mat, "light_offset_", ld);

		// GPU projection via terrainMVP (skip for HUD elements which need legacy viewport projection)
		gos_SetRenderMaterialParameterInt(mat, "gpuProjection", isHudElement ? 0 : 1);

		// VEHICLE-NIGHT-LIT-1 killswitch: feed calc_light MC2-frame position +
		// normal (matching mech.vert / static_prop.vert) instead of the legacy
		// Stuff-frame pair.
		// See the u_legacyLitMc2Frame block in shaders/gos_tex_vertex_lighted.vert
		// for the full root-cause writeup (vehicles received no point/spot light
		// because the frames were mixed inside GetFalloff's distance test).
		//
		// VEHICLE-LIGHTING-PATH-1: PROMOTED TO DEFAULT-ON, and this is a
		// CORRECTNESS REQUIREMENT of the CacheGpuLightData() call added at
		// gvactor.cpp -- NOT an optional enhancement. DO NOT "clean up" this
		// gate as inert.
		//
		// Why it looked inert before: until this slice, vehicles never
		// populated lightData_ at all, so calc_light took its numLights==0
		// early-return (lighting.hglsl:232-236) and returned vec3(1,1,1)
		// regardless of frame. Correcting the FRAME of a row containing ZERO
		// lights is necessarily a no-op -- which is exactly why
		// VEHICLE-NIGHT-LIT-1 measured as "disproven" on its own. It was never
		// wrong, only unreachable.
		//
		// Now that the row is populated, the frame becomes load-bearing:
		// feeding Stuff-frame position/normal against MC2-frame light rows
		// makes length(vertex_world_pos - light_pos) meaningless inside
		// GetFalloff and puts the sun NdotL ~90 degrees off. Shipping the
		// gather with this OFF would produce confidently-wrong lighting.
		//
		// Blast radius is now self-limiting: this program is shared with other
		// legacy lit shapes, but any shape that does not gather still has
		// numLights==0 and returns before the frame is ever read. Only shapes
		// that actually carry lights -- i.e. the vehicles this slice fixes --
		// can observe the flip.
		static const bool s_legacyLitMc2Frame = mc2::gate::get<"MC2_LEGACY_LIT_MC2_FRAME">();
		gos_SetRenderMaterialParameterInt(mat, "u_legacyLitMc2Frame",
			s_legacyLitMc2Frame ? 1 : 0);

		// SUN-AXIS-FIX-1. ObjectLights.light_dir is a STUFF-frame vector --
		// GetLocalForwardInWorld() on a BuildRotation(EulerAngles(pitch,
		// yaw+135, 0)) transform (this file :2621, mclib/camera.cpp:509/:1860/
		// :2110/:4252) returns matrix row 2 = (cosP*sinY, -sinP, cosP*cosY).
		// Applying the same Stuff->MC2 swap used for normals and negating for
		// the toward-sun convention gives (cosP*cos a, cosP*sin a, sinP) with
		// a = lightYaw + 45 -- the true spherical sun. The +90deg between
		// yaw+135 and the terrain sun's yaw+45 is EXACTLY cancelled by the
		// swap, which is why the one consumer that swaps (mech.vert:279,
		// specular) has always been right and the ones that do not are wrong.
		// Proven on all 24 shipping missions offline by
		// tools/class_light_band_harness Stage B/D -- no launch required.
		//
		// Only meaningful on the u_legacyLitMc2Frame=1 branch, which is where
		// the shader hands calc_light an MC2-frame normal; the shader applies
		// it there and nowhere else.
		//
		// DEFAULT OFF. This is a correctness fix but a LARGE and TWO-SIDED
		// visual one -- 22 of 24 missions move, up to 0.53 of full scale, and
		// almost exactly as many surface normals get DARKER as brighter. No
		// human has seen it (engine launches are hard-blocked in this lane), so
		// it ships opt-in. MC2_SUN_AXIS_FIX=1 enables.
		// Canonical DEFAULT-OFF OPT-IN idiom `(v && v[0] && v[0] != '0')`, so
		// scripts/check-gate-default-agreement.py can read the default OUT of
		// the reader. An unparseable idiom is a FAILURE for a Feature gate.
		static const bool s_sunAxisMc2Fix = mc2::gate::get<"MC2_SUN_AXIS_FIX">();
		gos_SetRenderMaterialParameterInt(mat, "u_sunAxisMc2Fix",
			s_sunAxisMc2Fix ? 1 : 0);

		// VEHICLE-AMBIENT-FLOOR-1: DEFAULT-ON. Finishes VEHICLE-LIGHTING-PATH-1
		// by giving this path the two terms mech.vert has and it did not --
		// the kAmbientFloor = 0.35 seed handed to calc_light, and the
		// clamp(...,0,1) on the result (shaders/mech.vert:204,:223).
		//
		// VEHICLE-LIGHTING-PATH-1 fixed "vehicles have no light row". It did
		// NOT fix "the truck is darker than the mech next to it", because a
		// vehicle computed base_light + A + S*ndl, unclamped, while a mech
		// computes clamp(0.35 + A + S*ndl, 0, 1). base_light is the per-vertex
		// aRGBLight seed: measured across the 8 stock ground-vehicle .tgl
		// binaries it is 0x000000 on 56-110 verts/file, 0x010101 on 3-6, and
		// 0x646257 (0.392, the only value above the 0.35 floor) on at most 2.
		// See the base_light block in shaders/gos_tex_vertex_lighted.vert.
		// On mc2_10 --
		// the map in the user's report -- that is 0.753 against 1.0 on shadow
		// faces, so the reported symptom survived the fix intended to cure it;
		// on mc2_03 the vehicle peak is 0.796, below 1.0 at EVERY normal, so
		// every vehicle pixel darkened. With this on, the vehicle expression is
		// arithmetically identical to the mech expression for the same row.
		//
		// Blast radius is self-limiting for the same reason as
		// MC2_LEGACY_LIT_MC2_FRAME above: a shape that never gathers still has
		// numLights == 0, so calc_light returns vec3(1,1,1) before it reads the
		// seed (lighting.hglsl:232-236), and clamp(vec3(1),0,1) == vec3(1).
		// Only shapes carrying a real light row can observe the flip.
		//
		// MC2_VEHICLE_AMBIENT_FLOOR=0 -> u_vehicleAmbientFloor=0 -> no floor,
		// no clamp -> byte-identical to the pre-slice shipped build.
		static const bool s_vehicleAmbientFloor = mc2::gate::get<"MC2_VEHICLE_AMBIENT_FLOOR">();
		gos_SetRenderMaterialParameterInt(mat, "u_vehicleAmbientFloor",
			s_vehicleAmbientFloor ? 1 : 0);

		// [LIGHTSSBO v1] FORK-2: LightsData is now an SSBO; the UBO-
		// reflection bind below would silently no-op (SSBO blocks are not
		// in GL_ACTIVE_UNIFORM_BLOCKS) -> legacy lit meshes would render
		// garbage lighting. Bind the storage block explicitly instead.
		// SceneData stays a UBO.
		gos_SetRenderMaterialUniformBlockBindingPoint(mat, "SceneData", SCENE_DATA_ATTACHMENT_SLOT);

		// GLSTATE-SSBO-SLOT5-RESTORE-1: save prior slot 5 binding so endShadowPass
		// restores it correctly instead of hard-zeroing (NVIDIA may generate
		// GL_INVALID_OPERATION on a shader read from a zero-bound SSBO slot).
		GLuint savedSsbo5 = 0;
		if (usePbrOverride) {
			static int s_pbrRenderTrace = 0;
			if (mc2::gate::get<"MC2_BUILDING_PBR_TRACE">() && s_pbrRenderTrace < 64) {
				++s_pbrRenderTrace;
				fprintf(stderr,
					"[BUILDING_PBR_TRACE] ShapeRenderer::render vb=%p ib=%p tex=%lu normal=%lu orm=%lu ssbo=%lu light=%d\n",
					(void*)vb, (void*)ib, (unsigned long)texture_id,
					(unsigned long)renderShape->pbrNormalTexture_,
					(unsigned long)renderShape->pbrOrmTexture_,
					(unsigned long)renderShape->pbrMaterialSsbo_,
					light_index);
				fflush(stderr);
			}
			float controls[4] = {
				renderShape->pbrTileScale_,
				renderShape->pbrRoughnessBias_,
				renderShape->pbrMetallicInfluence_,
				0.0f
			};
			gos_SetRenderMaterialParameterFloat4(mat, "u_buildingPbrControls", controls);
			gos_SetRenderMaterialSamplerUnit(mat, "tex1", 0);
			gos_SetRenderMaterialSamplerUnit(mat, "u_normalTex", 1);
			gos_SetRenderMaterialSamplerUnit(mat, "u_ormTex", 2);
			gos_SetRenderState(gos_State_Texture2, renderShape->pbrNormalTexture_);
			gos_SetRenderState(gos_State_Texture3, renderShape->pbrOrmTexture_);
			GLint q5 = 0;
			glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &q5);
			savedSsbo5 = static_cast<GLuint>(q5);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, (GLuint)renderShape->pbrMaterialSsbo_);
			gos_SetRenderState(gos_State_Culling, gos_Cull_None);
		}

		gos_ApplyRenderMaterial(mat);
		gos_BindLightDataStorageBlock(mat);

		// VEHICLE-LIGHTING-PATH-1: night light-pool parity. mech.frag:399 and
		// static_prop.frag:456 both consume the pool; this legacy lit program
		// was the only object shader with no pool term and no upload, so
		// vehicles received zero contribution from building pools, turret
		// lamps and their own headlights at night. Must come after
		// gos_ApplyRenderMaterial (needs the active program).
		// Gated off (MC2_TERRAIN_LIGHT_POOLS / MC2_LIGHT_POOL_OBJECTS) this
		// force-uploads u_numPoolLights=0 -> calcLightPools returns +0.0
		// exactly -> byte-identical.
		gos_UploadObjectLightPools(mat);

		// Bind shadow maps + terrainMVP after apply() (requires active program)
		if (!isHudElement) {
			gos_SetupObjectShadows(mat);
		}

		gos_RenderIndexedArray(ib, vb, vdecl);

		if (usePbrOverride) {
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, savedSsbo5);
			gos_SetRenderState(gos_State_Texture2, 0);
			gos_SetRenderState(gos_State_Texture3, 0);
			gos_SetRenderState(gos_State_Culling, gos_Cull_CW);
		}

	}

};


//----------------------------------------------------------------------
// Draws all textures with isTerrain set that are solid first,
// then draws all alpha with isTerrain set.
void MC_TextureManager::renderLists (void)
{
	ZoneScopedN("textureManagerRenderLists");
	// [RENDERLISTS_COST v1] whole-function span; constructed first so its dtor
	// (accumulate + 60-frame emit) runs after every phase span has closed.
	rlcost::TotalSpan _rlTotal(nextAvailableVertexNode,
	                           static_cast<long>(nextAvailableHardwareVertexNode));

	// LIGHT-CONE-MODELS-1 (B): fill the deferred cone-glow vertices into the
	// MC2_ISSPOTLGT batch. Must run before the ISSPOTLGT consumer loop below
	// and AFTER every legacy addVertices for the frame (all of which happen in
	// the object render pass, before renderLists). See mclib/cone_glow.cpp.
	mc2_cone_glow::FlushFrame();
	static bool bSkip = true; // used across preamble and Render.3DObjects
#ifdef MC2_VULKAN_ISLAND
	// VK-PARITY-F1: ground vehicles (and every other TG_Shape on the CPU-transformed
	// gos_VERTEX path) are flushed by the legacy masterVertexNodes loops below (solid,
	// blob-shadow, alpha, additive effects / spotlight cones). Under the VK umbrella the
	// native VK composite is the sole present producer, so those GL draws would land in a
	// DISCARDED GL backbuffer and the objects would vanish. When the VK legacy-shape island
	// is armed, each loop queues its nodes for the island (run right after the loop group)
	// instead of drawing them here. Gate off (default GL) -> vkLegacyOn == false -> every
	// loop is byte-identical.
	const bool vkLegacyOn = (mc2_vk_legacy_shape_island_enabled() != 0);
	auto vkLegacySubmitNode = [this](long i, uint32_t pass, uint32_t alphaTest) {
		DWORD totalVertices = masterVertexNodes[i].numVertices;
		if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			totalVertices = (DWORD)(masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices);
		if (totalVertices)
		{
			Mc2LegacyMlrDraw d{};
			d.vertices = masterVertexNodes[i].vertices;
			d.vertexCount = totalVertices;
			d.texNode = (uint32_t)masterVertexNodes[i].textureIndex;
			const DWORD gosTex = tex_resolve(masterVertexNodes[i].textureIndex);
			d.gosTexHandle = (gosTex == 0xffffffffu) ? 0u : (uint32_t)gosTex;
			d.pass = pass;
			d.alphaTest = alphaTest;
			mc2_vk_legacy_shape_submit(&d);
		}
		masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
	};
	auto vkLegacyRun = [this]() {
		// fog_color_ is what gosRenderer would have latched from gos_State_Fog (set in the
		// renderLists preamble: eye->fogColor when fog is on, else 0). The additive loops
		// clear fog themselves (the island ADD pass ignores this).
		Mc2LegacyMlrFrame ff{};
		const vec4 fcv = uint32_to_vec4(useFog ? (uint32_t)eye->fogColor : 0u);
		ff.fogColor[0] = fcv.x; ff.fogColor[1] = fcv.y; ff.fogColor[2] = fcv.z; ff.fogColor[3] = fcv.w;
		if (gosPostProcess* ppVkLeg = getGosPostProcess())
			(void)mc2_vk_legacy_shape_island_run(ppVkLeg->getWidth(), ppVkLeg->getHeight(), &ff);
	};
#endif
	{
	ZoneScopedN("RenderLists.Preamble");
	rlcost::Span _rl(rlcost::kPreamble);
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	DWORD fogColor = eye->fogColor;
	//-----------------------------------------------------
	// FOG time.  Set Render state to FOG on!
	if (useFog)
	{
		//gos_SetRenderState( gos_State_Fog, (int)&fogColor);
		gos_SetRenderState( gos_State_Fog, fogColor); // sebi
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	gos_SetRenderState(gos_State_Culling, gos_Cull_CW);
	} // end RenderLists.Preamble

	{
	ZoneScopedN("RenderLists.LightDataUpload");
	rlcost::Span _rl(rlcost::kLightUpload);
    // copy global list of light data into GPU buffer

    // [LIGHTSSBO v1] Upload-size FLOOR retained (NOT a removable UBO-window
    // artifact — falsified 2026-05-17). The engine deliberately tolerates
    // transient over-count lightDataIndex for cull-stale actors whose
    // update() was skipped offscreen (see gos_static_prop_registry.cpp
    // comment "...points at a slot ... beyond the upload count"). The
    // floor guarantees those indices still read valid backing memory
    // instead of past-end (-> zero -> black props). std::max keeps the
    // old max(count, 64) semantics; lightData_ is capacity(128)-sized so
    // sourcing 64 entries is in-bounds.
    // [LIGHTSSBO v2] kLightUploadFloor=64 is PRESERVED: totalBytes stays
    // max(count,64)*stride. When count<64 the padded tail [count..64) lands in
    // the per-frame SUFFIX [prefixBytes..totalBytes) and is uploaded every frame,
    // so the deliberate tolerance of transient over-count lightDataIndex for
    // cull-stale offscreen actors still reads valid backing memory — identical
    // in-bounds guarantee to the legacy whole-buffer upload. The split only
    // changes WHICH bytes re-upload each frame, never the uploaded extent.
    constexpr uint32_t kLightUploadFloor = 64u;
    const size_t lightUploadCount =
        std::max<uint32_t>(lightDataStructuresCount, kLightUploadFloor);
    const size_t totalBytes = lightUploadCount * sizeof(TG_HWLightsData);
    // mc2StaticLightHighWater / mc2ConsumeStaticLightPrefixDirty / the split gate
    // are all file-scope fns defined earlier in this TU — call directly.
    if (mc2StaticLightUploadSplitEnabled() && mc2LightBakeEnabled()) {
        // Prefix = permanent static table [0..S), clamped to the floored count.
        const uint32_t S = mc2StaticLightHighWater();
        const size_t prefixCount = (S < lightUploadCount) ? S : lightUploadCount;
        const size_t prefixBytes = prefixCount * sizeof(TG_HWLightsData);
        const bool prefixDirty = mc2ConsumeStaticLightPrefixDirty();
        gos_LightDataSsbo_UploadSplit(lightData_, prefixBytes, totalBytes, prefixDirty);
    } else {
        gos_LightDataSsbo_Upload(lightData_, totalBytes);   // legacy whole-buffer
    }
	} // end RenderLists.LightDataUpload
    //

    // update scene data uniform buffer
    Stuff::Vector3D cp = eye->getCameraOrigin();
    {
        ZoneScopedN("Camera.SceneDataUpload");
        rlcost::Span _rl(rlcost::kSceneData);
        sceneData_->fog_start = eye->fogStart;
        sceneData_->fog_end = eye->fogFull;
        sceneData_->min_haze_dist = Camera::MinHazeDistance;
        sceneData_->dist_factor = Camera::DistanceFactor;
        sceneData_->cam_pos[0] = cp.x;
        sceneData_->cam_pos[1] = cp.y;
        sceneData_->cam_pos[2] = cp.z;
        sceneData_->cam_pos[3] = 1.0f;
        vec4 fc = uint32_to_vec4(eye->fogColor);
        sceneData_->fog_color[0] = fc.z;
        sceneData_->fog_color[1] = fc.y;
        sceneData_->fog_color[2] = fc.x;
        sceneData_->fog_color[3] = fc.w;
#ifdef MC2_VULKAN_ISLAND
        // VK-MECH-MATERIAL-LIGHTING-1: cache the RGB fog color the mech VS reads as
        // g_scene.fogColor.rgb so the VK mech island can feed the same value into its
        // per-frame UBO (parity with mech.frag's fog mix). Snapshotted at the SAME site
        // GL uploads the scene UBO — no phase drift. Accessor: mc2GetSceneFogColorRGB().
        extern void mc2SetSceneFogColorRGB(float, float, float);
        mc2SetSceneFogColorRGB(sceneData_->fog_color[0], sceneData_->fog_color[1], sceneData_->fog_color[2]);
#endif
        sceneData_->baseVertexColor = uint32_to_vec4(BaseVertexColor).zyxw();
        gos_UpdateBuffer(sceneDataBuffer_, sceneData_, 0, sizeof(TG_HWSceneData));
    }
    
    

	{
		ZoneScopedN("Render.3DObjects");
		TracyGpuZone("Render.3DObjects");
		rlcost::Span _rl(rlcost::kObj3d);
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Obj3d);
		// SAME-ORDER-EXECUTOR-SLICE-2: validate-only ownership of MechOpaque top-level
		// pass. Body sets its own state (no apply); this just validates ambient+FBO.
		render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::OpaqueObject,
		                                          "renderLists_Render.3DObjects");
	for (size_t i = 0; i<nextAvailableHardwareVertexNode; i++)
	{
		if ((masterHardwareVertexNodes[i].flags & MC2_DRAWSOLID) &&
			(masterHardwareVertexNodes[i].shapes))
		{
			gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState(gos_State_AlphaTest,
				(masterHardwareVertexNodes[i].flags & MC2_ALPHATEST) ? 1 : 0);
			if (masterHardwareVertexNodes[i].flags & MC2_ISTERRAIN)
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
			else
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureWrap);

			uint32_t totalShapes = masterHardwareVertexNodes[i].numShapes;
			// in case less shapes were addded in Render() that it was "promised" in Update(), generally etter to investigate and remove all such cases
			if (masterHardwareVertexNodes[i].currentShape != (masterHardwareVertexNodes[i].shapes + masterHardwareVertexNodes[i].numShapes))
			{
				totalShapes = masterHardwareVertexNodes[i].currentShape - masterHardwareVertexNodes[i].shapes;
			}
			for (uint32_t sh = 0; sh < totalShapes; ++sh)
			{
				DWORD textureIndex = masterHardwareVertexNodes[i].textureIndex;
				if (textureIndex == 1227 && bSkip)
					continue;

				static bool b_old_way = false;
				if (b_old_way)
				{
					gos_SetRenderState(gos_State_Texture, masterTextureNodes[textureIndex].get_gosTextureHandle());
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;
					gos_SetRenderViewport(rs->viewport_[2], rs->viewport_[3], rs->viewport_[0], rs->viewport_[1]);


					//gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
					// TODO: set mvp_ in a separate function, like gos_set_render_camera(mvp_)...
					gos_RenderIndexedArray(rs->ib_, rs->vb_, rs->vdecl_, (const float*)rs->mvp_);
				}
				else
				{
					DWORD texture = masterTextureNodes[textureIndex].get_gosTextureHandle();
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;


					mat4 view_mat = gos2my(TG_Shape::s_worldToCamera);
					mat4 world_mat = gos2my(rs->mw_);
					mat4 wvp_mat = gos2my(rs->mvp_);

					ShapeRenderer shape_renderer;
					shape_renderer.setup(&world_mat, &view_mat, &wvp_mat, rs->viewport_);
					shape_renderer.render(rs->vb_, rs->ib_, rs->vdecl_, texture, rs->light_data_buffer_index_, rs->isHudElement_, rs);
				}

			}

			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterHardwareVertexNodes[i].currentShape = masterHardwareVertexNodes[i].shapes;
			//masterHardwareVertexNodes[i].numShapes = 0;
		}
	}
	gos_SetRenderState(gos_State_AlphaTest, 0);
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Obj3d);
	} // end Render.3DObjects zone
	drainGLErrors("objects_3d");

	{
	ZoneScopedN("RenderLists.PostObjectsStateRestore");
	rlcost::Span _rl(rlcost::kStateRestore);
	// [Moved in Phase 4 debug] flush() was originally here (after
	// Render.3DObjects). But Render.TerrainSolid runs AFTER us on line
	// ~1287, so terrain was overwriting our building pixels. Flush is
	// now relocated further down, after Render.TerrainSolid completes.

	// restore state as all old-style geometry is culled on CPU and all vertices are already pretransformed
	gos_SetRenderState(gos_State_Culling, gos_Cull_None);

	// restore viewport
	gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
	} // end RenderLists.PostObjectsStateRestore

	// VPL-#shadow Phase 1+2 (arch-doc docs/plans/static-terrain-shadow-
	// architecture.md): build the static terrain shadow from the FULL map
	// ONCE. Was: a prime + a camera-windowed accumulate behind a >100u
	// camera-move gate -> the shadow FBO was fed only the ~110 visible
	// terrain nodes and never cleared -> a near-empty depth atlas ->
	// soft half-map shadow wash. Root cause is feed-scope and was
	// probe-proven: the world-fixed ortho light matrix is correct and
	// built-once ([SHADOWFRUSTUM v1] n=1 mapHalfExtent=6400
	// orthoHalf=9503.5; build & sample share getLightSpaceMatrix()).
	// Phase 1 retires the prime block, the camera-motion gate, and the
	// gos_*ShadowRebuild* API (this was their only caller). The build is
	// gated solely by the gos_StaticLightMatrixBuilt() latch, which
	// Terrain::destroy re-arms per mission (C-1) so mission 2+ rebuilds
	// against fresh blocks[]. The MapData full-map feed is stock-safe
	// (no-ops if blocks[] unallocated -> shadow simply absent, never a
	// crash, never worse than a missing shadow).
	// ISSUE-34: keyed on the modern shadow path (shadow_terrain material),
	// not tessellation — static shadow map still builds when tess is off.
	if (gos_IsModernShadowPathActive() && !gos_StaticLightMatrixBuilt() &&
	    Terrain::mapData) {
		ZoneScopedN("Shadow.StaticFullMapBuild");
		TracyGpuZone("Shadow.StaticFullMapBuild");
		rlcost::Span _rl(rlcost::kStaticShadowBuild);
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_ShadowStatic);

		gos_BuildStaticLightMatrix();   // world-fixed, camera-independent
		gos_MarkStaticLightMatrixBuilt();

		// Any valid terrain colormap: the shadow prepass is depth-only
		// (shadow_terrain.tese = plain lightSpaceMatrix*worldPos, not
		// sampled for depth), so the exact texture is irrelevant -- bind
		// the first terrain node's, else the solid default (idx 0).
		unsigned long shTex = masterTextureNodes[0].get_gosTextureHandle();
		for (long si = 0; si < nextAvailableVertexNode; si++) {
			if ((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
				(masterVertexNodes[si].flags & MC2_ISTERRAIN)) {
				shTex = masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle();
				break;
			}
		}

		gos_BeginShadowPrePass(true);   // one-shot clear (no accumulate)
		Terrain::mapData->renderStaticTerrainShadowFullMap(indexArray, shTex);
		gos_EndShadowPrePass();

		// SHADOW-STATIC-BUILDINGS-2: append rigid BUILDING casters to the world-
		// fixed static shadow map, ONCE (same one-shot build block as terrain).
		// Source = the FULL registry (all buildings, visibility-independent, baked
		// at mission-load registerStatic), NOT per-frame visible buckets (Option B's
		// failure). Trees excluded by the Building population filter. Appends to the
		// terrain depth (gos_BeginShadowPrePass(false) = no clear). Gate
		// MC2_STATIC_PROP_BUILDING_SHADOW=1 (default OFF). Relies on C-pre min-combine
		// (7ea32b83) so a building in both this and the dynamic bounded-near map does
		// not double-darken terrain. Only runs when the prepass actually activates.
		{
			static const bool s_bldgStaticShadow = !mc2::gate::is<"MC2_STATIC_PROP_BUILDING_SHADOW", "0">();
			if (s_bldgStaticShadow) {
				const bool s_sbTxmTrace = mc2::gate::is<"MC2_STATIC_PROP_BUILDING_SHADOW", "2">();
				if (gos_BeginShadowPrePass(false)) {  // append, no clear
					// Local (not static): runs once per mission; getBuildingShadowInstances
					// clears+fills it. No need to retain capacity for the process lifetime.
					std::vector<GpuStaticPropInstance> bldgShadowInstances;
					GpuStaticPropRegistry::getBuildingShadowInstances(bldgShadowInstances);
					if (s_sbTxmTrace) {
						fprintf(stderr, "[SHADOW_STATIC_BLDG_TXM] prepass=1 instances=%zu\n",
							bldgShadowInstances.size());
						fflush(stderr);
					}
					GpuStaticPropBatcher::instance().drawStaticBuildingShadows(bldgShadowInstances);
					gos_EndShadowPrePass();
				} else if (s_sbTxmTrace) {
					fprintf(stderr, "[SHADOW_STATIC_BLDG_TXM] prepass=0 (shadows off, skipped)\n");
					fflush(stderr);
				}
			}
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_ShadowStatic);
	}
	// STATIC-PROP SHADOW-ORDER FIX (2026-05-29): inject static-registry instances
	// into batcher buckets BEFORE flushShadow() below. flushShadow() calls
	// uploadAllBucketsIfNeeded(), which uploads s_bucketsByType and LOCKS the
	// per-frame SSBO slot (s_lastUploadedSlot=s_frameSlot). If the registry flush
	// ran only at its old site (after this shadow block, before the main flush()),
	// terrain-object instances (trees/fences/prop-buildings) arrived AFTER the slot
	// was locked; the idempotency guard then skipped re-upload, so their typeIDs
	// were absent from s_typeRanges -> zero instances -> INVISIBLE whenever
	// MC2_SHADOW_ENABLE=1. (True buildings submit via submitMultiShape during
	// Render.3DObjects, so they were already in the buckets and unaffected.)
	// Running the registry flush here makes the single shadow-time upload include
	// terrain objects (they now also cast static shadows). Gate mirrors the
	// Render.GpuStaticProps block; runs regardless of tessellation/mech state.
	{
		ZoneScopedN("RenderLists.StaticPropRegistryFlush");
		rlcost::Span _rl(rlcost::kSpRegistryFlush);
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
		if (g_useGpuStaticProps || g_useGpuObjects) {
			// GPU-ORDERING-PROBE-1: glFinish() before static-prop flush to confirm
			// whether the flash is a pipeline ordering / missing-barrier issue.
			// If glFinish() here stops the flash → cause is a GPU sync gap introduced
			// by d65552ab's new GL_SHADER_STORAGE_BARRIER_BIT calls changing AMD
			// pipeline scheduling for s_indirectCmdBuf visibility on camera-move frames.
			// Kill this probe once root cause is identified.
			static const bool s_finishProbe = mc2::gate::get<"MC2_FLUSH_PROBE">();
			if (s_finishProbe) glFinish();
			GpuStaticPropRegistry::flush();
		}
	} // end RenderLists.StaticPropRegistryFlush
	// GPU-driven dynamic sun shadow (Phase 1): frustum-fit + flushShadow.
	// Runs BEFORE gpu_cull::compute_dispatch so the static-prop shadow uses the
	// full camera-visible per-type ranges (not the cull-narrowed indirect).
	// Casters = the camera-visible (inView) batched set (Phase 1 scope; the
	// off-screen-caster low-sun shadow is the documented Phase-2 gap).
	{
		ZoneScopedN("RenderLists.DynamicShadowPass");
		rlcost::Span _rl(rlcost::kDynShadow);
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_ShadowDyn);
		extern bool g_useGpuObjects;
		extern bool g_useGpuMechs;
		// ISSUE-34: modern-shadow-path predicate (was tess-keyed) — dynamic
		// shadow pass stays on when tessellation is off.
		if (gos_IsModernShadowPathActive() && (g_useGpuObjects || g_useGpuMechs)) {
			// Unproject 8 GL-NDC corners through the inverse of worldToClipGL().
			// SHADOW-DYNAMIC-PROJECTION-FIX-1: worldToClipGL() maps MC2 world (Z-up)
			//   = kAxisSwapMC2toGL * worldToCam * cameraToClipGL
			// -> GL clip (NDC xy[-1,1], z[0,1], w>0 in front), with the MC2->GL axis
			// swap (x'=-x, y'=z, z'=y) ALREADY BAKED IN. So inverting it maps a GL-NDC
			// corner straight back to MC2 world: NO manual (-x,z,y) swizzle and NO w<0
			// negate (this matrix emits positive-w-in-front).
			//
			// PRIOR BUG: the code inverted getWorldToClip() (D3D pixel-homogeneous,
			// Y-down, w<0 in front) but fed it the GL-NDC cube below. GL-NDC and that
			// native clip differ by kPixelHomogToGLNDC (Y-flip + x offset + w sign-flip,
			// camera.cpp:142-156), never applied -> the unprojected box was mirrored/
			// offset, only ~correct when the frustum was axis-aligned/centered. That is
			// why trees shadowed only at certain camera angles.
			static const float ndc[8][3] = {
				{-1.0f,-1.0f, 0.0f},{ 1.0f,-1.0f, 0.0f},
				{-1.0f, 1.0f, 0.0f},{ 1.0f, 1.0f, 0.0f},
				{-1.0f,-1.0f, 1.0f},{ 1.0f,-1.0f, 1.0f},
				{-1.0f, 1.0f, 1.0f},{ 1.0f, 1.0f, 1.0f}
			};
			float cornersMC2[8][3];
			// Invert(src) stores the inverse of src into *this (matrix.hpp:584).
			Stuff::Matrix4D clipToWorldGL;
			clipToWorldGL.Invert(eye->worldToClipGL());
			for (int c = 0; c < 8; ++c) {
				Stuff::Vector4D in, out;
				in.x = ndc[c][0]; in.y = ndc[c][1]; in.z = ndc[c][2]; in.w = 1.0f;
				out.Multiply(in, clipToWorldGL);      // row-vector * matrix; result is MC2-world homogeneous
				float inv = (fabsf(out.w) > 1e-6f) ? (1.0f / out.w) : 0.0f;
				cornersMC2[c][0] = out.x * inv;       // already MC2 (axis swap baked into worldToClipGL)
				cornersMC2[c][1] = out.y * inv;
				cornersMC2[c][2] = out.z * inv;
			}
			float lx, ly, lz;
			gos_GetTerrainLightDir(&lx, &ly, &lz);   // same accessor used by old shim
			// Focus source = the camera ORBIT TARGET (getPosition), not a
			// screen-center terrain raycast. The orbit target is yaw-invariant
			// MC2-world east/north (.x=east WU, .y=north WU; yaw is applied only
			// to the eye direction, never to position) and moves linearly with
			// panning, so the cascade no longer skews with the horizon or with
			// facing. Z = cameraShiftZ (terrain elevation at the target, already
			// clamped up to waterElevation), giving the correct water-clamped
			// look-at height in the same MC2 world space as cornersMC2
			// (x=east,y=north,z=elev). Always valid -- no raycast fallback needed.
			Stuff::Vector3D camTgt = eye->getPosition();        // MC2-world orbit target (east,north)
			float shadowCenterXYZ[3] = { camTgt.x, camTgt.y, eye->getCameraShiftZ() };
			bool scOk = true;                                   // camera target always valid
			gos_BuildDynamicLightMatrix(-lx, -ly, -lz, cornersMC2,
			                            shadowCenterXYZ, scOk);  // sign matches old shim
			// SAME-ORDER-EXECUTOR-SLICE-2: validate-only ownership of the Shadow top-level
			// pass (dynamic shadow only — static is once/mission, not wrapped here).
			// Body sets its own state (FBO bind, depth, lightSpaceMatrix); we just validate.
			render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                          "renderLists_gos_BeginDynamicShadowPass");
			gos_BeginDynamicShadowPass();             // no-op if shadowsEnabled_ false
#ifdef MC2_VULKAN_ISLAND
			// VK-P8a SHADOW-INTEROP: clear any stale VK-produced shadow tex at the top of
			// the bracket so a frame where the island declines/isn't reached (e.g. the
			// non-prop-caster flushShadow branch) never leaves a stale VK tex bound for the
			// forward consumers. The producer below re-publishes it on success. No-op when
			// the gate is OFF (the tex is already 0 and stays 0).
			if (mc2_vk_shadow_island_enabled()) {
				if (gosPostProcess* ppClr = getGosPostProcess())
					ppClr->setDynamicShadowInteropTexture(0);
			}
#endif
			// Item 1 P1: caster set used by the CSM cascade replay (set in the
			// prop-caster path below; nullptr => CSM replay uses flushShadow).
			const std::vector<GpuStaticPropInstance>* csmCasterSet = nullptr;
			// SHADOW-STATIC-BUILDINGS-2: when buildings cast via the world-fixed
			// static map, skip them in the dynamic pass to avoid a redundant fuzzy
			// double-shadow on buildings (trees/mechs still cast dynamically).
			static const bool s_skipBldgInDynamic = !mc2::gate::is<"MC2_STATIC_PROP_BUILDING_SHADOW", "0">();
			// SHADOW-DYNAMIC-PROP-CASTERS-1 (gate, DEFAULT ON; =0 kill-switch): the camera-visible
			// flushShadow feed (s_typeRanges) only admits props the camera frustum
			// cull marked visible this frame, so most trees never cast into the (now
			// correctly camera-fit) dynamic map -- only the few nearest the camera.
			// When enabled, replay ALL registered NON-building props from the
			// registry (visibility-independent) into the dynamic map instead, so
			// every tree/prop in the light box casts. Buildings keep casting via the
			// world-fixed static map, so this pairs with MC2_STATIC_PROP_BUILDING_SHADOW
			// (with that OFF, buildings get no dynamic shadow -- intended for the
			// combined config). flushShadow is skipped entirely in this mode (it would
			// only double-draw the camera-visible subset). Mechs still cast via their
			// own batcher. Gate OFF -> byte-identical to prior behavior.
			static const bool s_dynPropCasters =
				!mc2::gate::is<"MC2_SHADOW_DYNAMIC_PROP_CASTERS", "0">();  // DEFAULT ON; =0 kill-switch
			if (s_dynPropCasters) {
				static std::vector<GpuStaticPropInstance> s_dynPropInsts; // reused; accessor clears
				// Include buildings in the dynamic feed ONLY when the static building
					// map is NOT active (else buildings would double-shadow). This keeps
					// buildings casting under bare MC2_SHADOW_ENABLE (no regression).
					const bool includeBldg = !s_skipBldgInDynamic;
					// SHADOW-DYNAMIC-PROP-DIRTY-ONLY-1 (gate, DEFAULT OFF; set env to enable):
					// The caster set (registry-indexed instances: modelMatrix + typeID) is
					// REGISTRY-STATIC — it changes only when a prop spawns, despawns, or has
					// an immutable-field write, all of which bump s_registryGeneration. The
					// shadow pass uses only modelMatrix+typeID from the instances (no lighting
					// data, no per-frame color offset), so the vector is safe to cache across
					// frames as long as the generation is unchanged.
					// Cache policy: rebuild when (a) generation changes, or (b) includeBldg
					// toggles (env-gated at startup — in practice stable per session, but
					// compare it anyway). On a clean scene getDynamicPropShadowInstances walks
					// the full s_recipeRanges vector (~14K recipes); skipping it saves ~120-
					// 150µs of cache-cold memory traffic per frame.
					// NOTE: includeBldg is captured from a static at the outer scope, so it
					// is also effectively constant per session; the extra compare is free.
					// DEFAULT-ON since 2026-06-04 (smoke-clean mc2_24; proven
					// s_registryGeneration dirty-only pattern, shadow pass reads only
					// modelMatrix+typeID): a value starting with "0" opts out.
					static const bool s_dynPropDirtyOnly = mc2::gate::get<"MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY">();
					static uint64_t  s_dynPropInstsGeneration = UINT64_MAX; // sentinel: force first build
					static bool      s_dynPropInstsIncludeBldg = false;
					if (!s_dynPropDirtyOnly ||
					    GpuStaticPropRegistry::getRegistryGeneration() != s_dynPropInstsGeneration ||
					    includeBldg != s_dynPropInstsIncludeBldg) {
						GpuStaticPropRegistry::getDynamicPropShadowInstances(s_dynPropInsts, includeBldg);
						s_dynPropInstsGeneration  = GpuStaticPropRegistry::getRegistryGeneration();
						s_dynPropInstsIncludeBldg = includeBldg;
					}

					// SHADOW-CASTER-LIGHTBOX-CULL-1 (gate MC2_SHADOW_CASTER_LIGHTBOX_CULL,
					// DEFAULT OFF). The caster set above is the WHOLE registry (~14K props,
					// visibility-independent). The dynamic shadow map is camera-fit (small,
					// esp. under MC2_SHADOW_BOUNDED_NEAR_FIT), so thousands of off-map props
					// draw shadows that never land in the map = wasted GPU fill. When ON,
					// filter the casters to only those whose origin projects inside the
					// dynamic shadow frustum (+margin), using the EXACT same matrix the
					// shadow VS uses (getDynamicLightSpaceMatrix(), column-major, GL_FALSE).
					//
					// World-position frame: shaders/shadow_static_prop.vert computes
					//   worldStuff = vec4(a_position,1) * modelMatrix   (row-vector * M)
					//   worldMC2   = vec3(-worldStuff.x, worldStuff.z, worldStuff.y)
					//   gl_Position = lightSpaceMatrix * vec4(worldMC2,1)
					// For the local origin a_position=(0,0,0): worldStuff = (M[3],M[7],M[11])
					// in the column-major float[16] (v*M picks element c*4+3), so
					//   worldMC2 = (-M[3], M[11], M[7])
					// This is EXACTLY the registry's actorWorldCenter extraction
					// (gos_static_prop_registry.cpp:842-847) and the frame the light matrix
					// consumes — verified against both the VS and the SHADOWZRANGE probe.
					// DEFAULT ON since 2026-06-16 (render-hygiene-s1). Off-map props
					// never land their shadows on the playfield — filtering them saves
					// ~0.2-0.4ms GPU fill on prop-heavy missions (mc2_24). Kill: =0.
					static const bool s_casterLightboxCull = mc2::gate::get<"MC2_SHADOW_CASTER_LIGHTBOX_CULL">();
					const std::vector<GpuStaticPropInstance>* dynShadowSet = &s_dynPropInsts;
					if (s_casterLightboxCull) {
						ZoneScopedN("Shadow.CasterCull");
						static std::vector<GpuStaticPropInstance> s_culledDynPropInsts; // reused, no churn
						// SHADOW-CASTER-CULL-CACHE-1 (TXMMGR-PERF-EASYWINS-1, gate
						// MC2_SHADOW_CASTER_CULL_CACHE, DEFAULT OFF): the cull result is a
						// pure function of (caster set, light-space matrix, margin). The
						// caster set is generation-keyed (rebuilt above only on
						// s_registryGeneration change) and the matrix is camera-fit — on a
						// stationary camera its 16 floats are bit-identical across frames.
						// Reuse the previous culled vector when BOTH are unchanged; any
						// camera motion or registry mutation recomputes exactly as before.
						// margin + includeBldg are session-static (env-init'd) — no key part.
						static const bool s_cullCacheEnabled = mc2::gate::get<"MC2_SHADOW_CASTER_CULL_CACHE">();
						static uint64_t s_cullCacheGen = UINT64_MAX;   // sentinel: no cache
						static float    s_cullCacheM[16] = {};
						static const float s_cullMargin = mc2::gate::get<"MC2_SHADOW_CASTER_CULL_MARGIN">();
						static const bool s_cullDebug = mc2::gate::get<"MC2_SHADOW_CULL_DEBUG">();
						gosPostProcess* ppCull = getGosPostProcess();
						const float* M = ppCull ? ppCull->getDynamicLightSpaceMatrix() : nullptr;
						if (!M) {
							// No matrix yet (early frames) -> keep everything (safe).
							dynShadowSet = &s_dynPropInsts;
						} else if (s_cullCacheEnabled &&
						           s_cullCacheGen == GpuStaticPropRegistry::getRegistryGeneration() &&
						           0 == memcmp(s_cullCacheM, M, sizeof(s_cullCacheM))) {
							// Cache HIT: same caster generation + bit-identical light matrix
							// -> the cull result is unchanged; reuse the retained vector.
							dynShadowSet = &s_culledDynPropInsts;
						} else {
							s_culledDynPropInsts.clear();
							s_culledDynPropInsts.reserve(s_dynPropInsts.size());
							const float lim = 1.0f + s_cullMargin;
							int dbgN = 0;
							for (const GpuStaticPropInstance& inst : s_dynPropInsts) {
								const float* mm = inst.modelMatrix;
								// world (MC2 frame the light matrix consumes)
								const float wx = -mm[3];
								const float wy =  mm[11];
								const float wz =  mm[7];
								// clip = M * (wx,wy,wz,1), M column-major: (row r,col c)=M[c*4+r]
								const float cx = M[0*4+0]*wx + M[1*4+0]*wy + M[2*4+0]*wz + M[3*4+0];
								const float cy = M[0*4+1]*wx + M[1*4+1]*wy + M[2*4+1]*wz + M[3*4+1];
								const float cw = M[0*4+3]*wx + M[1*4+3]*wy + M[2*4+3]*wz + M[3*4+3];
								bool keep = true;
								if (cw > 0.0f) {
									const float ndcx = cx / cw;
									const float ndcy = cy / cw;
									keep = (ndcx >= -lim && ndcx <= lim &&
									        ndcy >= -lim && ndcy <= lim);
								} else {
									// behind the light near plane: cannot land in the map.
									keep = false;
								}
								if (s_cullDebug && dbgN < 8) {
									++dbgN;
									const float ndcx = (cw != 0.0f) ? cx / cw : 0.0f;
									const float ndcy = (cw != 0.0f) ? cy / cw : 0.0f;
									fprintf(stderr,
										"[SHADOW_CULL_DBG] world=(%.1f,%.1f,%.1f) "
										"clipW=%.3f ndc=(%.3f,%.3f) keep=%d\n",
										wx, wy, wz, cw, ndcx, ndcy, keep ? 1 : 0);
									fflush(stderr);
								}
								if (keep) s_culledDynPropInsts.push_back(inst);
							}
							dynShadowSet = &s_culledDynPropInsts;
							if (s_cullCacheEnabled) {
								s_cullCacheGen = GpuStaticPropRegistry::getRegistryGeneration();
								memcpy(s_cullCacheM, M, sizeof(s_cullCacheM));
							}

							static bool s_cullLogged = false;
							if (!s_cullLogged) {
								s_cullLogged = true;
								const size_t total = s_dynPropInsts.size();
								const size_t kept  = s_culledDynPropInsts.size();
								const float pct = total ? (100.0f * (float)(total - kept) / (float)total) : 0.0f;
								fprintf(stderr,
									"[SHADOW_CULL] casters %zu -> %zu (%.0f%% culled, margin=%.2f)\n",
									total, kept, pct, s_cullMargin);
								fflush(stderr);
							}
						}
					}
				{
				// LANE-D measure-first: give the dynamic prop shadow CASTER draw its
				// own GPU timestamp so its cost is deconflated from GpuSP.BatcherFlush
				// (which previously absorbed it as first-GPU-zone self-time).
				ZoneScopedN("RenderLists.DynShadowDraw");
				TracyGpuZone("GpuSP.DynShadowDraw");
				GpuStaticPropBatcher::instance().drawDynamicPropShadows(*dynShadowSet);
			}
#ifdef MC2_VULKAN_ISLAND
				// VK-P8a SHADOW-INTEROP (gate MC2_VK_SHADOW_INTEROP, default-OFF): the SECOND
				// VK geometry region and the FIRST VK-PRODUCES-DEPTH region. VK re-renders the
				// SAME dynShadowSet caster instances into an exportable D32 shadow map on the
				// on-demand shared engine VulkanContext, signals vk2gl, and hands GL the imported
				// GL-texture view of that D32. GL then binds it in place of getDynamicShadowTexture()
				// so the four forward-shading consumers sample the VK-produced map via the UNCHANGED
				// hardware sampler2DShadow calcShadow() path. Ordering: VK produce -> VK signal vk2gl
				// -> GL wait (inside the island) -> here we publish the tex -> GL consumes at forward
				// shading. The GL caster draw above still ran (it fills the engine-owned depth tex);
				// its output is simply unused this frame when VK succeeds. NO GL->VK acquire is issued
				// (VK never reads a GL-written depth) -- that direction poisons the AMD interop
				// timeline (scene blackout). Gate OFF -> enabled()==0 -> interop tex stays 0 ->
				// getDynamicShadowTexture() returns the GL tex -> byte-identical.
				if (mc2_vk_shadow_island_enabled()) {
					ZoneScopedN("RenderLists.VkShadowInterop");
					gosPostProcess* ppVk = getGosPostProcess();
					if (ppVk) {
						const unsigned int vkTex = mc2_vk_shadow_island_run(
							ppVk->getDynamicShadowMapSize(),
							ppVk->getDynamicLightSpaceMatrix(),
							dynShadowSet->data(),
							(unsigned int)dynShadowSet->size(),
							ppVk->getShadowBiasFactor(),
							ppVk->getShadowBiasUnits());
						// >0 => VK produced+signalled this frame; publish the shared D32's GL view so
						// forward consumers sample it. 0 => island declined (fail-soft) -> leave the
						// interop tex cleared so consumers use the GL-produced map (no regression).
						ppVk->setDynamicShadowInteropTexture((GLuint)vkTex);
					}
				}
#endif
				csmCasterSet = dynShadowSet;   // Item 1 P1: remember for the cascade replay
			} else {
				GpuStaticPropBatcher::instance().flushShadow(s_skipBldgInDynamic);
			}
			GpuMechBatcher::instance().flushShadow();
			gos_EndDynamicShadowPass();
			render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::ShadowCaster,
			                                        "renderLists_gos_EndDynamicShadowPass");

			// Item 1 P1: replay casters into each CSM array layer. The legacy
			// single-map pass above is fully complete (props+mech+EndPass restored
			// state). Each beginDynamicShadowCascade binds the array layer + sets
			// the active cascade matrix (via getDynamicLightSpaceMatrix resolving to
			// the active cascade) and clears forward-Z. Skipped entirely (no GL) and
			// byte-identical when MC2_SHADOW_CSM is OFF. csmCasterSet is non-null in
			// the prop-caster path; otherwise we fall back to flushShadow per layer.
			if (mc2ShadowCsmEnabled()) {
				gosPostProcess* ppCsm = getGosPostProcess();
				if (ppCsm && ppCsm->getDynamicShadowArrayTexture()) {
					ZoneScopedN("RenderLists.DynShadowCSM");
					const int csmN = ppCsm->getDynamicShadowCascadeCount();
					for (int ci = 0; ci < csmN; ++ci) {
						if (!ppCsm->beginDynamicShadowCascade(ci)) continue;
						if (csmCasterSet)
							GpuStaticPropBatcher::instance().drawDynamicPropShadows(*csmCasterSet);
						else
							GpuStaticPropBatcher::instance().flushShadow(s_skipBldgInDynamic);
						GpuMechBatcher::instance().flushShadow();
						ppCsm->endDynamicShadowCascadePass();
#ifdef MC2_VULKAN_ISLAND
						if (ci < csmN - 1 && csmCasterSet && mc2_vk_shadow_island_enabled()) {
							const float* mats = ppCsm->getDynamicCascadeMatrices();
							const float* layerMat = mats ? (mats + (size_t)ci * 16u) : nullptr;
							mc2_vk_shadow_island_run_csm_array_layer(
								ppCsm->getDynamicShadowMapSize(),
								csmN - 1,
								ci,
								layerMat,
								csmCasterSet->data(),
								(unsigned int)csmCasterSet->size(),
								ppCsm->getShadowBiasFactor(),
								ppCsm->getShadowBiasUnits());
						}
						if (ci == csmN - 1 && csmCasterSet && mc2_vk_shadow_island_enabled()) {
							const float* mats = ppCsm->getDynamicCascadeMatrices();
							const float* lastMat = mats ? (mats + (size_t)ci * 16u) : nullptr;
							mc2_vk_shadow_island_run_csm_fullmap(
								ppCsm->getDynamicFullMapSize(),
								lastMat,
								csmCasterSet->data(),
								(unsigned int)csmCasterSet->size(),
								ppCsm->getShadowBiasFactor(),
								ppCsm->getShadowBiasUnits());
						}
#endif
					}
					// P5 headless log: layers + prop caster-set size fed to each
					// cascade (per-cascade containment is decided GPU-side in the
					// shader; this proves the array pass ran with casters).
					static bool s_csmLogged = false;
					if (!s_csmLogged) {
						s_csmLogged = true;
						const size_t nCasters = csmCasterSet ? csmCasterSet->size() : 0;
						fprintf(stderr, "[CSM] layers=%d casters_per_layer=%zu\n",
						        csmN, nCasters);
						fflush(stderr);
					}
#ifdef MC2_VULKAN_ISLAND
					// MC2_VK_SHADOW_MATRIX_DIAG (Trace, default OFF = one cached-bool test).
					if (mc2_vk_shadow_matrix_diag_enabled()) {
						mc2_vk_shadow_matrix_diag_tick(
							ppCsm->getDynamicLightSpaceMatrix(), ppCsm->getDynamicShadowMapSize(),
							ppCsm->getDynamicShadowTextureRaw(),
							ppCsm->getDynamicCascadeMatrices(), csmN, ppCsm->getDynamicShadowMapSize(),
							ppCsm->getDynamicShadowArrayTexture(), ppCsm->getDynamicFullMapTexture(),
							ppCsm->getDynamicFullMapSize(), ppCsm->getDynamicFullMapTexelWorld());
					}
#endif
				}
			}
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_ShadowDyn);
	}
	g_numShadowShapes.store(0, std::memory_order_relaxed);

	// No special depth state for DRAWSOLID terrain

	{
		ZoneScopedN("Render.TerrainSolid");
	rlcost::Span _rl(rlcost::kTerrainSolid);
		TracyGpuZone("Render.TerrainSolid");
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_TerrainSolid);

		// MC2_LOAD_RACE_TRACE: terrain GPU-residency probe. The degraded
		// mission-load state renders terrain flat/untextured; this latches the
		// first frame on which terrain is fully GPU-resident. Two independent
		// readiness sources, selected by which renderer is actually active
		// (mc2TerrainLodChunkEnabled(), cached static bool -- cheap read, no walk):
		//   - Default chunk/GPU-indirect path: masterVertexNodes[] is NEVER
		//     populated with terrain nodes here (the legacy per-quad walk below
		//     is suppressed on this path), so the old terrTotal>0 walk always
		//     false-negatived (terrain_resident=N on every run, even healthy
		//     ones). Reuse the SAME cached latches gos_terrain_indirect.cpp
		//     already sets for its own "[LOADRACE] atlas first-resident" /
		//     "[LOADRACE] gpu terrain first-arm" one-shot lines -- two long
		//     compares, no new walk.
		//   - Legacy per-quad path (MC2_TERRAIN_LOD_CHUNK=0): unchanged
		//     masterVertexNodes walk (non-mutating isTextureNodeResident, so it
		//     never forces an upload).
		// Also refreshes g_loadRaceTerrainResidentNow each frame for the verdict.
		{
			extern bool     g_loadRaceTrace;
			extern long     g_loadRaceTerrainFirstResidentFrame;
			extern bool     g_loadRaceTerrainResidentNow;
			extern uint32_t g_mc2FrameCounter;
			extern long     g_loadRaceAtlasFirstFrame;   // gos_terrain_indirect.cpp latch
			extern long     g_loadRaceArmFirstFrame;     // gos_terrain_indirect.cpp latch
			if (g_loadRaceTrace)
			{
				bool fully;
				long terrTotal = 0;
				const bool chunkPath = mc2TerrainLodChunkEnabled();
				if (chunkPath)
				{
					fully = (g_loadRaceAtlasFirstFrame >= 0) &&
						(g_loadRaceArmFirstFrame >= 0);
				}
				else
				{
					long terrResident = 0;
					for (long ti = 0; ti < nextAvailableVertexNode; ++ti)
					{
						const DWORD tf = masterVertexNodes[ti].flags;
						if ((tf & MC2_DRAWSOLID) && (tf & MC2_ISTERRAIN) &&
							masterVertexNodes[ti].vertices)
						{
							++terrTotal;
							if (isTextureNodeResident(masterVertexNodes[ti].textureIndex))
								++terrResident;
						}
					}
					fully = (terrTotal > 0) && (terrResident == terrTotal);
				}
				g_loadRaceTerrainResidentNow = fully;
				if (fully && g_loadRaceTerrainFirstResidentFrame < 0)
				{
					g_loadRaceTerrainFirstResidentFrame = (long)g_mc2FrameCounter;
					if (chunkPath)
						std::printf("[LOADRACE] terrain first fully-resident frame=%ld "
							"(chunk path: atlas_frame=%ld arm_frame=%ld)\n",
							g_loadRaceTerrainFirstResidentFrame,
							g_loadRaceAtlasFirstFrame, g_loadRaceArmFirstFrame);
					else
						std::printf("[LOADRACE] terrain first fully-resident frame=%ld "
							"(terrainNodes=%ld)\n",
							g_loadRaceTerrainFirstResidentFrame, terrTotal);
					std::fflush(stdout);
				}
			}
		}

		// Modern path. flush() returns true on success and false on overflow
		// / not-ready / not-killswitched. On false we fall through to the
		// legacy loop for the WHOLE FRAME — never partial-frame. The legacy
		// ring data has been kept in sync by addVertices/fillTerrainExtra
		// running unconditionally in quad.cpp.
		// Bucket-census instrumentation (env-gated MC2_BUCKET_CENSUS=1).
		// Count legacy-eligible nodes BEFORE flush() runs (both the legacy
		// draw branch at ~1369 and the alternate reset branch at ~1382
		// zero currentVertex per node, so end-of-zone undercounts). The
		// filter mirrors the legacy DRAWSOLID|ISTERRAIN draw-emission
		// predicate so it is apples-to-apples with PatchStream's modern
		// scope.
		static const bool s_bucketCensusOn = mc2::gate::get<"MC2_BUCKET_CENSUS">();
		uint32_t legacyEligible = 0;
		if (s_bucketCensusOn) {
			for (long ci = 0; ci < nextAvailableVertexNode; ++ci) {
				const DWORD cf = masterVertexNodes[ci].flags;
				if ((cf & MC2_DRAWSOLID) && (cf & MC2_ISTERRAIN) &&
				    masterVertexNodes[ci].vertices &&
				    masterVertexNodes[ci].currentVertex !=
				        masterVertexNodes[ci].vertices)
				{
					++legacyEligible;
				}
			}
		}

		bool modernHandled = false;

		// TERRAIN-CULL-STATE-PROBE-1 (MC2_TERRAIN_CULL_PROBE, default OFF): the
		// terrain-solid dispatch CHOKEPOINT — runs every frame terrain renders,
		// before the indirect/patch-stream/legacy branch. None of those paths set
		// cull, so the ambient cull/frontFace read HERE is what terrain renders
		// under. Read-only; first few frames then stops.
		{
			static const bool s_cullProbe = mc2::gate::get<"MC2_TERRAIN_CULL_PROBE">();
			static int s_cullProbeFrames = 0;
			if (s_cullProbe && s_cullProbeFrames < 8) {
				++s_cullProbeFrames;
				GLboolean cullOn = glIsEnabled(GL_CULL_FACE);
				GLint cullMode = 0, frontFace = 0;
				glGetIntegerv(GL_CULL_FACE_MODE, &cullMode);
				glGetIntegerv(GL_FRONT_FACE, &frontFace);
				std::fprintf(stderr,
					"[TERRAIN_CULL_PROBE] path=dispatch frame=%d cull=%s cullMode=%s frontFace=%s\n",
					s_cullProbeFrames,
					cullOn ? "ENABLED" : "disabled",
					cullMode == GL_BACK ? "BACK" : cullMode == GL_FRONT ? "FRONT" :
						cullMode == GL_FRONT_AND_BACK ? "FRONT_AND_BACK" : "?",
					frontFace == GL_CCW ? "CCW" : frontFace == GL_CW ? "CW" : "?");
				std::fflush(stderr);
			}
		}

		if (gos_terrain_indirect::IsFrameSolidArmed()) {
			// Indirect SOLID owns this frame. The SOLID gate-off in setupTextures()
			// already fired, so TerrainPatchStream has no SOLID records — do NOT fall
			// back to flush() when DrawIndirect returns false (plan v2 advisor
			// stop-the-line #1). A false return is a hard failure: logged, arming
			// disabled process-wide; operator advice in event=hard_failure line.
			//
			// Phase 7.5 LOD chunk coexistence: when MC2_TERRAIN_LOD_CHUNK=1 the chunk
			// path owns terrain rendering (flushDrawCommands in gamecam.cpp). Suppress
			// DrawIndirect to prevent depth/color conflicts — both paths write the same
			// world-space surface and the indirect path draws first (land->render),
			// causing the coarser LOD chunk mesh to fail the depth test.
			if (mc2TerrainLodChunkEnabled()) {
				modernHandled = true;   // chunks handle it (existing)
			} else {
				// INDIRECT-BRIDGE-RETIRE-1: indirect-bridge SOLID terrain draw retired (deprecated branch,
				// terrain_path.indirect proven 0 in default/smoke/capture/editor). LOD-chunk owns terrain.
				// Do NOT DrawIndirect(); fire the telemetry counter as a regression tripwire. modernHandled
				// MUST be true here: on armed frames setupTextures already fired SOLID gate-off so PatchStream
				// has no SOLID records — a false value would fall through to the (now terrain-skipping) shared
				// loop and could mis-fire the LegacyMLR tripwire. NOTE: DrawIndirect() itself is RETAINED
				// (RenderWaterReflectionPass calls it internally, gos_terrain_indirect.cpp:3817); only this
				// txmmgr SOLID-draw caller is retired.
				RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::IndirectBridge);
				modernHandled = true;
			}
		} else if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
			// PATCHSTREAM-THIN-RETIRE-1: patch-stream-thin terrain draw retired (deprecated branch,
			// terrain_path.patch_stream proven 0 in default/smoke/capture/editor). LOD-chunk owns
			// terrain. Do NOT flush()-draw; fire the telemetry counter as a regression tripwire so
			// patch_stream>0 surfaces if a reachable config ever needed it. modernHandled stays false
			// (no gate-off fired for un-armed frames) — matching the prior flush()-returns semantics
			// when it drew nothing.
			// NOTE: flush() side-effects audit — flush() does: (a) draw calls, (b) glFenceSync on
			// current slot, (c) markTerrainDrawn() on post-process, (d) noteTerrainPath(PatchStreamThin).
			// The ring slot advance is in beginFrame() NOT flush(), so skipping flush() does not break
			// the ring. The getLastFlush* snapshot getters read statics reset by beginFrame() each frame
			// — they will correctly read 0 when this branch is not flushing. markTerrainDrawn() is NOT
			// called here because no terrain was drawn. NO load-bearing non-draw side effects are lost.
			RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::PatchStreamThin);
		}

		// RENDER-FRAME-PLAN-SCAFFOLD-1: tattle WHICH terrain-solid branch drew this
		// frame. This is the rake-prevention trace — capture takes the legacy MLR
		// branch (modern bridge not armed), which bound TerrainSolid 0x and confused
		// the routing recon. Observe only; no behavior change.
		if (render_frame_plan::traceEnabled()) {
			using namespace render_frame_plan;
			const bool armed = gos_terrain_indirect::IsFrameSolidArmed();
			if (modernHandled && armed && !mc2TerrainLodChunkEnabled())
				trace(Phase::TerrainOpaque, "TerrainSolid", PathKind::ApplyPipeline, -1, "TerrainSolid");      // modern indirect bridge (routed)
			else if (modernHandled && armed)
				trace(Phase::TerrainOpaque, "TerrainSolidLODChunk", PathKind::ApplyPipeline, -1, "TerrainSolid");  // LOD-chunk, routed (ROUTING-1)
			else if (modernHandled)
				trace(Phase::TerrainOpaque, "TerrainSolidThin", PathKind::RawGL, -1, "None");                 // un-armed patch-stream flush
			else
				trace(Phase::TerrainOpaque, "TerrainLegacyMLR", PathKind::MLR, -1, "None");                   // legacy master-node fall-through
		}

		// [TERRAIN_SURFACE] PR-2 (Wave 1, ADDITIVE / DEFAULT-OFF / DELETES
		// NOTHING). Screen-agnostic continuous-surface VALIDATION draw: runs
		// on EVERY frame regardless of arming (design Convergence C-1 --
		// surface existence is decoupled from IsFrameSolidArmed). A no-op
		// unless MC2_TERRAIN_SURFACE is set (gos_terrain_surface::IsEnabled,
		// checked inside the bridge), so the default path is byte-for-byte
		// behaviour-neutral. When ON, the surface draws ON TOP of the still-
		// running legacy/indirect terrain above for visual validation of the
		// V-ssbo VS + Fork D clip-space pre-divide reverse-Z bias. NO legacy
		// kill site lands here -- the substitutive draw-kill is PR-4.
		gos_terrain_surface_bridge_draw();

		bool bSkip_DRAWSOLID = false;
		for (long i=0;i<nextAvailableVertexNode && !bSkip_DRAWSOLID;i++)
		{
				if ((masterVertexNodes[i].flags & MC2_DRAWSOLID) &&
					(masterVertexNodes[i].vertices))
				{
					// LEGACY-MLR-DELETE-1: legacy masterVertexNode terrain draw retired. Terrain solid is
					// owned by LOD-chunk (gamecam.cpp:508) / indirect bridge / patch-stream. A terrain node
					// must NOT draw via this shared MC2_DRAWSOLID loop. Always skip terrain here. If a terrain
					// node arrives with modernHandled==false, no modern path drew terrain this frame — a real
					// regression (terrain would be invisible). Fire the LegacyMLR telemetry counter as a loud
					// tripwire so it surfaces in terrain_path.legacy_mlr instead of silently vanishing.
					if (masterVertexNodes[i].flags & MC2_ISTERRAIN) {
						if (!modernHandled)
							RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LegacyMLR);
						masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
						continue;
					}

					gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
					gos_SetRenderState( gos_State_Terrain, 0 );

					DWORD totalVertices = masterVertexNodes[i].numVertices;
					if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
					{
						totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
					}

#ifdef MC2_VULKAN_ISLAND
					if (vkLegacyOn)
					{
						// VK-PARITY-F1: hand this node's CPU-lit gos_VERTEX triangle list to the
						// VK legacy-shape island (drawn after the loop) instead of GL.
						vkLegacySubmitNode(i, MC2_LEGACY_PASS_SOLID, 0);
						continue;
					}
#endif
					// LEGACY-MLR-DELETE-1: dead (terrain skipped above); non-terrain nodes never have terrain extras.
					gos_SetTerrainBatchExtras(NULL, 0);

					if (totalVertices && (totalVertices < MAX_SENDDOWN))
					{
						gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));
						gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
					}
					else if (totalVertices > MAX_SENDDOWN)
					{
						gos_SetRenderState( gos_State_Texture, tex_resolve(masterVertexNodes[i].textureIndex));

						//Must divide up vertices into batches of 10,000 each to send down.
						// Somewhere around 20000 to 30000 it really gets screwy!!!
						long currentVertices = 0;
						while (currentVertices < totalVertices)
						{
							gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
							long tVertices = totalVertices - currentVertices;
							if (tVertices > MAX_SENDDOWN)
								tVertices = MAX_SENDDOWN;

							gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

							currentVertices += tVertices;
						}
					}
					//Reset the list to zero length to avoid drawing more then once!
					//Also comes in handy if gameLogic is not called.
					masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
				}
			}
#ifdef MC2_VULKAN_ISLAND
		if (vkLegacyOn)
			vkLegacyRun();   // VK-PARITY-F1: queued legacy SOLID nodes -> shared GBuffer
#endif
		// Emit one [BUCKET_CENSUS v1] line per frame (env-gated). Runs
		// after flush() has populated the modern-side stats and after
		// either branch has reset the legacy ring; legacy_eligible was
		// captured pre-flush above. emitCensus() is a no-op when the
		// env var is unset.
		if (s_bucketCensusOn) {
			TerrainPatchStream::emitCensus(legacyEligible);
		}
		gos_render_pass_timer::End(gos_render_pass_timer::Pass_TerrainSolid);
	}   // end Render.TerrainSolid zone
	drainGLErrors("terrain");

	// Task 10 flush() — moved here from after Render.3DObjects because
	// terrain renders AFTER 3D objects in this codebase; placing our
	// flush earlier meant terrain overwrote our pixels. Running after
	// terrain but before overlays gives buildings the right layering:
	// on top of terrain, below decals/roads.
	{
		ZoneScopedN("Render.GpuStaticProps");
		TracyGpuZone("Render.GpuStaticProps");
		gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_SpColor);
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
		if (g_useGpuStaticProps || g_useGpuObjects) {
			// [RENDERLISTS_COST v1] prep+cull-dispatch only; the two flush units
			// below carry their own spans (no double-count vs total).
			rlcost::Span _rl(rlcost::kGpuSpPrep);
			// Stage 3.C registry flush MOVED earlier (before flushShadow, see
			// STATIC-PROP SHADOW-ORDER FIX 2026-05-29) so terrain-object instances
			// are in s_bucketsByType before the shadow-time SSBO upload locks the
			// slot. It injects static-registry instances into batcher buckets AND
			// appends Cat_StaticProp substrate records for the cull shader; both are
			// now done above, before this block. Do NOT re-add a flush() call here
			// (double-flush). compute_dispatch() below still runs after it.

			// Step 4.6 (global-pool slice 1): compute per-cmd baseInstance prefix-sum
			// and advance the coalesce ring slot BEFORE compute_dispatch() so the
			// patch shader can read baseInstanceByCmd[] in the same dispatch.
			// [SPFLUSH_COST_SPLIT v1] baseinstance_upload span (nifty) wrapped in
			// the GpuSP.PrepBaseInstance Tracy zone (override branch).
			{
				ZoneScopedN("GpuSP.PrepBaseInstance"); GosGlLostDrawsScope _lost_("gpusp.prep_base_instance");
				const unsigned long long _t_bi0 = s_spflushTxmEnabled ? __rdtsc() : 0ULL;
				batcher_prepareBaseInstanceTable();
				if (s_spflushTxmEnabled) s_spflush_baseinstance_upload_cyc += __rdtsc() - _t_bi0;
			}

			// C1b GPU authority flip: compute_dispatch() is now called HERE
			// (moved from mission.cpp) so it processes BOTH dynamic actor records
			// (from substrate_flushUpload in objmgr::update) AND the static prop
			// records appended by GpuStaticPropRegistry::flush() above.
			// The patch shader then writes GPU-computed instanceCounts into the
			// indirect command buffer. GpuStaticPropBatcher::flush() below uses
			// glMultiDrawElementsIndirect which reads those GPU-authoritative counts.
			// M1 FROZEN-STATIC-CULL-RECORDS: build + install the frozen static
			// cull-record prefix now that baseInstanceForType is valid (set by
			// batcher_prepareBaseInstanceTable above) and before compute_dispatch
			// consumes the records. No-op on clean frames / unless the gate is set.
			{ GosGlLostDrawsScope _lost_("gpusp.static_prefix"); GpuStaticPropRegistry::buildStaticPrefixGolden(); }

			if (gpu_cull::compute_isEnabled()) {
				ZoneScopedN("GpuSP.CullDispatch"); GosGlLostDrawsScope _lost_(nullptr);
				// VK-GL-LOST-DRAWS: nullptr tag = unmeasured. compute_dispatch() is compute-only (no raster); a
				// GL_SAMPLES_PASSED segment holding only compute + barriers returns a garbage
				// ~1e12 on this driver (begin snapshot never taken) and mis-attributes the frame's
				// real draws to it. Suspend the query for it.
				TracyGpuZone("GpuSP.CullDispatch");
				gpu_cull::compute_dispatch();
			}

			// CAMERA-MOVE-DIAG: log substrate record count + cull output before/after
			// dispatch to distinguish compute early-return vs frustum-all-fail.
			// Gate: MC2_CAMERA_MOVE_DIAG=1. Causes GL stalls — diagnostic only.
			{
				static const bool s_diagEnabled = mc2::gate::get<"MC2_CAMERA_MOVE_DIAG">();
				if (s_diagEnabled) {
					static float s_lastMvp[16] = {};
					static uint32_t s_frameN = 0;
					extern const float* gos_GetTerrainMVPMat4();
					const float* mvp = gos_GetTerrainMVPMat4();
					bool mvpChanged = mvp && (memcmp(mvp, s_lastMvp, 16 * sizeof(float)) != 0);
					if (mvp && mvpChanged) memcpy(s_lastMvp, mvp, 16 * sizeof(float));
					++s_frameN;
					// Log every camera-move frame; stationary every 120 frames
					bool doLog = mvpChanged || (s_frameN % 120 == 0);
					if (doLog) {
						const uint32_t subRec = gpu_cull::substrate_getCurrentRecordCount();
						const GLintptr  subOff = gpu_cull::substrate_getCurrentSlotOffset();
						glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
						const GLuint bktBuf = gpu_cull::compute_getBucketCountsBuf();
						const GLuint cmdBuf = gpu_cull::compute_getIndirectCmdBuf();
						uint32_t bkt[4] = {};
						if (bktBuf) glGetNamedBufferSubData(bktBuf, 0, sizeof(bkt), bkt);
						uint32_t inst0 = 0;
						if (cmdBuf) glGetNamedBufferSubData(cmdBuf, 4, sizeof(inst0), &inst0); // struct[0].instanceCount
						// Sum ALL bucket counts across all 221+ buckets.
						uint32_t bktTotal = 0;
						const uint32_t nBkts = gpu_cull::compute_getBucketCount();
						if (bktBuf && nBkts > 0) {
							std::vector<uint32_t> allBkt(nBkts + 1u);
							glGetNamedBufferSubData(bktBuf, 0, (nBkts + 1u) * sizeof(uint32_t), allBkt.data());
							for (uint32_t b = 0; b < nBkts; ++b) bktTotal += allBkt[b];
						}
						printf("[DIAG] %s subRec=%u | bkt0=%u bkt1=%u bkt2=%u bkt3=%u bktTOTAL=%u | inst0=%u | mvp[0..3]=[%.3f %.3f %.3f %.3f]\n",
							mvpChanged ? "MOVE  " : "STATIC",
							subRec,
							bkt[0], bkt[1], bkt[2], bkt[3], bktTotal, inst0,
							mvp ? mvp[0] : 0.f, mvp ? mvp[1] : 0.f,
							mvp ? mvp[2] : 0.f, mvp ? mvp[3] : 0.f);
						fflush(stdout);
					}
				}
			}

		}

		// MEASURED-REORDER-SPMECH-1 (tier-C reorder EXPERIMENT, default-OFF):
		// The two opaque flush UNITS below — StaticProp BatcherFlush (+Pass_SpColor
		// timer) and the Mech block (+Pass_Mechs timer) — are the one legal adjacent
		// swap the legal-reorder oracle proved. StaticProp PREP + compute_dispatch
		// (above, inside Render.GpuStaticProps) must stay BEFORE both flushes in
		// EITHER path; only the two flush units swap order. The OpaqueObject
		// begin(executorOwnBeginTopLevel @ Render.3DObjects)/end wrapper brackets
		// both regardless. Gate OFF -> today's order verbatim (byte-identical).
		// Gate ON -> mech flush, then static-prop flush.
		static const bool s_reorderSpMech = mc2::gate::get<"MC2_FRAMEGRAPH_REORDER_SPMECH">();

		// Unit A: static-prop batcher flush, scoped by Pass_SpColor. Captured pre-
		// flush eligibility lives in the extern flags read in Render.GpuStaticProps;
		// re-read here (same flags) so the flush still respects the GPU-path gate.
		auto runSpFlushUnit = []() {
			extern bool g_useGpuStaticProps;
			extern bool g_useGpuObjects;
			if (g_useGpuStaticProps || g_useGpuObjects) {
				ZoneScopedN("GpuSP.BatcherFlush");
				TracyGpuZone("GpuSP.BatcherFlush");
				rlcost::Span _rl(rlcost::kSpBatcherFlush);
#ifdef MC2_VULKAN_ISLAND
				// VK-P11 STATIC-PROP COLOR Tier-1 (gate MC2_VK_PROP_COLOR_TIER1):
				// render visible static-prop color instances directly into the shared
				// scene GBuffer in Vulkan instead of the GL flush() color draws. The
				// island pulls its per-frame instance feed from the batcher
				// (s_bucketsByType still populated here because we skip flush()). On
				// success skip the GL flush; on any fail-soft (0) fall through to GL so
				// props still draw. Gate OFF -> enabled()==0 -> GL flush() as before.
				bool vkPropColorHandled = false;
				if (mc2_vk_color_prop_island_enabled()) {
					ZoneScopedN("GpuSP.VkColorPropInterop");
					if (gosPostProcess* ppVkClr = getGosPostProcess()) {
						vkPropColorHandled = (1 == mc2_vk_color_prop_island_run(
							ppVkClr->getWidth(), ppVkClr->getHeight()));
					}
				}
				if (vkPropColorHandled) {
					// flush() (sole per-frame bucket clear) was skipped: clear here so
					// dynamic submits do not accumulate across island frames.
					mc2_color_prop_island_end_frame();
				} else
					GpuStaticPropBatcher::instance().flush(getLastRenderSnapshot());
#else
				GpuStaticPropBatcher::instance().flush(getLastRenderSnapshot());
#endif
			}
			// Pass_SpColor ends after the static-prop flush: GL_TIME_ELAPSED
			// scopes cannot nest, so mechs get their own disjoint scope.
			gos_render_pass_timer::End(gos_render_pass_timer::Pass_SpColor);
		};

		// Unit B: GPU mech batcher Slice A flush — runs adjacent to the static-prop
		// flush, inside renderLists() so terrain has already been emitted by the
		// patch stream and the depth state is set up. Independent of
		// g_useGpuStaticProps; gated on its own MC2_GPU_MECHS env var inside the
		// flush itself. Pass_Mechs is a disjoint GL_TIME_ELAPSED scope.
		auto runMechFlushUnit = []() {
			ZoneScopedN("Render.GpuMechs");
			TracyGpuZone("Render.GpuMechs");
			rlcost::Span _rl(rlcost::kMechFlush);
			gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Mechs);
			// [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
			// MECHOPAQUE-NOTE-RELOCATE-1: note fires at the real mech GPU draw
			// site (after shadow, after static-prop flush) — NOT at the preamble.
			// FBO/viewport facts here are draw-time (scene FBO + SceneGEqual depth
			// state established by terrain/static-prop passes above).
			render_contract::noteRenderPass(render_contract::PassIdentity::OpaqueObject,
			                               "GpuMechBatcher_flush(submit)");
			// APPLY-STATE-MECHOPAQUE-1: when MC2_FRAMEGRAPH_EXECUTOR is ON and a
			// kTopLevelStateDesc row exists, the executor pre-applies the MechOpaque
			// pipeline here (at the real flush call site, NOT the begin seam which wraps
			// non-mech draws too); flush()'s body then skips its own applyPipeline.
			// Gate OFF -> not called -> body applies -> byte-identical.
			if (render_contract::isTopLevelExecutorEnabled() &&
			    RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::MechOpaque) != nullptr) {
				GpuMechBatcher::instance().executorApplyMechOpaqueState();
			}
			// VK-P14 MECH TIER-1 (gate MC2_VK_MECH_TIER1, requires MC2_VK_SHARED_SCENE_DEPTH,
			// default-OFF): flush() still BUILDS the instance/bone SSBOs + feed snapshot but
			// SKIPS its GL color draws when the gate is on; the island then co-writes the
			// skinned mechs IN-PLACE into the shared GBuffer. Fail-soft (island 0) = mechs
			// dropped this frame (GL draws already skipped), no corruption. Gate OFF ->
			// enabled()==0 -> flush() draws GL as before (byte-identical).
			GpuMechBatcher::instance().flush();
#ifdef MC2_VULKAN_ISLAND
			if (mc2_vk_mech_island_enabled()) {
				ZoneScopedN("Render.GpuMechs.VkTier1");
				if (gosPostProcess* ppVkMech = getGosPostProcess()) {
					(void)mc2_vk_mech_island_run(ppVkMech->getWidth(), ppVkMech->getHeight());
				}
			}
#endif
			gos_render_pass_timer::End(gos_render_pass_timer::Pass_Mechs);
		};

		if (s_reorderSpMech) {
			// EXPERIMENT (gate-ON): swap the two opaque flush units.
			runMechFlushUnit();
			runSpFlushUnit();
		} else {
			// Default path: today's order verbatim — static-prop flush, then mechs.
			runSpFlushUnit();
			runMechFlushUnit();
		}
		render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::OpaqueObject,
		                                        "renderLists_GpuMechBatcher_flush");
	}

	// DRAWSOLID done

	// B4 Slice Stage 1b — mask-SOLID dual-run dispatch.
	// Draws the same SOLID quads as the legacy drawPass (which is still active
	// in Stage 1b — both run; parity comparator validates the masks match).
	// Default-off: IsFrameMaskSolidArmed() returns false unless
	// MC2_TERRAIN_MASK_DISPATCH=1 AND MC2_TERRAIN_MASK_DISPATCH_SOLID != "0".
	// Pass_Overlays = one coarse scope spanning Render.TerrainMask.Solid
	// through Render.Decals (sub-zones stay Tracy-only).
	// [RENDERLISTS_COST v1] manual span: the overlay region is a run of sibling
	// blocks (mask-solid .. decals), not one lexical scope.
	const unsigned long long _rlOv0 = rlcost::s_enabled ? rlcost::nowNs() : 0ULL;
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Overlays);
	{
		ZoneScopedN("Render.TerrainMask.Solid"); GosGlLostDrawsScope _lost_("overlays.mask_solid");
		TracyGpuZone("Render.TerrainMask.Solid");
		if (gos_terrain_mask_dispatch::IsMaskDispatchReady()
		 && gos_terrain_mask_dispatch::IsFrameMaskSolidArmed()) {
			gos_terrain_mask_dispatch::DrawMaskSolid();
		}
	}

	// ── New world-space overlay batches ──────────────────────────────────────
	// These draw calls flush batches accumulated during land->render() and
	// craterManager->render().  They set their own GL state and restore it.
	// Render order matches design: terrain → cement overlays → decals → (old overlays).
	{
		ZoneScopedN("Render.TerrainOverlays"); GosGlLostDrawsScope _lost_("overlays.terrain_overlays");
		TracyGpuZone("Render.TerrainOverlays");
		gos_DrawTerrainOverlays();
	}
	// Slice A — cement-overlay static-bake draw. Mirrors the Render.Terrain
	// Mines hook below EXACTLY: gated on IsFrameOverlayArmed() (default OFF
	// unless MC2_TERRAIN_INDIRECT_OVERLAY=1). When armed, the per-quad M2d
	// gos_PushTerrainOverlay producer is skipped (quad.cpp gate-off) and
	// gos_DrawTerrainOverlays above flushes an empty batch (early-return);
	// DrawDecalStatic draws the persistent static bake instead. Placed right
	// after Render.TerrainOverlays so the static cement composites in the
	// same slot the per-frame batch used (before mines/decals/old overlays).
	{
		ZoneScopedN("Render.TerrainOverlaysStatic"); GosGlLostDrawsScope _lost_("overlays.terrain_overlays_static");
		TracyGpuZone("Render.TerrainOverlaysStatic");
		if (gos_terrain_indirect::IsFrameOverlayArmed()) {
			gos_terrain_indirect::DrawDecalStatic();
		}
	}
	// PR2c Stage 2c — mine static-bake draw. Hooks between TerrainOverlays
	// and Decals so mines composite ABOVE cement/road overlays and BENEATH
	// crater decals (state=2 blown-mine sprites coexist with crater decals).
	// Default-off: IsFrameMineArmed() returns false unless
	// MC2_TERRAIN_INDIRECT_MINE=1 AND the texture-array has been built.
	{
		ZoneScopedN("Render.TerrainMines"); GosGlLostDrawsScope _lost_("overlays.terrain_mines");
		TracyGpuZone("Render.TerrainMines");
		if (gos_terrain_indirect::IsFrameMineArmed()) {
			gos_terrain_indirect::DrawMineStatic();
		}
	}
	{
		ZoneScopedN("Render.Decals"); GosGlLostDrawsScope _lost_("overlays.decals");
		TracyGpuZone("Render.Decals");
		// MC2_DYNAMIC_DECALS: push dynamic impact decals into the batch before flushing.
		// Gate-off: gatherToDecalBatch() is a no-op; gate-on: pushes faded quads.
		// frameLength = duration of last frame in seconds (timing.h).
		DynDecal::gatherToDecalBatch(frameLength);
		gos_DrawDecals();
	}
#ifdef MC2_VULKAN_ISLAND
	// VK-PARITY-OVERLAY-1: the static cement/road overlay bake and the decal batch were queued
	// (not GL-drawn) when the terrain island is armed; draw them into the shared GBuffer now,
	// in the same slot (after terrain/objects, before the old overlays).
	if (mc2_vk_overlay_enabled())
	{
		if (gosPostProcess* ppVkOv = getGosPostProcess())
			(void)mc2_vk_overlay_run(ppVkOv->getWidth(), ppVkOv->getHeight());
	}
#endif
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Overlays);
	if (rlcost::s_enabled) rlcost::s_ns[rlcost::kOverlays] += rlcost::nowNs() - _rlOv0;
	// ── End new world-space overlay batches ───────────────────────────────────

	{
		ZoneScopedN("Render.Overlays"); GosGlLostDrawsScope _lost_("overlays.overlays_old");
		TracyGpuZone("Render.Overlays");
	if (Environment.Renderer == 3)
	{
		//Do NOT draw the water as transparent in software
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	
    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
	{
	ZoneScopedN("RenderLists.TerrainAlphaWaterLoops");
	rlcost::Span _rl(rlcost::kWaterLoops);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Water);
    for(int states = 0; states < 2; ++states)
    {
        gos_SetRenderState( gos_State_AlphaTest, states);

        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
                // The legacy non-water terrain alpha/detail layer sits on the original
                // flat terrain plane and shows through displaced tess terrain as the
                // dark striped under-pattern. Keep water passes, but drop this layer.
                if (!(masterVertexNodes[i].flags & MC2_ISWATER) &&
                    !(masterVertexNodes[i].flags & MC2_ISWATERDETAIL)) {
                    masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
                    continue;
                }

                {
                    int waterMode = 0;
                    if (masterVertexNodes[i].flags & MC2_ISWATER) waterMode = 1;
                    else if (masterVertexNodes[i].flags & MC2_ISWATERDETAIL) waterMode = 2;
                    gos_SetRenderState(gos_State_Water, waterMode);
                }

                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Water);
	} // end RenderLists.TerrainAlphaWaterLoops


	//<< sebi: added this section to draw objects which do not have terrain underlayer (those are added in quad.cpp, see (*) there )
	{ ZoneScopedN("Render.NoUnderlayer");
	  TracyGpuZone("Render.NoUnderlayer");
	  rlcost::Span _rl(rlcost::kNoUnderlayer);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
			!(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
			(masterVertexNodes[i].flags & MC2_GPUOVERLAY) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
	
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ZCompare, 0);
			gos_SetRenderState(gos_State_Overlay, 1);
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
			gos_SetRenderState(gos_State_Overlay, 0);
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
			
			//Reset the list to zero length to avoid drawing more then once!			
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	} // end ZoneScopedN("Render.NoUnderlayer")
	//<< sebi: end of added block

	// Cement overlays (MC2_ISCRATERS|MC2_ISTERRAIN) and decals (!MC2_ISTERRAIN|MC2_ISCRATERS)
	// are now drawn by gos_DrawTerrainOverlays() and gos_DrawDecals() before Render.Overlays.
	// The old Render.CraterOverlays and non-terrain crater loops are removed.

	{
	ZoneScopedN("RenderLists.ShadowBlobs");
	rlcost::Span _rl(rlcost::kShadowBlobs);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Blobs);
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState(	gos_State_ZWrite, 1);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}
	else
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		// sebi: shadows do not draw in depth, we do not want z-fighting
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}

	//NEVER draw shadows in Software.
	if (Environment.Renderer != 3)
	{
		for (int i=0;i<nextAvailableVertexNode;i++)
		{
			 if	((masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
				(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
				(masterVertexNodes[i].vertices))
			{
#ifdef MC2_VULKAN_ISLAND
				if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_SHADOW, 0); continue; }   // VK-PARITY-F1
#endif
				DWORD totalVertices = masterVertexNodes[i].numVertices;
				if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
				{
					totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
				}
			
				if (totalVertices && (totalVertices < MAX_SENDDOWN))
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
				}
				else if (totalVertices > MAX_SENDDOWN)
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					
					//Must divide up vertices into batches of 10,000 each to send down.
					// Somewhere around 20000 to 30000 it really gets screwy!!!
					long currentVertices = 0;
					while (currentVertices < totalVertices)
					{
						gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
						long tVertices = totalVertices - currentVertices;
						if (tVertices > MAX_SENDDOWN)
							tVertices = MAX_SENDDOWN;
						
						gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
						
						currentVertices += tVertices;
					}
				}
				
				//Reset the list to zero length to avoid drawing more then once!
				//Also comes in handy if gameLogic is not called.
				masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
			}
		}
	}
#ifdef MC2_VULKAN_ISLAND
	if (vkLegacyOn)
		vkLegacyRun();   // VK-PARITY-F1: queued legacy blob-shadow nodes -> shared GBuffer
#endif
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Blobs);
	} // end RenderLists.ShadowBlobs


	{
	ZoneScopedN("RenderLists.NonTerrainAlphaLoops");
	rlcost::Span _rl(rlcost::kAlphaLoops);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_AlphaVfx);
	gos_SetRenderState( gos_State_ZCompare, 1);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		// Blended alpha must not write depth: these are sorted-alpha nodes,
		// not the opaque/masked prepass. Applies to both the pure-blend
		// (states=0) and alpha-test cutout (states=1) loops, conservatively.
		gos_SetRenderState(	gos_State_ZWrite, 0);
	}

    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
    for(int states = 0; states < 2; ++states)
    {
        gos_SetRenderState( gos_State_AlphaTest, states);
        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if (!(masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    !(masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
                    !(masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
#ifdef MC2_VULKAN_ISLAND
                if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_ALPHA, (uint32_t)states); continue; }   // VK-PARITY-F1
#endif
                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);
	if (Environment.Renderer != 3)
	{
		// Restore legacy depth-write state for downstream passes (they
		// inherit render state from this block).
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_AlphaVfx);
	} // end RenderLists.NonTerrainAlphaLoops

	{
	ZoneScopedN("RenderLists.VfxHudSubmit");
	rlcost::Span _rl(rlcost::kVfxHud);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Hud);
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 0);
	}
				
	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISEFFECTS) &&
			(masterVertexNodes[i].vertices))
		{
#ifdef MC2_VULKAN_ISLAND
			if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_ADD, 0); continue; }   // VK-PARITY-F1
#endif
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	// Spotlight cones are blended/additive: no depth writes during the draw.
	// Downstream state is unchanged -- the post-loop reset below already
	// sets ZWrite 0 (legacy behavior).
	gos_SetRenderState(	gos_State_ZWrite, 0);

	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISSPOTLGT) &&
			(masterVertexNodes[i].vertices))
		{
#ifdef MC2_VULKAN_ISLAND
			if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_ADD, 0); continue; }   // VK-PARITY-F1
#endif
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
#ifdef MC2_VULKAN_ISLAND
	if (vkLegacyOn)
		vkLegacyRun();   // VK-PARITY-F1: queued legacy alpha + additive effect / spotlight-cone nodes
#endif
	gos_SetRenderState( gos_State_ZWrite, 0);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_Perspective, 1);
 	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_AlphaTest, 1);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISHUDLMNT) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}

	//Must turn zCompare back on for FXs
	gos_SetRenderState( gos_State_ZCompare, 1 );

	// Reset terrain extra buffer after rendering — will be re-filled during next frame's TerrainQuad::draw() calls
	gos_TerrainExtraReset();
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Hud);
	} // end RenderLists.VfxHudSubmit
	} // end Render.Overlays zone
}
