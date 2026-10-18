// GpuMechBatcher: mech type / LOD registration, the shared vertex buffer upload, normal recomputation and geometry finalise.
// Declared in the class headers; shared names in gos_mech_batcher_internal.h.
#include "gos_mech_batcher_internal.h"
#include "mc2fmt.h"


// ---------------------------------------------------------------------------
// Registration (Task 4)
// ---------------------------------------------------------------------------

// MECH-NORMALS-FIX-1: recompute normals for one node's triangle-soup vertices
// in-place, mech-locally, over the byte range [startByte, endByte). The verts
// are a triangle list (corners 3k,3k+1,3k+2 form triangle k), all in mech model
// space — same space as the cooked normal the shader already consumes, so no
// axis swap here. mode 1 = geometric face normal to all 3 corners (faceted).
// mode 2 = exact-position-grouped, angle-threshold accumulation that preserves
// hard edges (faces meeting at > smoothDeg do not blend). Grouping is exact on
// source position bits (shared verts have bit-identical TG positions) and stays
// within this node (separate rigid mech pieces are never blended). Face normals
// are oriented to the cooked-normal hemisphere to stay winding-agnostic.
// endByte: exclusive end of this node's data in vbo; use vbo.size() for
// "rest of buffer". Added for per-node bounding (MECH-NORMALS-FIX-1 rebuild).
void recomputeMechNodeNormals(std::vector<uint8_t>& vbo,
                                     size_t startByte, size_t endByte,
                                     int mode, float smoothDeg) {
    if (mode != 1 && mode != 2) return;
    const size_t stride = sizeof(GpuMechVertex);
    if (startByte >= endByte || startByte >= vbo.size()) return;
    const size_t clampedEnd = (endByte < vbo.size()) ? endByte : vbo.size();
    const size_t n = (clampedEnd - startByte) / stride;   // corners
    if (n < 3) return;
    GpuMechVertex* V = reinterpret_cast<GpuMechVertex*>(vbo.data() + startByte);
    const size_t triCount = n / 3;

    auto dot3 = [](const float* a, const float* b) {
        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
    };

    // Per-triangle area-weighted geometric normal (un-normalized cross), plus a
    // unit copy for the angle test. Oriented to agree with the cooked-normal
    // hemisphere (sum of the 3 corners' cooked normals) to handle winding.
    std::vector<std::array<float,3>> faceRaw(triCount), faceUnit(triCount);
    for (size_t k = 0; k < triCount; ++k) {
        const float* p0 = V[3*k+0].position;
        const float* p1 = V[3*k+1].position;
        const float* p2 = V[3*k+2].position;
        float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
        float fr[3] = { e1[1]*e2[2]-e1[2]*e2[1],
                        e1[2]*e2[0]-e1[0]*e2[2],
                        e1[0]*e2[1]-e1[1]*e2[0] };
        float ref[3] = {
            V[3*k+0].normal[0] + V[3*k+1].normal[0] + V[3*k+2].normal[0],
            V[3*k+0].normal[1] + V[3*k+1].normal[1] + V[3*k+2].normal[1],
            V[3*k+0].normal[2] + V[3*k+1].normal[2] + V[3*k+2].normal[2] };
        if (dot3(fr, ref) < 0.0f) { fr[0]=-fr[0]; fr[1]=-fr[1]; fr[2]=-fr[2]; }
        faceRaw[k] = { fr[0], fr[1], fr[2] };
        float m = std::sqrt(dot3(fr, fr));
        float inv = (m > 1e-12f) ? 1.0f/m : 0.0f;
        faceUnit[k] = { fr[0]*inv, fr[1]*inv, fr[2]*inv };
    }

    if (mode == 1) {
        for (size_t k = 0; k < triCount; ++k)
            for (int c = 0; c < 3; ++c) {
                V[3*k+c].normal[0] = faceUnit[k][0];
                V[3*k+c].normal[1] = faceUnit[k][1];
                V[3*k+c].normal[2] = faceUnit[k][2];
            }
        return;
    }

    // mode 2: group corners by exact source position (3 float-bit key), then for
    // each corner accumulate area-weighted face normals of same-position corners
    // whose face is within smoothDeg of this corner's face (hard-edge preserving).
    const float cosT = std::cos(smoothDeg * 3.14159265358979f / 180.0f);
    std::map<std::array<uint32_t,3>, std::vector<uint32_t>> groups;
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        std::array<uint32_t,3> kkey{};
        std::memcpy(kkey.data(), V[i].position, 12);
        groups[kkey].push_back(i);
    }
    std::vector<std::array<float,3>> out(n);
    for (auto& kv : groups) {
        const std::vector<uint32_t>& g = kv.second;
        for (uint32_t i : g) {
            const size_t ti = i / 3;
            float acc[3] = {0,0,0};
            for (uint32_t j : g) {
                const size_t tj = j / 3;
                if (dot3(faceUnit[ti].data(), faceUnit[tj].data()) >= cosT) {
                    acc[0] += faceRaw[tj][0];
                    acc[1] += faceRaw[tj][1];
                    acc[2] += faceRaw[tj][2];
                }
            }
            float m = std::sqrt(dot3(acc, acc));
            if (m > 1e-12f) { out[i] = { acc[0]/m, acc[1]/m, acc[2]/m }; }
            else            { out[i] = faceUnit[ti]; }  // degenerate fallback
        }
    }
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        V[i].normal[0] = out[i][0];
        V[i].normal[1] = out[i][1];
        V[i].normal[2] = out[i][2];
    }
}

