// GpuStaticPropBatcher: submit, submitMultiShape, the bucket upload, the V6 array build, the depth prepass and the executor apply-state hook.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"


bool GpuStaticPropBatcher::submit(TG_Shape* shape,
                                  const Stuff::Matrix4D& shapeToWorld,
                                  uint32_t highlightARGB,
                                  uint32_t fogARGB,
                                  uint32_t flags,
                                  uint32_t lightDataIndex) {
    if (!shape || s_fatalRegistrationFailure) return false;
    if (s_programLoadFailed) return false;

    // TG_Shape::myType is a TG_TypeNodePtr; for SHAPE_NODE leaves it's a TG_TypeShape.
    TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);
    if (!typeShape) return false;

    auto it = s_typeIndex.find(typeShape);
    if (it == s_typeIndex.end()) {
        if (!s_failedTypes[typeShape]) {
            mc2::fmt::print_err("[GPUPROPS] unregistered type {:016X} for shape {:016X} -- caller must CPU-fallback\n", mc2::fmt::as_ptr((void*)typeShape), mc2::fmt::as_ptr((void*)shape));
            s_failedTypes[typeShape] = true;
        }
        return false;  // Layer B: caller calls shape->Render() on false.
    }

    const uint32_t typeID = it->second;
    const GpuStaticPropType& type = s_types[typeID];
    PerTypeBucket& bucket = s_bucketsByType[typeID];

    // 2026-05-10 diag: per-typeID submit() histogram (H1 of substrate-coalesce
    // detail-pass plan). Init-once env probe + atexit registration; tally on
    // every leaf that reached this point with a registered typeID; dump once
    // after a useful sample window AND at process exit (whichever fires first).
    if (!s_submitTypeHistInit) {
        s_submitTypeHistInit    = true;
        s_submitTypeHistEnabled = mc2::gate::get<"MC2_SUBMIT_TYPEHIST">();
        if (s_submitTypeHistEnabled) atexit(emitSubmitTypeHistAtExit);
    }
    if (s_submitTypeHistEnabled) {
        if (typeID < s_submitTypeHist.size()) ++s_submitTypeHist[typeID];
        ++s_submitTypeHistCalls;
        // Empirically (mc2_10 substrate=ON sweep 2026-05-10):
        // submitted_children≈40/frame avg → ~24K calls/600 frames. The full
        // 30s 360° orbit is ~1800 frames ≈ 72K calls; we sample in three
        // windows so coverage growth across the orbit is visible (different
        // buildings revealed at different camera angles). atexit is suppressed
        // by quick_sweep's `taskkill /F`, so threshold dumps are load-bearing.
        if (s_submitTypeHistCalls == 20000  ||
            s_submitTypeHistCalls == 50000  ||
            s_submitTypeHistCalls == 100000) {
            emitSubmitTypeHistDump("threshold");
        }
    }

    // firstColorOffset is the index into the bucket's color array:
    // instance K's colors start at K * type.vertexCount (= bucket.colors.size()
    // BEFORE this push). The shader binds the bucket's color range, so this
    // becomes an index relative to the bound range.
    const uint32_t firstColorOffset =
        static_cast<uint32_t>(bucket.colors.size());

    GpuStaticPropInstance inst{};
    // Matrix4D is a plain row-major Scalar[16] (see stuff/matrix.hpp). Copy
    // as-is; shader uploads the worldToClip uniform with GL_FALSE.
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    inst.typeID           = typeID;
    inst.firstColorOffset = firstColorOffset;
    inst.flags            = flags;
    // Slice 2 (object-offload) — Stage 2.C: per-actor dedup-cache index
    // into the LightsData[32] UBO. Same value for every leaf of one
    // multishape (caller hoisted the gather between the two loops).
    inst.lightDataIndex   = lightDataIndex;
    inst.aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    inst.aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    inst.aRGBHighlight[2] = ((highlightARGB >>  0) & 0xFF) / 255.0f;
    inst.aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    inst.fogRGB[0] = ((fogARGB >> 16) & 0xFF) / 255.0f;
    inst.fogRGB[1] = ((fogARGB >>  8) & 0xFF) / 255.0f;
    inst.fogRGB[2] = ((fogARGB >>  0) & 0xFF) / 255.0f;
    inst.fogRGB[3] = ((fogARGB >> 24) & 0xFF) / 255.0f;
    {
        // MC2_TREE_DIAG_TRACE diagnostic. See macro definition near line ~50.
        // COUNTER-HONESTY-1: this is a SAMPLE (first kTreeDiagSample records),
        // never a census. Counting [TREE_DIAG] path=dyn lines in a log does NOT
        // count submissions. Every line now says so, and the running ordinal `n`
        // plus the one-shot cap notice let a reader see the truncation instead of
        // inferring a total from the line count.
        static int      s_dynPrinted = 0;
        static uint64_t s_dynSeen    = 0;
        if (s_treeDiagTrace) ++s_dynSeen;
        if (s_treeDiagTrace && s_dynPrinted == kTreeDiagSample) {
            ++s_dynPrinted;   // == kTreeDiagSample+1: notice emitted, never again
            TREE_DIAG("path=dyn sample=first%d SAMPLE-CAP-REACHED further path=dyn records suppressed; "
                      "this stream is a SAMPLE not a census -- line count != submission count; "
                      "and it covers ALL prop populations (see pop=), not just trees",
                      kTreeDiagSample);
        }
        if (s_treeDiagTrace && s_dynPrinted < kTreeDiagSample) {
            ++s_dynPrinted;
            TREE_DIAG("path=dyn sample=first%d n=%llu pop=%s typeID=%u flags=0x%x lightIdx=%u colOff=%u tx=%.2f ty=%.2f tz=%.2f fog=%.3f,%.3f,%.3f,%.3f hi=%.3f,%.3f,%.3f,%.3f bucketInsts=%zu",
                kTreeDiagSample, (unsigned long long)s_dynSeen,
                popShortName(s_currentSubmitPop),
                inst.typeID, inst.flags, inst.lightDataIndex, inst.firstColorOffset,
                // Stuff::Matrix4D translation lives at (3,0)/(3,1)/(3,2) which the
                // operator()(row,col) maps to entries[(col<<2)+row] = [3]/[7]/[11].
                // The W-column is at [12]/[13]/[14] which is always (0,0,0) for
                // affine matrices — wrong field. Verified via mclib/stuff/matrix.cpp:214 BuildTranslation.
                inst.modelMatrix[3], inst.modelMatrix[7], inst.modelMatrix[11],
                inst.fogRGB[0], inst.fogRGB[1], inst.fogRGB[2], inst.fogRGB[3],
                inst.aRGBHighlight[0], inst.aRGBHighlight[1], inst.aRGBHighlight[2], inst.aRGBHighlight[3],
                bucket.instances.size());
        }
    }
    bucket.instances.push_back(inst);
    s_lastBuiltBatch.push_back(inst);  // Stage 3.C: batch accumulator

    // 2026-05-10: dynamic-path substrate sync. Without this, an actor whose
    // appearance class lacks a registerStatic override (notably
    // GenericAppearance — warehouses, S_admin, watchtowers, control buildings)
    // populates bucket.instances every frame via submitMultiShape but never
    // appears in the substrate's GpuActorRecord stream. The compute cull then
    // writes 0 to bucketCountData[typeID] for those types, and the coalesce
    // multi-draw uses that GPU-authoritative count → renders zero instances of
    // those types even though the bucket has CPU data. The legacy per-bucket
    // draw branch (substrate killswitch) is unaffected because it draws
    // bucket.instances.size() directly and does not consult bucketCountData.
    //
    // Mirrors GpuStaticPropRegistry::flush()'s substrate append: same
    // worldCenter (Stuff/MLR → raw MC2 axis swap), same boundingRadius=200,
    // same category encoding (Cat_StaticProp + typeID<<4). Static-registry
    // and dynamic-submit paths now produce equivalent substrate records.
    if (gpu_cull::substrate_isEnabled()) {
        gpu_cull::GpuActorRecord rec{};
        // 2026-05-10 actor-center fix: every leaf of one multishape shares
        // the SAME substrate worldCenter so the GPU frustum sphere-test
        // accepts/rejects all leaves together (one cull unit per actor).
        // The first submit() call in a submitMultiShape pass captures the
        // current leaf's translation as the actor center; subsequent calls
        // reuse it. This matches registry::flush's per-range parent-center
        // (the first leaf's modelMatrix is the actor's xlatPosition because
        // the root leaf's local-to-actor transform is identity for stock
        // ASE buildings, trees, and animated buildings alike).
        if (!s_currentSubmittingActorCenterValid) {
            s_currentSubmittingActorCenter[0] = -inst.modelMatrix[3];   // raw.x = -stuff.x
            s_currentSubmittingActorCenter[1] =  inst.modelMatrix[11];  // raw.y =  stuff.z
            s_currentSubmittingActorCenter[2] =  inst.modelMatrix[7];   // raw.z =  stuff.y (elev)
            s_currentSubmittingActorCenterValid = true;
        }
        rec.worldCenter[0]  = s_currentSubmittingActorCenter[0];
        rec.worldCenter[1]  = s_currentSubmittingActorCenter[1];
        rec.worldCenter[2]  = s_currentSubmittingActorCenter[2];
        rec.boundingRadius  = 200.0f;
        rec.worldAabbMin[0] = rec.worldCenter[0] - rec.boundingRadius;
        rec.worldAabbMin[1] = rec.worldCenter[1] - rec.boundingRadius;
        rec.worldAabbMin[2] = rec.worldCenter[2] - rec.boundingRadius;
        rec.worldAabbMax[0] = rec.worldCenter[0] + rec.boundingRadius;
        rec.worldAabbMax[1] = rec.worldCenter[1] + rec.boundingRadius;
        rec.worldAabbMax[2] = rec.worldCenter[2] + rec.boundingRadius;
        rec.category        = (static_cast<uint32_t>(typeID) << 4)
                            | static_cast<uint32_t>(gpu_cull::Cat_StaticProp);
        // 2026-05-10 diag: MC2_STATIC_FORCE_ADMIT mirrors registry::flush
        // behavior so the env-var A/B (cull-bypassed vs cull-active) covers
        // BOTH substrate-record producers — otherwise dynamic-path leaves
        // (GenericAppearance buildings post-b2fb1cc) stay subject to cull
        // even when FORCE_ADMIT is on, masking the diagnostic signal.
        static const bool s_diag_forceAdmit = mc2::gate::get<"MC2_STATIC_FORCE_ADMIT">();
        rec.flags           = s_diag_forceAdmit
                              ? static_cast<uint32_t>(gpu_cull::Flag_AlwaysVisible)
                              : gpu_cull::Flag_None;
        rec.actorId         = 0u;
        rec.prevVisibilityBit = 1u;
        rec.consumerFlags   = 0u;
        // C1b temporal-superset Slice 1: real terrain block index so the
        // block rollup can stamp the right block. worldCenter[0] is raw-MC2
        // east, [1] raw-MC2 north (the -inst.modelMatrix[3] unswap above
        // produced the east-frame). Feed [0],[1] ONLY; NEVER [2] (elev).
        // worldCenter fully populated above (rec.worldCenter[0..2]).
        rec.blockIdx        = static_cast<uint32_t>(
            Terrain::worldToBlockIdx(rec.worldCenter[0],
                                     rec.worldCenter[1]));
        // [BLKIDX v1] env-gated GEOMETRIC probe (demote-not-delete).
        {
            static const bool s_blkidxTrace = mc2::gate::get<"MC2_BLKIDX_TRACE">();
            if (s_blkidxTrace) {
                const float pwx = rec.worldCenter[0];
                const float pwy = rec.worldCenter[1];
                long mx = ((long)pwx >> 7) + Terrain::halfVerticesMapSide;
                long bx = (long)(mx * Terrain::oneOverVerticesBlockSide);
                long my = Terrain::halfVerticesMapSide -
                          (((long)pwy >> 7) + 1);
                long by = (long)(my * Terrain::oneOverVerticesBlockSide);
                long cpuBlk = bx + (by * Terrain::blocksMapSide);
                long helperBlk = Terrain::worldToBlockIdx(pwx, pwy);
                mc2::fmt::print_err("[BLKIDX v1] event=geom_check src=batcher wx={:.1f} wy={:.1f} wz={:.1f} helper={:d} cpu={:d} match={:d}\n", pwx, pwy, rec.worldCenter[2], mc2::fmt::as_i32(helperBlk), mc2::fmt::as_i32(cpuBlk), mc2::fmt::as_i32((helperBlk == cpuBlk) ? 1 : 0));
                mc2::fmt::print_err("[BLKIDX v1] event=zero_verify src=batcher blockIdx={:d} z_excluded=1\n", mc2::fmt::as_u32(rec.blockIdx));
                fflush(stderr);
            }
        }
        gpu_cull::substrate_appendStaticPropRecord(rec);
    }

    // Stage 2.D.2 — dual-emit snapshot collection.
    // When the latch is Armed, capture per-vertex lit ARGB in triangle-soup
    // expanded order (matching gl_VertexID in static_prop.vert). The shader
    // writes parityOut_[gl_InstanceID * parityVerts + gl_VertexID] where
    // gl_VertexID is the expanded VBO index (0 .. numTris*3-1). We build the
    // snapshot in the same order: corner (j,c) → index 3*j+c.
    // Note: typeShape->numTypeTriangles is the TOTAL triangle count across all
    // packets (same as the total triangles emitted into the VBO by registerType).
    //
    // Source: listOfVertices[typeTriangles[j].Vertices[c]].argb — NOT
    //   listOfTriangles[j].aRGBLight[c]. The face-loop only writes
    //   aRGBLight[c] for FRONT-FACING triangles; back-facing positions
    //   retain stale pool memory from previous frames (not zeroed on alloc).
    //   listOfVertices[v].argb is always written by the vertex lighting loop
    //   for every vertex (front AND back facing), making it the correct
    //   per-vertex ground truth. Both values are identical for front-facing
    //   vertices when useFaceLighting=false (stock mc2_01 condition).
    // Stage 2.D.3: per-actor narrowing of snapshot capture. The mclib gate
    // sites only fire the full TransformMultiShape (CPU lighting bake) for
    // the sampled actor in SampledOnly arm mode — non-sampled actors have
    // stale listOfVertices[].argb. Capturing their bytes would corrupt the
    // snapshot map. Use the per-actor gate here too: when the enclosing
    // multishape (s_currentSubmittingMulti, set by submitMultiShape) doesn't
    // match the sampler's pick, skip the snapshot. In bootstrap arm (state=
    // Armed + armMode=All) IsDualEmitArmedForActor returns true for every
    // shape, so 2.D.2 baseline coverage is preserved.
    if (gos_object_parity::IsDualEmitArmedForActor(s_currentSubmittingMulti) &&
        shape->listOfVertices &&
        typeShape->listOfTypeTriangles &&
        typeShape->numTypeTriangles > 0) {
        const DWORD numTris = typeShape->numTypeTriangles;
        const uint32_t expandedVerts = numTris * 3u;
        std::vector<uint32_t> perVertexARGB(expandedVerts, 0u);
        // Stage 2.D.2 diagnostic: one-shot dump of original aRGBLight tags vs
        // computed listOfVertices[vi].argb for first instance of typeID=82.
        // Controlled by MC2_OBJECT_PARITY_TRACE env var.
        static bool s_parity82Printed = false;
        const bool doTrace82 = (typeID == 82 && !s_parity82Printed &&
                                gos_object_parity::IsParityTraceEnabled());
        if (doTrace82) s_parity82Printed = true;
        for (DWORD j = 0; j < numTris; ++j) {
            for (int c = 0; c < 3; ++c) {
                const DWORD vi = typeShape->listOfTypeTriangles[j].Vertices[c];
                perVertexARGB[j * 3u + c] = shape->listOfVertices[vi].argb;
                if (doTrace82 && (j * 3u + c) < 20u) {
                    mc2::fmt::print_err("[PARITY_DIAG v1] typeId=82 inst=first vert={:d} aRGBLight_tag=0x{:08X} computed_argb=0x{:08X}\n", mc2::fmt::as_u32((unsigned)(j * 3u + c)), mc2::fmt::as_u32((unsigned)typeShape->listOfTypeVertices[vi].aRGBLight), mc2::fmt::as_u32((unsigned)shape->listOfVertices[vi].argb));
                    std::fflush(stderr);
                }
            }
        }
        // Stage 2.D.3: pass explicit instanceIdx = bucket position this leaf
        // just took. bucket.instances.push_back ran above, so size()-1 is
        // the just-pushed entry's index. Required so SampledOnly mode can
        // address the correct GPU slot in CompareAndReport (the sampled
        // leaf might not be at instance 0 — there are typically many
        // unrelated submissions of the same type per frame).
        const uint32_t instanceIdx =
            static_cast<uint32_t>(bucket.instances.size() - 1u);
        gos_object_parity::RecordInstanceSnapshot(typeID,
                                                   instanceIdx,
                                                   perVertexARGB.data(),
                                                   expandedVerts);
    }

    // Append this instance's per-vertex ARGB block.
    // IMPORTANT: listOfColors (TG_Vertex: fog+redSpec+greenSpec+blueSpec) is
    // specular-only and is zero for most buildings — reading it produces
    // black. The real per-vertex lit ARGB is computed by TransformShape
    // into `listOfVertices[j].argb` (mclib/tgl.cpp:2119). gos_VERTEX is
    // 32 bytes with `argb` at offset 16.
    const uint32_t numColors = type.vertexCount;
    if (numColors > 0 && shape->listOfVertices) {
        bucket.colors.reserve(bucket.colors.size() + numColors);
        const gos_VERTEX* src = shape->listOfVertices;
        for (uint32_t v = 0; v < numColors; ++v) {
            bucket.colors.push_back(src[v].argb);
        }
    } else {
        // No source vertices -- pad with zeros so the color block still
        // matches type.vertexCount and indexing math stays valid.
        bucket.colors.insert(bucket.colors.end(), numColors, 0u);
    }

    return true;
}

