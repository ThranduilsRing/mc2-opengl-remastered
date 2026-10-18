// GpuStaticPropBatcher: prop shadow helpers, flushShadow, static-building and dynamic-prop shadow draws and the VK shadow-island geometry feed.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"
#include "view_uniforms_gl.h"   // RenderCore::getCurrentView: the eye position of the VK prop specular
#include "../../RenderCore/FramePackets.h"


// SHADOW-PROP-ALPHA-1 -------------------------------------------------------
// Foliage/tree cards are textured quads with a binary-alpha leaf cutout. The
// legacy prop shadow caster used the empty depth frag (shadow_instanced.frag,
// `void main(){}`) so the WHOLE quad wrote depth -> square tree shadows. The
// new "shadow_static_prop_alpha" program (shadow_static_prop.vert forwarding
// a_uv + shadow_static_prop.frag) alpha-tests the card so the cast silhouette
// matches the visible foliage. MC2_SHADOW_PROP_ALPHA=0 = A/B kill switch
// (revert to the empty-frag program).
static bool shadowPropAlphaEnabled() {
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_PROP_ALPHA">();
    return s_on;
}

// SHADOW-PROP-ALPHA-ATLAS-1: kill switch for routing the alpha-tested prop
// shadow through the coalesce/BC7-atlas sampling path (default ON). =0 reverts
// to the legacy per-texture binds (pre-slice behavior: atlas trees cast solid
// square shadows when their legacy gos texture is dead/stale).
static bool shadowPropAlphaAtlasEnabled() {
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_PROP_ALPHA_ATLAS">();
    return s_on;
}

// Resolve the prop shadow caster program. Returns the program handle and sets
// outIsAlpha=true iff we landed on an alpha-test program (so the caller knows
// to do the per-packet texture/material binds) and outIsAtlas=true iff it is
// the SHADOW-PROP-ALPHA-ATLAS-1 array-sampling variant (per-packet layer/
// uvScale binds instead of the legacy texture handle). Falls back one rung at
// a time: atlas -> legacy-texture alpha -> empty-frag.
static GLuint resolvePropShadowProgram(bool& outIsAlpha, bool& outIsAtlas) {
    outIsAlpha = false;
    outIsAtlas = false;
    if (shadowPropAlphaEnabled()) {
        // Atlas variant only when the color pass actually draws from the
        // coalesce arrays this session (IsCoalesceEnabled) AND the per-packet
        // sidecars were persisted by finalizeGeometry (size lockstep with
        // s_packets) -- otherwise the legacy binds are the color-parity path.
        if (shadowPropAlphaAtlasEnabled() && IsCoalesceEnabled() &&
            s_packetTexArrayLayer.size() == s_packets.size() &&
            s_packetUvScaleX.size()     == s_packets.size() &&
            s_packetUvScaleY.size()     == s_packets.size()) {
            auto at = glsl_program::s_programs.find("shadow_static_prop_alpha_atlas");
            if (at != glsl_program::s_programs.end() && at->second && at->second->shp_) {
                outIsAlpha = true;
                outIsAtlas = true;
                return at->second->shp_;
            }
        }
        auto a = glsl_program::s_programs.find("shadow_static_prop_alpha");
        if (a != glsl_program::s_programs.end() && a->second && a->second->shp_) {
            outIsAlpha = true;
            return a->second->shp_;
        }
    }
    auto pit = glsl_program::s_programs.find("shadow_static_prop");
    if (pit == glsl_program::s_programs.end() || !pit->second || !pit->second->shp_)
        return 0;
    return pit->second->shp_;
}

// Per-program u_tex / u_debugAddrMode setup for the alpha-test prop shadow
// program. Sets the sampler unit (0) + mode-8 bypass parity once after
// glUseProgram. Cheap (called once per pass).
static void setupPropShadowAlphaUniforms(GLuint prog, int debugAddrMode) {
    const GLint locTex = glGetUniformLocation(prog, "u_tex");
    if (locTex >= 0) glUniform1i(locTex, 0);
    // SHADOW-PROP-ALPHA-ATLAS-1: the atlas variant declares u_texArr instead
    // of u_tex; same texture unit 0. -1 lookups no-op, so this helper serves
    // both programs.
    const GLint locTexArr = glGetUniformLocation(prog, "u_texArr");
    if (locTexArr >= 0) glUniform1i(locTexArr, 0);
    const GLint locDbg = glGetUniformLocation(prog, "u_debugAddrMode");
    if (locDbg >= 0) glUniform1i(locDbg, debugAddrMode);
}

// Bind the per-packet foliage texture (unit 0, REPEAT/LINEAR) + u_materialFlags
// for the alpha-test prop shadow program. Mirrors the LEGACY color flush loop
// (gos_static_prop_batcher.cpp ~7042-7106). Bind ONCE per packet. Opaque
// packets (ALPHA_TEST clear) still bind so the shader's `u_materialFlags &
// ALPHA_TEST_BIT` gate skips the sample correctly.
//
// NOTE: glTexId + effFlags are resolved by the CALLER (a GpuStaticPropBatcher
// member, which is a `friend` of TG_TypeShape and so may read the protected
// listOfTextures/numTextures — this free helper is not a friend).
static void bindPropShadowAlphaPacket(GLuint prog, uint32_t glTexId,
                                      uint32_t effFlags) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glTexId);
    // Sampler-state inheritance trap: the shadow pass binds no sampler object,
    // so the inherited GL_TEXTURE_2D wrap may be CLAMP (corrupting atlas-edge
    // alpha) — match the color path's explicit REPEAT.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    const GLint locFlags = glGetUniformLocation(prog, "u_materialFlags");
    if (locFlags >= 0) glUniform1i(locFlags, (int)effFlags);
}