void GpuMechBatcher::registerTypeLod(const Mech3DAppearanceType* mechType, int lod) {
    if (!mechType) return;
    const TypeLodKey key{mechType, lod};
    if (s_typeLodIndex.count(key)) return;  // idempotent
    static const bool s_mechRestoreTrace = mc2::gate::get<"MC2_MECH_RESTORE_TRACE">();
    if (s_geometryFinalized) {
        // Late registration (post-finalizeGeometry). DO NOT drop: stage
        // the type into the retained s_stagingVbo/Ibo (fall through to
        // the body below) and mark pending so finalizePending() rebuilds
        // the shared buffers. Caller MUST invoke finalizePending() once
        // its late-spawn batch completes (VPL-#11: logistics.cpp SP
        // force-group loop). Until then submitActor still fast-rejects
        // this type (s_typeLodIndex has the entry but the GL buffers
        // predate it) -- harmless because no frame renders between the
        // late registers and the finalizePending() call on that path.
        MC2_DIAGFE(stderr,
            "[MECHBATCHER v1] event=late_register type=%p lod=%d (staged_pending)\n",
            (void*)mechType, lod);
        if (s_mechRestoreTrace)
            mc2::fmt::print_err("[MECHRESTORE v1] event=register type={:016X} lod={:d} result=staged_pending finalized=1\n", mc2::fmt::as_ptr((void*)mechType), mc2::fmt::as_i32(lod));
        s_pendingLateTypes = true;
        ++s_lateStagedCount;
        // fall through -- stage like a normal pre-finalize registration
    }

    TG_TypeMultiShape* typeMulti = mechType->mechShape[lod];
    if (!typeMulti) return;

    const int numNodes = typeMulti->GetNumShapes();
    if (numNodes == 0 || numNodes > 255) {
        if (numNodes > 255) {
            MC2_DIAGFE(stderr,
                "[MECHBATCHER v1] event=u8_bone_overflow type=%p lod=%d numNodes=%d\n",
                (void*)mechType, lod, numNodes);
        }
        return;
    }

    // BT2018-SKEL-GPU-PALETTE-PLACEMENT-1: an imported animated mech is ONE merged
    // node but skins over its full BT skeleton. Use its joint count as numBones and
    // its per-vertex bone index for packing. Stock mechs (importJointCount==0) are
    // byte-unchanged.
    const unsigned char* importBoneIdx = nullptr;
    int importNumVerts = 0;
    const int importJointCount =
        mc2mechanim::ImportedGpuTypeInfo((const void*)typeMulti, &importBoneIdx, &importNumVerts);
    const bool importGpu = (importJointCount > 0 && importJointCount <= 255);

    static const bool s_nodeTrace = mc2::gate::get<"MC2_MECH_NODE_TRACE">();
    if (s_nodeTrace) {
        mc2::fmt::print_err("[MECHREG v1] event=register type={:016X} lod={:d} numBones={:d}\n", mc2::fmt::as_ptr((void*)mechType), mc2::fmt::as_i32(lod), mc2::fmt::as_i32(numNodes));
    }

    const uint32_t typeLodIdx = (uint32_t)s_typeLodRecords.size();
    s_typeLodIndex[key] = typeLodIdx;
    if (s_mechRestoreTrace)
        mc2::fmt::print_err("[MECHRESTORE v1] event=register type={:016X} lod={:d} result=registered finalized=0 numBones={:d}\n", mc2::fmt::as_ptr((void*)mechType), mc2::fmt::as_i32(lod), mc2::fmt::as_i32(numNodes));

    GpuMechTypeLodRecord rec{};
    rec.firstBoneIndex = 0;
    rec.numBones       = importGpu ? (uint32_t)importJointCount : (uint32_t)numNodes;
    rec.importedGpuType = importGpu ? (const void*)typeMulti : nullptr;
    if (importGpu)
        mc2::fmt::print_err("[MECH_IMPORT_GPU] registerTypeLod lod={:d} numBones {:d}->{:d} verts={:d}\n", mc2::fmt::as_i32(lod), mc2::fmt::as_i32(numNodes), mc2::fmt::as_i32(importJointCount), mc2::fmt::as_i32(importNumVerts));
    rec.firstPacket    = (uint32_t)s_packets.size();
    rec.packetCount    = 0;
    rec.vertexCount    = 0;
    rec.sourceNode0    = nullptr;

    for (int nodeIdx = 0; nodeIdx < numNodes; ++nodeIdx) {
        TG_TypeNodePtr tnode = typeMulti->GetTypeNode(nodeIdx);
        if (!tnode || tnode->GetNodeType() != SHAPE_NODE) continue;
        TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(tnode);
        if (nodeIdx == 0) rec.sourceNode0 = typeShape;

        // Skip spotlight leaves: TG_Shape::isSpotlight is set on the
        // per-instance shape from "SpotLight_*" node name prefix at
        // tgl.cpp:259/475. Post-T3.1 ((E) SpotLight_ -> real illumination),
        // mech spotlight child shapes are emitted as real TG_Light
        // registrations via Mech3DAppearance::updateGeometry (T1.6/T1.7,
        // commit fceb304). Skip the geometry emission here — the cone is
        // no longer drawn, only the light contribution lives downstream.
        // See docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md.
        const char* nodeName = tnode->getNodeId();
        if (nodeName && S_strnicmp(nodeName, "SpotLight_", 10) == 0) {
            if (s_nodeTrace) {
                mc2::fmt::print_err("[MECHREG v1] event=skip_spotlight type={:016X} lod={:d} nodeIdx={:d} name={}\n", mc2::fmt::as_ptr((void*)mechType), mc2::fmt::as_i32(lod), mc2::fmt::as_i32(nodeIdx), mc2::fmt::safe(nodeName));
            }
            // [SPOTLIGHT_REAL_TRACE v1] T0.2 (a) — record SpotLight_ TypeNode
            // observed at recipe build. Tracks max across LODs since each
            // instance only renders at one LOD; the running submitActor counter
            // (b) below cares about whether the actor has ANY spotlight nodes,
            // and lazy-registration in T1.6 walks the live mechShape regardless
            // of LOD layout.
            ++s_spotlightReal_typeNodeEvents;
            const uint32_t newCount = s_spotlightRealTypeNodeCount[mechType] + 1;
            s_spotlightRealTypeNodeCount[mechType] = newCount;
            if (!s_spotlightReal_firstHitTypeNode) {
                s_spotlightReal_firstHitTypeNode = true;
                MC2_DIAGFE(stderr,
                    "[SPOTLIGHT_REAL_TRACE v1] event=first_hit site=mech_recipe_build "
                    "type=%p lod=%d nodeIdx=%d name=%s\n",
                    (const void*)mechType, lod, nodeIdx, nodeName);
                std::fflush(stderr);
            }
            if (s_mechSpotlightRealTrace) {
                MC2_DIAGFE(stderr,
                    "[SPOTLIGHT_REAL_TRACE v1] event=type_spotlight_node "
                    "type=%p lod=%d nodeIdx=%d name=%s type_node_count=%u\n",
                    (const void*)mechType, lod, nodeIdx, nodeName, newCount);
            }
            continue;
        }

        if (!typeShape->numTypeTriangles || !typeShape->listOfTypeTriangles ||
            !typeShape->listOfTypeVertices) continue;

        const int32_t baseVertex = (int32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex));

        // Group triangles by localTextureHandle (same as static prop batcher).
        const uint32_t numTris = typeShape->numTypeTriangles;
        uint32_t runStart = 0;
        while (runStart < numTris) {
            const DWORD runTexSlot =
                typeShape->listOfTypeTriangles[runStart].localTextureHandle;
            uint32_t runEnd = runStart;
            while (runEnd < numTris &&
                   typeShape->listOfTypeTriangles[runEnd].localTextureHandle == runTexSlot)
                ++runEnd;

            const uint32_t packetFirstIndex = (uint32_t)s_stagingIbo.size();

            for (uint32_t t = runStart; t < runEnd; ++t) {
                const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];
                const float cornerU[3] = { tri.uvdata.u0, tri.uvdata.u1, tri.uvdata.u2 };
                const float cornerV[3] = { tri.uvdata.v0, tri.uvdata.v1, tri.uvdata.v2 };

                // NORMALS-1: per-face UV-gradient tangent in object/local space (same frame
                // as src.normal — the vert shader transforms both identically). Encoded
                // octahedral into tangentOct for imported-GPU types ONLY; stock types keep
                // the zero-fill below (VBO byte-identical). Degenerate UV (|det|~0) or zero-
                // length tangent -> leave zero, which the frag treats as "no tangent" and
                // falls back to v_normal. No NaN can escape (all divides are guarded).
                int16_t faceTanOct[2] = { 0, 0 };
                if (importGpu) {
                    const TG_TypeVertex& p0 = typeShape->listOfTypeVertices[tri.Vertices[0]];
                    const TG_TypeVertex& p1 = typeShape->listOfTypeVertices[tri.Vertices[1]];
                    const TG_TypeVertex& p2 = typeShape->listOfTypeVertices[tri.Vertices[2]];
                    const float e1x = p1.position.x - p0.position.x;
                    const float e1y = p1.position.y - p0.position.y;
                    const float e1z = p1.position.z - p0.position.z;
                    const float e2x = p2.position.x - p0.position.x;
                    const float e2y = p2.position.y - p0.position.y;
                    const float e2z = p2.position.z - p0.position.z;
                    const float du1 = cornerU[1] - cornerU[0];
                    const float dv1 = cornerV[1] - cornerV[0];
                    const float du2 = cornerU[2] - cornerU[0];
                    const float dv2 = cornerV[2] - cornerV[0];
                    const float det = du1 * dv2 - du2 * dv1;
                    if (std::fabs(det) > 1e-8f) {
                        const float r = 1.0f / det;
                        float tx = r * (dv2 * e1x - dv1 * e2x);
                        float ty = r * (dv2 * e1y - dv1 * e2y);
                        float tz = r * (dv2 * e1z - dv1 * e2z);
                        const float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
                        if (tl > 1e-8f) {
                            tx /= tl; ty /= tl; tz /= tl;
                            // octahedral encode: unit vec3 -> vec2 in [-1,1]
                            const float invL1 = 1.0f / (std::fabs(tx) + std::fabs(ty) + std::fabs(tz));
                            float ox = tx * invL1;
                            float oy = ty * invL1;
                            if (tz < 0.0f) {
                                const float oxn = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
                                const float oyn = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
                                ox = oxn; oy = oyn;
                            }
                            auto packS = [](float v) -> int16_t {
                                float s = v * 32767.0f;
                                if (s >  32767.0f) s =  32767.0f;
                                if (s < -32767.0f) s = -32767.0f;
                                return (int16_t)std::lrintf(s);
                            };
                            faceTanOct[0] = packS(ox);
                            faceTanOct[1] = packS(oy);
                        }
                    }
                }

                for (int c = 0; c < 3; ++c) {
                    const TG_TypeVertex& src =
                        typeShape->listOfTypeVertices[tri.Vertices[c]];

                    GpuMechVertex vert{};
                    std::memcpy(vert.position, &src.position.x, 12);
                    std::memcpy(vert.normal,   &src.normal.x,   12);
                    vert.uv[0] = cornerU[c];
                    vert.uv[1] = cornerV[c];
                    // boneIndices: .x = nodeIdx (rigid, Slice A), .yzw = 0.
                    // 1B-GPU: imported mech uses its per-vertex BT bone index (one bone
                    // per vertex) into the imported joint palette instead of nodeIdx.
                    uint8_t boneSel = (uint8_t)(nodeIdx & 0xFF);
                    if (importGpu && importBoneIdx) {
                        const uint32_t gv = tri.Vertices[c];
                        if ((int)gv < importNumVerts) boneSel = importBoneIdx[gv];
                    }
                    vert.boneIndices[0] = boneSel;
                    vert.boneIndices[1] = 0;
                    vert.boneIndices[2] = 0;
                    vert.boneIndices[3] = 0;
                    // boneWeights: .x = 255 (= 1.0 normalized), .yzw = 0
                    vert.boneWeights[0] = 255;
                    vert.boneWeights[1] = 0;
                    vert.boneWeights[2] = 0;
                    vert.boneWeights[3] = 0;
                    // tangentOct: NORMALS-1 face tangent for imported types; zero for stock
                    // (no .tglgpu sidecar) -> frag falls back to v_normal.
                    vert.tangentOct[0] = faceTanOct[0];
                    vert.tangentOct[1] = faceTanOct[1];
                    vert.aRGBLight = src.aRGBLight;

                    s_stagingVbo.insert(s_stagingVbo.end(),
                        reinterpret_cast<uint8_t*>(&vert),
                        reinterpret_cast<uint8_t*>(&vert) + sizeof(GpuMechVertex));

                    // Write index LOCAL to this packet's baseVertex.
                    // glDrawElementsInstancedBaseVertex adds pkt.baseVertex at draw time,
                    // so IBO must contain (globalVertex - baseVertex), not globalVertex.
                    const uint32_t localIdx =
                        (uint32_t)(s_stagingVbo.size() / sizeof(GpuMechVertex) - 1u)
                        - (uint32_t)baseVertex;
                    s_stagingIbo.push_back(localIdx);
                    ++rec.vertexCount;
                }
            }

            // Derive ALPHA_TEST_BIT from the texture slot's textureAlpha flag.
            uint32_t matFlags = 0;
            if (runTexSlot < (DWORD)typeShape->numTextures &&
                typeShape->listOfTextures[runTexSlot].textureAlpha) {
                matFlags = 1u;  // ALPHA_TEST_BIT (matches mech.frag ALPHA_TEST_BIT constant)
            }

            GpuMechPacket pkt{};
            pkt.firstIndex          = packetFirstIndex;
            pkt.indexCount          = (runEnd - runStart) * 3;
            pkt.baseVertex          = baseVertex;
            pkt.textureSlot         = (uint32_t)runTexSlot;
            pkt.materialFlags       = matFlags;
            pkt.owningTypeLodRecord = typeLodIdx;
            pkt.nodeLocalIndex      = (uint32_t)nodeIdx;
            pkt.owningTypeShape     = typeShape;
            s_packets.push_back(pkt);
            ++rec.packetCount;

            runStart = runEnd;
        }

        // MECH-NORMALS-FIX-1: record this node's byte range in s_stagingVbo
        // so uploadMechGeometryVbo() can apply recomputeMechNodeNormals per-node
        // on a transient copy (preserving s_stagingVbo as pristine cooked data).
        // Registration no longer mutates normals; the recompute runs at upload time.
        {
            const size_t nodeStart = (size_t)baseVertex * sizeof(GpuMechVertex);
            const size_t nodeEnd   = s_stagingVbo.size();
            s_nodeVboRanges.push_back({nodeStart, nodeEnd});
        }
    }

    s_typeLodRecords.push_back(rec);
}