void GpuStaticPropBatcher::recordEligibleActor(GpuStaticPropPopulation pop) {
    initTraceOnce();
    s_counters.eligible_actors_by_pop[popIndex(pop)]++;
}

void GpuStaticPropBatcher::recordCpuFallback(GpuStaticPropPopulation pop) {
    s_counters.cpu_fallback_by_pop[popIndex(pop)]++;
}

bool GpuStaticPropBatcher::submitMultiShape(TG_MultiShape* multi,
                                            GpuStaticPropPopulation pop,
                                            const char* callerName) {
    initTraceOnce();
    s_lastBuiltBatch.clear();  // Stage 3.C: reset per-call accumulator
    // Clear the late-reg signal at the top of every call so a stale "true"
    // from a prior submitMultiShape never masquerades as a signal for this one.
    s_lastSubmitWasLateReg = false;
    // Stage 2.D.3: track the currently-submitting multishape so per-leaf
    // submit() calls can compare against the sampler's pick. Cleared at every
    // exit path of this function to keep the window tight.
    s_currentSubmittingMulti = multi;
    // 2026-05-10 actor-center fix: cleared per-call so a stale value from a
    // prior submitMultiShape can't leak into a multishape that has zero valid
    // SHAPE_NODE leaves. Populated below when firstShapeNodeLeaf is captured.
    s_currentSubmittingActorCenterValid = false;
    s_currentSubmittingActorCenter[0]   = 0.0f;
    s_currentSubmittingActorCenter[1]   = 0.0f;
    s_currentSubmittingActorCenter[2]   = 0.0f;
    // pop consumed by counters below.
    // TREE-POP-TAG-1: publish it to the per-leaf [TREE_DIAG] record too, so a
    // `path=dyn` line names the population of the actor it came from instead of
    // leaving the reader to infer it from the macro's (misleading) name. Guarded
    // on the trace flag: with MC2_TREE_DIAG_TRACE unset nothing is written and
    // this function's behaviour is unchanged.
    if (s_treeDiagTrace) s_currentSubmitPop = pop;
    if (!multi || s_fatalRegistrationFailure) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }
    if (s_programLoadFailed || s_staticPropProgram == 0) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) {
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    // Skip-child-not-fail-multishape policy. The CPU path (TG_Shape::Render
    // in tgl.cpp ~2530) silently returns for any of:
    //   - helper/bone node (numVertices == 0; GetNodeType() != SHAPE_NODE)
    //   - listOfVertices == NULL (TransformShape early-out for
    //       isSpotlight && !isNight, tgl.cpp ~1657)
    //   - listOfColors == NULL (shape not yet transformed this frame)
    //   - lastTurnTransformed != turn (stale)
    // Previously we failed the entire multishape on any of these, which
    // cascaded one helper/spotlight child into a full CPU fallback of every
    // building. Now we skip only the ineligible children and submit the
    // rest. Only fail the whole multishape if a SHAPE_NODE child has an
    // unregistered type — that's a plumbing problem that invalidates
    // self-consistency.

    // First pass: check for unregistered SHAPE_NODE types (fatal for this
    // multishape). All other ineligibility is handled in the submit pass.
    // Slice 2 (object-offload) — Stage 2.C: capture the first valid
    // SHAPE_NODE leaf as the per-actor anchor for the GatherGpuObjectLightDataOnly
    // call hoisted between this loop and the second submit loop. Recon
    // Section 9 Item 5 confirmed all leaves of a multishape see identical
    // lightData_, so any non-null SHAPE_NODE works as the gather receiver.
    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < n; ++i) {
        const TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        const TG_Shape* child = rec.node;
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;  // skip helpers
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(child->myType);
        if (firstShapeNodeLeaf == nullptr) firstShapeNodeLeaf = rec.node;
        if (s_typeIndex.find(ts) == s_typeIndex.end()) {
            loadLateRegisterAllowlistOnce();
            // Stage 2.C+ instrumentation: build a richer identifier for the
            // unregistered type. The dedup key still uses the pointer so the
            // count-once-per-type logic stays stable across name lookups,
            // but the human-readable log line includes:
            //   - caller actor's appearType->name (e.g. "house3.ase")
            //   - the TG_TypeShape's nodeId (inherited from TG_TypeNode;
            //     getNodeId() is public, returns char*; const_cast is the
            //     minimal-touch workaround for the const TG_TypeShape* we
            //     have here)
            //   - the bare pointer (still useful for cross-referencing
            //     across log files / ts identity over a process lifetime)
            char addrBuf[32];
            mc2::fmt::to_buf(addrBuf, "{:016X}", mc2::fmt::as_ptr((const void*)ts));
            const std::string typeKey = addrBuf;  // dedup key — pointer-stable within one run
            auto& count = s_lateRegisterCounts[typeKey];
            // Stage 2.D.3: compute `allowed` for EVERY event so the per-event
            // counters tick correctly. The PRINT (and registered_dump) below
            // remain gated by `count == 0` so the log stays one-line-per-type.
            const char* nodeIdEvt = nullptr;
            if (ts) {
                nodeIdEvt = const_cast<TG_TypeShape*>(ts)->getNodeId();
            }
            const bool allowedEvt =
                nodeIdEvt && nodeIdEvt[0] &&
                (s_lateRegisterAllowlist.find(nodeIdEvt)
                 != s_lateRegisterAllowlist.end());
            if (allowedEvt) ++s_lateReg_allowed_events;
            else            ++s_lateReg_disallowed_events;
            if (count == 0) {
                const char* nodeId = nodeIdEvt;
                // Allowlist matching uses the nodeId, NOT the pointer-key.
                // Pointers are not stable across process runs, so a pointer
                // entry in data/objbatcher_late_register_allowlist.txt is
                // useless. nodeId comes from the .ase author and is stable
                // across runs and across the same nodeId-family of types
                // (LOD variants of the same shape share a nodeId — e.g.,
                // multiple TG_TypeShape* with nodeId="Centipede" at
                // different vertexCounts; allowlisting "Centipede" matches
                // the family). Empty-name shapes cannot be allowlisted —
                // the file would have no useful key for them.
                const bool allowed = allowedEvt;
                MC2_DIAGFE(stderr,
                       "[OBJBATCHER v1] event=late_register type=%s nodeId=%s caller=%s allowed=%d\n",
                       typeKey.c_str(),
                       (nodeId && nodeId[0]) ? nodeId : "<unnamed>",
                       (callerName && callerName[0]) ? callerName : "<unknown>",
                       allowed ? 1 : 0);
                std::fflush(stderr);

                // Stage 2.C+ instrumentation: one-shot registered-types
                // dump. Emitted on the FIRST late-register event of the run
                // only (s_emittedRegisteredDump is file-static, set true
                // here). Lists the registered TG_TypeShape pointers AND
                // their nodeIds so an operator can grep this output to find
                // "is the late-reg one in the same family as registered
                // ones, just with a different nodeId variant?" or "is the
                // late-reg type entirely absent from the s_typeIndex, vs.
                // registered as a peer?". Cap at 30 to keep the log
                // tractable; if more types are registered, the count is
                // shown so the truncation is visible.
                // Gated behind MC2_STATIC_PROP_TRACE 2026-05-17: one-shot
                // but ~600-line load-time dump; demote-not-delete.
                static const bool s_gpuPropsRegDump = mc2::gate::get<"MC2_STATIC_PROP_TRACE">();
                static bool s_emittedRegisteredDump = false;
                if (s_gpuPropsRegDump && !s_emittedRegisteredDump) {
                    s_emittedRegisteredDump = true;
                    const size_t total = s_types.size();
                    // 2026-05-10 diag: bumped 30 -> 600 to surface buildings.
                    const size_t cap = total < 600 ? total : 600;
                    mc2::fmt::print_err("[GPUPROPS_REG] event=registered_dump total={:d} shown={:d}\n", mc2::fmt::as_size(total), mc2::fmt::as_size(cap));
                    for (size_t k = 0; k < cap; ++k) {
                        const TG_TypeShape* rt = s_types[k].source;
                        const char* rnode = nullptr;
                        if (rt) rnode = const_cast<TG_TypeShape*>(rt)->getNodeId();
                        mc2::fmt::print_err("[GPUPROPS_REG] entry typeID={:d} ptr={:016X} nodeId={} vertexCount={:d}\n", mc2::fmt::as_size(k), mc2::fmt::as_ptr((const void*)rt), mc2::fmt::safe((rnode && rnode[0]) ? rnode : "<unnamed>"), mc2::fmt::as_u32(s_types[k].vertexCount));
                    }
                    std::fflush(stderr);
                }
            }
            ++count;
            // Stage 2.A: signal the caller that this failure was specifically
            // a late-registration miss (not a fatal GPU error). Caller (Stage
            // 2.B) queries wasLastFailureLateRegistration() and sets
            // needsFullBakeNextFrame=true on the owning actor.
            s_lastSubmitWasLateReg = true;
            ++s_late_register_recovery_skips;
            s_currentSubmittingMulti = nullptr;
            return false;
        }
    }

    // Slice 2 (object-offload) — Stage 2.C: per-actor light-data gather.
    // Stage 2.D.2 fix: use cachedGpuLightIndex_ if it was pre-gathered during
    // update() (while worldLights[0]->aRGB was per-actor-correct).
    // Fallback to GatherGpuObjectLightDataOnly() here only when the cache is
    // invalid (0xFFFFFFFF sentinel = not yet cached, e.g. first frame).
    //
    // Root cause of the timing bug this fixes:
    //   BldgAppearance::update() sets worldLights[0]->aRGB = terrainLight(pos).
    //   GatherGpuObjectLightDataOnly() called here (during renderLists()) reads
    //   worldLights[0]->aRGB AFTER all actors have updated — by then later
    //   actors have overwritten it with their own terrain-position-scaled values.
    //   The last-actor's value (often white at high-elevation positions) is what
    //   the GPU UBO receives, while the CPU vertex loop used the per-building
    //   terrain-scaled value during its own update(). This causes the hot-green
    //   (daytime base=0) vertex mismatch: GPU applies white directional light,
    //   CPU applied the terrain-tinted (~0.88) directional light.
    // ------------------------------------------------------------------
    // LIGHTROW-FRESHNESS-1
    //
    // THE DEFECT THIS FIXES. cachedGpuLightIndex_ is a DYNAMIC slot index, and
    // dynamic slot indices are rebased EVERY FRAME: MC_TextureManager::resetLightData
    // (mclib/txmmgr.cpp:2137) resets lightDataStructuresCount to the static
    // high-water and clears every dedup map, and its own comment states that
    // "any stale hash->slot entries from the prior frame are invalid". A slot
    // number from frame N therefore names a DIFFERENT actor's light row -- or no
    // written row at all -- on frame N+1.
    //
    // The other two consumers of this field both know that. TG_MultiShape::
    // ResubmitCachedGpuLightData (mclib/msl.cpp:2147-2149) requires
    // `cachedFrame_ == g_mc2FrameCounter` before trusting the index, and
    // GpuStaticPropRegistry's flush (gos_static_prop_registry.cpp:1051) compares
    // getCachedFrame() against the current frame and suppresses the draw when it
    // is stale -- its comment at :1045 names the consequence exactly: a slot
    // "whose content this frame was filled by a different actor -- emitting a
    // draw with that index produces wrong/black lighting". The declaration of
    // cachedFrame_ itself (mclib/msl.h:315-320) documents the same contract.
    //
    // This site -- the DYNAMIC submit path -- was the one consumer that checked
    // only the 0xFFFFFFFF sentinel and never the frame stamp, so it accepted a
    // prior-frame index verbatim. Animated buildings are precisely the actors
    // that land here: BldgAppearance::setGesture calls invalidateStaticRegistration()
    // (mclib/bdactor.cpp:1874) the moment a real animation starts, dropping the
    // actor out of the static registry (which HAS the check) and onto this path
    // (which did not). The shader indexes light[] unchecked
    // (shaders/static_prop.vert:714), so a stale or never-written row shades the
    // actor with zeroes -- pure black, texture-independent, silhouette-shaped.
    //
    // WHAT HAPPENS WHEN THE CHECK FAILS: we RE-GATHER, we do not substitute.
    // Falling back to slot 0 (the pre-slice behaviour when the sentinel hit) is
    // what made this silent, but pointing at some other actor's row would just
    // trade a black building for a wrongly-lit one. GatherGpuObjectLightDataOnly()
    // allocates/dedups a row for THIS actor in THIS frame, which is the same
    // repair msl.cpp:2171 performs when its guard rejects the cache. If even that
    // is impossible (no SHAPE_NODE leaf to gather through) the actor is dropped
    // rather than drawn against an arbitrary slot, matching the registry's choice.
    //
    // THE TRADE, WRITTEN DOWN BECAUSE THIS IS NOT A PURE WIN. The re-gather is
    // not free: addLightDataStructureWithPerActorColor (mclib/txmmgr.cpp:2062)
    // does `*light_data = it->second.data`, overwriting lightData_ from a scene
    // template that GatherLightsParameters builds off the TG_Shape::s_listOfLights
    // / s_numLights STATICS the last-transformed shape set. That is the very
    // render-time timing bug the Stage 2.D.2 comment above describes. So on the
    // stale path we swap one wrong answer for another: BEFORE = a prior-frame
    // slot that may name another actor's row or an unwritten zero row (possibly
    // PURE BLACK); AFTER = a valid current-frame row that may carry the
    // last-transformed actor's tint (MISTINTED, never black). Strictly better in
    // the worst case; a REGRESSION for an actor whose stale index happened to
    // still point at its own correct row, which self-heals on its next update().
    // mono_stale_light_row_regathered sizes this. If it reads high in steady
    // state, the right fix is to re-stamp the row at update() time rather than
    // re-gather here, and this gate should be reconsidered rather than defended.
    // ------------------------------------------------------------------
    const bool cacheFresh  = (multi->cachedFrame_ == g_mc2FrameCounter);
    const bool cacheSet    = (multi->cachedGpuLightIndex_ != 0xFFFFFFFFu);

    uint32_t lightDataIndex = 0;
    bool     lightRowResolved = false;
    if (cacheSet && cacheFresh) {
        lightDataIndex   = multi->cachedGpuLightIndex_;
        lightRowResolved = true;
    } else if (firstShapeNodeLeaf != nullptr) {
        // Fallback: first frame, non-GPU-object path cached nothing, or (new)
        // the cached index is a stale prior-frame slot. Re-gather for this frame.
        if (cacheSet && !cacheFresh) {
            ++s_counters.mono_stale_light_row_regathered;
            // FAIL-LOUD: one line per distinct typeID, uncapped counter in the
            // message so nobody counts log lines. Silence here is what let this
            // ship; steady-state hits mean the defect was live on this run.
            static std::set<const TG_TypeShape*> s_loggedStaleTypes;
            const TG_TypeShape* ts0 =
                static_cast<const TG_TypeShape*>(firstShapeNodeLeaf->myType);
            if (s_loggedStaleTypes.insert(ts0).second) {
                MC2_DIAGFE(stderr,
                    "[LIGHTROW_FRESHNESS v1] event=stale_row_regathered "
                    "nodeId=%s caller=%s staleIndex=%u cachedFrame=%u currentFrame=%u "
                    "regathered_so_far=%llu (prior-frame light slot rejected; "
                    "see LIGHTROW-FRESHNESS-1)\n",
                    (ts0 && const_cast<TG_TypeShape*>(ts0)->getNodeId() &&
                     const_cast<TG_TypeShape*>(ts0)->getNodeId()[0])
                        ? const_cast<TG_TypeShape*>(ts0)->getNodeId() : "<unnamed>",
                    (callerName && callerName[0]) ? callerName : "<unknown>",
                    multi->cachedGpuLightIndex_, multi->cachedFrame_,
                    g_mc2FrameCounter,
                    (unsigned long long)s_counters.mono_stale_light_row_regathered);
                std::fflush(stderr);
            }
        }
        lightDataIndex   = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();
        lightRowResolved = true;
    }

    // Unresolvable row. Pre-slice this fell through with lightDataIndex == 0 --
    // a REAL slot, not a sentinel, so the actor drew against whatever row 0
    // happened to hold. Drop it instead; drawing an actor against another
    // actor's lighting is not a smaller error than not drawing it.
    //
    // Reachability note, stated so the next reader does not have to re-derive it:
    // firstShapeNodeLeaf == nullptr means the first pass (:5788-5796) found no
    // SHAPE_NODE leaf, and the submit loop below applies the same predicate, so
    // it would emit nothing anyway. This branch is therefore expected to be
    // unreachable in practice -- which is exactly why it gets a counter rather
    // than a silent `= 0`. If it ever fires, that assumption has broken.
    if (!lightRowResolved) {
        ++s_counters.mono_stale_light_row_unresolvable;
        static bool s_loggedUnresolvable = false;
        if (!s_loggedUnresolvable) {
            s_loggedUnresolvable = true;
            MC2_DIAGFE(stderr,
                "[LIGHTROW_FRESHNESS v1] event=unresolvable_row caller=%s "
                "cachedIndex=%u cachedFrame=%u currentFrame=%u "
                "(no SHAPE_NODE leaf to gather through; actor dropped)\n",
                (callerName && callerName[0]) ? callerName : "<unknown>",
                multi->cachedGpuLightIndex_, multi->cachedFrame_, g_mc2FrameCounter);
            std::fflush(stderr);
        }
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    // RANGE CHECK. shaders/static_prop.vert:714 does `light[int(inst.lightDataIndex)]`
    // with no bounds test, so an index >= the live row count is an out-of-bounds
    // SSBO read. A row resolved above is always < getLightStructCount() by
    // construction, so this is an invariant assertion, not a expected path --
    // it must read zero. It is NOT repaired by clamping (clamping would just
    // re-introduce the silent wrong-row substitution this slice removes).
    if (mcTextureManager &&
        lightDataIndex >= mcTextureManager->getLightStructCount()) {
        ++s_counters.mono_light_row_out_of_range;
        static bool s_loggedOob = false;
        if (!s_loggedOob) {
            s_loggedOob = true;
            MC2_DIAGFE(stderr,
                "[LIGHTROW_FRESHNESS v1] event=row_out_of_range caller=%s "
                "index=%u liveRowCount=%u currentFrame=%u "
                "(INVARIANT BREAK -- shader would read light[] out of bounds; "
                "actor dropped)\n",
                (callerName && callerName[0]) ? callerName : "<unknown>",
                lightDataIndex, mcTextureManager->getLightStructCount(),
                g_mc2FrameCounter);
            std::fflush(stderr);
        }
        s_currentSubmittingMulti = nullptr;
        return false;
    }

    // Second pass: submit each eligible child, silently skipping those the
    // CPU path would also skip (helpers, untransformed, daytime spotlights).
    for (int i = 0; i < n; ++i) {
        TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) { s_counters.skipped_children++; continue; }
        TG_Shape* child = rec.node;
        if (!child->myType) { s_counters.skipped_children++; continue; }
        // Helper/bone nodes (non-SHAPE). CPU Render silently returns because
        // numVertices == 0. Do the same — no instance to emit.
        if (child->myType->GetNodeType() != SHAPE_NODE) {
            s_counters.skipped_children++;
            continue;
        }
        // Spotlights during day + other early-outs in TransformShape leave
        // listOfVertices NULL. CPU Render early-outs on the same condition.
        // Also listOfColors NULL: CPU also early-outs. Submit's zero-pad
        // path would render this child black, which is the bug we're
        // avoiding — so skip here.
        //
        // GPU-INSTANCE-SKIP-POOLS-1: in the new GPU-only path, registered
        // types intentionally run the zero-pool hierarchy walk
        // (TransformMultiShape_HierarchyOnly) in *Appearance::update, which
        // leaves listOfVertices/listOfColors NULL but DOES populate
        // rec.shapeToWorld (used at :4391). Such a child is a legitimate GPU
        // instance: its geometry is the immutable per-type VBO and its colors
        // are the debug-only (zero-padded) Colors SSBO the lit shader ignores
        // (static_prop.vert:5). So admit a NULL-vertex child IFF its type is
        // registered in s_typeIndex AND we are not in the legacy escape hatch.
        // A NULL-vertex child whose type is NOT registered (true day-spotlight
        // early-out) is still skipped, preserving stock behavior.
        if (!child->listOfVertices || !child->listOfColors) {
            bool admitAsGpuOnly = false;
            if (!s_legacyInstancePools && child->myType) {
                const TG_TypeShape* ts =
                    static_cast<const TG_TypeShape*>(child->myType);
                admitAsGpuOnly = (s_typeIndex.find(ts) != s_typeIndex.end());
            }
            if (!admitAsGpuOnly) {
                s_counters.skipped_children++;
                continue;
            }
        }

        // [T3.1] SpotLight_-prefixed children are illuminated as real TG_Lights
        // by BldgAppearance::update (T1.4 / per-instance). The legacy cone
        // billboard packet is suppressed at the source so substitutive-completion
        // criterion #2 ("zero static-prop draws contain spotlight-tagged
        // packets") is satisfied by construction. Public accessor
        // GetIsSpotlight() is forward-compatible; direct field access also
        // works because GpuStaticPropBatcher is in the TG_MultiShape friend
        // list at msl.h:251-256. Gate retired in T3.1; behavior is now
        // unconditional.
        if (const_cast<TG_Shape*>(child)->GetIsSpotlight()) {
            s_counters.skipped_children++;
            continue;
        }

        uint32_t flags = 0;
        if (child->lightsOut)   flags |= (1u << 0);
        if (child->isWindow)    flags |= (1u << 1);
        // Bit-2: foliage/alpha-test. Enables abs(NdotL) two-sided lighting in
        // static_prop.vert so cross-quad tree faces light consistently from
        // all camera angles (face normals rotate with view → dark face shadow
        // rotates with camera without this). alphaTestOn lives on TG_TypeShape.
        if (const TG_TypeShape* ts2 = static_cast<const TG_TypeShape*>(child->myType)) {
            if (ts2->alphaTestOn) flags |= (1u << 2);
        }
        // LIGHT-BILLBOARD-GPU-1: bit-4 = camera-facing light billboard,
        // bit-5 = exaggerate (debug). Bit-3 is kFlagDecalFill (cliff decal,
        // bdactor.cpp:4227) — do NOT reuse. Emitted only for children the
        // shape loader classified as beacon/runway glow cards; every other
        // static prop's flags word is bit-for-bit unchanged, and with the
        // gate at 0 no instance in the scene carries the bit at all.
        if (const_cast<TG_Shape*>(child)->GetIsLightBillboard()) {
            const int lbMode = gos_lightBillboardFacingMode();
            if (lbMode != 0) {
                flags |= (1u << 4);
                if (lbMode == 2) flags |= (1u << 5);
            }
        }
        // [T3.2] Bit-2 was kFlagIsSpotlight (deleted). After T3.1 the
        // spotlight `continue` skip above is unconditional, so this code path
        // is structurally unreachable for spotlight children. The dead
        // shader-side read in static_prop.vert is removed in lockstep per
        // memory/cpp_glsl_ubo_struct_lockstep.md. The static-prop branch of
        // [SPOTLIGHT_REAL_TRACE v1] never fires post-T3.1; the per-actor
        // mech/gv first-hit traces remain via the registration sites in
        // mech3d.cpp / gvactor.cpp (Debug Instrumentation Rule). Bit-2 is now
        // reused as kFlagIsAlphaTest above.

        // rec.shapeToWorld is LinearMatrix4D; convert to Matrix4D for submit().
        Stuff::Matrix4D xform(rec.shapeToWorld);
        if (!submit(child, xform,
                    child->aRGBHighlight, child->fogRGB, flags,
                    lightDataIndex)) {
            // submit() rejected after we passed the registration gate —
            // typically a buffer-full condition. Fall back for this frame
            // to keep the visual self-consistent.
            s_currentSubmittingMulti = nullptr;
            return false;
        }
        s_counters.submitted_children++;
    }
    s_counters.submitted_instances_by_pop[popIndex(pop)]++;
    // Stage 2.D.3 — sampler observation. Record the multishape pointer in
    // the parity sidecar's per-frame observation list. Default-off short-
    // circuits internally when MC2_OBJECT_PARITY_CHECK is unset; safe to
    // call unconditionally. Only successful submissions reach here, so
    // late-registered actors (skybox/compass) are structurally excluded
    // from the sampler pool — matching the "registered types only"
    // contract from the dispatch.
    gos_object_parity::ObserveSubmittedShape(multi);
    s_currentSubmittingMulti = nullptr;
    return true;
}

