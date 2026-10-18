// gameos_graphics: shadow-pass state tracing, the static and dynamic shadow passes, gos_SetupObjectShadows and the shadow wrappers.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"
#include "static_shadow_spec.h"

#include <cstring>

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
static ShadowPassGLState s_dynamicPassEntry;

static bool shadowStateTraceEnabled() {
    static int s_on = -1;
    if (s_on < 0) {
        s_on = mc2::gate::get<"MC2_SHADOW_STATE_TRACE">();
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
    const char bldg[1] = {0};
    mc2::fmt::print_err("[SHADOW_STATE v1] pass={} fbo={:d} vp={:d},{:d},{:d},{:d} clearDepth={:g} depthTest={:d} depthFunc={} depthMask={:d} colorMask={:d}{:d}{:d}{:d} cull={}/{}/{} clipOrigin=0x{:X} clipDepth=0x{:X} restored={:d}{}\n", mc2::fmt::safe(passName), mc2::fmt::as_i32(pass.fbo), mc2::fmt::as_i32(pass.viewport[0]), mc2::fmt::as_i32(pass.viewport[1]), mc2::fmt::as_i32(pass.viewport[2]), mc2::fmt::as_i32(pass.viewport[3]), (double)pass.clearDepth, mc2::fmt::as_i32(pass.depthTest ? 1 : 0), mc2::fmt::safe(depthFuncName(pass.depthFunc)), mc2::fmt::as_i32(pass.depthMask ? 1 : 0), mc2::fmt::as_i32(pass.colorMask[0]?1:0), mc2::fmt::as_i32(pass.colorMask[1]?1:0), mc2::fmt::as_i32(pass.colorMask[2]?1:0), mc2::fmt::as_i32(pass.colorMask[3]?1:0), mc2::fmt::safe(pass.cullEnabled ? "on" : "off"), mc2::fmt::safe(pass.cullFace == GL_FRONT ? "front" : "back"), mc2::fmt::safe(pass.frontFace == GL_CW ? "CW" : "CCW"), mc2::fmt::as_u32((unsigned)pass.clipOrigin), mc2::fmt::as_u32((unsigned)pass.clipDepth), mc2::fmt::as_i32(restored ? 1 : 0), mc2::fmt::safe(bldg));
    fflush(stderr);
}
}  // namespace

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

}

// Shared helper called from each of the three terrain uniform-upload sites below.
// Binds the per-mission R32F height texture to unit 11, pushes the worldspace
// mapping params and the cliff shadow floor. Caller is responsible for restoring
// the active texture unit afterwards (existing sites already do glActiveTexture
// (GL_TEXTURE0) immediately after this helper).
void bindTerrainHeightTexUniforms(GLint heightTexLoc, GLint paramsLoc,
                                         GLint cliffShadowFloorLoc,
                                         float cliffShadowFloor)
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
    // CLIFF SHADOW FLOOR: lifts shadow-side steep terrain faces off near-black.
    // Env gate MC2_TERRAIN_CLIFF_SHADOW_FLOOR is authoritative — unset/=0 forces
    // 0.0 (shader max(shadow, 0*blend) => no-op, byte-identical). When set to a
    // non-zero float the member value (ImGui-tunable) is uploaded, bounded 0..1.
    float effectiveCliffFloor = 0.0f;
    if (mc2::gate::get<"MC2_TERRAIN_CLIFF_SHADOW_FLOOR">()) {
        effectiveCliffFloor = cliffShadowFloor;
        if (effectiveCliffFloor < 0.0f) effectiveCliffFloor = 0.0f;
        if (effectiveCliffFloor > 1.0f) effectiveCliffFloor = 1.0f;
    }
    if (cliffShadowFloorLoc >= 0) {
        glUniform1f(cliffShadowFloorLoc, effectiveCliffFloor);
    }
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

	// MERGE-CONFLICT-UI-PHASE1: theirs (ui-phase1 fork) wanted to move this
	// `gosPostProcess* pp = getGosPostProcess(); if (!pp) return;` guard to
	// AFTER the terrainMVP upload block below, so that 3D camera views shown
	// during the logistics/menu phase (encyclopedia mech viewer, options
	// gameplay preview — added by the ui-phase1 GuiRuntime work) still get a
	// valid MVP uniform even when gosPostProcess hasn't been created yet
	// (pp is only created once a mission/scene is loaded). Ours has since
	// substantially rewritten the terrainMVP upload block itself
	// (OBJECT-DECAL-MATRIX-SHARE-1 / OBJECT-SHADOW-MVP-CURRENCY-1: routes
	// object MVP through gos_GetObjectDrawMVP with a view-epoch check +
	// MC2_PROP_FIXB_MVP killswitch, rather than always using
	// getTerrainMVP() directly) — the exact lines theirs' hunk reorders.
	// Reordering the pp-null-check without re-verifying that rewritten
	// block's assumptions (e.g. whether gos_GetObjectDrawMVP or
	// g_gos_renderer state depends on pp already existing) is exactly the
	// kind of renderer-internals change this merge must not blind-apply.
	// Preserving ours' early-return-before-MVP-upload ordering as-is;
	// theirs' pp-guard-after-MVP reordering was NOT applied. If the
	// encyclopedia/options 3D preview needs a valid MVP without
	// gosPostProcess, this is the seam to revisit — verify the rewritten
	// MVP block tolerates pp==nullptr before moving the guard.
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
			static const bool s_objShadowFixB = mc2::gate::get<"MC2_PROP_FIXB_MVP">();
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
// VK-SHADOW-COPY-ONCE-1: generation counter bumped each time the static
// shadow map is (re)built (mission load / re-arm). 0 while never built.
uint64_t gos_GetStaticShadowGeneration() {
    gosPostProcess* pp = getGosPostProcess();
    return pp ? pp->staticShadowGeneration() : 0;
}
bool gos_GetStaticShadowSpec(StaticShadowSpec* out) {
    gosPostProcess* pp = getGosPostProcess();
    if (!out || !pp || !pp->shadowsEnabled_ || !pp->staticLightMatrixBuilt() || pp->staticShadowGeneration() == 0) return false;
    std::memcpy(out->lightSpace, pp->getLightSpaceMatrix(), sizeof(out->lightSpace));
    out->size = pp->getShadowMapSize();
    out->biasFactor = pp->getShadowBiasFactor();
    out->biasUnits = pp->getShadowBiasUnits();
    out->generation = pp->staticShadowGeneration();
    return out->size > 0;
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
