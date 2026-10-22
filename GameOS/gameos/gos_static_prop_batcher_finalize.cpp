// Finalize native prop geometry, CPU material sources and packet metadata once per mission.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "../../mclib/texture_assets.h"
#include <cstring>
#include <map>
#include "gos_static_prop_finalize_frame.h"
#include "vulkan_color_prop_island.h"   // mc2_static_prop_geometry_cpu
#include "mc2fmt.h"

// Does a packet alpha-test: its own flag, or its texture slot carrying alpha on the type source (null/released texture list reads as no alpha).
bool GpuStaticPropBatcher::packetHasAlpha(const GpuStaticPropPacket& pkt, const TG_TypeShape* src) {
    return (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) != 0 ||
           (src && src->listOfTextures && pkt.textureSlot < src->numTextures && src->listOfTextures[pkt.textureSlot].textureAlpha);
}

// Log the texture tier, clear the per-packet side tables and load the programs.
void GpuStaticPropBatcher::finalizePrologue() {
    // One-shot log: always emit so the log confirms which tier is active.
    mc2::fmt::print_out("[StaticPropBatcher] tex tier: {:d}px (MC2_STATICPROP_TEX_TIER) ktx={}\n", mc2::fmt::as_i32(texTier()), mc2::fmt::safe(s_materialKtxEnabled ? "ON" : "OFF"));
    std::fflush(stdout);

    GpuStaticPropRegistry::staticPropRegistryClearMaterialCache();
    s_packetTexArrayLayer.clear();  // v2: will be repopulated from layerForPacket below
    s_packetUvScaleX.clear();       // SHADOW-PROP-ALPHA-ATLAS-1: repopulated below
    s_packetUvScaleY.clear();

    coalesce_resetEnvOnce();
}

void GpuStaticPropBatcher::publishGeometry() {
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
        }
        // The Vulkan prop island reads the same table from this CPU copy.
        s_typeHotColors.resize(s_types.size() * 12);
        for (size_t i = 0; i < s_types.size(); ++i)
            std::memcpy(&s_typeHotColors[i * 12], &perTypeBlob[i], sizeof(PerTypeShaderData));
    }

    s_sharedVboBytes = static_cast<uint64_t>(s_stagingVbo.size());
    s_sharedIboBytes = static_cast<uint64_t>(s_stagingIbo.size()) * sizeof(uint32_t);
    s_stagingVbo.shrink_to_fit();
    s_stagingIbo.shrink_to_fit();

    mc2::fmt::print_err("[GPUPROPS] finalize: {:d} types, {:d} packets, cpu_geometry_bytes={:d}\n",
                 mc2::fmt::as_size(s_types.size()), mc2::fmt::as_size(s_packets.size()),
                 mc2::fmt::as_u64(s_sharedVboBytes + s_sharedIboBytes));

    if (s_gpuPropsTrace) {
        mc2::fmt::print_err("[GPUPROPS v1] event=register_summary path={:d} regCalls[init={:d} ims={:d} splog={:d} unk={:d}] lateDrops[init={:d} ims={:d} splog={:d} unk={:d}] typeIndexSize={:d}\n",
            mc2::fmt::as_i32(g_lightProbeSetupPath),
            mc2::fmt::as_u32(g_regCall[1]), mc2::fmt::as_u32(g_regCall[2]), mc2::fmt::as_u32(g_regCall[3]), mc2::fmt::as_u32(g_regCall[0]),
            mc2::fmt::as_u32(g_regLateDrop[1]), mc2::fmt::as_u32(g_regLateDrop[2]), mc2::fmt::as_u32(g_regLateDrop[3]), mc2::fmt::as_u32(g_regLateDrop[0]),
            mc2::fmt::as_size(s_typeIndex.size()));
    }

    // Bake each packet's alpha-test answer now, while the type sources are alive: the Vulkan colour island asks per draw, after a source's
    // texture list may have been released (second campaign mission).
    s_packetAlphaBaked.assign(s_packets.size(), 0u);
    for (size_t pi = 0; pi < s_packets.size(); ++pi) {
        const auto& pkt = s_packets[pi];
        const auto* type = pkt.owningTypeID < s_types.size() ? &s_types[pkt.owningTypeID] : nullptr;
        // Authored override cutouts must occlude later vegetation. Stock partial
        // coverage retains PROP-COVERAGE-1; opaque override packets stay opaque.
        if (packetHasAlpha(pkt, type ? type->source : nullptr))
            s_packetAlphaBaked[pi] = type && type->isOverride ? 2u : 1u;
    }

    s_geometryFinalized = true;
}