// SHADOW-PROP-ALPHA-ATLAS-1: per-packet bind for the atlas-variant prop shadow
// program. Resolves the SAME array + layer the coalesce color pass samples for
// this packet (BC7 bucket array when the BC7 gate built buckets for it, else
// the alpha-group array selected by the type's alphaClass) and uploads the
// per-packet layer + uv sub-region scale. If the packet's array/layer cannot
// be resolved (layer -1 sentinel, missing array), the alpha bit is CLEARED so
// the packet depth-writes opaquely -- i.e. exactly today's square-shadow
// behavior for that packet, never a wrong-texture cutout. Array sampler state
// is deliberately NOT touched (shared with the color pass; group/bucket arrays
// are CLAMP_TO_EDGE + the shader's fract(v_uv)*uvScale handles tiling).
static void bindPropShadowAtlasPacket(GLuint prog,
                                      const GpuStaticPropType& type,
                                      uint32_t globalPktIdx,
                                      uint32_t effFlags) {
    GLuint  arr   = 0;
    int32_t layer = (globalPktIdx < s_packetTexArrayLayer.size())
                        ? s_packetTexArrayLayer[globalPktIdx] : -1;
    if (layer >= 0) {
        if (s_staticPropBc7Enabled &&
            globalPktIdx < s_packetBucketIndex.size() &&
            s_packetBucketIndex[globalPktIdx] >= 0 &&
            static_cast<size_t>(s_packetBucketIndex[globalPktIdx]) < s_bucketArrays.size()) {
            arr = s_bucketArrays[static_cast<size_t>(s_packetBucketIndex[globalPktIdx])];
        } else {
            arr = (type.alphaClass != 0u) ? s_texArrayOn : s_texArrayOff;
        }
    }
    if (arr == 0 || layer < 0) {
        // Unresolvable: opaque depth write (today's behavior), no sample.
        effFlags &= ~static_cast<uint32_t>(STATIC_PROP_FLAG_ALPHA_TEST);
        const GLint locFlags = glGetUniformLocation(prog, "u_materialFlags");
        if (locFlags >= 0) glUniform1i(locFlags, (int)effFlags);
        return;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
    const GLint locLayer = glGetUniformLocation(prog, "u_texLayer");
    if (locLayer >= 0) glUniform1i(locLayer, (int)layer);
    const GLint locScale = glGetUniformLocation(prog, "u_uvScale");
    if (locScale >= 0) glUniform2f(locScale,
                                   s_packetUvScaleX[globalPktIdx],
                                   s_packetUvScaleY[globalPktIdx]);
    const GLint locFlags = glGetUniformLocation(prog, "u_materialFlags");
    if (locFlags >= 0) glUniform1i(locFlags, (int)effFlags);
}

static int s_shadowTypesDrawn = 0;
static int s_shadowInstDrawn  = 0;

void GpuStaticPropBatcher::flushShadow() {
    // Non-indirect, full-range, depth-only draw for GPU static props.
    // Uses the shadow_static_prop program (Task 1) and the same per-frame
    // SSBO upload as flush() (shared via uploadAllBucketsIfNeeded /
    // s_lastUploadedSlot). Never uses compute_getIndirectCmdBuf() --
    // the indirect buffer carries camera-cull-narrowed counts and must
    // not be used for a shadow pass that needs all instances.
    //
    // Spec §6.Y: IsCoalesceEnabled() is irrelevant for the shadow path;
    // the coalesce program uses gl_DrawIDARB / gl_BaseInstanceARB which
    // is not available here. Always use the legacy non-indirect path.

    // Geometry-readiness guard, mirroring the color flush() path (:2749).
    // The new txmmgr shadow region calls this from frame ~1; before
    // finalizeGeometry() s_sharedVao==0 and s_packets is empty while
    // s_typeRanges/type.packetCount can already be non-zero, so the
    // per-packet loop below would index an empty s_packets and fault in
    // glDrawElementsInstancedBaseVertex. flush() bails on the same
    // condition; flushShadow() must honor the identical precondition.
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return;

    // DEFAULT ON. Kill-switch: MC2_SHADOW_ENABLE=0.
    static const bool s_shadowEnabled = mc2::gate::get<"MC2_SHADOW_ENABLE">();
    if (!s_shadowEnabled) return;

    if (!uploadAllBucketsIfNeeded()) return;

    // SHADOW-PROP-ALPHA-1: prefer the alpha-tested prop shadow program so
    // foliage/tree cards cast a leaf-shaped (not square) silhouette. Falls back
    // to the empty-frag program when MC2_SHADOW_PROP_ALPHA=0 or the alpha
    // program failed to link. flushShadow is the camera-visible static-prop sun
    // shadow caster (it routes tree types), so it is the primary fix site.
    // SHADOW-PROP-ALPHA-ATLAS-1: when coalesce is armed the atlas variant is
    // preferred so the shadow alpha-test samples the same group/bucket array
    // as the color pass (usingAtlasProg -> per-packet layer/uvScale binds).
    bool usingAlphaProg = false;
    bool usingAtlasProg = false;
    const GLuint shadowProg = resolvePropShadowProgram(usingAlphaProg, usingAtlasProg);
    if (shadowProg == 0) return;
    // One-shot arming trace (proof hook for SHADOW-PROP-ALPHA-ATLAS-1 A/B).
    {
        static bool s_atlasTraceOnce = false;
        if (usingAtlasProg && !s_atlasTraceOnce) {
            s_atlasTraceOnce = true;
            MC2_DIAGFE(stderr, "[SHADOW_PROP_ATLAS v1] event=engaged pass=flushShadow "
                                 "packets=%zu buckets=%zu bc7=%d\n",
                         s_packets.size(), s_bucketArrays.size(),
                         s_staticPropBc7Enabled ? 1 : 0);
        }
    }

    if (s_typeRanges.empty()) return;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Save/restore the GL state this pass perturbs, mirroring flush()'s
    // bracket (:2844-2854 / :3509-3521). flushShadow runs BEFORE
    // gpu_cull::compute_dispatch() and the main flush(); leaking the
    // program / VAO / element-buffer / SSBO binding-0 here poisons the
    // cull dispatch and the indirect draw -> the entire prop+mech render
    // is suppressed (the invisible-casters regression). All early returns
    // above are before any GL mutation, so a single capture here + restore
    // at the one exit is correct.
    // GLSTATE-GUARD-SITE5: capture-only guards replace the hand-rolled
    // prevProgram/prevVao/prevElemBuf/prevSsbo0 save + restore below. No
    // gos_InvalidateRenderStateCache() in this function and the single exit
    // is at the function's natural end, so function scope is correct (mirrors
    // GpuMechBatcher::flushShadow(), gos_mech_batcher.cpp:1012-1029). Declared
    // in reverse of the restore order the manual epilogue used (ssbo0, VAO,
    // elem-buffer, program — VAO before elem-buffer per the header's ordering
    // note) so scope-exit destruction restores identically.
    mc2gl::GlScopedProgram       guardProgram;
    mc2gl::GlScopedBufferBinding guardElemBuf(GL_ELEMENT_ARRAY_BUFFER);
    mc2gl::GlScopedVertexArray   guardVao;
    mc2gl::GlScopedSsboBinding   guardSsbo0(0);

    glUseProgram(shadowProg);

    // SSBO-BIND-ALIGN: flushShadow binds a per-type RANGE (r.instanceByteOffset) with
    // gl_InstanceID relative to the range start, so the shared u_instBase uniform
    // (added in shadow_static_prop.vert for the building/dynamic WHOLE-buffer passes)
    // MUST be 0 here. Uniforms persist across glUseProgram, so without this reset
    // flushShadow inherits a stale non-zero base left by drawDynamicPropShadows ->
    // instances_.i[staleBase + gl_InstanceID] reads out of range -> shadows garbled.
    { const GLint ibLoc = glGetUniformLocation(shadowProg, "u_instBase");
      if (ibLoc >= 0) glUniform1i(ibLoc, 0); }

    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());

    // SHADOW-PROP-ALPHA-1: one-time u_tex(=0) + u_debugAddrMode(mode-8 bypass)
    // setup for the alpha-test program. No-op for the empty-frag fallback.
    if (usingAlphaProg) setupPropShadowAlphaUniforms(shadowProg, debugAddrMode_);

    glBindVertexArray(s_sharedVao);
    // Root cause of the frame-2 0x2A18 crash: the GPU-cull/indirect flush()
    // path runs between this frame's and last frame's flushShadow and leaves
    // s_sharedVao's VAO-resident GL_ELEMENT_ARRAY_BUFFER binding at 0. An
    // indexed draw with no element buffer treats firstIndex*4 as a client
    // pointer (2694*4 == 0x2A18) and faults. Do NOT rely on the VAO carrying
    // the IBO; bind s_sharedIbo explicitly (also repairs the VAO binding).
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    const bool s_shDiag = mc2::gate::get<"MC2_SHADOW_DIAG">();
    if (s_shDiag) {
        GLint elemBuf = -1, linkOk = -1;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
        glGetProgramiv(shadowProg, GL_LINK_STATUS, &linkOk);
        GLenum ePre = glGetError();
        mc2::fmt::print_err("[SHADOW_DIAG] sp finalized={:d} vao={:d} isVao={:d} ssbo={:d} isBuf={:d} elemBind={:d} prog={:d} link={:d} errPre=0x{:x} typeRanges={:d} types={:d} packets={:d}\n", mc2::fmt::as_i32((int)s_geometryFinalized), mc2::fmt::as_u32(s_sharedVao), mc2::fmt::as_i32((int)glIsVertexArray(s_sharedVao)), mc2::fmt::as_u32(s_instanceSsbo), mc2::fmt::as_i32((int)glIsBuffer(s_instanceSsbo)), mc2::fmt::as_i32(elemBuf), mc2::fmt::as_u32(shadowProg), mc2::fmt::as_i32(linkOk), mc2::fmt::as_u32((unsigned)ePre), mc2::fmt::as_size(s_typeRanges.size()), mc2::fmt::as_size(s_types.size()), mc2::fmt::as_size(s_packets.size()));
        fflush(stderr);
    }

    int typesDrawn = 0;
    int instDrawn  = 0;

    for (uint32_t typeID = 0; typeID < static_cast<uint32_t>(s_types.size()); ++typeID) {
        auto rit = s_typeRanges.find(typeID);
        if (rit == s_typeRanges.end()) continue;
        const TypeRangeSsbo& r = rit->second;
        const GpuStaticPropType& type = s_types[typeID];
        if (r.instanceCount == 0 || type.packetCount == 0) continue;
        // SHADOW-STATIC-BUILDINGS-2: skip building types in the DYNAMIC pass — they
        // already cast a crisp shadow via the world-fixed static map; the dynamic copy
        // is a redundant, fuzzy double-shadow. Trees/mechs/other props still cast dynamically.
        if (s_buildingShadowTypeIDs.count(typeID)) continue;

        // Bind the per-type instance SSBO range (binding 0 = Instances block
        // in shadow_static_prop.vert, indexed by gl_InstanceID).
        gpuBindSsboRange(0, s_instanceSsbo,
                         (long long)r.instanceByteOffset, (long long)r.instanceByteSize,
                         "sp.instance.shadow");

        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const uint32_t pktIdx = type.firstPacket + p;
            if (pktIdx >= s_packets.size()) break;  // defense-in-depth: stale range
            const GpuStaticPropPacket& pkt = s_packets[pktIdx];
            // SHADOW-PROP-ALPHA-1: bind the foliage texture + material flags so
            // the alpha-test frag can cut the leaf silhouette (legacy color
            // flush parity). Skipped under the empty-frag fallback. Resolution
            // is inline here (member = TG_TypeShape friend) so the protected
            // listOfTextures/numTextures stay accessible.
            if (usingAlphaProg) {
                const TG_TypeShape* shSrc = type.source;
                uint32_t shEffFlags = pkt.materialFlags;
                if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures &&
                    shSrc->listOfTextures[pkt.textureSlot].textureAlpha)
                    shEffFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
                if (usingAtlasProg) {
                    // SHADOW-PROP-ALPHA-ATLAS-1: sample the coalesce array the
                    // color pass draws from (per-packet layer/uvScale).
                    bindPropShadowAtlasPacket(shadowProg, type, pktIdx, shEffFlags);
                } else {
                    uint32_t shGosHandle = 0;
                    if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures)
                        shGosHandle = shSrc->listOfTextures[pkt.textureSlot].gosTextureHandle;
                    bindPropShadowAlphaPacket(shadowProg, gos_GetGLTextureId(shGosHandle), shEffFlags);
                }
            }
            if (s_shDiag) {
                mc2::fmt::print_err("[SHADOW_DIAG] sp draw type={:d} byteOff={:d} byteSize={:d} instCnt={:d} firstPkt={:d} pktCnt={:d} pktIdx={:d} idxCnt={:d} firstIdx={:d} baseV={:d} errPre=0x{:x}\n", mc2::fmt::as_u32(typeID), mc2::fmt::as_u64((unsigned long long)r.instanceByteOffset), mc2::fmt::as_u64((unsigned long long)r.instanceByteSize), mc2::fmt::as_u32(r.instanceCount), mc2::fmt::as_u32(type.firstPacket), mc2::fmt::as_u32(type.packetCount), mc2::fmt::as_u32(pktIdx), mc2::fmt::as_u32(pkt.indexCount), mc2::fmt::as_u32(pkt.firstIndex), mc2::fmt::as_i32(pkt.baseVertex), mc2::fmt::as_u32((unsigned)glGetError()));
                fflush(stderr);
            }
            glDrawElementsInstancedBaseVertex(
                GL_TRIANGLES,
                static_cast<GLsizei>(pkt.indexCount),
                GL_UNSIGNED_INT,
                reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                static_cast<GLsizei>(r.instanceCount),
                pkt.baseVertex);
        }

        ++typesDrawn;
        instDrawn += static_cast<int>(r.instanceCount);
    }

    s_shadowTypesDrawn = typesDrawn;
    s_shadowInstDrawn  = instDrawn;

    // GL state restored by the guards' destructors at scope exit (below), in
    // ssbo0 -> VAO -> elem-buffer -> program order — VAO before element-
    // buffer so glBindBuffer(ELEMENT_ARRAY_BUFFER, ...) writes into the
    // restored VAO's state, not into s_sharedVao's. compute_dispatch()/
    // flush() rely on this exact order (see GlScopedVertexArray's header
    // comment); reversing it leaves s_sharedVao.elemBuf=0 and flush() faults
    // (firstIndex*4 read as a client pointer, e.g. 0x2A18).
}

