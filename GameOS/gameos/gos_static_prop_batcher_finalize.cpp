// GpuStaticPropBatcher: GpuStaticPropBatcher::finalizeGeometry (one 2,050-line function).
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"


void GpuStaticPropBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;

    // One-shot log: always emit so the log confirms which tier is active.
    std::printf("[StaticPropBatcher] tex tier: %dpx (MC2_STATICPROP_TEX_TIER)"
                " ktx=%s\n", texTier(), s_materialKtxEnabled ? "ON" : "OFF");
    std::fflush(stdout);

    GpuStaticPropRegistry::staticPropRegistryClearMaterialCache();
    s_packetTexArrayLayer.clear();  // v2: will be repopulated from layerForPacket below
    s_packetUvScaleX.clear();       // SHADOW-PROP-ALPHA-ATLAS-1: repopulated below
    s_packetUvScaleY.clear();

    // Compile shader programs NOW, at map-load time, while we're on the
    // same code path that compiles every other engine shader. Doing it
    // from inside a mid-render submit() triggers a crash somewhere inside
    // shader_builder — possibly related to the shadow_screen compile
    // failure also seen at map load. Mid-render compile is not a pattern
    // this engine is tested for, so we hoist it here.
    loadProgramsIfNeeded();

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingVbo.size()),
                    s_stagingVbo.data(),
                    0);  // flags=0 -> fully immutable, GPU-only (AMD-safe)

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(),
                    0);

    // Vertex attribute layout -- position MUST be location 0 (AMD invariant 1).
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)24);
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT,      kVertexStride, (void*)32);
    // Slice 2 (object-offload) — Stage 2.C.2: per-vertex aRGBLight tag at
    // offset 36, written by registerType from TG_TypeVertex::aRGBLight.
    // Consumed by static_prop.vert -> get_base_light() to decode the
    // hot-color magic and emit per-vertex base lighting before calc_light.
    glEnableVertexAttribArray(4);
    glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT,      kVertexStride, (void*)36);

    glBindVertexArray(0);

    // Slice 2 (object-offload) — Stage 2.C.2: build per-type hot-color SSBO.
    // 3 vec4 per type, 48 B per type. Decoded from TG_TypeShape::hotPinkRGB
    // (DWORD ARGB) into vec3 + 0 padding (std430 vec4 alignment). Indexed
    // in the shader by inst.typeID — entry 0 corresponds to s_types[0].
    {
        struct PerTypeShaderData {
            float hotPinkRGB[4];
            float hotYellowRGB[4];
            float hotGreenRGB[4];
        };
        static_assert(sizeof(PerTypeShaderData) == 48, "PerTypeShaderData layout must be 48 bytes (3 × vec4 std430)");

        std::vector<PerTypeShaderData> perTypeBlob(s_types.size());
        auto unpack = [](DWORD argb, float out[4]) {
            // Stored as 0xAARRGGBB on the C++ side (matching DWORD convention
            // in tgl.cpp:1799-1801 where the per-vertex hot-color decode
            // pulls (argb>>16) & 0xff = R). Match that.
            out[0] = ((argb >> 16) & 0xFF) / 255.0f;  // R
            out[1] = ((argb >>  8) & 0xFF) / 255.0f;  // G
            out[2] = ((argb >>  0) & 0xFF) / 255.0f;  // B
            out[3] = 0.0f;
        };
        for (size_t i = 0; i < s_types.size(); ++i) {
            const TG_TypeShape* ts = s_types[i].source;
            DWORD pink = 0, yellow = 0, green = 0;
            if (ts) {
                pink   = ts->hotPinkRGB;
                yellow = ts->hotYellowRGB;
                green  = ts->hotGreenRGB;
            }
            unpack(pink,   perTypeBlob[i].hotPinkRGB);
            unpack(yellow, perTypeBlob[i].hotYellowRGB);
            unpack(green,  perTypeBlob[i].hotGreenRGB);
            // BEACON-FIXTURE-ID-1: ride the existing std430 vec4 padding lane
            // (hotPinkRGB.w, previously a hardcoded 0.0) rather than growing the
            // struct -- no size change, no lockstep risk, no new binding.
            // 1.0 = light fixture, 0.0 = everything else.
            perTypeBlob[i].hotPinkRGB[3] =
                isEmissiveFixtureType(pink, yellow, green) ? 1.0f : 0.0f;
            if (perTypeBlob[i].hotPinkRGB[3] > 0.0f) {
                ++s_fixtureTypeCount;
                // FIXTURE-GLOW-1: name the classified types, not just count
                // them. The count alone hid the fact that a map can register
                // five fixture types that are all the SAME colour -- which is
                // exactly what mc2_10 does, and why four lanes only ever put
                // eyes on green.
                const char* fname = "<unknown>";
                for (const auto& kv : s_typeIndex) {
                    if (kv.second == i && kv.first) {
                        fname = const_cast<TG_TypeShape*>(kv.first)->getNodeId();
                        break;
                    }
                }
                std::fprintf(stderr,
                    "[GPUPROPS] fixture-light type: id=%zu name=%s rgb=0x%06lX\n",
                    i, fname, (unsigned long)(pink & 0x00FFFFFFu));
            }
        }
        std::fprintf(stderr, "[GPUPROPS] fixture-lights: %u of %zu types classified"
                             " as emissive fixtures (scale=%.2f)\n",
                     s_fixtureTypeCount, s_types.size(), s_fixtureLightsScale);

        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &s_perTypeSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perTypeSsbo);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                        static_cast<GLsizeiptr>(perTypeBlob.size() * sizeof(PerTypeShaderData)),
                        perTypeBlob.data(),
                        0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    // VK-P8a SHADOW-INTEROP: capture the shared VBO/IBO byte sizes BEFORE the CPU
    // staging is freed, so the Vulkan shadow island can pull the geometry from the
    // GL buffers (glGetBufferSubData) once per map.
    s_sharedVboBytes = (uint64_t)s_stagingVbo.size();
    s_sharedIboBytes = (uint64_t)s_stagingIbo.size() * sizeof(uint32_t);

    s_stagingVbo.clear(); s_stagingVbo.shrink_to_fit();
    s_stagingIbo.clear(); s_stagingIbo.shrink_to_fit();

    std::fprintf(stderr, "[GPUPROPS] finalize: %zu types, %zu packets\n",
                 s_types.size(), s_packets.size());

    if (s_gpuPropsTrace) {
        std::fprintf(stderr,
            "[GPUPROPS v1] event=register_summary path=%d "
            "regCalls[init=%u ims=%u splog=%u unk=%u] "
            "lateDrops[init=%u ims=%u splog=%u unk=%u] "
            "typeIndexSize=%zu\n",
            g_lightProbeSetupPath,
            g_regCall[1], g_regCall[2], g_regCall[3], g_regCall[0],
            g_regLateDrop[1], g_regLateDrop[2], g_regLateDrop[3], g_regLateDrop[0],
            s_typeIndex.size());
    }

    s_geometryFinalized = true;

    //
    // Step 5.1 — §CRITICAL-C invariant (pure documentation):
    //
    // From this point on, EVERY return path must leave:
    //   * s_geometryFinalized == true (guaranteed: legacy already wrote it).
    //   * s_permutationSsbo != 0 (identity OR sorted) — Step 10.3 binds
    //     slot 15 every patch dispatch unconditionally; a zero handle
    //     unbinds slot 15 → AMD-specific UB → invisible static props on
    //     legacy fallback. Step 5.4 alloc is UNCONDITIONAL before any
    //     Step 5.5 early-return.
    //   * Three coalesce flags ALL true (success) or ALL false (any
    //     failure path) — no half-armed state.
    //
    // Failure rollback contract (out3-MIN-F): tex-array build's temp
    // pins are released via coalesceRollbackTexBuild before return; the
    // OFF/ON group already-promoted pins in s_coalescePinnedNodes stay
    // until onMapUnload(). Legacy state (s_sharedVbo/Ibo/Vao,
    // s_perTypeSsbo, s_staticPropProgram, s_geometryFinalized) is NEVER
    // touched — that's the whole point of §5.0's "legacy first" rule.
    // =======================================================================

    const auto coalesceStart = std::chrono::steady_clock::now();

    // Step 5.3 — env decision (defensive idempotent guard; loadProgramsIfNeeded
    // already called this once before the latch — Step 7.2 wires that).
    coalesce_resetEnvOnce();
    const bool coalesceWanted =
        !s_coalesceEnvDisabled && s_hasShaderDrawParams;

    // Step 5.4 — ALWAYS alloc identity permutation FIRST, before any
    // not-wanted return. Patch shader (Step 10.3) binds slot 15 every
    // dispatch regardless of armed state.
    allocPermutationSsboAsIdentity(static_cast<uint32_t>(s_types.size()));

    // Step 12.5 — readback the identity permutation when forced-disarm is
    // active so smoke can verify the slot-15 buffer holds [0,1,2,3,...]
    // even after the disarm path runs. Bounded to 16 bytes per mission
    // load when forced; zero cost when env unset.
    if (s_coalesceForceDisarm != CoalesceForceDisarm::None) {
        uint32_t first4[4] = { 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu };
        const uint32_t want = std::min<uint32_t>(4u, static_cast<uint32_t>(s_types.size()));
        if (s_permutationSsbo && want > 0u) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                               static_cast<GLsizeiptr>(want * sizeof(uint32_t)),
                               first4);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        MC2_DIAGFE(stderr, "[COALESCE v1] event=permutation_state ssbo=%u "
                     "typeCount=%zu first4=%u,%u,%u,%u\n",
                     s_permutationSsbo, s_types.size(),
                     first4[0], first4[1], first4[2], first4[3]);
        std::fflush(stderr);
    }

    // Step 5.NEW — alpha-class OR-reduce: unconditional (emitter/v0 prerequisite in all modes).
    // Malformed types disarm coalesce when coalesceWanted; in legacy-only mode, alphaClass=0
    // (conservative: emitter treats type as opaque, legacy loop draws it unchanged).
    for (uint32_t typeID = 0; typeID < static_cast<uint32_t>(s_types.size()); ++typeID) {
        auto& type = s_types[typeID];
        bool typeAlpha = false;
        bool malformed = false;
        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const auto& pkt = s_packets[type.firstPacket + p];
            const TG_TypeShape* src = type.source;
            if (!src || !src->listOfTextures) { malformed = true; break; }
            bool pktAlpha = (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
            if (pkt.textureSlot < src->numTextures &&
                src->listOfTextures[pkt.textureSlot].textureAlpha)
                pktAlpha = true;
            typeAlpha = typeAlpha || pktAlpha;
        }
        if (malformed) {
            if (coalesceWanted) {
                MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=malformed_type type=%u\n",
                             typeID);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }
            // Non-coalesce: conservative fallback — legacy draws it correctly regardless.
            std::fprintf(stderr, "[DRAW_PACKET v1] WARNING: type=%u malformed src; alphaClass=0\n",
                         typeID);
            type.alphaClass = 0u;
            continue;
        }
        type.alphaClass = typeAlpha ? 1u : 0u;
    }

    // Step 5.NEW.2 — s_typeDescTable population: unconditional (emitter/v0 prerequisite).
    // Must run after alpha-class OR-reduce above so desc.alphaClass is correct.
    {
        s_typeDescTable.clear();
        s_typeDescTable.reserve(s_types.size());
        uint32_t noGeom = 0u, alphaOn = 0u;
        for (uint32_t i = 0u; i < static_cast<uint32_t>(s_types.size()); ++i) {
            const GpuStaticPropType& t = s_types[i];
            RenderCore::StaticPropTypeDesc desc{};
            desc.typeId      = i;
            desc.firstPacket = t.firstPacket;
            desc.packetCount = t.packetCount;
            desc.alphaClass  = static_cast<uint32_t>(t.alphaClass);
            if (t.packetCount == 0u) ++noGeom;
            if (t.alphaClass  == 1u) ++alphaOn;
            s_typeDescTable.push_back(desc);
        }
        MC2_DIAGFE(stderr,
            "[STATIC_PROP_TYPE_TABLE v0] types=%u packet_ranges=%u alpha_on=%u noGeom=%u\n",
            static_cast<uint32_t>(s_typeDescTable.size()),
            static_cast<uint32_t>(s_packets.size()),
            alphaOn, noGeom);
    }

    if (!coalesceWanted) {
        s_coalesceLayoutReady = false;
        s_coalesceEnabled     = false;
        s_coalesceArmed       = false;
        const char* reason = s_coalesceEnvDisabled ? "env_killswitch"
                                                   : "no_extension";
        // Plan v3.8 12B.2 — distinguish forced vs organic in smoke logs.
        // env_killswitch is never forced (MC2_SUBSTRATE_COALESCE_LEGACY is
        // user-driven, not a force-disarm value); only no_extension can be
        // forced via MC2_COALESCE_FORCE_DISARM=no_extension.
        const bool forced = (s_coalesceForceDisarm == CoalesceForceDisarm::NoExtension)
                            && !s_coalesceEnvDisabled;
        MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=%s%s\n",
                     reason, forced ? " (forced)" : "");
        return;
    }

    const uint32_t typeCount = static_cast<uint32_t>(s_types.size());
    if (typeCount == 0) {
        // No types registered — nothing to coalesce; legacy will draw
        // nothing either. Leave flags false; permutation is empty (5.4
        // helper no-ops on typeCount==0). Treat as silent disarm.
        s_coalesceLayoutReady = false;
        s_coalesceEnabled     = false;
        s_coalesceArmed       = false;
        return;
    }

    // Step 12B.4 — MixedAlpha forced-disarm hook (coalesce testing only).
    // alphaClass already computed unconditionally above (Step 5.NEW).
    // Simulates disarm at the coalesce-arm decision point; legacy path unaffected.
    if (s_coalesceForceDisarm == CoalesceForceDisarm::MixedAlpha &&
        typeCount > 0u) {
        MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=mixed_alpha "
                             "type=0 (forced)\n");
        s_coalesceLayoutReady = false;
        s_coalesceEnabled     = false;
        s_coalesceArmed       = false;
        return;
    }

    // Step 5.7 — sort + per-type caps + group totals + per-type byte offsets.
    // Capacity formula per spec §5.1 (verbatim — no ceiling, no grow path):
    //   MIN_PER_TYPE_CAP; globalCap = max(s_instanceCapacity,
    //   INITIAL_INSTANCES_PER_FRAME); cap = max(MIN, (globalCap*2)/typeCount).
    //
    // 2026-05-11 NOTE: per-type cap is a known band-aid. Wolfman zoom can
    // still hit `event=disarmed reason=type_overflow` on tree-heavy stock
    // content (observed type=280 count=259 cap=256 in mc2_10), and on
    // future denser maps any fixed cap will break. The durable fix is a
    // single global instance pool replacing per-type slot allocation.
    // See `docs/superpowers/plans/2026-05-11-global-instance-pool.md`.
    // Until that ships, callers can fall back to MC2_SUBSTRATE_COALESCE_LEGACY=1
    // for wolfman-zoom-correct rendering.
    constexpr uint32_t MIN_PER_TYPE_CAP = 256;
    const uint32_t globalCap = std::max<uint32_t>(
        static_cast<uint32_t>(s_instanceCapacity),
        static_cast<uint32_t>(INITIAL_INSTANCES_PER_FRAME));
    const uint32_t avg2x = (globalCap * 2u) / std::max<uint32_t>(1, typeCount);
    const uint32_t perTypeCap = std::max(MIN_PER_TYPE_CAP, avg2x);
    for (auto& t : s_types) {
        t.instanceCap = perTypeCap;
    }

    s_sortedTypeOrder.clear();
    s_sortedTypeOrder.reserve(typeCount);
    for (uint32_t t = 0; t < typeCount; ++t) {
        if (s_types[t].alphaClass == 0u) s_sortedTypeOrder.push_back(t);
    }
    s_alphaOffCount = static_cast<uint32_t>(s_sortedTypeOrder.size());
    for (uint32_t t = 0; t < typeCount; ++t) {
        if (s_types[t].alphaClass == 1u) s_sortedTypeOrder.push_back(t);
    }
    s_alphaOnCount = static_cast<uint32_t>(s_sortedTypeOrder.size())
                   - s_alphaOffCount;

    {
        size_t offByteCursor = 0;
        size_t onByteCursor  = 0;
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            auto& t = s_types[typeID];
            const size_t typeBytes =
                static_cast<size_t>(t.instanceCap) * sizeof(GpuStaticPropInstance);
            if (t.alphaClass == 0u) {
                t.coalesceByteOffsetWithinGroup =
                    static_cast<uint32_t>(offByteCursor);
                offByteCursor += typeBytes;
            } else {
                t.coalesceByteOffsetWithinGroup =
                    static_cast<uint32_t>(onByteCursor);
                onByteCursor += typeBytes;
            }
        }
        s_offGroupTotalBytes = offByteCursor;
        s_onGroupTotalBytes  = onByteCursor;
    }
    s_coalescePerFrameInstanceBytes =
        s_offGroupTotalBytes + s_onGroupTotalBytes;
    // Step 4.0 — override with global-pool size under non-legacy mode.
    // s_coalescePerFrameInstanceBytes now drives both the alloc (Step 5.9) and
    // batcher_getCoalescePerFrameInstanceBytes() (legacy returns off+on sum).
    if (!s_globalPoolLegacy) {
        s_coalescePerFrameInstanceBytes =
            static_cast<size_t>(s_globalInstanceCap) * sizeof(GpuStaticPropInstance);
    }

    // Step 5.8 — alignment asserts (§3.Z). The std430 stride is 112 bytes
    // for GpuStaticPropInstance (compile-time static_assert in the header),
    // which is a multiple of 16 on every driver we target. The runtime
    // glBindBufferRange offset alignment can be stricter — query and
    // disarm if our group-base alignment is insufficient.
    {
        GLint ssboAlign = 0;
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssboAlign);
        if (ssboAlign <= 0) ssboAlign = 16;  // safety floor
        const auto isAligned = [&](size_t v) {
            return (v % static_cast<size_t>(ssboAlign)) == 0;
        };
        if (!isAligned(sizeof(GpuStaticPropInstance)) ||
            !isAligned(s_offGroupTotalBytes) ||
            !isAligned(s_coalescePerFrameInstanceBytes)) {
            MC2_DIAGFE(stderr,
                "[COALESCE v1] event=disarmed reason=alloc_failed "
                "(alignment: ssboAlign=%d sizeofInst=%zu offTotal=%zu perFrame=%zu)\n",
                ssboAlign,
                sizeof(GpuStaticPropInstance),
                s_offGroupTotalBytes,
                s_coalescePerFrameInstanceBytes);
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }
    }

    {
        // SSBO-BIND-ALIGN: pad the per-frame ring stride to the SSBO offset
        // alignment IDENTICALLY to batcher_getCoalescePerFrameInstanceBytes() (which
        // drives the per-slot bind offset fr_off_bytes_d), so slot offsets are
        // aligned AND never overrun this allocation.
        const size_t alignedPerFrame = (size_t)gpuAlignUp(
            (unsigned long long)s_coalescePerFrameInstanceBytes,
            (unsigned long long)gpuSsboOffsetAlignment());
        const size_t totalBytes =
            static_cast<size_t>(RING_FRAMES) * alignedPerFrame;
        // TIER2-EXCLUDED: ring
        glGenBuffers(1, &s_coalesceInstanceSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_coalesceInstanceSsbo);
        const GLbitfield mapFlags =
            GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        // Drain any pre-existing GL error so our check is clean.
        while (glGetError() != GL_NO_ERROR) {}
        glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, mapFlags);
        const GLenum storageErr = glGetError();
        if (storageErr != GL_NO_ERROR) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            glDeleteBuffers(1, &s_coalesceInstanceSsbo);
            s_coalesceInstanceSsbo = 0;
            MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                                 "(glBufferStorage err=0x%04x bytes=%zu)\n",
                         storageErr, totalBytes);
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }
        s_coalesceInstanceMap = MC2_GL_MapBufferRange(
            GL_SHADER_STORAGE_BUFFER, 0, totalBytes, mapFlags);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        if (!s_coalesceInstanceMap) {
            // Step 4.3 handles "buffer alloc'd, map failed" via inner null
            // check on s_coalesceInstanceMap; leave the buffer for unload
            // to release.
            MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                                 "(glMapBufferRange returned null)\n");
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }

        // Plan v3.8 Step 12B.3 — alloc_failed forced-disarm hook. Fires
        // AFTER the real alloc completes successfully, then simulates the
        // failure by tearing down the resources we just allocated. Mirrors
        // the partial-failure cleanup path in Step 4.3 / above.
        if (s_coalesceForceDisarm == CoalesceForceDisarm::AllocFailed) {
            if (s_coalesceInstanceMap) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_coalesceInstanceSsbo);
                glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                s_coalesceInstanceMap = nullptr;
            }
            if (s_coalesceInstanceSsbo) {
                glDeleteBuffers(1, &s_coalesceInstanceSsbo);
                s_coalesceInstanceSsbo = 0;
            }
            MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed (forced)\n");
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }
    }

    s_coalesceLayoutReady = true;

    // Step 5.10 — build per-group GL_TEXTURE_2D_ARRAY (§5.4).
    // layerForType[typeID] = group-relative layer in {s_texArrayOff,
    // s_texArrayOn}; -1 sentinel for zero-packet / no-dim types
    // (Step 5.11 emits a no-op draw entry but keeps array indexing
    // aligned).
    //
    // v3.8 mixed-size handling: stock content has both 32x32 and 64x64
    // textures in the same alpha group. Strict same-size assertion
    // would disarm coalesce on every mc2_NN mission. Instead, allocate
    // the array at MAX dimensions across the group's uniques and blit
    // each smaller texture into the upper-left sub-region (0,0,W,H) of
    // its layer. Per-type uvScaleX/Y in PerDrawEntry (§5.4) maps
    // [0,1] UV → the actual texture sub-region in the larger layer.
    // Shader applies `fract(v_uv) * uvScale` before sampling so
    // GL_REPEAT semantics work for tiling textures.
    std::vector<int32_t> layerForType(typeCount, -1);
    // 2026-05-11 per-packet rework: layerForPacket[globalPacketIdx] gives the
    // group-relative texture-array layer for that packet. The previous
    // per-type layerForType is kept for backward-compat but now records the
    // FIRST packet's layer (used only by zero-packet guards in stale call
    // sites). Multi-packet types had their detail collapsed under per-type
    // layering — see memory/substrate_coalesce_armed_multi_packet_limitation.md.
    std::vector<int32_t> layerForPacket(s_packets.size(), -1);
    std::vector<float>   uvScaleXByPacket(s_packets.size(), 1.0f);
    std::vector<float>   uvScaleYByPacket(s_packets.size(), 1.0f);

    struct UniqueTex {
        GLuint glTexId;
        GLint  w;
        GLint  h;
        DWORD  nodeIdx;   // MC_TextureManager node index for KTX2 sidecar lookup
    };

    // COMPRESSION-BC7-STATICPROP-2 — bucket bookkeeping is rebuilt every
    // finalizeGeometry() under the gate. Release any prior arrays + size the
    // per-packet bucket index now (parallel to layerForPacket). Gate-OFF leaves
    // all of this empty (and untouched below), preserving byte-identical output.
    staticPropReleaseBuckets();
    if (s_staticPropBc7Enabled) {
        s_packetBucketIndex.assign(s_packets.size(), -1);
    }

    // COMPRESSION-BC7-STATICPROP-2 — build ONE uniform-dim GL_TEXTURE_2D_ARRAY
    // from a list of same-dimension uniques. Tries BC7 first (all-or-nothing per
    // array — a GL array has a single internalformat); falls back to the RGBA8
    // upload path on any condition miss. Returns the GL handle (0 on hard
    // failure) and sets outIsBc7. This is the per-bucket equivalent of the
    // group-level BC7/RGBA8 logic below; bucketDims == every layer's dims so
    // uvScale is always 1.0 and no sub-region blits occur.
    auto buildBucketArray =
        [&](const std::vector<UniqueTex>& bUniques, GLint bw, GLint bh,
            uint8_t group, bool& outIsBc7, size_t& outBytes) -> GLuint {
        outIsBc7 = false;
        outBytes = 0;
        GLuint arr = 0;

        if (s_materialKtxEnabled && mcTextureManager &&
            GLEW_ARB_texture_compression_bptc) {
            std::vector<RenderCore::KtxImage> bc7Imgs(bUniques.size());
            bool bc7Ok = true;
            const char* failReason = nullptr;
            int bc7MipCount = 0;
            uint32_t bc7VkFormat = 0;
            for (size_t k = 0; bc7Ok && k < bUniques.size(); ++k) {
                const auto& u = bUniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { bc7Ok = false; failReason = "load_fail"; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (!srcName || !*srcName) { bc7Ok = false; failReason = "load_fail"; break; }
                const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                if (!RenderCore::ktxLoadRgba8(ktxPath.c_str(), bc7Imgs[k])) {
                    bc7Ok = false; failReason = "load_fail"; break;
                }
                const RenderCore::KtxImage& img = bc7Imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) {
                    bc7Ok = false; failReason = "not_all_bc7"; break;
                }
                if (img.width != bw || img.height != bh) {
                    bc7Ok = false; failReason = "dim_mismatch"; break;
                }
                if (k == 0) { bc7VkFormat = img.vkFormat; bc7MipCount = img.mipCount; }
                if (img.mipCount != bc7MipCount) { bc7Ok = false; failReason = "dim_mismatch"; break; }
            }
            if (bc7Ok) {
                // linear BC7 (UNORM): matches GL_RGBA8 baseline; engine has no GL_FRAMEBUFFER_SRGB
                const GLenum internalformat = GL_COMPRESSED_RGBA_BPTC_UNORM;
                const int levels = (bc7MipCount > 0) ? bc7MipCount : 1;
                // TEX-CLASS: asset-pool -- static-prop BC7 albedo 2D_ARRAY (content)
                glGenTextures(1, &arr);
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               bw, bh, static_cast<GLsizei>(bUniques.size()));
                for (size_t k = 0; k < bUniques.size(); ++k) {
                    const RenderCore::KtxImage& img = bc7Imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (bw >> lvl) ? (bw >> lvl) : 1;
                        const int lh = (bh >> lvl) ? (bh >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(k),
                            lw, lh, 1, internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                        outBytes += static_cast<size_t>(imageSize);
                    }
                }
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                outIsBc7 = true;
                std::fprintf(stderr,
                    "[STATICPROP_BC7] bucket group=%u dims=%dx%d layers=%zu format=BC7 "
                    "levels=%d srcVkFormat=%u bytes=%zu\n",
                    (unsigned)group, bw, bh, bUniques.size(), levels,
                    bc7VkFormat, outBytes);
                std::fflush(stderr);
                return arr;
            } else {
                std::fprintf(stderr,
                    "[STATICPROP_BC7] bucket fallback group=%u dims=%dx%d reason=%s\n",
                    (unsigned)group, bw, bh, failReason ? failReason : "unknown");
                std::fflush(stderr);
            }
        }

        // ---- RGBA8 fallback (mirrors the group-level RGBA8 path; uniform-dim
        //      so every layer fills the whole level → no sub-region blits) ----
        // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
        glGenTextures(1, &arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, bw, bh,
                     static_cast<GLsizei>(bUniques.size()),
                     0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        std::vector<uint8_t> pixelBuf(static_cast<size_t>(bw) * bh * 4u);
        std::vector<std::pair<size_t, RenderCore::KtxImage>> ktxMipLayers;
        for (size_t k = 0; k < bUniques.size(); ++k) {
            const auto& u = bUniques[k];
            bool ktxUsed = false;
            if (s_materialKtxEnabled && u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                    RenderCore::KtxImage ktxImg;
                    const bool ktxOk = RenderCore::ktxLoadRgba8(ktxPath.c_str(), ktxImg);
                    // Reject compressed sidecars here — block bytes are not RGBA8.
                    const bool dimOk = ktxOk && !ktxImg.isCompressed &&
                                       ktxImg.width == u.w && ktxImg.height == u.h;
                    if (dimOk) {
                        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                        u.w, u.h, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        ktxImg.pixels.data());
                        ktxUsed = true;
                        outBytes += ktxImg.pixels.size();
                        if (ktxImg.mipCount > 1) ktxMipLayers.emplace_back(k, std::move(ktxImg));
                    }
                }
            }
            if (!ktxUsed) {
                glBindTexture(GL_TEXTURE_2D, u.glTexId);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                u.w, u.h, 1, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                outBytes += static_cast<size_t>(bw) * bh * 4u;
            }
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        { GLint w = bw, h = bh; while (w > 1 || h > 1) { ++maxLevel; if (w>1) w>>=1; if (h>1) h>>=1; } }
        if (s_materialKtxEnabled && !ktxMipLayers.empty()) {
            for (const auto& lm : ktxMipLayers) {
                const size_t layerK = lm.first;
                const RenderCore::KtxImage& img = lm.second;
                const int lastLvl = (img.mipCount - 1 < maxLevel) ? (img.mipCount - 1) : maxLevel;
                for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                    int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                    int lh = img.height >> lvl; if (lh < 1) lh = 1;
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(layerK),
                                    lw, lh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                    img.pixels.data() +
                                        img.mipByteOffsets[static_cast<size_t>(lvl)]);
                }
            }
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        std::fprintf(stderr,
            "[STATICPROP_BC7] bucket group=%u dims=%dx%d layers=%zu format=RGBA8 bytes=%zu\n",
            (unsigned)group, bw, bh, bUniques.size(), outBytes);
        std::fflush(stderr);
        return arr;
    };

    // STATICPROP-MATERIAL-ORM-1 — build the per-bucket LINEAR ORM sibling array.
    // bUniques / bw / bh are the SAME inputs the albedo buildBucketArray got, so
    // ORM layer k corresponds to albedo layer k (the layer-alignment invariant).
    // Tries a BC7-linear fast path (all-or-nothing per array — a GL array has a
    // single internalformat) when every layer has a compressed sidecar matching
    // bw x bh with identical mipCount; otherwise an RGBA8 array where each layer
    // is either the loaded uncompressed sidecar (dims must equal bw x bh) or a
    // 1x1-replicated neutral (255,255,255,255) texel so the array stays DENSE/
    // bindable. outHasMap[k] = true only where layer k got a REAL ORM map.
    // ORM is LINEAR: bptcInternalFormatFor(false) for BC7, GL_RGBA8 otherwise.
    auto buildOrmBucketArray =
        [&](const std::vector<UniqueTex>& bUniques, GLint bw, GLint bh,
            uint8_t group, std::vector<bool>& outHasMap) -> GLuint {
        outHasMap.assign(bUniques.size(), false);
        GLuint arr = 0;
        const size_t layerCount = bUniques.size();

        if (mcTextureManager && GLEW_ARB_texture_compression_bptc) {
            std::vector<RenderCore::KtxImage> imgs(layerCount);
            bool ok = true;
            int  mipCount = 0;
            for (size_t k = 0; ok && k < layerCount; ++k) {
                const auto& u = bUniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { ok = false; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (!srcName || !*srcName) { ok = false; break; }
                const std::string ormPath = deriveOrmSidecar(srcName);
                if (!RenderCore::ktxLoadRgba8(ormPath.c_str(), imgs[k])) { ok = false; break; }
                const RenderCore::KtxImage& img = imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) { ok = false; break; }
                if (img.width != bw || img.height != bh) { ok = false; break; }
                if (k == 0) mipCount = img.mipCount;
                if (img.mipCount != mipCount) { ok = false; break; }
            }
            if (ok) {
                const GLenum internalformat = bptcInternalFormatFor(false); // LINEAR
                const int levels = (mipCount > 0) ? mipCount : 1;
                // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
                glGenTextures(1, &arr);
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               bw, bh, static_cast<GLsizei>(layerCount));
                for (size_t k = 0; k < layerCount; ++k) {
                    const RenderCore::KtxImage& img = imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (bw >> lvl) ? (bw >> lvl) : 1;
                        const int lh = (bh >> lvl) ? (bh >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(k),
                            lw, lh, 1, internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                    }
                    outHasMap[k] = true;
                }
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                std::fprintf(stderr,
                    "[STATICPROP_ORM] bucket group=%u dims=%dx%d layers=%zu format=BC7-linear "
                    "levels=%d realMaps=%zu\n",
                    (unsigned)group, bw, bh, layerCount, levels, layerCount);
                std::fflush(stderr);
                return arr;
            }
        }

        // ---- RGBA8 fallback (per-layer: real sidecar or neutral fill) ----
        // TEX-CLASS: asset-pool -- static-prop albedo/normal 2D_ARRAY (content)
        glGenTextures(1, &arr);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, bw, bh,
                     static_cast<GLsizei>(layerCount),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        std::vector<uint8_t> neutral(static_cast<size_t>(bw) * bh * 4u, 255u);
        size_t realMaps = 0;
        std::vector<std::pair<size_t, RenderCore::KtxImage>> ormMipLayers;
        for (size_t k = 0; k < layerCount; ++k) {
            const auto& u = bUniques[k];
            bool used = false;
            if (u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    const std::string ormPath = deriveOrmSidecar(srcName);
                    // STATICPROP-ORM-VISUAL-CHECK-1: observability — log every ORM
                    // unique probed (src path + expected sidecar + dims) so an
                    // operator can author/place a matching .orm.ktx2. Gated; off by default.
                    static const bool s_ormProbeTrace = mc2::gate::get<"MC2_STATICPROP_ORM_TRACE">();
                    if (s_ormProbeTrace) {
                        std::fprintf(stderr,
                            "[STATICPROP_ORM_PROBE] group=%u layer=%zu dims=%dx%d src=%s sidecar=%s\n",
                            (unsigned)group, k, bw, bh, srcName, ormPath.c_str());
                        std::fflush(stderr);
                    }
                    RenderCore::KtxImage img;
                    const bool loadOk = RenderCore::ktxLoadRgba8(ormPath.c_str(), img);
                    // Reject compressed sidecars (block bytes are not RGBA8) and
                    // dim mismatches (must equal the albedo unique's dims).
                    const bool dimOk = loadOk && !img.isCompressed &&
                                       img.width == bw && img.height == bh;
                    if (loadOk && !dimOk) {
                        std::fprintf(stderr,
                            "[STATICPROP_ORM] reject layer=%zu group=%u sidecar=%s "
                            "reason=%s got=%dx%d want=%dx%d\n",
                            k, (unsigned)group, ormPath.c_str(),
                            img.isCompressed ? "compressed_in_rgba8_path" : "dim_mismatch",
                            img.width, img.height, bw, bh);
                        std::fflush(stderr);
                    }
                    if (dimOk) {
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                        bw, bh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                        img.pixels.data());
                        used = true;
                        outHasMap[k] = true;
                        ++realMaps;
                        if (img.mipCount > 1) ormMipLayers.emplace_back(k, std::move(img));
                    }
                }
            }
            if (!used) {
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<GLint>(k),
                                bw, bh, 1, GL_RGBA, GL_UNSIGNED_BYTE, neutral.data());
            }
        }
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        { GLint w = bw, h = bh; while (w > 1 || h > 1) { ++maxLevel; if (w>1) w>>=1; if (h>1) h>>=1; } }
        for (const auto& lm : ormMipLayers) {
            const size_t layerK = lm.first;
            const RenderCore::KtxImage& img = lm.second;
            const int lastLvl = (img.mipCount - 1 < maxLevel) ? (img.mipCount - 1) : maxLevel;
            for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                int lh = img.height >> lvl; if (lh < 1) lh = 1;
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, static_cast<GLint>(layerK),
                                lw, lh, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                img.pixels.data() +
                                    img.mipByteOffsets[static_cast<size_t>(lvl)]);
            }
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        std::fprintf(stderr,
            "[STATICPROP_ORM] bucket group=%u dims=%dx%d layers=%zu format=RGBA8 realMaps=%zu\n",
            (unsigned)group, bw, bh, layerCount, realMaps);
        std::fflush(stderr);
        return arr;
    };

    for (uint8_t group = 0; group <= 1; ++group) {
        std::vector<DWORD> newlyPinnedThisBuild;
        GLuint& outArray = (group == 0u) ? s_texArrayOff : s_texArrayOn;

        std::vector<UniqueTex> uniques;             // unique textures in this group
        std::unordered_map<GLuint, int32_t> glTexIdToLayer;
        // Track packets that landed in this group, for the second-pass uvScale
        // assignment. Each entry: { globalPacketIdx, layer }.
        std::vector<std::pair<uint32_t, int32_t>> packetsInGroup;

        // 5.10.a — walk types in sorted order belonging to this group; for
        // each type, walk ALL its packets. Each packet gets its own layer
        // (deduped by glTexId) — this is the per-packet rework's core change.
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            auto& type = s_types[typeID];
            if (type.alphaClass != group) continue;

            // Zero-packet guard mirrors batcher_getTypeDrawInfo pattern;
            // leave layer = -1 (already initialized) and continue.
            if (type.packetCount == 0u) {
                continue;
            }

            for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                const uint32_t globalPktIdx = type.firstPacket + pIdx;
                const auto& pkt = s_packets[globalPktIdx];
                const TG_TypeShape* src = type.source;
                if (!src || !src->listOfTextures ||
                    pkt.textureSlot >= src->numTextures) {
                    MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed "
                                 "reason=malformed_type type=%u pkt=%u group=%s\n",
                                 typeID, pIdx, group == 0u ? "off" : "on");
                    coalesceRollbackTexBuild(newlyPinnedThisBuild);
                    s_coalesceLayoutReady = false;
                    s_coalesceEnabled     = false;
                    s_coalesceArmed       = false;
                    return;
                }

                const DWORD nodeIdx =
                    src->listOfTextures[pkt.textureSlot].mcTextureNodeIndex;
                if (mcTextureManager && nodeIdx != 0xFFFFFFFFu) {
                    mcTextureManager->pinNode(nodeIdx);
                    newlyPinnedThisBuild.push_back(nodeIdx);
                }

                DWORD gosHandle = (mcTextureManager && nodeIdx != 0xFFFFFFFFu)
                    ? mcTextureManager->get_gosTextureHandle(nodeIdx)
                    : src->listOfTextures[pkt.textureSlot].gosTextureHandle;
                // Snapshot first-packet handle for the runtime eviction
                // detect (Step 11.4 still per-type). Multi-packet types use
                // their first packet as the canary — sufficient for the
                // mid-mission cache-evict guard (multi-textured types
                // typically share underlying texture lifetime).
                if (pIdx == 0) {
                    type.lastSeenGosHandle = gosHandle;
                }

                const GLuint glTexId = static_cast<GLuint>(gos_GetGLTextureId(gosHandle));

                auto it = glTexIdToLayer.find(glTexId);
                if (it != glTexIdToLayer.end()) {
                    layerForPacket[globalPktIdx] = it->second;
                    // VK-P11 MATERIAL FIX: FIRST-VALID-LAYER-WINS (was pIdx==0 only).
                    // A multi-packet type whose packet 0 hit the no-dims skip left
                    // layerForType==-1 (the "no_primary" teal bucket) even though a
                    // later packet had a valid layer. Record the first VALID (>=0)
                    // layer so the VK material feed textures the type.
                    if (layerForType[typeID] < 0) layerForType[typeID] = it->second;
                    packetsInGroup.emplace_back(globalPktIdx, it->second);
                    continue;
                }

            // v3.8 dimension fetch: use mcTextureManager metadata first
            // (avoids the lazy-GL-upload trap where glGetTexLevelParameteriv
            // returns 0x0 for textures that have a handle but haven't been
            // glTexImage2D'd yet). MC2 textures are square (only a single
            // `width` field exists in MC_TextureNode). Fall back to GL if
            // metadata is unavailable.
            GLint W = (mcTextureManager && nodeIdx != 0xFFFFFFFFu)
                ? static_cast<GLint>(mcTextureManager->getWidth(nodeIdx))
                : 0;
            GLint H = W;
            if (W <= 0) {
                glBindTexture(GL_TEXTURE_2D, glTexId);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &W);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &H);
            }
            // If both metadata AND GL fail, the texture is genuinely
            // unavailable at finalize-time (no MC node, never cached,
            // and no GL upload). Skip this packet via layer=-1 sentinel
            // rather than disarming the whole coalesce build — graceful
            // degradation: other valid packets still arm.
            if (W <= 0 || H <= 0) {
                COALESCE_TRACE("skip_packet type=%u pkt=%u group=%s reason=no_dims "
                               "nodeIdx=%lu gosHandle=%lu glTexId=%u",
                               typeID, pIdx, group == 0u ? "off" : "on",
                               (unsigned long)nodeIdx,
                               (unsigned long)gosHandle, glTexId);
                // [SEAMPROBE] stage 10: layer-assignment census. When the
                // override prop/tree packets land here, they get layer=-1 and
                // are dropped from the multidraw (the bdactor.cpp:262 skip
                // rule) -> override never rasterizes. Gated, capped.
                {
                    static int s_seamLayerSkip = 0;
                    if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">() && s_seamLayerSkip < 200) {
                        ++s_seamLayerSkip;
                        std::fprintf(stderr, "[SEAMPROBE] sample=first200 layer=-1 SKIP type=%u pkt=%u "
                            "nodeIdx=%lu gosHandle=%lu glTexId=%u W=%d H=%d override=%d uniques=%zu\n",
                            typeID, pIdx, (unsigned long)nodeIdx,
                            (unsigned long)gosHandle, glTexId, (int)W, (int)H,
                            (int)type.isOverride, uniques.size());
                        std::fflush(stderr);
                    }
                }
                // MODEL-OVERRIDE-GPU-BATCHER-SEAM-PROBE-1 fix: a modder glTF
                // render-override imports with unresolved "NULLTXM" texture
                // handles (nodeIdx=0xFFFFFFFF, no GL upload -> W<=0 here). The
                // stock skip rule (layer=-1 -> dropped from the coalesce
                // multidraw, see bdactor.cpp:262) makes the override geometry
                // invisible. For override-backed types ONLY, route the packet
                // to an already-built valid layer in this group (layer 0) so it
                // RASTERIZES with that texture (wrong albedo, but visible) —
                // the MVP success criterion is a box that draws, not correct
                // texturing. Stock/damage-shape packets are UNAFFECTED: they
                // keep the layer=-1 skip (no isOverride flag), so the orange-
                // ghost destroyed-building regression cannot recur. If no valid
                // layer exists yet in this group, fall through to the skip.
                if (type.isOverride && !uniques.empty()) {
                    layerForPacket[globalPktIdx] = 0;
                    if (layerForType[typeID] < 0) layerForType[typeID] = 0; // VK-P11: first-valid wins
                    packetsInGroup.emplace_back(globalPktIdx, 0);
                    if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">()) {
                        std::fprintf(stderr, "[SEAMPROBE] OVERRIDE_ROUTE type=%u pkt=%u "
                            "-> layer=0 (default) group=%s\n",
                            typeID, pIdx, group == 0u ? "off" : "on");
                        std::fflush(stderr);
                    }
                    continue;
                }
                continue;
            }

            if (s_coalesceForceDisarm == CoalesceForceDisarm::SizeMismatch &&
                group == 0u && uniques.size() == 1u) {
                MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=size_mismatch "
                             "group=off expected=%dx%d got=%dx%d (forced)\n",
                             uniques[0].w, uniques[0].h,
                             uniques[0].w + 1, uniques[0].h + 1);
                coalesceRollbackTexBuild(newlyPinnedThisBuild);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }

            // v3.8 mixed-size: append unique with its actual W,H.
            // Array allocation uses max(W,H) across all uniques.
            const int32_t layer = static_cast<int32_t>(uniques.size());
            uniques.push_back({glTexId, W, H, nodeIdx});
            glTexIdToLayer[glTexId] = layer;
            layerForPacket[globalPktIdx] = layer;
            if (layerForType[typeID] < 0) layerForType[typeID] = layer; // VK-P11: first-valid wins
            packetsInGroup.emplace_back(globalPktIdx, layer);
            } // end per-packet loop
        }

        if (uniques.empty()) {
            // Empty group — Step 11.7.g/h skip the bind+draw pair when
            // count == 0 (out-CRIT-5). No array allocated; no pins to
            // promote (newlyPinnedThisBuild is empty).
            continue;
        }

        // ====================================================================
        // COMPRESSION-BC7-STATICPROP-2 — bucketed build (gate ON only).
        // Partition this group's uniques by (w,h). Each bucket is uniform-dim
        // by construction → eligible for an independent BC7 or RGBA8 array.
        // layerForPacket becomes BUCKET-RELATIVE (layer within the bucket
        // array, same semantics the shader already consumes). uvScale = 1.0.
        // The gate-OFF group-build below is left UNTOUCHED and never runs here.
        // ====================================================================
        if (s_staticPropBc7Enabled) {
            struct DimKey { GLint w, h; };
            std::vector<DimKey>               dimKeys;       // unique dims, stable order of first appearance
            std::vector<std::vector<int32_t>> bucketLayers;  // per dim: list of group-relative layer indices
            std::vector<int32_t>              layerToBucketLocal(uniques.size(), -1);   // group-rel layer -> local bucket
            std::vector<int32_t>              layerToBucketRelLayer(uniques.size(), -1); // group-rel layer -> bucket-relative layer

            for (size_t lyr = 0; lyr < uniques.size(); ++lyr) {
                const GLint w = uniques[lyr].w, h = uniques[lyr].h;
                int found = -1;
                for (size_t b = 0; b < dimKeys.size(); ++b) {
                    if (dimKeys[b].w == w && dimKeys[b].h == h) { found = static_cast<int>(b); break; }
                }
                if (found < 0) {
                    found = static_cast<int>(dimKeys.size());
                    dimKeys.push_back({w, h});
                    bucketLayers.emplace_back();
                }
                layerToBucketLocal[lyr]    = found;
                layerToBucketRelLayer[lyr] = static_cast<int32_t>(bucketLayers[found].size());
                bucketLayers[found].push_back(static_cast<int32_t>(lyr));
            }

            // Build one GL array per dim bucket; record into the global vectors.
            // localBucketToGlobal maps this group's local bucket index -> global
            // s_bucketArrays index (used to tag packets).
            std::vector<int32_t> localBucketToGlobal(dimKeys.size(), -1);
            size_t groupTotalBytes = 0;
            for (size_t b = 0; b < dimKeys.size(); ++b) {
                std::vector<UniqueTex> sub;
                sub.reserve(bucketLayers[b].size());
                for (int32_t lyr : bucketLayers[b]) sub.push_back(uniques[static_cast<size_t>(lyr)]);

                bool isBc7 = false; size_t bytes = 0;
                const GLuint arr = buildBucketArray(sub, dimKeys[b].w, dimKeys[b].h,
                                                    group, isBc7, bytes);
                groupTotalBytes += bytes;

                const int32_t globalBucket = static_cast<int32_t>(s_bucketArrays.size());
                localBucketToGlobal[b] = globalBucket;
                s_bucketArrays.push_back(arr);
                StaticPropBucketInfo info{};
                info.group      = group;
                info.w          = static_cast<uint16_t>(dimKeys[b].w);
                info.h          = static_cast<uint16_t>(dimKeys[b].h);
                info.layerCount = static_cast<uint32_t>(sub.size());
                info.isBc7      = isBc7;
                info.glArray    = arr;
                s_bucketInfo.push_back(info);

                // STATICPROP-MATERIAL-ORM-1 — build the layer-aligned ORM sibling
                // for THIS bucket from the SAME `sub` uniques, so ORM array index
                // == albedo array index (globalBucket) and ORM layer k == albedo
                // layer k. Gate-OFF builds nothing (vectors stay empty); gate-ON
                // keeps s_ormBucketArrays.size() == s_bucketArrays.size().
                if (s_ormSlotsEnabled) {
                    std::vector<bool> ormHasMap;
                    const GLuint ormArr = buildOrmBucketArray(sub, dimKeys[b].w, dimKeys[b].h,
                                                              group, ormHasMap);
                    s_ormBucketArrays.push_back(ormArr);
                    s_ormLayerHasMap.push_back(std::move(ormHasMap));
                }
            }

            // Reassign layerForPacket -> bucket-relative layer, tag each packet
            // with its global bucket index, and set uvScale = 1.0 (uniform-dim).
            for (const auto& kv : packetsInGroup) {
                const uint32_t globalPktIdx = kv.first;
                const int32_t  grpLayer     = kv.second;   // group-relative layer
                if (grpLayer < 0) continue;
                const int32_t localB   = layerToBucketLocal[static_cast<size_t>(grpLayer)];
                const int32_t relLayer = layerToBucketRelLayer[static_cast<size_t>(grpLayer)];
                layerForPacket[globalPktIdx]      = relLayer;            // bucket-relative
                s_packetBucketIndex[globalPktIdx] = localBucketToGlobal[static_cast<size_t>(localB)];
                uvScaleXByPacket[globalPktIdx]    = 1.0f;
                uvScaleYByPacket[globalPktIdx]    = 1.0f;
            }

            std::fprintf(stderr,
                "[STATICPROP_BC7] group=%u buckets=%zu total_bytes=%zu\n",
                (unsigned)group, dimKeys.size(), groupTotalBytes);
            std::fflush(stderr);

            s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                          newlyPinnedThisBuild.begin(),
                                          newlyPinnedThisBuild.end());
            continue;
        }

        // 5.10.d — find max dimensions across the group's uniques.
        // The texture array is allocated at maxW × maxH; smaller
        // textures get blitted into upper-left sub-region of their
        // layer; per-type uvScale maps [0,1] UV → sub-region.
        GLint maxW = 0, maxH = 0;
        for (const auto& u : uniques) {
            if (u.w > maxW) maxW = u.w;
            if (u.h > maxH) maxH = u.h;
        }
        if (maxW <= 0 || maxH <= 0) {
            // Defensive — shouldn't happen because skip-path filtered W/H<=0.
            MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=alloc_failed "
                         "(group=%s maxDims=%dx%d after skip-no-dims pass)\n",
                         group == 0u ? "off" : "on", maxW, maxH);
            coalesceRollbackTexBuild(newlyPinnedThisBuild);
            s_coalesceLayoutReady = false;
            s_coalesceEnabled     = false;
            s_coalesceArmed       = false;
            return;
        }

        // -------------------------------------------------------------------
        // COMPRESSION-BC7-STATICPROP-1 — BC7-compressed array fast path.
        // Gate: MC2_MATERIAL_KTX=1 AND MC2_STATICPROP_BC7=1.
        // A GL array has ONE internalformat, so BC7 is ALL-OR-NOTHING per
        // array. Additionally we require the group to be UNIFORM-DIM (every
        // layer fills the whole level) so compressed sub-region uploads never
        // violate 4x4 block alignment at small mips. If any condition fails we
        // fall through to the existing RGBA8 path below, byte-identical.
        // -------------------------------------------------------------------
        if (s_materialKtxEnabled && s_staticPropBc7Enabled && mcTextureManager) {
            std::vector<RenderCore::KtxImage> bc7Imgs(uniques.size());
            bool bc7Ok      = true;
            const char* failReason = nullptr;
            uint32_t bc7VkFormat = 0;   // shared format across the array
            int      bc7MipCount = 0;   // shared mip count across the array

            // Uniform-dim check first (cheap, no file IO).
            for (const auto& u : uniques) {
                if (u.w != maxW || u.h != maxH) { bc7Ok = false; failReason = "mixed_dims"; break; }
            }

            for (size_t k = 0; bc7Ok && k < uniques.size(); ++k) {
                const auto& u = uniques[k];
                if (u.nodeIdx == 0xFFFFFFFFu) { bc7Ok = false; failReason = "load_fail"; break; }
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (!srcName || !*srcName) { bc7Ok = false; failReason = "load_fail"; break; }
                const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                if (!RenderCore::ktxLoadRgba8(ktxPath.c_str(), bc7Imgs[k])) {
                    bc7Ok = false; failReason = "load_fail"; break;
                }
                const RenderCore::KtxImage& img = bc7Imgs[k];
                if (!img.isCompressed ||
                    (img.vkFormat != 145u && img.vkFormat != 146u)) {
                    bc7Ok = false; failReason = "not_all_bc7"; break;
                }
                if (img.width != u.w || img.height != u.h) {
                    bc7Ok = false; failReason = "dim_mismatch"; break;
                }
                if (k == 0) { bc7VkFormat = img.vkFormat; bc7MipCount = img.mipCount; }
                // Require a consistent mip count so glTexStorage3D's level count
                // is valid for every layer.
                if (img.mipCount != bc7MipCount) { bc7Ok = false; failReason = "dim_mismatch"; break; }
            }

            if (bc7Ok && !GLEW_ARB_texture_compression_bptc) {
                bc7Ok = false; failReason = "no_bptc_support";
            }

            if (bc7Ok) {
                // linear BC7 (UNORM): matches GL_RGBA8 baseline; engine has no GL_FRAMEBUFFER_SRGB
                const GLenum internalformat = GL_COMPRESSED_RGBA_BPTC_UNORM;
                const int levels = (bc7MipCount > 0) ? bc7MipCount : 1;

                // TEX-CLASS: asset-pool -- static-prop material 2D_ARRAY (content)
                glGenTextures(1, &outArray);
                glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                               maxW, maxH, static_cast<GLsizei>(uniques.size()));

                size_t bc7TotalBytes = 0;
                for (size_t k = 0; k < uniques.size(); ++k) {
                    const RenderCore::KtxImage& img = bc7Imgs[k];
                    for (int lvl = 0; lvl < levels; ++lvl) {
                        const int lw = (maxW >> lvl) ? (maxW >> lvl) : 1;
                        const int lh = (maxH >> lvl) ? (maxH >> lvl) : 1;
                        const GLsizei imageSize =
                            static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);
                        glCompressedTexSubImage3D(
                            GL_TEXTURE_2D_ARRAY, lvl,
                            0, 0, static_cast<GLint>(k),
                            lw, lh, 1,
                            internalformat, imageSize,
                            img.pixels.data() +
                                img.mipByteOffsets[static_cast<size_t>(lvl)]);
                        bc7TotalBytes += static_cast<size_t>(imageSize);
                    }
                }

                // Sampler: match the RGBA8 path. NO glGenerateMipmap (compressed
                // mips come straight from the cooked KTX). MAX_LEVEL = levels-1.
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  levels - 1);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

                for (const auto& kv : packetsInGroup) {
                    const uint32_t globalPktIdx = kv.first;
                    if (kv.second < 0) continue;
                    uvScaleXByPacket[globalPktIdx] = 1.0f;
                    uvScaleYByPacket[globalPktIdx] = 1.0f;
                }

                std::fprintf(stderr,
                    "[STATICPROP_BC7] group=%u layers=%zu dims=%dx%d levels=%d "
                    "internalformat=0x%x bytes=%zu\n",
                    (unsigned)group, uniques.size(), maxW, maxH, levels,
                    (unsigned)internalformat, bc7TotalBytes);
                std::fprintf(stderr,
                    "[STATICPROP_BC7] summary group=%u built BC7 array "
                    "(srcVkFormat=%u, %zu layers)\n",
                    (unsigned)group, bc7VkFormat, uniques.size());
                std::fflush(stderr);

                COALESCE_TRACE("group_built_bc7 group=%s uniques=%zu maxDims=%dx%d levels=%d",
                               group == 0u ? "off" : "on",
                               uniques.size(), maxW, maxH, levels);

                s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                              newlyPinnedThisBuild.begin(),
                                              newlyPinnedThisBuild.end());
                continue;
            } else {
                std::fprintf(stderr,
                    "[STATICPROP_BC7] fallback group=%u reason=%s\n",
                    (unsigned)group, failReason ? failReason : "unknown");
                std::fflush(stderr);
            }
        }

        // 5.10.d — allocate the array at max dims.
        // TEX-CLASS: asset-pool -- static-prop material 2D_ARRAY (content)
        glGenTextures(1, &outArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                     maxW, maxH,
                     static_cast<GLsizei>(uniques.size()),
                     0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);

        // 5.10.e — per-unique blit. Each unique blits at sub-region
        // (0,0,W,H) of its layer; the unused (W..maxW, H..maxH) area
        // is left at the glTexImage3D-zeroed state. Shader uvScale
        // ensures sampling never reaches that area for the type's UVs.
        size_t maxBytes = static_cast<size_t>(maxW) *
                          static_cast<size_t>(maxH) * 4u;
        std::vector<uint8_t> pixelBuf(maxBytes);
        // KTX2 layers that carry a pre-baked mip chain: cache the loaded image
        // keyed by layer index so we can OVERWRITE the auto-generated mips with
        // the sharp pre-baked levels after the single glGenerateMipmap pass.
        std::vector<std::pair<size_t, RenderCore::KtxImage>> s_ktxMipLayers;
        for (size_t k = 0; k < uniques.size(); ++k) {
            const auto& u = uniques[k];
            bool ktxUsed = false;
            if (s_materialKtxEnabled && u.nodeIdx != 0xFFFFFFFFu && mcTextureManager) {
                const char* srcName = mcTextureManager->getTextureName(u.nodeIdx);
                if (srcName && *srcName) {
                    // Derive tiered .ktx2 path: data/tgl/<tier>/foo.ktx2
                    const std::string ktxPath = deriveTieredKtxPath(srcName, texTier());
                    RenderCore::KtxImage ktxImg;
                    const bool ktxOk = RenderCore::ktxLoadRgba8(ktxPath.c_str(), ktxImg);
                    // This RGBA8 upload path cannot consume compressed (BC7) KTX2:
                    // its bytes are block data, not RGBA8 texels. If a BC7 sidecar
                    // is present but this group fell back to the RGBA8 path (e.g.
                    // mixed-dim, so the BC7 array path was declined), reject it here
                    // (dimOk=false) so we fall through to the original GL texture
                    // upload below instead of mis-uploading block data as RGBA8.
                    const bool dimOk = ktxOk && !ktxImg.isCompressed &&
                                       ktxImg.width == u.w && ktxImg.height == u.h;
                    // Always-on debug: log attempt result so we can diagnose path/dim issues.
                    std::fprintf(stderr,
                        "[KTX_SIDECAR] node=%lu srcName=%s path=%s "
                        "load=%d ktxDims=%dx%d glDims=%dx%d match=%d\n",
                        (unsigned long)u.nodeIdx, srcName, ktxPath.c_str(),
                        (int)ktxOk,
                        ktxOk ? ktxImg.width : 0, ktxOk ? ktxImg.height : 0,
                        u.w, u.h, (int)dimOk);
                    std::fflush(stderr);
                    if (dimOk) {
                        // KTX2 pixels are RGBA order; GL_RGBA matches.
                        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                        0, 0, static_cast<GLint>(k),
                                        u.w, u.h, 1,
                                        GL_RGBA, GL_UNSIGNED_BYTE, ktxImg.pixels.data());
                        ktxUsed = true;
                        // Stash for post-glGenerateMipmap pre-baked mip overwrite.
                        if (ktxImg.mipCount > 1) {
                            s_ktxMipLayers.emplace_back(k, std::move(ktxImg));
                        }
                        COALESCE_TRACE("ktx_sidecar_hit type=? node=%lu path=%s",
                                       (unsigned long)u.nodeIdx, ktxPath.c_str());
                    }
                }
            }
            if (!ktxUsed) {
                glBindTexture(GL_TEXTURE_2D, u.glTexId);
                const size_t need = static_cast<size_t>(u.w) *
                                    static_cast<size_t>(u.h) * 4u;
                if (pixelBuf.size() < need) pixelBuf.resize(need);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                                0, 0, static_cast<GLint>(k),
                                u.w, u.h, 1,
                                GL_BGRA, GL_UNSIGNED_BYTE, pixelBuf.data());
            }
        }

        // 5.10.f — mipmap + sampler params (forestall AMD strict-fail).
        // GL_CLAMP_TO_EDGE on the array (was GL_REPEAT pre-v3.8) so
        // sub-region textures don't bleed into the unused area at the
        // edge. Tiling textures rely on the shader's `fract(v_uv) *
        // uvScale` to keep sampling within the sub-region.
        glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        GLint maxLevel = 0;
        {
            GLint w = maxW, h = maxH;
            while (w > 1 || h > 1) {
                ++maxLevel;
                if (w > 1) w >>= 1;
                if (h > 1) h >>= 1;
            }
        }
        // 5.10.f.ktx — OVERWRITE auto-generated mips with the pre-baked KTX2
        // mip chain for layers that carried one. glGenerateMipmap above filled
        // mips for ALL layers (KTX + non-KTX) from level 0; here we replace the
        // blurry auto-gen mips on KTX layers with the sharp baked levels.
        // Non-KTX layers keep their auto-generated mips. Only overwrite levels
        // that exist both in the baked chain AND within the array's maxLevel.
        if (s_materialKtxEnabled && !s_ktxMipLayers.empty()) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, outArray);
            int totalMipsUploaded = 0;
            for (const auto& lm : s_ktxMipLayers) {
                const size_t layerK = lm.first;
                const RenderCore::KtxImage& img = lm.second;
                const int lastLvl = (img.mipCount - 1 < maxLevel)
                                        ? (img.mipCount - 1) : maxLevel;
                int uploaded = 0;
                for (int lvl = 1; lvl <= lastLvl; ++lvl) {
                    int lw = img.width  >> lvl; if (lw < 1) lw = 1;
                    int lh = img.height >> lvl; if (lh < 1) lh = 1;
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl,
                                    0, 0, static_cast<GLint>(layerK),
                                    lw, lh, 1,
                                    GL_RGBA, GL_UNSIGNED_BYTE,
                                    img.pixels.data() +
                                        img.mipByteOffsets[static_cast<size_t>(lvl)]);
                    ++uploaded;
                }
                totalMipsUploaded += uploaded;
                std::fprintf(stderr,
                    "[KTX2_MIP] uploaded %d pre-baked mips for layer %zu (%dx%d)\n",
                    uploaded, layerK, img.width, img.height);
            }
            std::fprintf(stderr,
                "[KTX2_MIP] summary: %zu KTX layers, %d pre-baked mips overwritten\n",
                s_ktxMipLayers.size(), totalMipsUploaded);
            std::fflush(stderr);
        }

        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  maxLevel);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 2026-05-11 per-packet rework: uvScale is per-PACKET, derived from
        // each packet's unique texture dimensions vs the array max dims.
        const float invMaxW = 1.0f / static_cast<float>(maxW);
        const float invMaxH = 1.0f / static_cast<float>(maxH);
        for (const auto& kv : packetsInGroup) {
            const uint32_t globalPktIdx = kv.first;
            const int32_t layer = kv.second;
            if (layer < 0) continue;
            const auto& u = uniques[static_cast<size_t>(layer)];
            uvScaleXByPacket[globalPktIdx] = static_cast<float>(u.w) * invMaxW;
            uvScaleYByPacket[globalPktIdx] = static_cast<float>(u.h) * invMaxH;
        }

        COALESCE_TRACE("group_built group=%s uniques=%zu maxDims=%dx%d",
                       group == 0u ? "off" : "on",
                       uniques.size(), maxW, maxH);

        s_coalescePinnedNodes.insert(s_coalescePinnedNodes.end(),
                                      newlyPinnedThisBuild.begin(),
                                      newlyPinnedThisBuild.end());
    }

    // v2: persist per-packet texArrayLayer before layerForPacket goes stale.
    // layerForPacket is fully populated here; the PerDrawEntry loop below reads
    // but does not modify it. -1 entries = packets with unavailable textures.
    s_packetTexArrayLayer.assign(layerForPacket.begin(), layerForPacket.end());
    // FENCE-UVSCALE-1: uvScaleXByPacket/uvScaleYByPacket are fully populated by
    // the per-group loop above (same "packetsInGroup" pass that fills
    // layerForPacket) -- persist alongside the layer sidecar for the VK feed.
    s_packetUvScaleX.assign(uvScaleXByPacket.begin(), uvScaleXByPacket.end());
    s_packetUvScaleY.assign(uvScaleYByPacket.begin(), uvScaleYByPacket.end());

    // VK-P11 STATIC-PROP MATERIAL: persist per-type group-relative albedo layer +
    // alpha-class for the VK color island's copy-once material feed. layerForType[]
    // is fully populated above (group-relative layer into s_texArrayOff/On); the
    // island only textures alpha-OFF (class 0) types from s_texArrayOff, so we keep
    // the raw group-relative layer and the class side-by-side. Compiled always (cheap
    // pair of vector copies); read only when MC2_VK_PROP_MATERIAL is armed.
    s_typeAlbedoLayer.assign(layerForType.begin(), layerForType.end());
    s_typeAlphaClass.assign(s_types.size(), 0);
    for (size_t t = 0; t < s_types.size(); ++t)
        s_typeAlphaClass[t] = static_cast<int32_t>(s_types[t].alphaClass);

    // 2026-05-11 per-packet rework: build s_sortedPacketOrder, s_alphaOff/OnCmdCount,
    // s_perDrawSsbo (one entry per packet in sorted order), and s_cmdToBucketSsbo
    // (one uint typeID per cmd). Fragment shader reads
    // entries[v_drawID + u_drawIDBase] where v_drawID = gl_DrawID = sorted packet
    // slot, u_drawIDBase = 0 for alpha-OFF group, s_alphaOffCmdCount for alpha-ON.
    s_sortedPacketOrder.clear();
    s_alphaOffCmdCount = 0;
    s_alphaOnCmdCount  = 0;
    {
        // 2026-05-11 packet-skip rule: only enroll packets whose texture-
        // array layer was successfully assigned. Damage-shape leaves
        // (bldgDmgShape) typically have nodeIdx=0xFFFFFFFF at finalize
        // because their textures are loaded only at destruction time per-
        // instance — they end up with layerForPacket=-1. Without skipping,
        // the indirect cmd still emits, the fragment samples layer 0 of
        // the array, and a wrong-texture (orange-rectangle) ghost renders
        // at the prop's location. Skipping leaves their geometry out of
        // the multidraw entirely.
        // COMPRESSION-BC7-STATICPROP-2: when the gate is ON, sorted slots must
        // be grouped into contiguous per-bucket RUNS so each dispatch can bind
        // one bucket array and draw its run. We order buckets ascending by
        // GLOBAL bucket index — and group buckets carry the alpha group in
        // s_bucketInfo, with all alpha-OFF buckets built before alpha-ON ones
        // (the build walks group 0 then group 1) → bucket index order already
        // respects alpha order (OFF runs precede ON runs). s_slotBucketIndex /
        // s_bucketCmdCount are populated in lockstep. Gate-OFF leaves both
        // empty and uses the original 2-group emission below (byte-identical).
        s_slotBucketIndex.clear();
        s_bucketCmdCount.assign(s_bucketArrays.size(), 0u);
        for (uint8_t group = 0; group <= 1; ++group) {
            if (s_staticPropBc7Enabled) {
                for (uint32_t b = 0; b < s_bucketInfo.size(); ++b) {
                    if (s_bucketInfo[b].group != group) continue;
                    for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
                        const uint32_t typeID = s_sortedTypeOrder[i];
                        const auto& type = s_types[typeID];
                        if (type.alphaClass != group) continue;
                        for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                            const uint32_t globalPktIdx = type.firstPacket + pIdx;
                            if (layerForPacket[globalPktIdx] < 0) continue;
                            if (s_packetBucketIndex[globalPktIdx] != static_cast<int32_t>(b)) continue;
                            s_sortedPacketOrder.push_back(globalPktIdx);
                            s_slotBucketIndex.push_back(static_cast<uint8_t>(b));
                            ++s_bucketCmdCount[b];
                        }
                    }
                }
            } else {
                for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
                    const uint32_t typeID = s_sortedTypeOrder[i];
                    const auto& type = s_types[typeID];
                    if (type.alphaClass != group) continue;
                    for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                        const uint32_t globalPktIdx = type.firstPacket + pIdx;
                        if (layerForPacket[globalPktIdx] < 0) continue; // texture unavailable; skip
                        s_sortedPacketOrder.push_back(globalPktIdx);
                    }
                }
            }
            if (group == 0u) {
                s_alphaOffCmdCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
            } else {
                s_alphaOnCmdCount = static_cast<uint32_t>(s_sortedPacketOrder.size())
                                  - s_alphaOffCmdCount;
            }
        }

        // --- MaterialGpu-2 sidecar (MC2_MATERIAL_GPU=1 only) ---
        // Runs after s_sortedPacketOrder is populated (skip-filtered, layer >= 0 only).
        // Iterates s_sortedPacketOrder in draw-slot order so that:
        //   s_packetMaterialIdx.size() == s_sortedPacketOrder.size() == PerDrawEntry count
        // This is the v3 invariant: draw slot i reads materials[s_packetMaterialIdx[i]].
        if (s_materialGpuEnabled) {
            s_materialGpuSidecarValid = false;  // MAJ-1: reset before sidecar loop
            s_packetMaterialIdx.clear();
            s_materialGpuTable.clear();
            s_materialInventory.clear();  // V-MATERIAL-STATIC-0: rebuild in lockstep

            std::unordered_map<int64_t, uint32_t> layerToMaterialIdx;
            const uint32_t emittedCount =
                static_cast<uint32_t>(s_sortedPacketOrder.size());

            // Iterate in draw-slot order (same as PerDrawEntry build below).
            // All entries in s_sortedPacketOrder have layerForPacket >= 0 (skip rule above).
            for (uint32_t i = 0; i < emittedCount; ++i) {
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                const int32_t  layer        = layerForPacket[globalPktIdx]; // >= 0 guaranteed

                // COMPRESSION-BC7-STATICPROP-2: layer is BUCKET-relative when the
                // gate is ON, so the same layer index can recur across buckets.
                // Dedup material rows by (bucket,layer) to keep distinct textures
                // distinct. albedoTex still stores `layer` (bucket-relative) so the
                // texArrayLayer==albedoTex compare below stays valid. Gate-OFF
                // (bucketIdx 0) → key == layer → byte-identical dedup.
                const int32_t bucketIdx = s_staticPropBc7Enabled
                    ? s_packetBucketIndex[globalPktIdx] : 0;
                const int64_t matKey =
                    (static_cast<int64_t>(bucketIdx) << 32) | static_cast<uint32_t>(layer);

                auto [it, inserted] = layerToMaterialIdx.try_emplace(
                    matKey, static_cast<uint32_t>(s_materialGpuTable.size()));
                if (inserted) {
                    RenderCore::MaterialGpu m = {};
                    m.albedoTex            = static_cast<uint32_t>(layer);
                    m.normalTex            = RenderCore::kMaterialTexAbsent;
                    m.metallicRoughnessTex = RenderCore::kMaterialTexAbsent;
                    m.emissiveTex          = RenderCore::kMaterialTexAbsent;
                    m.flags                = 0;
                    m.baseColorFactor      = 1.0f;   // neutral: full brightness
                    m.metallicFactor       = 0.0f;  // V-MATERIAL-PBR-1: dielectric default
                    // MATERIAL-M0: this is the AUTHORITATIVE roughness default
                    // (the MaterialGpu record producer). The static_prop.frag
                    // fallback and the asset viewer (LocalPbrMaterialBackend.cpp /
                    // MaterialRenderBackend.h) are pinned to MATCH this value.
                    // All three sites must agree. See docs/material-m0-contract.md.
                    m.roughnessFactor      = 1.0f;  // V-MATERIAL-PBR-1: fully rough default (was 0.0)

                    // STATICPROP-MATERIAL-ORM-1 — keep metallicRoughnessTex ==
                    // albedoTex (the SAME bucket-relative layer in the SAME bucket;
                    // the ORM array is a layer-aligned sibling of the albedo array).
                    // Set the kMetallicRoughness flag ONLY when this (bucket,layer)
                    // had a REAL ORM map; otherwise leave it kMaterialTexAbsent so
                    // the shader uses the metallic/roughness factor defaults.
                    if (s_ormSlotsEnabled && layer >= 0 &&
                        bucketIdx >= 0 &&
                        static_cast<size_t>(bucketIdx) < s_ormLayerHasMap.size() &&
                        static_cast<size_t>(layer) < s_ormLayerHasMap[static_cast<size_t>(bucketIdx)].size() &&
                        s_ormLayerHasMap[static_cast<size_t>(bucketIdx)][static_cast<size_t>(layer)]) {
                        m.metallicRoughnessTex = static_cast<uint32_t>(layer);
                        m.flags |= RenderCore::MaterialFlags::kMetallicRoughness;
                    }
                    s_materialGpuTable.push_back(m);

                    // V-MATERIAL-STATIC-0: build a parallel inventory row.
                    // Resolve back to the source TG_TypeShape's listOfTextures
                    // for nodeIdx / human-readable name / dimensions.
                    StaticPropMaterialInventoryEntry inv = {};
                    inv.materialIdx    = static_cast<uint32_t>(s_materialGpuTable.size() - 1);
                    inv.albedoTexLayer = static_cast<uint32_t>(layer);
                    inv.alphaGroup     = (i < s_alphaOffCmdCount) ? 0u : 1u;
                    inv.flags          = m.flags;
                    inv.nodeIdx        = 0xFFFFFFFFu;
                    inv.textureWidth   = 0u;
                    inv.textureHeight  = 0u;
                    inv.usageCount     = 0u;  // accumulated below
                    inv.metallicFactor  = m.metallicFactor;   // V-MATERIAL-PBR-1
                    inv.roughnessFactor = m.roughnessFactor;  // V-MATERIAL-PBR-1
                    std::snprintf(inv.textureName, sizeof(inv.textureName), "(absent)");
                    if (globalPktIdx < s_packets.size()) {
                        const auto& pkt = s_packets[globalPktIdx];
                        if (pkt.owningTypeID < s_types.size()) {
                            const TG_TypeShape* src = s_types[pkt.owningTypeID].source;
                            if (src && src->listOfTextures &&
                                pkt.textureSlot < src->numTextures) {
                                const DWORD nodeIdx =
                                    src->listOfTextures[pkt.textureSlot].mcTextureNodeIndex;
                                inv.nodeIdx = static_cast<uint32_t>(nodeIdx);
                                if (mcTextureManager && nodeIdx != 0xFFFFFFFFu) {
                                    const char* nm = mcTextureManager->getTextureName(nodeIdx);
                                    if (nm && *nm) {
                                        std::snprintf(inv.textureName,
                                                      sizeof(inv.textureName),
                                                      "%s", nm);
                                    } else {
                                        std::snprintf(inv.textureName,
                                                      sizeof(inv.textureName),
                                                      "(unnamed)");
                                    }
                                    const DWORD w = mcTextureManager->getWidth(nodeIdx);
                                    if (w != 0xFFFFFFFFu) {
                                        inv.textureWidth  = static_cast<uint32_t>(w);
                                        inv.textureHeight = static_cast<uint32_t>(w);
                                    }
                                }
                            }
                        }
                    }
                    inv.placeholder = (inv.nodeIdx == 0xFFFFFFFFu)
                                    || (inv.textureName[0] == '(')
                                    || (inv.textureWidth == 0u);
                    s_materialInventory.push_back(inv);
                }
                // Per-draw-slot usage counter accumulates on every reference (insert OR hit).
                if (it->second < s_materialInventory.size())
                    s_materialInventory[it->second].usageCount += 1u;
                s_packetMaterialIdx.push_back(it->second);
            }

            // --- Upload ---
            // GL_STATIC_DRAW: table is mission/map lifetime, not per-frame.
            // Guard byteSize > 0: some drivers misbehave on a zero-byte glBufferData.
            // When no static props exist, s_materialGpuSsbo stays 0 and flush() skips the bind.
            const size_t byteSize =
                s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);

            if (byteSize > 0) {
                // Idempotent: delete any buffer from a prior finalizeGeometry call.
                // finalizeGeometry() should be once-per-map, but defensive cleanup
                // prevents leaks if it is ever called again (partial reinit, hot-reload, etc.).
                if (s_materialGpuSsbo.backendHandle != 0) {
                    GLuint local = static_cast<GLuint>(s_materialGpuSsbo.backendHandle);
                    glDeleteBuffers(1, &local);
                    s_materialGpuSsbo.backendHandle = 0;
                }
                while (glGetError() != GL_NO_ERROR) {}  // drain stale BEFORE operation
                {
                    GLuint local = 0;
                    glGenBuffers(1, &local);
                    s_materialGpuSsbo.backendHandle = static_cast<uint32_t>(local);
                }
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_materialGpuSsbo.backendHandle));
                MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                             static_cast<GLsizeiptr>(byteSize),
                             s_materialGpuTable.data(),
                             GL_STATIC_DRAW);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                const GLenum uploadErr = glGetError();  // sample THIS operation AFTER
                if (uploadErr != GL_NO_ERROR) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "[MATERIAL_GPU v4] GL ERROR after upload: 0x%x — buffer deleted, sampling disabled\n",
                                  uploadErr);
                    MC2_DIAG_FPUTS(buf, stderr);
                    // Delete the corrupt/incomplete buffer so s_materialGpuSsbo returns to 0.
                    // sampleOn checks s_materialGpuSsbo.backendHandle != 0, so this guarantees sampling is
                    // disabled without needing a separate s_materialGpuSidecarValid invalidation.
                    GLuint local = static_cast<GLuint>(s_materialGpuSsbo.backendHandle);
                    glDeleteBuffers(1, &local);
                    s_materialGpuSsbo.backendHandle = 0;

                    // STATICPROP-MATERIAL-SSBO-OWNER-1: corrupt buffer dropped — keep slot unregistered.
                    RenderCore::RenderResourceDesc invalid;
                    invalid.id    = RenderCore::RenderResourceId::StaticPropMaterialGpuBuffer;
                    invalid.valid = false;
                    RenderCore::registerOrUpdateRenderResource(invalid);
                } else {
                    // STATICPROP-MATERIAL-SSBO-OWNER-1: register the live material SSBO
                    // (observe-only metadata; never read by the draw path). Registered at
                    // create, since the byte size is known once the table is uploaded.
                    RenderCore::RenderResourceDesc d;
                    d.id        = s_materialGpuSsbo.id;
                    d.kind      = RenderCore::RenderResourceKind::Buffer;
                    d.lifetime  = s_materialGpuSsbo.lifetime;
                    d.format    = RenderCore::RenderResourceFormat::BufferRaw;
                    d.debugName = s_materialGpuSsbo.debugName;
                    d.glName    = static_cast<GLuint>(s_materialGpuSsbo.backendHandle);
                    d.sizeBytes = static_cast<uint64_t>(byteSize);
                    d.valid     = true;
                    RenderCore::registerOrUpdateRenderResource(d);
                }
            }
            // s_materialGpuSsbo remains 0 when byteSize == 0.

            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=table_upload"
                              " materials=%zu bytes=%zu emitted=%u\n",
                              s_materialGpuTable.size(), byteSize, emittedCount);
                MC2_DIAG_FPUTS(buf, stderr);
            }

            // --- Debug compare ---
            // Load-bearing invariant check: MaterialGpu[materialIdx].albedoTex == texArrayLayer.
            // texArrayLayer is the compare authority until it is deliberately retired.
            // mismatches > 0 means the MaterialGpu table has diverged from the legacy path.
            // This check runs by default (s_materialGpuEnabled is default-ON).
            int mismatches = 0;
            for (uint32_t i = 0; i < emittedCount; ++i) {
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];
                const uint32_t idx          = s_packetMaterialIdx[i];
                const uint32_t albedo       = s_materialGpuTable[idx].albedoTex;
                const uint32_t expected     =
                    static_cast<uint32_t>(layerForPacket[globalPktIdx]);
                if (albedo != expected) {
                    if (mismatches < 10) {  // first-10-only: throttle per-slot noise
                        char buf[128];
                        std::snprintf(buf, sizeof(buf),
                                      "[MATERIAL_GPU v4] MISMATCH slot=%u pkt=%u"
                                      " materialIdx=%u albedoTex=%u expected=%u\n",
                                      i, globalPktIdx, idx, albedo, expected);
                        MC2_DIAG_FPUTS(buf, stderr);
                    }
                    ++mismatches;
                }
            }
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] event=compare emitted=%u mismatches=%d\n",
                              emittedCount, mismatches);
                MC2_DIAG_FPUTS(buf, stderr);
            }

            // C1 fix: record whether the sidecar is aligned with the emitted draw count.
            // This flag gates sampling in flush(). If sizes diverge, sampling is
            // disabled for the whole pass — legacy texArrayLayer is the fallback.
            s_materialGpuSidecarValid =
                (s_packetMaterialIdx.size() == s_sortedPacketOrder.size());

            if (!s_materialGpuSidecarValid) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "[MATERIAL_GPU v4] ERROR materialIdx sidecar size mismatch"
                              " emitted=%zu sidecar=%zu sample_forced=0\n",
                              s_sortedPacketOrder.size(), s_packetMaterialIdx.size());
                MC2_DIAG_FPUTS(buf, stderr);
            }
        } // end s_materialGpuEnabled (sidecar)

        // MC2_EDITOR_STATIC_PRIME_DIAG=1 — compact aggregate DrawCmd census at
        // finalize. Counts types that carry geometry vs types that ended up with
        // a live DrawCmd vs types whose every packet was dropped at the
        // texture-unavailable skip rule (no layer -> no DrawCmd -> invisible).
        // Aggregate-only; bounded; behind the env gate.
        static const bool s_editorStaticPrimeDiag = mc2::gate::get<"MC2_EDITOR_STATIC_PRIME_DIAG">();
        if (s_editorStaticPrimeDiag) {
            // O(1) presence lookup: mark each globalPktIdx that made it into
            // s_sortedPacketOrder. Built once before the type loop.
            std::vector<char> inSorted(s_packets.size(), 0);
            for (uint32_t s = 0; s < s_sortedPacketOrder.size(); ++s) {
                const uint32_t gp = s_sortedPacketOrder[s];
                if (gp < inSorted.size()) inSorted[gp] = 1;
            }

            uint32_t typesWithGeom = 0;
            uint32_t typesWithDrawCmd = 0;
            uint32_t typesDroppedNoLayer = 0;
            for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
                const auto& type = s_types[typeID];
                if (type.packetCount == 0u) continue;
                ++typesWithGeom;

                int hasDrawCmd = 0;
                for (uint32_t pIdx = 0; pIdx < type.packetCount; ++pIdx) {
                    const uint32_t globalPktIdx = type.firstPacket + pIdx;
                    if (globalPktIdx < inSorted.size() && inSorted[globalPktIdx]) {
                        hasDrawCmd = 1;
                        break;
                    }
                }

                if (hasDrawCmd) ++typesWithDrawCmd;
                else            ++typesDroppedNoLayer;
            }
            std::fprintf(stderr,
                "[EDITOR_STATIC_PRIME] typesWithGeom=%u typesWithDrawCmd=%u "
                "typesDroppedNoLayer=%u alphaOffCmd=%u alphaOnCmd=%u\n",
                typesWithGeom, typesWithDrawCmd, typesDroppedNoLayer,
                s_alphaOffCmdCount, s_alphaOnCmdCount);
            std::fflush(stderr);
        }

        std::vector<PerDrawEntry> entries(s_sortedPacketOrder.size());
        std::vector<uint32_t> cmdToBucket(s_sortedPacketOrder.size());
        for (uint32_t i = 0; i < s_sortedPacketOrder.size(); ++i) {
            const uint32_t globalPktIdx = s_sortedPacketOrder[i];
            const auto& pkt = s_packets[globalPktIdx];
            const uint32_t typeID = pkt.owningTypeID;
            cmdToBucket[i] = typeID;
            const auto& type = s_types[typeID];
            PerDrawEntry e{};
            if (layerForPacket[globalPktIdx] >= 0) {
                e.packetID = static_cast<int32_t>(globalPktIdx);
                // Per-packet alpha-test bit: use the packet's own
                // materialFlags from registerType. This is the load-bearing
                // fix for fence-fall / multi-textured buildings — each
                // packet's own alpha bit drives the fragment-shader discard.
                bool packetAlpha =
                    (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0;
                if (!packetAlpha && type.source && type.source->listOfTextures &&
                    pkt.textureSlot < type.source->numTextures &&
                    type.source->listOfTextures[pkt.textureSlot].textureAlpha) {
                    packetAlpha = true;
                }
                e.materialFlags    = packetAlpha
                    ? static_cast<int32_t>(STATIC_PROP_FLAG_ALPHA_TEST) : 0;
                e.maxLocalVertexID = (type.vertexCount > 0u)
                    ? static_cast<int32_t>(type.vertexCount - 1u) : 0;
                e.texArrayLayer    = layerForPacket[globalPktIdx];
                e.uvScaleX         = uvScaleXByPacket[globalPktIdx];
                e.uvScaleY         = uvScaleYByPacket[globalPktIdx];
                // M1.5 C1 fix: three-owner chain.
                //   1. Registry resolves typeID -> recipeIndex (-1 on miss).
                //   2. RenderWorld encodes recipeIndex -> handle bits (0 if invalid).
                //   3. Batcher writes objectIdRaw.
                // Env-OFF behavior: writes nonzero bits, but shader does not declare
                // out uint (Task 8 controls the macro), so writes are invisible.
                // Cheaper than a branch per entry.
                const int32_t m1_5_recipeIndex =
                    GpuStaticPropRegistry::getRecipeIndexForType(typeID);
                e.objectIdRaw =
                    static_cast<int32_t>(RenderWorld::objectIdRawForStaticPropRecipe(m1_5_recipeIndex));

                // MaterialGpu-3: fill materialIdx from v2 sidecar.
                // Guard: sidecar valid means s_packetMaterialIdx.size() == entries.size(),
                // so index i is in bounds.
                if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
                    e.materialIdx = s_packetMaterialIdx[i];
                } else {
                    e.materialIdx = 0u;
                }
                // v1.1: cache primary material per typeID for extraction snapshot.
                // First-time-wins with prefer-alpha-off; s_sortedPacketOrder is alpha-off-first
                // so alpha-off primaries are naturally seen before alpha-on ones.
                {
                    const bool wasAlphaOn = (i >= s_alphaOffCmdCount);
                    const bool hasMat     = (s_materialGpuEnabled && s_materialGpuSidecarValid);
                    GpuStaticPropRegistry::staticPropCacheTypePrimaryMaterial(
                        typeID,
                        e.texArrayLayer,
                        hasMat ? e.materialIdx : 0xFFFFFFFFu,
                        hasMat,
                        wasAlphaOn,
                        type.packetCount > 1u,
                        type.alphaClass,          // v2: 0=alpha-off, 1=alpha-on
                        type.packetCount,         // v2: total packet count for this type
                        type.firstPacket);        // v2: global index of first packet
                }
            }   // end if (layerForPacket[globalPktIdx] >= 0)
            entries[i] = e;
        }

        // MAJ-1: cross-system check — verify entries[i].materialIdx maps to a table entry
        // whose albedoTex equals layerForPacket[s_sortedPacketOrder[i]].
        // This is a non-tautological end-to-end check (PerDrawEntry -> MaterialTable -> layer).
        if (s_materialGpuEnabled && s_materialGpuSidecarValid) {
            int entryMismatches = 0;
            const uint32_t emittedCount2 = static_cast<uint32_t>(s_sortedPacketOrder.size());
            for (uint32_t i = 0; i < emittedCount2; ++i) {
                const uint32_t globalPktIdx  = s_sortedPacketOrder[i];
                const uint32_t expectedAlbedo = static_cast<uint32_t>(layerForPacket[globalPktIdx]);
                const uint32_t mIdx          = entries[i].materialIdx;
                const uint32_t actualAlbedo  = (mIdx < static_cast<uint32_t>(s_materialGpuTable.size()))
                    ? s_materialGpuTable[mIdx].albedoTex
                    : 0xFFFFFFFFu;
                if (actualAlbedo != expectedAlbedo) {
                    ++entryMismatches;
                }
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "[MATERIAL_GPU v4] event=entry_material_idx emitted=%u mismatches=%d\n",
                          emittedCount2, entryMismatches);
            MC2_DIAG_FPUTS(buf, stderr);
        }

        // TIER2-EXCLUDED: dead-path
        glGenBuffers(1, &s_perDrawSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perDrawSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(entries.size() * sizeof(PerDrawEntry)),
                     entries.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        // Cmd-to-bucket SSBO (binding 7 in patch shader): each cmd index
        // maps to its parent typeID so the patch shader can write the
        // type's instanceCount into all cmds (= all packets) of that type.
        if (s_cmdToBucketSsbo == 0) {
            // TIER2-EXCLUDED: dead-path
            glGenBuffers(1, &s_cmdToBucketSsbo);
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_cmdToBucketSsbo);
        MC2_GL_BufferData(GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(cmdToBucket.size() * sizeof(uint32_t)),
                     cmdToBucket.data(),
                     GL_STATIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    {
        GpuStaticPropRegistry::MaterialCacheStats mcs{};
        GpuStaticPropRegistry::staticPropGetMaterialCacheStats(&mcs);
        std::fprintf(stderr,
            "[GPUPROPS] material_cache cache_slots=%u tex_wired=%u mat_wired=%u "
            "multi_packet=%u alpha_on_fallback=%u no_primary=%u\n",
            mcs.cacheVectorSize, mcs.texWired, mcs.matWired,
            mcs.multiPacket, mcs.alphaOnFallback, mcs.noPrimary);
    }

    // Slice 1 step group 3 — allocate base-instance table under !s_globalPoolLegacy.
    // Must come after the per-packet sort (lines above) so s_alphaOffCmdCount /
    // s_alphaOnCmdCount are valid. One uint32_t per draw command per ring frame.
    if (!s_globalPoolLegacy) {
        const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;
        // SSBO-BIND-ALIGN: this per-frame byte count is used as a ring-slot
        // glBindBufferRange offset (slot * stride) at
        // batcher_bindBaseInstanceByCmdSsboForPatch. totalCmds*4 is not generally
        // a multiple of GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, so slots 1+ would
        // misalign and the bind would fail on NVIDIA (GL_INVALID_VALUE). Pad the
        // per-frame stride; alloc (RING_FRAMES*stride), CPU write (slot*stride) and
        // bind all read this same value so they stay consistent.
        s_baseInstanceByCmdBytesPerFrame = static_cast<size_t>(gpuAlignUp(
            static_cast<unsigned long long>(totalCmds) * sizeof(uint32_t),
            static_cast<unsigned long long>(gpuSsboOffsetAlignment())));
        const size_t totalBytes =
            static_cast<size_t>(RING_FRAMES) * s_baseInstanceByCmdBytesPerFrame;
        if (totalBytes > 0) {
            // TIER2-EXCLUDED: dead-path
            glGenBuffers(1, &s_baseInstanceByCmdSsbo);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_baseInstanceByCmdSsbo);
            const GLbitfield mapFlags =
                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            while (glGetError() != GL_NO_ERROR) {}
            glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalBytes, nullptr, mapFlags);
            const GLenum storageErr = glGetError();
            if (storageErr != GL_NO_ERROR) {
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glDeleteBuffers(1, &s_baseInstanceByCmdSsbo);
                s_baseInstanceByCmdSsbo          = 0;
                s_baseInstanceByCmdBytesPerFrame = 0;
                MC2_DIAGFE(stderr,
                    "[COALESCE v1] event=disarmed reason=alloc_failed "
                    "(baseInstanceByCmdSsbo glBufferStorage err=0x%04x bytes=%zu)\n",
                    storageErr, totalBytes);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }
            s_baseInstanceByCmdMap = MC2_GL_MapBufferRange(
                GL_SHADER_STORAGE_BUFFER, 0, totalBytes, mapFlags);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            if (!s_baseInstanceByCmdMap) {
                MC2_DIAGFE(stderr,
                    "[COALESCE v1] event=disarmed reason=alloc_failed "
                    "(baseInstanceByCmdSsbo glMapBufferRange returned null)\n");
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return;
            }
            MC2_DIAGFE(stderr,
                "[COALESCE v1] event=baseInstance_buffer_ok cmds=%u "
                "bytes_per_frame=%zu total_bytes=%zu\n",
                totalCmds, s_baseInstanceByCmdBytesPerFrame, totalBytes);
        }
    }

    // Step 5.12 — sort permutation overwrite (LAST step on success). The
    // identity from Step 5.4 stays if any earlier step returned.
    {
        std::vector<uint32_t> permutation(typeCount, 0u);
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            permutation[typeID] = i;  // typeID → sortedSlot
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_permutationSsbo);
        MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        static_cast<GLsizeiptr>(permutation.size() * sizeof(uint32_t)),
                        permutation.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    s_coalesceEnabled = true;
    s_coalesceArmed   = true;
    {
        const auto coalesceEnd = std::chrono::steady_clock::now();
        const long long elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                coalesceEnd - coalesceStart).count();
        // unique_tex_off / unique_tex_on counts: peak texture-array layer
        // count per group (≤ count of types in that group; equal when
        // every type has a unique texture). Computed by scanning
        // layerForType against s_sortedTypeOrder.
        uint32_t uniqueTexOff = 0, uniqueTexOn = 0;
        for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
            const uint32_t typeID = s_sortedTypeOrder[i];
            const auto& t = s_types[typeID];
            if (layerForType[typeID] < 0) continue;
            const uint32_t layer = static_cast<uint32_t>(layerForType[typeID]);
            if (t.alphaClass == 0u) {
                if (layer + 1u > uniqueTexOff) uniqueTexOff = layer + 1u;
            } else {
                if (layer + 1u > uniqueTexOn)  uniqueTexOn  = layer + 1u;
            }
        }
        MC2_DIAGFE(stderr, "[COALESCE v1] event=armed types=%u "
                     "off_types=%u on_types=%u unique_tex_off=%u unique_tex_on=%u "
                     "per_frame_inst_bytes=%zu elapsed_ms=%lld\n",
                     typeCount,
                     s_alphaOffCount, s_alphaOnCount,
                     uniqueTexOff, uniqueTexOn,
                     s_coalescePerFrameInstanceBytes,
                     elapsedMs);
    }
}