// Decide whether coalesce is wanted and lay out the permutation, the group ranges and the instance pool.
bool GpuStaticPropBatcher::prepareMaterialLayout(StaticPropFinalizeFrame& f) {
    auto& coalesceStart = f.coalesceStart; auto& typeCount = f.typeCount;
    coalesceStart = std::chrono::steady_clock::now();

    coalesce_resetEnvOnce();
    const bool coalesceWanted =
        !s_coalesceEnvDisabled;

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
            typeAlpha = typeAlpha || packetHasAlpha(pkt, src);
        }
        if (malformed) {
            if (coalesceWanted) {
                MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=malformed_type type=%u\n",
                             typeID);
                s_coalesceLayoutReady = false;
                s_coalesceEnabled     = false;
                s_coalesceArmed       = false;
                return true;
            }
            // Non-coalesce: conservative fallback — legacy draws it correctly regardless.
            mc2::fmt::print_err("[DRAW_PACKET v1] WARNING: type={:d} malformed src; alphaClass=0\n",
                         mc2::fmt::as_u32(typeID));
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
        return true;
    }

    typeCount = static_cast<uint32_t>(s_types.size());
    if (typeCount == 0) {
        // No types registered — nothing to coalesce; legacy will draw
        // nothing either. Leave flags false; permutation is empty (5.4
        // helper no-ops on typeCount==0). Treat as silent disarm.
        s_coalesceLayoutReady = false;
        s_coalesceEnabled     = false;
        s_coalesceArmed       = false;
        return true;
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
        return true;
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

    s_coalesceLayoutReady = true;
    return false;
}

void GpuStaticPropBatcher::buildPerPacketDrawTables(StaticPropFinalizeFrame& f) {
    auto& layerForPacket = f.layerForPacket; auto& uvScaleXByPacket = f.uvScaleXByPacket; auto& uvScaleYByPacket = f.uvScaleYByPacket;
    {
        s_slotBucketIndex.clear();
        s_bucketCmdCount.clear();
        for (uint8_t group = 0; group <= 1; ++group) {

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
                    mc2::fmt::to_buf_n(inv.textureName, sizeof(inv.textureName), "(absent)");
                    if (globalPktIdx < s_packets.size()) {
                        const auto& pkt = s_packets[globalPktIdx];
                        if (pkt.owningTypeID < s_types.size()) {
                            const TG_TypeShape* src = s_types[pkt.owningTypeID].source;
                            if (src && src->listOfTextures &&
                                pkt.textureSlot < src->numTextures) {
                                const DWORD nodeIdx =
                                    src->listOfTextures[pkt.textureSlot].mcTextureNodeIndex;
                                inv.nodeIdx = static_cast<uint32_t>(nodeIdx);
                                if (mc2::textures::ready() && nodeIdx != 0xFFFFFFFFu) {
                                    const char* nm = mc2::textures::name(nodeIdx);
                                    if (nm && *nm) {
                                        mc2::fmt::to_buf_n(inv.textureName, sizeof(inv.textureName), "{}", mc2::fmt::safe(nm));
                                    } else {
                                        mc2::fmt::to_buf_n(inv.textureName, sizeof(inv.textureName), "(unnamed)");
                                    }
                                    const auto pixels = gos_GetTextureTexels(mc2::textures::sourceId(nodeIdx));
                                    if (pixels.status == GosTexels::Status::Ready) {
                                        inv.textureWidth  = pixels.width;
                                        inv.textureHeight = pixels.height;
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

            const size_t byteSize = s_materialGpuTable.size() * sizeof(RenderCore::MaterialGpu);

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
                        mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] MISMATCH slot={:d} pkt={:d} materialIdx={:d} albedoTex={:d} expected={:d}\n", mc2::fmt::as_u32(i), mc2::fmt::as_u32(globalPktIdx), mc2::fmt::as_u32(idx), mc2::fmt::as_u32(albedo), mc2::fmt::as_u32(expected));
                        MC2_DIAG_FPUTS(buf, stderr);
                    }
                    ++mismatches;
                }
            }
            {
                char buf[96];
                mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] event=compare emitted={:d} mismatches={:d}\n", mc2::fmt::as_u32(emittedCount), mc2::fmt::as_i32(mismatches));
                MC2_DIAG_FPUTS(buf, stderr);
            }

            // C1 fix: record whether the sidecar is aligned with the emitted draw count.
            // This flag gates sampling in flush(). If sizes diverge, sampling is
            // disabled for the whole pass — legacy texArrayLayer is the fallback.
            s_materialGpuSidecarValid =
                (s_packetMaterialIdx.size() == s_sortedPacketOrder.size());

            if (!s_materialGpuSidecarValid) {
                char buf[128];
                mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] ERROR materialIdx sidecar size mismatch emitted={:d} sidecar={:d} sample_forced=0\n", mc2::fmt::as_size(s_sortedPacketOrder.size()), mc2::fmt::as_size(s_packetMaterialIdx.size()));
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
            mc2::fmt::print_err("[EDITOR_STATIC_PRIME] typesWithGeom={:d} typesWithDrawCmd={:d} typesDroppedNoLayer={:d} alphaOffCmd={:d} alphaOnCmd={:d}\n", mc2::fmt::as_u32(typesWithGeom), mc2::fmt::as_u32(typesWithDrawCmd), mc2::fmt::as_u32(typesDroppedNoLayer), mc2::fmt::as_u32(s_alphaOffCmdCount), mc2::fmt::as_u32(s_alphaOnCmdCount));
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
            mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] event=entry_material_idx emitted={:d} mismatches={:d}\n", mc2::fmt::as_u32(emittedCount2), mc2::fmt::as_i32(entryMismatches));
            MC2_DIAG_FPUTS(buf, stderr);
        }

    }
}