// ---------------------------------------------------------------------------
// SHADOW-STATIC-BUILDINGS-2: world-fixed static building shadow caster pass.
// `instances` = all registered rigid-building recipe leaves (from the registry,
// visibility-INDEPENDENT). Depth-only; shadow_static_prop reads only modelMatrix
// and GpuStaticPropInstance is binary-compatible with its Instance block, so the
// registry records upload directly. Uses the STATIC world-fixed light matrix.
// ---------------------------------------------------------------------------
// s_staticBldgShadowSsbo declared near s_perTypeSsbo (so onMapUnload can free it).
static int    s_staticBldgShadowTypes = 0;
static int    s_staticBldgShadowInst  = 0;
static int    s_staticBldgShadowDraws = 0;

void GpuStaticPropBatcher::drawStaticBuildingShadows(
        const std::vector<GpuStaticPropInstance>& instances) {
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return;
    if (instances.empty()) return;

    auto pit = glsl_program::s_programs.find("shadow_static_prop");
    if (pit == glsl_program::s_programs.end() || !pit->second || !pit->second->shp_)
        return;
    const GLuint shadowProg = pit->second->shp_;

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Group leaves into contiguous per-typeID runs so each type draws from one
    // SSBO range with gl_InstanceID 0..n-1. Sort an INDEX array, not the struct
    // array: GpuStaticPropInstance is alignas(16) (over-aligned), and
    // std::stable_sort's temp buffer trips MSVC's std::aligned_storage extended-
    // alignment static_assert (C2338). Index-sort (uint32_t) + gather avoids it.
    std::vector<uint32_t> order(instances.size());
    for (uint32_t k = 0; k < static_cast<uint32_t>(order.size()); ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [&instances](uint32_t a, uint32_t b) {
            return instances[a].typeID < instances[b].typeID;
        });
    std::vector<GpuStaticPropInstance> sorted;
    sorted.reserve(instances.size());
    for (uint32_t idx : order) sorted.push_back(instances[idx]);

    // Rebuild the building-typeID set so the dynamic pass can skip these (they
    // cast via this crisp static map; the dynamic copy is redundant + fuzzy).
    s_buildingShadowTypeIDs.clear();

    // Save the GL state this pass perturbs (mirror flushShadow's bracket) so the
    // subsequent dynamic shadow pass + main flush see the state they expect.
    // GLSTATE-GUARD-SITE5: capture-only guards, same shape/ordering rationale
    // as flushShadow() above.
    mc2gl::GlScopedProgram       guardProgram;
    mc2gl::GlScopedBufferBinding guardElemBuf(GL_ELEMENT_ARRAY_BUFFER);
    mc2gl::GlScopedVertexArray   guardVao;
    mc2gl::GlScopedSsboBinding   guardSsbo0(0);

    // TIER2-EXCLUDED: substrate-gated
    if (s_staticBldgShadowSsbo == 0) glGenBuffers(1, &s_staticBldgShadowSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_staticBldgShadowSsbo);
    // GPU-UPDATE-BUFFER-COUNTER-1: per-render transient shadow SSBO orphan-on-write.
    MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER,
        static_cast<GLsizeiptr>(sorted.size() * sizeof(GpuStaticPropInstance)),
        sorted.data(), GL_STATIC_DRAW, StaticPropShadow);

    glUseProgram(shadowProg);
    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getLightSpaceMatrix()); // static matrix

    glBindVertexArray(s_sharedVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Contact-acne bias (the static terrain prepass sets none).
    // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: enable polygon offset via the
    // ShadowStaticProp pipeline row (polygonOffsetEnable=true). applyPipeline also
    // re-asserts the identical shadow base (depth LESS, cull none, frontFace ccw,
    // depthMask) — a redundant no-op vs the bracket. glProgramName=0 → the bound
    // shadow program is untouched. The MAGNITUDE below and the teardown glDisable
    // stay exactly where they are (factor/units ownership unchanged).
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowStaticProp), "ShadowStaticProp");
    glPolygonOffset(pp->shadowBiasFactor_, pp->shadowBiasUnits_);

    int typesDrawn = 0, instDrawn = 0, drawCalls = 0;
    size_t i = 0;
    while (i < sorted.size()) {
        const uint32_t tid = sorted[i].typeID;
        size_t j = i;
        while (j < sorted.size() && sorted[j].typeID == tid) ++j;
        const uint32_t instCount = static_cast<uint32_t>(j - i);
        if (tid < static_cast<uint32_t>(s_types.size())) {
            const GpuStaticPropType& type = s_types[tid];
            if (type.packetCount > 0) {
                // SSBO-BIND-ALIGN: bind the WHOLE buffer at offset 0 (always aligned)
                // and pass the per-type instance base as a uniform. A range bind at
                // i*sizeof(GpuStaticPropInstance) (=i*112) is not a multiple of
                // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT (32 on NVIDIA) -> rejected
                // with GL_INVALID_VALUE -> the SSBO never binds -> shadow casters drop.
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_staticBldgShadowSsbo);
                { const GLint locB = glGetUniformLocation(shadowProg, "u_instBase");
                  if (locB >= 0) glUniform1i(locB, (GLint)i); }
                for (uint32_t p = 0; p < type.packetCount; ++p) {
                    const uint32_t pk = type.firstPacket + p;
                    if (pk >= s_packets.size()) break;
                    const GpuStaticPropPacket& pkt = s_packets[pk];
                    glDrawElementsInstancedBaseVertex(
                        GL_TRIANGLES, static_cast<GLsizei>(pkt.indexCount), GL_UNSIGNED_INT,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                        static_cast<GLsizei>(instCount), pkt.baseVertex);
                    ++drawCalls;
                }
                ++typesDrawn;
                instDrawn += static_cast<int>(instCount);
                s_buildingShadowTypeIDs.insert(tid);  // skip in the dynamic pass
            }
        }
        i = j;
    }

    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);

    // GL state restored by the guards' destructors at scope exit below
    // (ssbo0, VAO, elem, program — same order as flushShadow).

    s_staticBldgShadowTypes = typesDrawn;
    s_staticBldgShadowInst  = instDrawn;
    s_staticBldgShadowDraws = drawCalls;
}