namespace gos_static_prop_batcher_detail {

// Per-frame upload state shared between flushShadow() (Task 13) and flush().
// Whichever runs first this frame owns the upload; the other skips it when
// s_lastUploadedSlot == s_frameSlot.
std::unordered_map<uint32_t, TypeRangeSsbo> s_typeRanges;
uint32_t s_lastUploadedSlot = 0xFFFFFFFFu;

bool uploadAllBucketsIfNeeded() {
    if (s_lastUploadedSlot == s_frameSlot) return true;

    // M2a POPULATION-SPLIT: when staticPopSplitArmed(), statics are excluded from
    // s_bucketsByType (reinjectPersistentStatic early-out). If there are also no
    // dynamic props, the bucket is empty but flush() must still proceed so the M2a
    // block can draw all statics from s_staticInstanceSsbo. Do NOT return false here
    // when armed — fall through to the fence wait + slot advance below.
    if (s_bucketsByType.empty() && !staticPopSplitArmed()) return false;

    loadProgramsIfNeeded();
    if (s_fatalRegistrationFailure) return false;

    // SSBO offset alignment must be queried to size the ring correctly.
    // glBindBufferRange(GL_SHADER_STORAGE_BUFFER, ..., offset, size) requires
    // offset % GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT == 0 (minimum 256).
    // Each per-type range starts at an aligned offset, which wastes up to
    // (alignment - 1) bytes per bucket. The CAPACITY request must include
    // that slack or we overrun the mapped buffer on zoom-out (more buckets
    // active -> more padding overhead).
    static GLint s_ssboAlignment = 0;
    if (s_ssboAlignment == 0) {
        glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &s_ssboAlignment);
        if (s_ssboAlignment < 16) s_ssboAlignment = 256;  // sane fallback
    }
    auto alignUp = [](size_t v, size_t a) {
        return (v + (a - 1)) & ~(a - 1);
    };