// Report the material cache and allocate the base-instance table and permutation.
bool GpuStaticPropBatcher::reportMaterialCache(StaticPropFinalizeFrame& f) {
    {
        GpuStaticPropRegistry::MaterialCacheStats mcs{};
        GpuStaticPropRegistry::staticPropGetMaterialCacheStats(&mcs);
        mc2::fmt::print_err("[GPUPROPS] material_cache cache_slots={:d} tex_wired={:d} mat_wired={:d} multi_packet={:d} alpha_on_fallback={:d} no_primary={:d}\n",
            mc2::fmt::as_u32(mcs.cacheVectorSize), mc2::fmt::as_u32(mcs.texWired), mc2::fmt::as_u32(mcs.matWired),
            mc2::fmt::as_u32(mcs.multiPacket), mc2::fmt::as_u32(mcs.alphaOnFallback), mc2::fmt::as_u32(mcs.noPrimary));
    }

    return false;
}

// Arm coalesce and log the finalize summary.
void GpuStaticPropBatcher::armCoalesce(StaticPropFinalizeFrame& f) {
    auto& coalesceStart = f.coalesceStart; auto& typeCount = f.typeCount; auto& layerForType = f.layerForType;
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

// Builds the shared geometry buffers, the coalesce layout, the texture arrays and the per-packet draw tables once per map.
// The already-finalized bail-out stays here; each phase is a member helper declared in gos_static_prop_batcher.h.
// A helper returns true when the original function would have returned at that point.
void GpuStaticPropBatcher::buildAuthoredMaterials() {
    materialSources_ = {};
    materialSources_.packetIds.assign(s_packets.size(), -1);
    // Record identity includes the winning manifest/provider and override, then
    // the authored slot. Albedo sharing cannot collapse different detail sets.
    std::map<std::pair<const ModelOverrideRecord*, uint32_t>, int32_t> ids;
    for (size_t pi = 0; pi < s_packets.size(); ++pi) {
        const auto& packet = s_packets[pi];
        if (packet.owningTypeID >= s_types.size()) continue;
        const auto& type = s_types[packet.owningTypeID];
        const auto* owner = type.isOverride ? type.materialSource : nullptr;
        if (!owner) continue;
        for (const auto& input : owner->materials) {
            if (input.slot != packet.textureSlot) continue;
            const auto key = std::make_pair(owner, input.slot);
            auto [item, added] = ids.try_emplace(key, -1);
            auto& [identity, materialId] = *item;
            if (added) {
                const int32_t id = static_cast<int32_t>(materialSources_.table.size());
                const bool ok = mc2prop::addMaterial(materialSources_, *owner, input);
                materialId = ok ? id : -1;
                mc2::fmt::print_err("[MODOVERRIDE] authored material {} slot={} id={}\n",
                    owner->appearanceName, input.slot, materialId);
            }
            materialSources_.packetIds[pi] = materialId;
            break;
        }
    }
}

void GpuStaticPropBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;

    StaticPropFinalizeFrame f;
    finalizePrologue();
    publishGeometry();
    buildAuthoredMaterials();

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

    if (prepareMaterialLayout(f)) return;

    if (buildTextureArrays(f)) return;
    buildPerPacketDrawTables(f);

    if (reportMaterialCache(f)) return;

    armCoalesce(f);
}

bool GpuStaticPropBatcher::packetAlphaTest(uint32_t packet) const {
    if (!s_geometryFinalized || packet >= s_packets.size() || packet >= s_packetAlphaBaked.size()) return false;
    return s_packetAlphaBaked[packet] != 0;   // baked at finalize (VK-PROP-ALPHA-BAKE-1)
}

extern "C" int mc2_static_prop_packet_alpha_test(uint32_t packet) {
    if (!s_geometryFinalized || packet >= s_packetAlphaBaked.size()) return 0;
    return s_packetAlphaBaked[packet];
}

extern "C" int mc2_static_prop_geometry_cpu(const void** vbo, uint64_t* vboBytes, const void** ibo, uint64_t* iboBytes) {
    if (!vbo || !vboBytes || !ibo || !iboBytes) return 0;
    if (!s_geometryFinalized || s_fatalRegistrationFailure || s_stagingVbo.empty() || s_stagingIbo.empty()) return 0;
    *vbo = s_stagingVbo.data();
    *vboBytes = static_cast<uint64_t>(s_stagingVbo.size());
    *ibo = s_stagingIbo.data();
    *iboBytes = static_cast<uint64_t>(s_stagingIbo.size()) * sizeof(uint32_t);
    return 1;
}