// SHADOW-STABILITY-1: read-only accessor so the shadow-pass state trace
// (gameos_graphics.cpp) can emit registry-driven static-building caster
// counts without duplicating the per-element counting. Reuses the latched
// totals from the last drawStaticBuildingShadows; zero when the gate is off.
void gos_GetStaticBuildingShadowCounts(int& types, int& inst, int& draws) {
    types = s_staticBldgShadowTypes;
    inst  = s_staticBldgShadowInst;
    draws = s_staticBldgShadowDraws;
}

// ---------------------------------------------------------------------------
// SHADOW-DYNAMIC-PROP-CASTERS-1: depth-only draw of registry-supplied NON-building
// prop leaves into the currently-bound DYNAMIC shadow FBO, using the per-frame
// dynamic light matrix. Mirror of drawStaticBuildingShadows EXCEPT: (a) binds
// getDynamicLightSpaceMatrix() not the static matrix; (b) uses its own per-frame
// SSBO (s_dynamicPropShadowSsbo, GL_DYNAMIC_DRAW — rebuilt every frame); (c) does
// NOT touch s_buildingShadowTypeIDs. Replaces flushShadow's camera-visible
// s_typeRanges feed for props so EVERY registered prop casts, fixing "only
// near-camera trees shadow". Caller brackets with gos_BeginDynamicShadowPass().
static int s_dynPropShadowTypes = 0;
static int s_dynPropShadowInst  = 0;
static int s_dynPropShadowDraws = 0;