// MECH-NORMALS-FIX-1: Create (or recreate) the immutable VBO from s_stagingVbo,
// applying the current s_mechNormalsMode/s_mechNormalsSmoothDeg on a transient
// copy so s_stagingVbo stays pristine (cooked normals). Also sets up all 7 vertex
// attribute pointers on the already-bound VAO (s_sharedVao must be bound by caller).
//
// VBO recreation:
//   - If s_sharedVbo already exists (rebuild path): delete it first (glBufferStorage
//     buffers are immutable — they CANNOT be resized or re-uploaded in place).
//   - Allocate a new s_sharedVbo with glGenBuffers + glBufferStorage.
//   - Re-run glVertexAttribPointer for all 7 attribs, which bind to the currently
//     bound GL_ARRAY_BUFFER. The element-array binding (s_sharedIbo) is VAO state
//     and is NOT disturbed by rebinding GL_ARRAY_BUFFER or by glVertexAttribPointer
//     calls — the VAO already has s_sharedIbo bound from finalizeGeometry, and the
//     standard guarantees that GL_ELEMENT_ARRAY_BUFFER VAO state is only changed by
//     explicit glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...) while a VAO is bound.
//     Therefore the element binding is preserved across VBO replacement and is safe
//     to skip re-binding here.
//
// Called from: finalizeGeometry() (initial) and batcher_rebuildMechNormals() (live).
void uploadMechGeometryVbo() {
    // Build transient work copy of the staging VBO (s_stagingVbo = pristine cooked).
    // mode 0 means no recompute — work is byte-identical to cooked.
    std::vector<uint8_t> work = s_stagingVbo;

    if (s_mechNormalsMode != 0) {
        // Apply recompute per-node using the recorded per-node byte ranges.
        // Each range is [start, end) in work; recomputeMechNodeNormals modifies
        // work in place over that range only (bounded by the endByte param added
        // in MECH-NORMALS-FIX-1 so normals from adjacent nodes are never blended).
        for (const auto& rng : s_nodeVboRanges) {
            recomputeMechNodeNormals(work, rng.first, rng.second,
                                     s_mechNormalsMode, s_mechNormalsSmoothDeg);
        }
    }

    // The exact bytes the Vulkan islands upload (the normals-recomputed copy; s_stagingVbo stays the pristine cooked data).
    // Copy-once per map / normals rebuild.
    s_vkVboNativeCopy = std::move(work);
    ++s_vkMechGeomGen;
}

void GpuMechBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;
    noteDeprecatedMechFeedNative();
    // Slice C1: init the named material profile table once per process (idempotent).
    gos_materials::init();

    if (s_stagingVbo.empty()) {
        MC2_DIAGFE(stderr, "[MECHBATCHER v1] event=finalize_empty — no types registered\n");
        s_geometryFinalized = true;
        return;
    }

    // VBO bytes via uploadMechGeometryVbo() so batcher_rebuildMechNormals() can re-run just that part.
    uploadMechGeometryVbo();
    // The VK shadow feed hands out s_stagingIbo too: a new index set is a new geometry generation.
    ++s_vkMechGeomGen;

    // Staging RETAINED (not cleared/shrunk) so finalizePending() can
    // append a late type and rebuild from the full original+late
    // staging. See s_stagingVbo declaration. Cleared only at onMapLoad().

    s_geometryFinalized = true;
    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=finalize_ok types=%zu packets=%zu\n",
        s_typeLodRecords.size(), s_packets.size());
}

// VPL-#11 SP-logistics fix. Rebuild the immutable shared VBO/IBO/VAO so
// types registered AFTER a prior finalizeGeometry() (campaign .fit
// resume spawns the player force-group post-Mission::init-finalize, via
// logistics.cpp addMover) become drawable. No-op unless a late type was
// staged. SAFE because: (a) no frame renders between the late registers
// and this call on the SP path (adversarial-review grep-closed:
// GpuMechBatcher::flush only via renderLists from the frame loop, after
// logistics.cpp mission->update()); (b) glBufferStorage buffers are
// immutable so they MUST be deleted before recreate; (c) s_typeLodIndex
// /s_typeLodRecords/s_packets are append-only and preserved here, so the
// ~pre-finalize types keep stable indices and the retained staging holds
// their original-offset vertices + the appended late ones.
void GpuMechBatcher::finalizePending() {
    if (!s_pendingLateTypes) return;

    const uint32_t lateAdded = s_lateStagedCount;
    s_lateStagedCount   = 0;
    s_pendingLateTypes  = false;
    s_geometryFinalized = false;   // re-open the guard so finalizeGeometry runs
    finalizeGeometry();            // rebuilds VBO/IBO/VAO from full retained staging

    // The rebuild above repopulates s_packets with new indices/order, so the feed snapshots (run firstIndex values
    // of the old set) are stale: the geometry generation bump already makes the feeds decline them until the next
    // flush() rebuilds them against the new set.

    MC2_DIAGFE(stderr,
        "[MECHBATCHER v1] event=finalize_pending types=%zu lateAdded=%u rebuilt=%d\n",
        s_typeLodRecords.size(), lateAdded, s_geometryFinalized ? 1 : 0);
    if (mc2::gate::get<"MC2_MECH_RESTORE_TRACE">())
        mc2::fmt::print_err("[MECHRESTORE v1] event=finalize_pending types={:d} lateAdded={:d} rebuilt={:d}\n", mc2::fmt::as_size(s_typeLodRecords.size()), mc2::fmt::as_u32(lateAdded), mc2::fmt::as_i32(s_geometryFinalized ? 1 : 0));
}