    // Compute EXACT total byte usage with per-bucket alignment padding.
    size_t instBytesNeeded = 0;
    size_t colBytesNeeded  = 0;
    for (auto& kv : s_bucketsByType) {
        instBytesNeeded = alignUp(instBytesNeeded, (size_t)s_ssboAlignment);
        colBytesNeeded  = alignUp(colBytesNeeded,  (size_t)s_ssboAlignment);
        instBytesNeeded += kv.second.instances.size() * sizeof(GpuStaticPropInstance);
        colBytesNeeded  += kv.second.colors.size() * sizeof(uint32_t);
    }
    // Same reasoning: M2a armed with no dynamic props → instBytesNeeded stays 0
    // but we still need the ring slot advanced + fence waited so flush() can proceed.
    if (instBytesNeeded == 0 && !staticPopSplitArmed()) return false;

    // Convert back to element counts (ceil) for ensureRingCapacity, which is
    // element-based. Round up so subsequent ring-indexing in bytes fits.
    const size_t instCountNeeded =
        (instBytesNeeded + sizeof(GpuStaticPropInstance) - 1) / sizeof(GpuStaticPropInstance);
    const size_t colCountNeeded =
        (colBytesNeeded + sizeof(uint32_t) - 1) / sizeof(uint32_t);