void GpuStaticPropBatcher::drawDynamicPropShadows(
        const std::vector<GpuStaticPropInstance>& instances) {
    s_dynPropShadowTypes = 0; s_dynPropShadowInst = 0; s_dynPropShadowDraws = 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return;
    if (instances.empty()) return;
    // MECH-BAY-SHADOW-CRASH (2026-06-19): skip the terrain static-prop dynamic-shadow
    // caster replay during a SimpleCamera UI preview (mech bay / purchase / mechlab).
    // After a mission ends, the prop registry still holds the prior mission's
    // instances, but their TG_TypeShape sources (type.source) are freed when the bay
    // loads preview shapes -> dereferencing type.source->listOfTextures here faults
    // (READ at 0xFFFF... in MechBayScreen::render). The preview only needs the mech's
    // own shadow (GpuMechBatcher::flushShadow), not terrain props. Crash repro:
    // ClanEagle, win mission, enter mech bay.
    if (g_mechPreviewRenderDepth > 0) return;

    // SHADOW-PROP-ALPHA-1: alpha-tested prop shadow program (foliage silhouette)
    // with empty-frag fallback. Full-registry dynamic prop caster.
    // SHADOW-PROP-ALPHA-ATLAS-1: atlas variant preferred when coalesce is armed
    // (same resolution ladder as flushShadow).
    bool usingAlphaProg = false;
    bool usingAtlasProg = false;
    const GLuint shadowProg = resolvePropShadowProgram(usingAlphaProg, usingAtlasProg);
    if (shadowProg == 0) return;

    // flushShadow can run before coalesce reaches READY, while this full-registry
    // replay runs after the color dispatch has armed it. Keep a proof hook here
    // too so the default atlas path is observable even when the earlier pass
    // correctly used the legacy-texture fallback.
    {
        static bool s_atlasTraceOnce = false;
        if (usingAtlasProg && !s_atlasTraceOnce) {
            s_atlasTraceOnce = true;
            MC2_DIAGFE(stderr, "[SHADOW_PROP_ATLAS v1] event=engaged pass=dynamicProp "
                                 "packets=%zu buckets=%zu bc7=%d\n",
                         s_packets.size(), s_bucketArrays.size(),
                         s_staticPropBc7Enabled ? 1 : 0);
        }
    }

    gosPostProcess* pp = getGosPostProcess();
    if (!pp) return;

    // Group leaves into contiguous per-typeID runs (index-sort + gather; the
    // struct is over-aligned alignas(16) so std::stable_sort on it trips MSVC's
    // aligned_storage static_assert — same constraint as drawStaticBuildingShadows).
    std::vector<uint32_t> order(instances.size());
    for (uint32_t k = 0; k < static_cast<uint32_t>(order.size()); ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
        [&instances](uint32_t a, uint32_t b) {
            return instances[a].typeID < instances[b].typeID;
        });
    std::vector<GpuStaticPropInstance> sorted;
    sorted.reserve(instances.size());
    for (uint32_t idx : order) sorted.push_back(instances[idx]);

    // Save the GL state this pass perturbs (mirror flushShadow's bracket).
    // GLSTATE-GUARD-SITE5: capture-only guards, same shape/ordering rationale
    // as flushShadow() above.
    mc2gl::GlScopedProgram       guardProgram;
    mc2gl::GlScopedBufferBinding guardElemBuf(GL_ELEMENT_ARRAY_BUFFER);
    mc2gl::GlScopedVertexArray   guardVao;
    mc2gl::GlScopedSsboBinding   guardSsbo0(0);

    if (s_dynamicPropShadowSsbo == 0) {
        GLuint dynShadowGen = 0;
        glGenBuffers(1, &dynShadowGen);
        s_dynamicPropShadowOwner.backendHandle = dynShadowGen;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_dynamicPropShadowSsbo);
    // GPU-UPDATE-BUFFER-COUNTER-1: per-frame transient shadow SSBO orphan-on-write.
    const GLsizeiptr dynShadowBytes =
        static_cast<GLsizeiptr>(sorted.size() * sizeof(GpuStaticPropInstance));
    MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER,
        dynShadowBytes,
        sorted.data(), GL_DYNAMIC_DRAW, StaticPropShadow);   // rebuilt every frame
    {
        // SCENE-SSBO-OWNER-SWEEP-1: (re)register the live dynamic prop-shadow SSBO
        // (observe-only metadata; never read by the draw path). Registered at create;
        // glGenBuffers above allocates the handle once per map (Mission lifetime), and
        // the per-frame orphaning bufferData refreshes the size without a new handle.
        RenderCore::RenderResourceDesc d;
        d.id        = s_dynamicPropShadowOwner.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_dynamicPropShadowOwner.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_dynamicPropShadowOwner.debugName;
        d.glName    = static_cast<GLuint>(s_dynamicPropShadowOwner.backendHandle);
        d.sizeBytes = static_cast<uint64_t>(dynShadowBytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    glUseProgram(shadowProg);
    const GLint lsLoc = glGetUniformLocation(shadowProg, "lightSpaceMatrix");
    if (lsLoc >= 0)
        glUniformMatrix4fv(lsLoc, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix()); // DYNAMIC matrix

    // SHADOW-PROP-ALPHA-1: one-time u_tex(=0)+u_debugAddrMode setup (alpha prog).
    if (usingAlphaProg) setupPropShadowAlphaUniforms(shadowProg, debugAddrMode_);

    glBindVertexArray(s_sharedVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);

    // Contact-acne bias (same as the building static pass).
    // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: enable polygon offset via the
    // ShadowStaticProp pipeline row (polygonOffsetEnable=true). Re-asserts the
    // identical shadow base (redundant no-op). MAGNITUDE below + teardown glDisable
    // stay put (factor/units ownership unchanged); glProgramName=0 keeps the bound
    // shadow program.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::ShadowStaticProp), "ShadowStaticProp");
    glPolygonOffset(pp->shadowBiasFactor_, pp->shadowBiasUnits_);

    int typesDrawn = 0, instDrawn = 0, drawCalls = 0;
    size_t i = 0;
    while (i < sorted.size()) {
        const uint32_t tid = sorted[i].typeID;
        size_t j = i;
        while (j < sorted.size() && sorted[j].typeID == tid) ++j;
        const uint32_t instCount = static_cast<uint32_t>(j - i);
        if (tid < static_cast<uint32_t>(s_types.size())) {
            const GpuStaticPropType& type = s_types[tid];
            if (type.packetCount > 0) {
                // SSBO-BIND-ALIGN: bind whole buffer at offset 0 (aligned) + per-type
                // base uniform, instead of a misaligned i*112 range bind (see the
                // building shadow pass above; NVIDIA rejects misaligned SSBO offsets).
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s_dynamicPropShadowSsbo);
                { const GLint locB = glGetUniformLocation(shadowProg, "u_instBase");
                  if (locB >= 0) glUniform1i(locB, (GLint)i); }
                for (uint32_t p = 0; p < type.packetCount; ++p) {
                    const uint32_t pk = type.firstPacket + p;
                    if (pk >= s_packets.size()) break;
                    const GpuStaticPropPacket& pkt = s_packets[pk];
                    // SHADOW-PROP-ALPHA-1: per-packet foliage texture + flags so
                    // the alpha-test frag cuts the leaf silhouette. Inline
                    // resolution (member = TG_TypeShape friend).
                    if (usingAlphaProg) {
                        const TG_TypeShape* shSrc = type.source;
                        uint32_t shEffFlags = pkt.materialFlags;
                        if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures &&
                            shSrc->listOfTextures[pkt.textureSlot].textureAlpha)
                            shEffFlags |= STATIC_PROP_FLAG_ALPHA_TEST;
                        if (usingAtlasProg) {
                            // SHADOW-PROP-ALPHA-ATLAS-1: same array/layer the
                            // coalesce color pass samples for this packet.
                            bindPropShadowAtlasPacket(shadowProg, type, pk, shEffFlags);
                        } else {
                            uint32_t shGosHandle = 0;
                            if (shSrc && shSrc->listOfTextures && pkt.textureSlot < shSrc->numTextures)
                                shGosHandle = shSrc->listOfTextures[pkt.textureSlot].gosTextureHandle;
                            bindPropShadowAlphaPacket(shadowProg, gos_GetGLTextureId(shGosHandle), shEffFlags);
                        }
                    }
                    glDrawElementsInstancedBaseVertex(
                        GL_TRIANGLES, static_cast<GLsizei>(pkt.indexCount), GL_UNSIGNED_INT,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                        static_cast<GLsizei>(instCount), pkt.baseVertex);
                    ++drawCalls;
                }
                ++typesDrawn;
                instDrawn += static_cast<int>(instCount);
            }
        }
        i = j;
    }

    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);

    // GL state restored by the guards' destructors at scope exit below
    // (ssbo0, VAO, elem, program — same order as flushShadow).

    s_dynPropShadowTypes = typesDrawn;
    s_dynPropShadowInst  = instDrawn;
    s_dynPropShadowDraws = drawCalls;
    if (mc2::gate::is<"MC2_SHADOW_DYNAMIC_PROP_CASTERS", "2">()) {
        mc2::fmt::print_err("[SHADOW_DYN_PROP v1] recipes_in={:d} types={:d} inst={:d} draws={:d} (registry props; full registry, NOT per-frame buckets)\n", mc2::fmt::as_size(instances.size()), mc2::fmt::as_i32(typesDrawn), mc2::fmt::as_i32(instDrawn), mc2::fmt::as_i32(drawCalls));
        fflush(stderr);
    }
}

void GpuStaticPropBatcher::setDebugAddrMode(int mode) { debugAddrMode_ = mode; }

// SHADOW-SPINE-0: read-only accessors for static-prop shadow caster counts
// written at the end of flushShadow(). Returns last-frame values; not reset.
extern "C" uint32_t gos_getStaticPropShadowTypesDrawn() {
    return (uint32_t)s_shadowTypesDrawn;
}
extern "C" uint32_t gos_getStaticPropShadowInstDrawn() {
    return (uint32_t)s_shadowInstDrawn;
}

// VK-P8a SHADOW-INTEROP: caster-geometry feed for the Vulkan shadow island. Exposes
// the finalized shared VBO/IBO staging bytes + per-type packet ranges so the island
// can copy them ONCE per map into VK-owned buffers (STEP 0 feed decision, fallback
// (B): VK owns a copy). Read-only; buffers persist until onMapUnload() clears the
// staging vectors. Returns false if geometry is not yet finalized.
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_shadow_island_live.h"
extern "C" int mc2_shadow_island_get_geometry_feed(Mc2ShadowGeomFeed* out) {
    if (!out) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return 0;
    if (s_sharedVboBytes == 0 || s_sharedIboBytes == 0 || s_packets.empty()) return 0;

    // Per-packet ranges (subset of GpuStaticPropPacket) + per-type packet spans.
    // Rebuilt when the packet/type count changes (finalize is once per map).
    static std::vector<Mc2ShadowPacket> s_feedPackets;
    static std::vector<uint32_t>        s_feedTypeFirstPacket;
    static std::vector<uint32_t>        s_feedTypePacketCount;
    static size_t s_feedPacketGen = SIZE_MAX;
    static size_t s_feedTypeGen   = SIZE_MAX;
    if (s_feedPacketGen != s_packets.size()) {
        s_feedPackets.resize(s_packets.size());
        for (size_t i = 0; i < s_packets.size(); ++i) {
            s_feedPackets[i].firstIndex = s_packets[i].firstIndex;
            s_feedPackets[i].indexCount = s_packets[i].indexCount;
            s_feedPackets[i].baseVertex = (uint32_t)s_packets[i].baseVertex;
        }
        s_feedPacketGen = s_packets.size();
    }
    if (s_feedTypeGen != s_types.size()) {
        s_feedTypeFirstPacket.resize(s_types.size());
        s_feedTypePacketCount.resize(s_types.size());
        for (size_t t = 0; t < s_types.size(); ++t) {
            s_feedTypeFirstPacket[t] = s_types[t].firstPacket;
            s_feedTypePacketCount[t] = s_types[t].packetCount;
        }
        s_feedTypeGen = s_types.size();
    }

    out->vboBytes        = s_sharedVboBytes;
    out->iboBytes        = s_sharedIboBytes;
    out->vertexStride    = (uint32_t)kVertexStride;
    out->packets         = s_feedPackets.data();
    out->packetCount     = (uint32_t)s_feedPackets.size();
    out->typeFirstPacket = s_feedTypeFirstPacket.data();
    out->typePacketCount = s_feedTypePacketCount.data();
    out->typeCount       = (uint32_t)s_types.size();
    return 1;
}