    ensureRingCapacity(instCountNeeded, colCountNeeded);
    if (s_fatalRegistrationFailure) return false;

    s_frameSlot = (s_frameSlot + 1) % RING_FRAMES;
    // Step 4.4 — legacy mode: coalesce ring mirrors legacy ring slot.
    // Non-legacy mode: s_coalesceFrameSlot is advanced inside
    // batcher_prepareBaseInstanceTable() (called from txmmgr.cpp).
    if (s_globalPoolLegacy) {
        s_coalesceFrameSlot = s_frameSlot;
    }
    if (s_fence[s_frameSlot]) {
        ZoneScopedN("GpuSP.FenceWaitRender");
        MC2_GL_ClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    const size_t slotInstByteBase = s_frameSlot * s_instanceCapacity * sizeof(GpuStaticPropInstance);
    const size_t slotColByteBase  = s_frameSlot * s_colorCapacity    * sizeof(uint32_t);
    auto* instMapBase = static_cast<uint8_t*>(s_instanceMap) + slotInstByteBase;
    auto* colMapBase  = static_cast<uint8_t*>(s_colorMap)    + slotColByteBase;

    // Deterministic ascending typeID iteration — makes Tracy / RenderDoc
    // diffs stable and shader-debug repro repeatable across runs.
    std::vector<uint32_t> sortedTypeIDs;
    sortedTypeIDs.reserve(s_bucketsByType.size());
    for (auto& kv : s_bucketsByType) sortedTypeIDs.push_back(kv.first);
    std::sort(sortedTypeIDs.begin(), sortedTypeIDs.end());

    s_typeRanges.clear();
    size_t instCursor = 0;
    size_t colCursor  = 0;
    for (uint32_t typeID : sortedTypeIDs) {
        PerTypeBucket& b = s_bucketsByType[typeID];

        // Align the start of each per-type region to the SSBO alignment
        // requirement before writing.
        instCursor = alignUp(instCursor, static_cast<size_t>(s_ssboAlignment));
        colCursor  = alignUp(colCursor,  static_cast<size_t>(s_ssboAlignment));

        TypeRangeSsbo r{};
        r.instanceByteOffset = slotInstByteBase + instCursor;
        r.instanceByteSize   = b.instances.size() * sizeof(GpuStaticPropInstance);
        r.colorByteOffset    = slotColByteBase  + colCursor;
        r.colorByteSize      = b.colors.size() * sizeof(uint32_t);
        r.instanceCount      = static_cast<uint32_t>(b.instances.size());

        if (r.instanceByteSize)
            std::memcpy(instMapBase + instCursor, b.instances.data(), r.instanceByteSize);
        if (r.colorByteSize)
            std::memcpy(colMapBase  + colCursor,  b.colors.data(),    r.colorByteSize);

        instCursor += r.instanceByteSize;
        colCursor  += r.colorByteSize;
        s_typeRanges[typeID] = r;
    }

    s_lastUploadedSlot = s_frameSlot;
    return true;
}

}  // namespace gos_static_prop_batcher_detail

// v8: single source of truth for the live v6 array build. Writes outPackets/outMeta
// (lockstep) and the s_v6Frame* diagnostic counters as a side effect. Returns the
// lockstep-violation count. Behavior is identical to the inlined loop it replaces.
uint32_t buildLiveV6Arrays(
        uint32_t totalCmds,
        const uint32_t* baseInstanceMap,
        std::vector<RenderCore::DrawPacket>& outPackets,
        std::vector<StaticPropDispatchMeta>& outMeta)
{
    outPackets.resize(totalCmds);
    outMeta.resize(totalCmds);
    uint32_t lockstepViolations = 0u;

    constexpr uint32_t kNoObjectIndex = 0xFFFFFFFFu;

    for (uint32_t i = 0u; i < totalCmds; ++i) {
        // Guard 1: sorted_oob — i must be valid index into s_sortedPacketOrder
        if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
            ++s_v6FrameSortedOob;
            outMeta[i]    = {};
            outPackets[i] = {};
            continue;
        }
        const uint32_t globalPktIdx = s_sortedPacketOrder[i];

        // Guard 2: packet_oob — globalPktIdx must be valid index into s_packets
        if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) {
            ++s_v6FramePacketOob;
            outMeta[i]    = {};
            outPackets[i] = {};
            continue;
        }
        const GpuStaticPropPacket& pkt = s_packets[globalPktIdx];

        // Guard 3: type_oob — owningTypeID outside static all-types table
        if (pkt.owningTypeID >= static_cast<uint32_t>(s_types.size())) {
            ++s_v6FrameTypeOob;
            outMeta[i]    = {};
            outPackets[i] = {};
            continue;
        }

        const auto typeIt = s_typeRanges.find(pkt.owningTypeID);
        const uint32_t instCount =
            (typeIt != s_typeRanges.end()) ? typeIt->second.instanceCount : 0u;

        const uint32_t grp = (i < s_alphaOffCmdCount) ? 0u : 1u;

        outMeta[i].sortedSlot      = i;
        outMeta[i].globalPacketIdx = globalPktIdx;
        outMeta[i].typeId          = pkt.owningTypeID;
        outMeta[i].group           = grp;
        outMeta[i].instanceCount   = instCount;
        outMeta[i].baseInstance    = baseInstanceMap[i];
        outMeta[i].drawIDBase      = i;
        outMeta[i].baseVertex      = pkt.baseVertex;

        outPackets[i].pipelineId    = static_cast<uint32_t>(
            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                      : RenderCore::PipelineId::StaticPropAlphaTest);
        outPackets[i].mesh          = {};
        outPackets[i].material      = {};
        outPackets[i].objectIndex   = kNoObjectIndex;
        outPackets[i].lightIndex    = kNoObjectIndex;
        outPackets[i].firstIndex    = pkt.firstIndex;
        outPackets[i].indexCount    = pkt.indexCount;
        outPackets[i].instanceCount = instCount;
        outPackets[i].sortKey       = 0u;
    }

    // --- Lockstep validator (ALL checks before any glDraw) ---
    if (outPackets.size() != outMeta.size()) {
        mc2::fmt::print_err("[DRAW_PACKET_V6] frame={:d} event=lockstep_violation slot=- field=array_size expected={:d} got={:d}\n", mc2::fmt::as_u32(s_v6TotalFrameCount), mc2::fmt::as_size(outMeta.size()), mc2::fmt::as_size(outPackets.size()));
        ++lockstepViolations;
    }

    for (uint32_t i = 0u; i < totalCmds; ++i) {
        // Skip slots zeroed by builder guard continues (sorted/packet/type OOB).
        if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) continue;
        const uint32_t gIdx = s_sortedPacketOrder[i];
        if (gIdx >= static_cast<uint32_t>(s_packets.size())) continue;
        const GpuStaticPropPacket& p = s_packets[gIdx];
        if (p.owningTypeID >= static_cast<uint32_t>(s_types.size())) continue;

        const uint32_t grp = (i < s_alphaOffCmdCount) ? 0u : 1u;
        const uint32_t expectedPipelineId = static_cast<uint32_t>(
            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                      : RenderCore::PipelineId::StaticPropAlphaTest);