// VK-P11 STATIC-PROP COLOR — flatten the full static-prop instance set into a single
// GpuStaticPropInstance array grouped by typeID, and hand out the world->clip matrix
// + reverse-Z depth bias. Called by the color island ONLY when its gate is ON (the
// GL flush() is skipped in that case).
//
// CURRENT BEHAVIOUR (supersedes the history below): per type the feed is the
// persistent-store block FIRST, then this frame's dynamic submits from
// s_bucketsByType (excluding the reinjPos/reinjCount range copied from the store);
// mc2_color_prop_island_end_frame() performs flush()'s end-of-frame bookkeeping.
// The 'persistent store only' text below describes the original fix, kept for context.
//
// VK-PROP-COVERAGE FIX (source correction): originally this read s_bucketsByType.
// That map is a per-FRAME accumulator whose ONLY per-frame clear lives inside
// GpuStaticPropBatcher::flush() (line ~7661) — which is SKIPPED when the island owns
// the frame. So s_bucketsByType was simultaneously (a) unbounded-accumulating across
// island frames (measured on mc2_01: feed_total 968 -> 1.17M over ~10 frames as
// reinjectPersistentStatic() re-appended the whole store every frame with nothing
// clearing it), overflowing the island's instance/buffer capacity so only low-offset
// building runs survived -> "props are building-only"; and (b) on frames where the
// registry's frame-currency optimisation left it near-empty, feeding only 1-2 props.
// Either way the buckets are the WRONG source for a "draw one frame's full prop set"
// island that runs in place of flush().
//
// The AUTHORITATIVE, stable, all-population set is the persistent-static store
// (s_persistentStaticStore) — the registry recipes for every registered static prop:
// Buildings AND Trees AND Generic (all use the registry markVisible path; measured:
// 733 trees eligible, all via the store, dyn-submit=0). It is population-agnostic and
// is exactly what reinjectPersistentStatic() copies into the GL buckets each frame, so
// feeding it directly gives the island the SAME prop set GL flush() draws — trees,
// tents, misc props included — without the accumulation hazard. Read-only: does NOT
// mutate any batcher state. Output is a reused static scratch vector.
extern "C" uint32_t mc2_static_prop_vertex_stride(void) { return (uint32_t)kVertexStride; }

// Called by txmmgr once per frame at the point where the GL flush() is skipped because
// the VK color island handled the frame (flush() owns the clear on the GL path).
extern "C" void mc2_color_prop_island_end_frame() {
    // Mirror flush()'s end-of-frame bookkeeping (see flush(): s_bucketsByType.clear
    // + s_lastUploadedSlot reset + prev-frame per-type instance count snapshot).
    s_bucketsByType.clear();
    s_reinjectedThisFrame = false;
    s_lastUploadedSlot = 0xFFFFFFFFu;
    s_typeInstanceCountPrevFrame.clear();
    for (const auto& kv : s_typeRanges)
        s_typeInstanceCountPrevFrame[kv.first] = kv.second.instanceCount;
}

extern "C" int mc2_color_prop_island_get_frame_feed(Mc2ColorPropFeed* out) {
    if (!out) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return 0;
    static std::vector<GpuStaticPropInstance> s_colorFeedFlat;
    s_colorFeedFlat.clear();

    // Grouped by typeID for correct per-type geometry addressing (the island issues
    // one type-run per contiguous typeID block via firstInstance). Deterministic
    // ordering (ascending typeID) so the flattening is stable frame-to-frame.
    // Per type: persistent-store block first, then this frame's DYNAMIC submits
    // (buildings failing isStaticEligible etc.) from s_bucketsByType, excluding the
    // range reinjectPersistentStatic() copied from the store (else double-draw).
    // Read point: inside the island run, which txmmgr calls after renderLists() has
    // issued all submits and the registry reinject for the frame.
    std::vector<uint32_t> typeIds;
    typeIds.reserve(s_persistentStaticStore.size() + s_bucketsByType.size());
    for (const auto& kv : s_persistentStaticStore) typeIds.push_back(kv.first);
    for (const auto& kv : s_bucketsByType) typeIds.push_back(kv.first);
    std::sort(typeIds.begin(), typeIds.end());
    typeIds.erase(std::unique(typeIds.begin(), typeIds.end()), typeIds.end());
    for (uint32_t tid : typeIds) {
        auto it = s_persistentStaticStore.find(tid);
        if (it != s_persistentStaticStore.end()) {
            const std::vector<GpuStaticPropInstance>& insts = it->second;
            s_colorFeedFlat.insert(s_colorFeedFlat.end(), insts.begin(), insts.end());
        }
        auto bt = s_bucketsByType.find(tid);
        if (bt != s_bucketsByType.end()) {
            const PerTypeBucket& b = bt->second;
            const uint32_t n  = (uint32_t)b.instances.size();
            const uint32_t rp = std::min(b.reinjPos, n);
            const uint32_t re = std::min(rp + b.reinjCount, n);
            s_colorFeedFlat.insert(s_colorFeedFlat.end(), b.instances.begin(), b.instances.begin() + rp);
            s_colorFeedFlat.insert(s_colorFeedFlat.end(), b.instances.begin() + re, b.instances.end());
        }
    }
    if (s_colorFeedFlat.empty()) return 0;

    out->instances112 = s_colorFeedFlat.data();
    out->count        = (uint32_t)s_colorFeedFlat.size();
    const float* mvp = gos_GetObjectFixBMVPMat4();
    if (mvp) std::memcpy(out->worldToClipGL, mvp, sizeof(out->worldToClipGL));
    else     std::memset(out->worldToClipGL, 0, sizeof(out->worldToClipGL));
    // OBJECT_DEPTH_BIAS mirror (shaders/include/terrain_depth_bias.hglsl:62).
    out->objectDepthBias = 0.0001f;

    // VK-OFFLINE-PROOFS-1: golden feed dump. Fires once per process (see
    // vk_feed_dump.h). No-op unless MC2_VK_FEED_DUMP is set.
    if (mc2vkfeeddump::enabled() && mc2vkfeeddump::readyToDump() && !mc2vkfeeddump::latch("prop")) {
        if (FILE* f = mc2vkfeeddump::openAppend(/*truncateFirst=*/false)) {
            mc2::fmt::print(f, "{{\"island\":\"prop\",\"count\":{:d},\"objectDepthBias\":{:.6f},\"instances\":[", mc2::fmt::as_u32(out->count), out->objectDepthBias);
            const GpuStaticPropInstance* insts =
                reinterpret_cast<const GpuStaticPropInstance*>(out->instances112);
            for (uint32_t i = 0; i < out->count; ++i) {
                const GpuStaticPropInstance& gi = insts[i];
                // Translation lives in row 3 of a row-major 4x4 (col-major layout:
                // indices 12,13,14 are the last column == translation for either
                // convention used consistently across the island's mirror).
                mc2::fmt::print(f, "{}{{\"typeID\":{:d},\"posX\":{:.4f},\"posY\":{:.4f},\"posZ\":{:.4f},\"flags\":{:d}}}",
                    mc2::fmt::safe(i ? "," : ""), mc2::fmt::as_u32(gi.typeID),
                    gi.modelMatrix[12], gi.modelMatrix[13], gi.modelMatrix[14],
                    mc2::fmt::as_u32(gi.flags));
            }
            mc2::fmt::print(f, "]}}\n");
            std::fclose(f);
        }
        mc2vkfeeddump::latch("prop") = true;
    }
    return 1;
}