#define V6_CHECK(field, expected_expr, got_expr) \
    if ((expected_expr) != (got_expr)) { \
        std::fprintf(stderr, \
            "[DRAW_PACKET_V6] frame=%u event=lockstep_violation slot=%u" \
            " field=" #field " expected=%u got=%u\n", \
            s_v6TotalFrameCount, i, \
            static_cast<unsigned>(expected_expr), \
            static_cast<unsigned>(got_expr)); \
        ++lockstepViolations; \
    }
        V6_CHECK(sortedSlot,     i,                  outMeta[i].sortedSlot)
        V6_CHECK(globalPacketIdx, gIdx,              outMeta[i].globalPacketIdx)
        V6_CHECK(drawIDBase,     i,                  outMeta[i].drawIDBase)
        V6_CHECK(typeId,         p.owningTypeID,     outMeta[i].typeId)
        V6_CHECK(group,          grp,                outMeta[i].group)
        V6_CHECK(pipelineId,     expectedPipelineId, outPackets[i].pipelineId)
        V6_CHECK(indexCount,     p.indexCount,       outPackets[i].indexCount)
        V6_CHECK(firstIndex,     p.firstIndex,       outPackets[i].firstIndex)
        V6_CHECK(instanceCount,  outMeta[i].instanceCount, outPackets[i].instanceCount)
#undef V6_CHECK
        if (outMeta[i].baseVertex != p.baseVertex) {
            mc2::fmt::print_err("[DRAW_PACKET_V6] frame={:d} event=lockstep_violation slot={:d} field=baseVertex expected={:d} got={:d}\n", mc2::fmt::as_u32(s_v6TotalFrameCount), mc2::fmt::as_u32(i), mc2::fmt::as_i32(p.baseVertex), mc2::fmt::as_i32(outMeta[i].baseVertex));
            ++lockstepViolations;
        }
    }

    return lockstepViolations;
}

// FOLIAGE-STATICPROP-DEPTH-PREPASS-1 ----------------------------------------
// In-flush depth prepass for the v6/coalesced static-prop dispatch.
//
// DELIBERATE DEVIATION from the plan's txmmgr-level call: the plan called
// flushDepthPrepass() from txmmgr between compute_dispatch() and flush(). But
// the per-frame dispatch lists (pDispatchPackets / pDispatchMeta / totalCmds)
// are built INSIDE flush() (snapshot vs live selection). A txmmgr-level prepass
// cannot see those lists and could draw a DIFFERENT set, which breaks the
// GL_EQUAL color pass (every color fragment must have a matching depth from the
// prepass). So the prepass runs here, inside flush(), consuming the EXACT same
// lists the color loop draws — ONE source of truth for the draw set.
//
// Lays the nearest reverse-Z depth (GEQUAL + write) with the cheap depth-only
// program (alpha-test discard only), color writes masked off. Caller flips the
// color pass to GL_EQUAL + depthWrite-off afterward, keyed off this function's
// inline return value (true == prepass drew this frame).
//
// Mirrors the v6 color draw loop's per-packet GL exactly: same VAO/IBO/SSBO,
// same OFF→ON texture-array flip at the alpha-ON group boundary, same
// glDrawElementsInstancedBaseVertexBaseInstance args. Returns true iff it drew.
//
// Gated by MC2_STATIC_PROP_DEPTH_PREPASS (default OFF). No-op + log-once when
// the depth program failed to link or the bc7-bucket texture path is active
// (per-bucket array bind differs from the simple OFF/ON bind — handled in a
// later slice). It does NOT touch SSBO slot 0 (the coalesce instance range
// bound earlier in flush()) so that binding survives for the color loop.
// OBJECT-TERRAIN-COPLANAR-OFFSET-1: slope-scaled polygon offset for opaque world
// objects (props/buildings/turrets/mechs) so they win the coplanar GEQUAL tie
// over true-depth terrain WITHOUT the constant-NDC-bias tradeoff (too small ->
// far-zoom z-fight; too large -> near show-through). Polygon offset tracks the
// per-fragment depth slope/precision, which is the correct shape for this.
// Gated default-OFF (gate OFF == committed TERRAIN-DEPTH-CONTRACT-1 behavior).
// Tunable; reverse-Z/GEQUAL so sign is determined by tuning (start -1/-1).
bool objPolyOffsetEnabled() {
    return mc2::gate::get<"MC2_OBJECT_POLY_OFFSET">();
}
float objPolyOffsetFactor() {
    static const float v = mc2::gate::get<"MC2_OBJECT_POLY_OFFSET_FACTOR">();
    return v;
}
float objPolyOffsetUnits() {
    static const float v = mc2::gate::get<"MC2_OBJECT_POLY_OFFSET_UNITS">();
    return v;
}