// VK-P11 STATIC-PROP MATERIAL — expose the alpha-OFF albedo 2D_ARRAY + per-type
// layer table so the color island can copy-once import it and sample real textures.
// Read-only; buffers persist until onMapUnload(). Returns 0 (fail soft -> island
// keeps flat teal) when geometry is not finalized, the alpha-OFF array is unbuilt,
// or the group was BC7-bucketed (bucket-relative layers invalidate the single-array
// import — materials for BC7 are a follow-on).
extern "C" int mc2_color_prop_island_get_material_feed(Mc2ColorPropMaterialFeed* out) {
    if (!out) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure) return 0;
    if (s_texArrayOff == 0) return 0;                 // alpha-OFF array not built
    if (s_typeAlbedoLayer.empty() ||
        s_typeAlbedoLayer.size() != s_types.size()) return 0;

    out->glTexArrayOff   = s_texArrayOff;
    // VK-P11 STATIC-PROP MATERIAL alpha-ON: expose the alpha-ON group array (0 if not
    // built). typeAlbedoLayer[] already carries the group-relative layer for class-1
    // types, so the island routes them into this array with the same layer table.
    out->glTexArrayOn    = s_texArrayOn;
    {
        const auto& batcher = GpuStaticPropBatcher::instance();
        out->albedoOff = batcher.albedoArray(0).levels.empty() ? nullptr : &batcher.albedoArray(0);
        out->albedoOn  = batcher.albedoArray(1).levels.empty() ? nullptr : &batcher.albedoArray(1);
    }
    // BC7-bucketed: per-bucket sibling arrays with bucket-relative layers. The
    // single-array (group-relative) import + layer table below do not describe that
    // layout, so signal the island to fail soft (keep teal) rather than mis-sample.
    out->bucketed        = s_bucketArrays.empty() ? 0u : 1u;
    out->typeAlbedoLayer = s_typeAlbedoLayer.data();
    out->typeAlphaClass  = s_typeAlphaClass.data();
    out->typeCount       = (uint32_t)s_typeAlbedoLayer.size();
    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-packet layer (see header comment).
    // s_packetTexArrayLayer is populated in finalizeGeometry() from the SAME
    // per-packet, group-relative layerForPacket[] GL's coalesce PerDrawEntry table
    // uses — it is the correct source, unlike the per-type "first-valid-wins" table.
    out->packetAlbedoLayer = s_packetTexArrayLayer.empty() ? nullptr : s_packetTexArrayLayer.data();
    out->packetCount       = (uint32_t)s_packetTexArrayLayer.size();
    // FENCE-UVSCALE-1: same per-packet indexing as packetAlbedoLayer, populated
    // in finalizeGeometry() from uvScaleXByPacket/uvScaleYByPacket (the same
    // source PerDrawEntry.uvScaleX/Y draws from on the GL side). nullptr if the
    // sidecar size mismatches packetAlbedoLayer's -- island falls back to 1.0.
    out->packetUvScaleX = (s_packetUvScaleX.size() == s_packetTexArrayLayer.size())
                              ? s_packetUvScaleX.data() : nullptr;
    out->packetUvScaleY = (s_packetUvScaleY.size() == s_packetTexArrayLayer.size())
                              ? s_packetUvScaleY.data() : nullptr;

    // Terrain sun (MC2 world, surface->sun) — same vector the GL prop VS uses for the
    // directional term (STATIC-PROP-TERRAIN-SUN-DIFFUSE). Provides computePBR's L.
    extern void gos_GetTerrainLightDir(float*, float*, float*);
    float tsx = 0.0f, tsy = 0.0f, tsz = 1.0f;
    gos_GetTerrainLightDir(&tsx, &tsy, &tsz);
    out->terrainSunMC2[0] = tsx;
    out->terrainSunMC2[1] = tsy;
    out->terrainSunMC2[2] = tsz;

    out->typeHotColors = (s_typeHotColors.size() == s_types.size() * 12) ? s_typeHotColors.data() : nullptr;

    return 1;
}

// VK-PROP-AMBIENT-IBL-1: real ObjectLights ambient + IBL-SH feed. GL-owner accessors
// (this runs in GL context, mirrors mc2_mech_island_get_frame_feed's setup at
// gos_mech_batcher.cpp:2910-2915).
extern "C" int mc2_color_prop_island_get_light_feed(Mc2ColorPropLightFeed* out) {
    if (!out) return 0;
    // VK-PROP-LIGHTFEED-NATIVE-1: native CPU feed (mech item-9 pattern, mirrors
    // mc2_mech_island_get_frame_feed's lightDataCpu export) — the byte source of
    // the GL SSBO upload. VK-PERF-2 cleanup deleted the GL SSBO name/size export
    // with the audit-gated readback it existed for (clean audit banked).
    out->lightDataCpu = nullptr;
    out->lightDataCpuBytes = 0;
    if (mcTextureManager) {
        out->lightDataCpu      = mcTextureManager->getLightDataPtr();
        out->lightDataCpuBytes = mcTextureManager->getLightDataBytes();
    }
    // s_currentShSet: the live per-mission SH-L2 set (RenderCore::IblShRegistry),
    // same table the GL static-prop VS uniform upload uses (u_iblSh[9]).
    // IBL-SUN-ROTATE-1: routed through the SAME accessor as the two GL upload
    // sites, so VK and the debug state dump see the rotated set when the gate
    // is on and cannot silently disagree with GL. Gate OFF -> identical bytes.
    std::memcpy(out->iblSh, iblShCoeffsForUpload(), sizeof(out->iblSh));
    // The StaticPropIblSh option (default on). Mirrors the GL per-frame upload rule
    // exactly: on -> g_iblShStrength; off -> 0.0.
    out->iblShStrength = iblShStrengthNightScaled();
    bool isNight = false;
    float nightFactor = 0.0f;
    gos_GetNightState(&isNight, &nightFactor);
    out->isNight      = isNight ? 1 : 0;
    out->nightFactor  = nightFactor;
    out->ambientV1Strength = staticPropAmbientV1Enabled() ? 1.0f : 0.0f;
    // The GL upload zeroes the strength when the view-uniform block is off; the island always has the eye position.
    out->pbrV1Strength = pbrV1Enabled() ? g_pbrV1Strength : 0.0f;
    out->pbrV1RoughnessOverride = g_pbrV1RoughnessOverrideEnabled ? g_pbrV1RoughnessOverrideValue : -1.0f;
    const RenderCore::ViewUniforms& vu = RenderCore::framePackets().mainView().uniforms;
    for (int k = 0; k < 4; ++k) out->cameraWorldPos[k] = vu.cameraWorldPos[k];
    // The same pool set and knobs GL static_prop.frag receives (UploadObjectLightPoolUniforms).
    gos_terrain_lighting::ObjectLightPoolParams pp{};
    gos_terrain_lighting::GetObjectLightPoolParams(&pp);
    out->poolLights48   = pp.pool;
    out->numPoolLights  = pp.pool ? pp.count : 0;
    out->poolSoft       = pp.soft;
    out->poolMoverDim   = pp.moverDim;
    out->poolMoverScale = pp.moverScale;
    out->poolMoverMax   = pp.moverMax;
    out->poolCompress   = pp.compress;
    out->poolObjScale   = pp.objScale;
    return 1;
}
#endif // MC2_VULKAN_ISLAND