bool flushDepthPrepassV6(
    const std::vector<RenderCore::DrawPacket>&  dispatchPackets,
    const std::vector<StaticPropDispatchMeta>&  dispatchMeta,
    uint32_t                                    totalCmds,
    bool                                        bc7Buckets,
    int                                         debugAddrMode_)  // IMPORTANT-1: mode-8 bypass parity
{
    ZoneScopedN("GpuSP.DepthPrepass");

    // OBJECT-TERRAIN-COPLANAR-OFFSET-1: skip the prepass when the polygon-offset
    // gate is on, so the color pass uses single-pass GEQUAL (no GL_EQUAL parity
    // constraint) and the offset applied to the color draw fully governs depth.
    if (objPolyOffsetEnabled()) return false;

    static const bool s_prepassEnabled = mc2::gate::get<"MC2_STATIC_PROP_DEPTH_PREPASS">();
    if (!s_prepassEnabled)              return false;
    if (s_staticPropDepthProgram == 0)  return false;   // program didn't link
    if (totalCmds == 0u)                return false;

    // bc7-bucket path: the color loop binds a different per-bucket texture array
    // per slot for alpha sampling. The depth shader must sample the SAME array to
    // make the SAME alpha decision; the simple OFF/ON bind below would diverge.
    // Skip + log for now (handle bc7 in a follow-up slice).
    if (bc7Buckets) {
        static bool warnedBc7 = false;
        if (!warnedBc7) {
            warnedBc7 = true;
            mc2::fmt::print_err("[GPUPROPS] depth-prepass skipped: bc7-bucket texture path active (per-bucket array bind unsupported in this slice)\n");
        }
        return false;
    }

    // GL_EQUAL-parity (review CRITICAL-2): when MC2_VIEW_UNIFORMS=0 the VS uses a
    // plain `uniform mat4 u_worldToClipGL` (cached on the COLOR program as
    // s_locsCoalesce.terrainMVP) instead of the view-uniform UBO. The depth
    // program is a distinct GL object and the prepass does NOT upload that MVP
    // to it, so its u_worldToClipGL would be the zero matrix → gl_Position = 0 →
    // the prepass lays no usable depth and GL_EQUAL makes all props vanish.
    // s_viewUniformsDisabled (file-scope, == !s_viewUniformsShaderEnabled) is the
    // non-default kill-switch combo; skip the prepass entirely so the color path
    // falls back to single-pass GEQUAL (return false → caller draws unchanged).
    if (s_viewUniformsDisabled) {
        static bool warnedNoUbo = false;
        if (!warnedNoUbo) {
            warnedNoUbo = true;
            mc2::fmt::print_err("[GPUPROPS] depth-prepass skipped: MC2_VIEW_UNIFORMS=0 (non-UBO MVP path unsupported)\n");
        }
        return false;
    }

    // Save the color write mask so the color pass writes normally afterward.
    // GLSTATE-GUARD-SITE5: capture-only guard replaces the hand-rolled
    // prevColorMask[4] save + glColorMask() restore below. Single exit path,
    // no gos_InvalidateRenderStateCache() in this function, so function scope
    // is correct (matches gl_state_guard.h's scoping note).
    mc2gl::GlScopedColorMask guardColorMask;

    // Depth-only: bind the depth program + GEQUAL/write via the StaticPropDepth
    // desc, then mask color. applyPipeline also (re)sets blend-off + cull-back,
    // matching the color pass.
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropDepth),
        "StaticPropDepth");   // STATIC-PROP-COLOR-LABEL-HARDENING-1: label the live depth-prepass bind
    render_frame_plan::trace(render_frame_plan::Phase::StaticOpaque, "StaticPropDepth",
        render_frame_plan::PathKind::Batcher, -1, "StaticPropDepth");
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // Re-assert the geometry bindings the v6 loop relies on. SSBO slot 0 (the
    // coalesce instance range) and slot 2 (per-type hot-color) were bound earlier
    // in flush(); we do NOT touch slot 0 (so it survives for the color loop) but
    // re-bind VAO/IBO/slot-2/texture defensively in case applyPipeline or a prior
    // pass left them in an unexpected state.
    glBindVertexArray(s_sharedVao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    if (s_perTypeSsbo) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s_perTypeSsbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);   // alpha-OFF group default

    // GL_EQUAL-parity (review CRITICAL-1): the depth frag derives effectiveLayer
    // (texArrayLayer vs MaterialTable.albedoTex @ binding 5) under u_materialGpuSample
    // EXACTLY like the color frag. Mirror the color pass's slot-5 bind + sampleOn
    // computation (flush() ~line 5598-5621) on THIS program so the alpha-test UV →
    // tex_color.a → discard decision is byte-identical. sampleOn replicates the
    // color pass's five-condition gate verbatim (incl. s_materialGpuSidecarValid).
    if (s_materialGpuEnabled && s_materialGpuSsbo.backendHandle != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, static_cast<GLuint>(s_materialGpuSsbo.backendHandle));
    const bool depthSampleOn = s_materialGpuEnabled
                            && s_materialGpuSampleEnabled
                            && s_materialGpuSsbo.backendHandle != 0
                            && s_materialGpuSidecarValid
                            && s_locsDepthCoalesce.materialGpuSample >= 0;
    if (s_locsDepthCoalesce.materialGpuSample >= 0)
        glUniform1i(s_locsDepthCoalesce.materialGpuSample, depthSampleOn ? 1 : 0);
    // GL_EQUAL-parity (review IMPORTANT-1): mode-8 LODBUG bypass must match the
    // color pass's u_debugAddrMode upload (flush() ~line 5532) so the depth frag's
    // `u_debugAddrMode != 8` discard gate agrees with the color frag.
    if (s_locsDepthCoalesce.debugAddrMode >= 0)
        glUniform1i(s_locsDepthCoalesce.debugAddrMode, debugAddrMode_);

    bool enteredOnGroup = false;
    for (uint32_t i = 0u; i < totalCmds; ++i) {
        const StaticPropDispatchMeta& m  = dispatchMeta[i];
        const RenderCore::DrawPacket&  dp = dispatchPackets[i];
        if (m.instanceCount == 0u) continue;

        // Texture-array switch at the alpha-ON boundary (mirror flush()'s loop:
        // alpha-OFF slots come first, then alpha-ON; one bind flip total).
        if (m.group == 1u && !enteredOnGroup) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
            enteredOnGroup = true;
        }

        // The DEPTH program's OWN u_drawIDBase location (coalesce variant).
        if (s_locsDepthCoalesce.drawIDBase >= 0)
            glUniform1i(s_locsDepthCoalesce.drawIDBase,
                        static_cast<GLint>(m.drawIDBase));

        glDrawElementsInstancedBaseVertexBaseInstance(
            GL_TRIANGLES,
            static_cast<GLsizei>(dp.indexCount),
            GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(dp.firstIndex) * sizeof(uint32_t)),
            static_cast<GLsizei>(m.instanceCount),
            m.baseVertex,
            m.baseInstance);
    }
    if (enteredOnGroup)
        glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

    // Color write mask restored by guardColorMask's destructor (scope exit,
    // right here). Depth state + program are reset by the color applyPipeline
    // the caller issues immediately after this returns.
    return true;
}

// APPLY-STATE-STATICPROP-1: file-static flag mirroring decalStateAppliedByExecutor_.
// When MC2_FRAMEGRAPH_EXECUTOR is ON, executorApplyStaticPropOpaqueState() pre-applies
// the StaticPropOpaque pipeline and sets this; flush()'s COLOR-pass body then skips its
// own applyPipeline (one-shot reset). Gate OFF -> flag stays false -> body applies as
// before -> byte-identical.
bool s_staticPropStateAppliedByExecutor = false;

// APPLY-STATE-STATICPROP-1: pre-apply the declared StaticPropOpaque pipeline. Top-level
// analog of gosRenderer::executorApplyTerrainDecalState() / ...OverlayState(): performs
// the SOLE entry render-state of the static-prop COLOR pass (applyPipeline(StaticPropOpaque)
// at flush ~:5503), bumps the shared apply-state counter, and flags the body to skip its
// own applyPipeline (one-shot). Only the pipeline is lifted; FBO/MRT/drawBuffers/viewport
// are inherited from the preceding pass and are NOT touched here (kTopLevelStateDesc records
// them as Unknown/Inherit). The separate StaticPropDepth prepass (~:5252) is NOT lifted.
// Idempotent: the body makes the identical call when the flag is false. Only reached when
// MC2_FRAMEGRAPH_EXECUTOR is ON and the pass runs.
void executorApplyStaticPropOpaqueState() {
    // FRAMEGRAPH-APPLY-STATE-EXTEND-1 SELF-PROOF: apply the EXPLICIT generic axes the
    // StaticPropOpaque row now declares (fboTarget=MainColor, viewport=MainScene, clear=None)
    // BEFORE the pipeline. StaticProp already inherits exactly the scene FBO + full-scene
    // viewport, so this explicit bind is idempotent and byte-identical to inheritance — it
    // proves the richer apply path at runtime (fbo_mismatches must stay 0). If the scene FBO/
    // size are unavailable we skip the generic-axes (defensive; the desc still drives the
    // pipeline apply below) — but getGosPostProcess() is established by the time static props
    // flush every tier1 frame.
    const RenderCore::framegraph::TopLevelStateDesc* desc =
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::StaticPropOpaque);
    gosPostProcess* pp = getGosPostProcess();
    if (desc && pp) {
        RenderCore::framegraph::applyTopLevelGenericAxes(
            *desc, (unsigned)pp->getSceneFBO(), pp->getWidth(), pp->getHeight());
    }
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque),
        "StaticPropOpaque");
    mc2_framegraph_executor_bump_apply_state((unsigned)RenderCore::framegraph::ApplyPassId::StaticPropOpaque);
    s_staticPropStateAppliedByExecutor = true;
}
