// GpuStaticPropBatcher: the flush gate arming, DrawPacket build, v5 / v6 dispatch, legacy setup and the fence /
// restore tail. The orchestrator is in gos_static_prop_batcher_flush.cpp.
#include "gos_static_prop_batcher_internal.h"
#include "mdi_submit.h"                          // MDI-SUBMISSION-SCAFFOLD-1: flush issues the indirect draws
#include "gos_static_prop_flush_frame.h"


// Run the one-time DrawPacket v5 gate-arm checks.
void GpuStaticPropBatcher::armV5() {
        // DrawPacket v5: gate-arm checks (run once per process on first gate-ON flush).
        if (s_v5Enabled && !s_v5Armed && !s_v5Disarmed) {
            s_v5Armed = true;
            s_baseInstanceSupported = (GLEW_ARB_base_instance == GL_TRUE);
            bool disarm = false;

            // Precondition 1: base-instance map must be non-null.
            // Under MC2_STATIC_PROP_GLOBAL_POOL_LEGACY=1, s_baseInstanceByCmdSsbo is
            // never allocated and s_baseInstanceByCmdMap stays nullptr.
            // Check here (not per-slot) to avoid 753x base_instance_missing masking root cause.
            if (s_baseInstanceByCmdMap == nullptr) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=disarmed reason=legacy_pool_no_baseinst_map\n");
                disarm = true;
            }

            // Precondition 2: u_drawIDBase uniform location must be valid.
            // If -1, glUniform1i is a GL no-op and the shader reads the wrong PerDrawEntry.
            if (!disarm && s_locsCoalesce.drawIDBase < 0) {
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] WARNING event=disarmed reason=drawid_loc_missing"
                    " loc=%d\n",
                    static_cast<int>(s_locsCoalesce.drawIDBase));
                disarm = true;
            }

            if (disarm) {
                s_v5Disarmed = true;
            } else {
                const uint32_t totalSlots = s_alphaOffCmdCount + s_alphaOnCmdCount;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] event=armed slots=%u ext_supported=%d"
                    " drawid_loc=%d\n",
                    totalSlots,
                    static_cast<int>(s_baseInstanceSupported),
                    static_cast<int>(s_locsCoalesce.drawIDBase));
            }
        }
}

// Run the one-time DrawPacket v6 gate-arm checks.
void GpuStaticPropBatcher::armV6() {
        // DrawPacket v6: gate-arm checks (run once per process on first gate-ON flush).
        if (s_v6Enabled && !s_v6Armed && !s_v6Disarmed) {
            s_v6Armed = true;
            bool disarm = false;
            const char* disarmReason = nullptr;

            // Check ARB_base_instance independently — do NOT read s_baseInstanceSupported
            // (written only inside v5 arm block; may be false if v5 gate is OFF).
            if (GLEW_ARB_base_instance != GL_TRUE) {
                disarmReason = "base_instance_ext_missing";
                disarm = true;
            }

            if (!disarm && s_baseInstanceByCmdMap == nullptr) {
                disarmReason = "legacy_pool_no_baseinst_map";
                disarm = true;
            }

            if (!disarm && s_locsCoalesce.drawIDBase < 0) {
                disarmReason = "drawid_loc_missing";
                disarm = true;
            }

            if (!disarm &&
                static_cast<uint32_t>(RenderCore::PipelineId::StaticPropOpaque) >=
                static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
                disarmReason = "pipeline_enum_stale_opaque";
                disarm = true;
            }

            if (!disarm &&
                static_cast<uint32_t>(RenderCore::PipelineId::StaticPropAlphaTest) >=
                static_cast<uint32_t>(RenderCore::PipelineId::Count_)) {
                disarmReason = "pipeline_enum_stale_alphatest";
                disarm = true;
            }

            if (disarm) {
                s_v6Disarmed = true;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] event=disarmed reason=%s\n",
                    disarmReason);
            } else {
                const uint32_t totalSlots = s_alphaOffCmdCount + s_alphaOnCmdCount;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] event=armed slots=%u alpha_off=%u alpha_on=%u"
                    " ext_supported=1 drawid_loc=%d default=1\n",
                    totalSlots, s_alphaOffCmdCount, s_alphaOnCmdCount,
                    static_cast<int>(s_locsCoalesce.drawIDBase));
            }
        }
}

// Decide whether the DrawPacket v6 path runs this frame.
void GpuStaticPropBatcher::decideV6(StaticPropFlushFrame& f) {
    auto& runV6 = f.runV6;
        // DrawPacket v6: build packet+meta arrays then validate before dispatch.
        runV6 = s_v6Enabled && !s_v6Disarmed;
}

// Reset the v6 lock-step and live-builder state.
void GpuStaticPropBatcher::initV6State(StaticPropFlushFrame& f) {
    auto& v6LockstepViolations = f.v6LockstepViolations; auto& retireLiveBuilder = f.retireLiveBuilder;
        v6LockstepViolations = 0u;
        // v8: snapshot-sole-owner decision. Set in the runV6 preamble, read again in
        // the dispatch block (a separate scope) to select dispatch + structural fallback.
        retireLiveBuilder = false;
}

// Build and validate the DrawPacket v6 arrays.
void GpuStaticPropBatcher::buildV6Packets(std::vector<RenderCore::DrawPacket>& v6Packets, std::vector<StaticPropDispatchMeta>& v6Meta, StaticPropFlushFrame& f) {
    auto& runV6 = f.runV6; auto& v6LockstepViolations = f.v6LockstepViolations; auto& retireLiveBuilder = f.retireLiveBuilder;
        if (runV6) {
            s_v6FrameDrawsIssued        = 0u;
            s_v6FrameZeroInstSkips      = 0u;
            s_v6FrameSortedOob          = 0u;
            s_v6FramePacketOob          = 0u;
            s_v6FrameTypeOob            = 0u;
            s_v6FrameLockstepViolations = 0u;
            s_v6FrameGlErrors           = 0u;
            ++s_v6TotalFrameCount;

            // v8: retirement predicate. Consumed just below to gate the live builder,
            // set s_spBuildRetired, and emit the one-shot arm log.
            //
            // Why !s_snapCullEnabled is a condition: the snap-cull path (MC2_SNAP_CULL,
            // opt-in, default OFF) still relies on the live<->snapshot compare for slot
            // ownership in v8; it has not been ported to sole-owner packet build. Keep
            // the live builder whenever snap-cull is active until that port lands.
            const bool retireLiveBuilderPlanned =
                !s_keepLiveBuilder && s_snapshotBuildEnabled && !s_snapCullEnabled;

            // v8: snapshot is now the SOLE owner when retired. Consume the planned
            // predicate, set telemetry, emit the one-shot arm log.
            retireLiveBuilder = retireLiveBuilderPlanned;
            if (!s_v8ArmLogged) {
                s_v8ArmLogged = true;
                const char* reason =
                    retireLiveBuilder       ? "snapshot_sole_owner" :
                    s_keepLiveBuilder       ? "live_builder_forced" :
                    !s_snapshotBuildEnabled ? "snapshot_packet_build_disabled_keep_live" :
                                              "snap_cull_collision_keep_live";
                MC2_DIAGFE(stderr,
                    "[STATIC_PROP_PACKET_DISPATCH v8] event=arm live_builder_retired=%d"
                    " snapshot_packet_build=%d live_builder_forced=%d reason=%s\n",
                    retireLiveBuilder ? 1 : 0, s_snapshotBuildEnabled ? 1 : 0,
                    s_keepLiveBuilder ? 1 : 0, reason);
                std::fflush(stderr);
            }

            // v3: reset per-flush build counters so stale stats never persist.
            s_spBuildAttempted = s_spBuildCountMismatch = s_spBuildPacketMismatch =
            s_spBuildMetaMismatch = s_spBuildFallback = s_spBuildRetired = 0u;

            // v8: set retirement telemetry AFTER the unconditional reset above so it
            // is not clobbered. 1 = live builder skipped this frame (snapshot sole owner).
            s_spBuildRetired = retireLiveBuilder ? 1u : 0u;

            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            if (g_mc2HitchEnabled) {
                g_mc2HitchAccum.staticRecords  = static_cast<uint32_t>(s_bucketsByType.size());
                g_mc2HitchAccum.staticDrawCmds = totalCmds;
            }

            const size_t fr_off_bi_v6 =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi_v6);

            // When retired and the snapshot builds clean, s_v6FrameLockstepViolations
            // stays 0 from the unconditional per-flush reset above (no live build runs).
            if (!retireLiveBuilder) {
                ZoneScopedN("StaticProp.LiveBuild");
                v6LockstepViolations = buildLiveV6Arrays(totalCmds, baseInstanceMap, v6Packets, v6Meta);
                s_v6FrameLockstepViolations = v6LockstepViolations;
            }
        }
}

// Decide whether the DrawPacket v5 path runs this frame.
void GpuStaticPropBatcher::decideV5(StaticPropFlushFrame& f) {
    auto& runV5 = f.runV5;
        // DrawPacket v5: per-draw-call dispatch (MC2_DRAW_PACKET_COALESCE_V5=1).
        // When active, replaces both glMultiDrawElementsIndirect calls with a
        // per-slot glDrawElementsInstancedBaseVertexBaseInstance loop.
        runV5 = s_v5Enabled && s_baseInstanceSupported && !s_v5Disarmed;
}

// DrawPacket v6 dispatch.
void GpuStaticPropBatcher::dispatchV6(const RenderSnapshot* snap, std::vector<RenderCore::DrawPacket>& v6Packets, std::vector<StaticPropDispatchMeta>& v6Meta, StaticPropFlushFrame& f) {
    auto& fr_off_bytes_d = f.fr_off_bytes_d; auto& v6LockstepViolations = f.v6LockstepViolations; auto& retireLiveBuilder = f.retireLiveBuilder;
            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            // v8: re-home the ring-slot base-instance map for this dispatch. The snapshot
            // build borrows baseInstance from here (NOT from the live build, which is
            // skipped when retired). Same arithmetic as the live path's baseInstanceMap.
            const size_t fr_off_bi_disp =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMapDisp = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi_disp);

            {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.v6a");
            }

            glActiveTexture(GL_TEXTURE0);
            // COMPRESSION-BC7-STATICPROP-2: gate ON binds per-bucket inside the
            // loop (init to an invalid bucket so the first drawn slot binds).
            // Gate OFF: original OFF→ON flip, byte-identical.
            const bool bc7Buckets = s_staticPropBc7Enabled && !s_slotBucketIndex.empty();
            int32_t curBucket = -1;  // gate-ON tracker
            if (!bc7Buckets) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
            bool enteredOnGroup = false;

            // v2.3 snap-cull: determine whether to use prev-frame snapshot for slot skipping.
            // Activation requires:
            //   - MC2_SNAP_CULL=1 and snap non-null
            //   - snap->ok == 1u (snapshot was structurally valid — v2.2 gates passed)
            //   - count matches totalCmds (snapshot covers exactly the live draw slots)
            // Warmup guard: even when all conditions above hold, if ALL instanceCounts are zero
            // (frame 1 or all-invisible scene) don't cull — avoids frame-1 blank screen.
            bool snapCullActive = false;
            uint32_t snapN      = 0u;
            if (s_snapCullEnabled &&
                snap != nullptr &&
                snap->ok == 1u &&
                snap->staticPropPackets.data != nullptr &&
                snap->staticPropPackets.count == totalCmds &&
                snap->staticPropPackets.count > 0u) {
                bool allZero = true;
                for (size_t scj = 0; scj < snap->staticPropPackets.count; ++scj) {
                    if (snap->staticPropPackets.data[scj].instanceCount != 0u) {
                        allZero = false;
                        break;
                    }
                }
                snapCullActive = !allZero;
                snapN          = static_cast<uint32_t>(snap->staticPropPackets.count);
            }

            bool snapBuilt = false;
            s_spBuildAttempted = s_snapshotBuildEnabled ? 1u : 0u;

            if (s_snapshotBuildEnabled) {
                // Stage 1: snap-cull collision — both gates active → disable v3.
                if (s_snapCullEnabled) {
                    if (!s_spBuildCollisionLoggedOnce) {
                        s_spBuildCollisionLoggedOnce = true;
                        std::fprintf(stderr,
                            "[RENDER_SNAPSHOT v3] frame=%u disabled — MC2_SNAP_CULL collision\n",
                            s_v6TotalFrameCount);
                    }
                    ++s_spBuildFallback;
                }
                // Stage 2: structural guards — snap unusable (no counter).
                else if (snap == nullptr || snap->ok != 1u ||
                         snap->staticPropPackets.data == nullptr) {
                    /* no counter — snap is unusable, not a v3 failure */
                }
                // Stage 3: v2.2 structural must be clean.
                else if (snap->spCountMismatch       != 0u ||
                         snap->spSortedSlotMismatch  != 0u ||
                         snap->spGlobalPacketMismatch != 0u ||
                         snap->spPipelineMismatch     != 0u ||
                         snap->spMaterialIdxMismatch  != 0u ||
                         snap->spTexLayerMismatch     != 0u) {
                    /* no counter — v2.2 gate failed; ordering unreliable */
                }
                else if (snap->staticPropPackets.count != totalCmds) {
                    ++s_spBuildCountMismatch;
                    ++s_spBuildFallback;
                }
                else {
                    {
                    ZoneScopedN("StaticProp.SnapshotBuild");
                    mc2_hitch::HitchScope _hitchSnap(mc2_hitch::HitchSpanKind::GpuStaticPropsSnapshot);
                    s_snapV6Packets.resize(totalCmds);
                    s_snapV6Meta.resize(totalCmds);

                    for (uint32_t si = 0u; si < totalCmds; ++si) {
                        const ExtractedStaticPropPacket& row =
                            snap->staticPropPackets.data[si];

                        // Snapshot owns slot identity — row must claim this slot.
                        if (row.sortedSlot != si) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Guard: globalPacketIdx in bounds.
                        if (row.globalPacketIdx >=
                                static_cast<uint32_t>(s_packets.size())) {
                            ++s_spBuildPacketMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        const GpuStaticPropPacket& spkt = s_packets[row.globalPacketIdx];

                        if (row.typeId >= static_cast<uint32_t>(s_types.size())) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // Fail closed on unknown pipelineId.
                        const uint32_t grp = pipelineId_to_group(row.pipelineId);
                        if (grp == 0xFFFFFFFFu) {
                            ++s_spBuildMetaMismatch;
                            s_snapV6Meta[si]    = {};
                            s_snapV6Packets[si] = {};
                            continue;
                        }
                        // instanceCount from current-frame live (NOT snapshot prev-frame).
                        const auto typeIt = s_typeRanges.find(row.typeId);
                        const uint32_t instCount =
                            (typeIt != s_typeRanges.end())
                                ? typeIt->second.instanceCount : 0u;

                        s_snapV6Meta[si].sortedSlot      = row.sortedSlot;
                        s_snapV6Meta[si].globalPacketIdx = row.globalPacketIdx;
                        s_snapV6Meta[si].typeId          = row.typeId;
                        s_snapV6Meta[si].group           = grp;
                        s_snapV6Meta[si].instanceCount   = instCount;
                        s_snapV6Meta[si].baseInstance    = baseInstanceMapDisp[si];
                        s_snapV6Meta[si].drawIDBase      = si;
                        s_snapV6Meta[si].baseVertex      = spkt.baseVertex;

                        s_snapV6Packets[si]            = RenderCore::DrawPacket{};
                        s_snapV6Packets[si].pipelineId = static_cast<uint32_t>(
                            grp == 0u ? RenderCore::PipelineId::StaticPropOpaque
                                      : RenderCore::PipelineId::StaticPropAlphaTest);
                        s_snapV6Packets[si].firstIndex = spkt.firstIndex;
                        s_snapV6Packets[si].indexCount = spkt.indexCount;
                    }

                    }  // end StaticProp.SnapshotBuild

                    // Compare snapshot-built vs live-built, field-by-field per slot.
                    // v8: skip when the live builder is retired — v6Meta/v6Packets are
                    // STALE (not rebuilt this frame), so the compare is meaningless and
                    // would set spurious mismatches. Snapshot is sole owner in that mode.
                    if (!retireLiveBuilder) {
                    ZoneScopedN("StaticProp.BuildCompare");
                    for (uint32_t ci = 0u; ci < totalCmds; ++ci) {
                        const StaticPropDispatchMeta& sm = s_snapV6Meta[ci];
                        const StaticPropDispatchMeta& lm = v6Meta[ci];
                        const RenderCore::DrawPacket&  sp = s_snapV6Packets[ci];
                        const RenderCore::DrawPacket&  lp = v6Packets[ci];

                        if (sm.globalPacketIdx != lm.globalPacketIdx) ++s_spBuildMetaMismatch;
                        if (sm.typeId          != lm.typeId)          ++s_spBuildMetaMismatch;
                        if (sm.group           != lm.group)           ++s_spBuildMetaMismatch;
                        if (sm.sortedSlot      != lm.sortedSlot)      ++s_spBuildMetaMismatch;
                        if (sm.baseVertex      != lm.baseVertex)      ++s_spBuildMetaMismatch;
                        if (sm.drawIDBase      != lm.drawIDBase)      ++s_spBuildMetaMismatch;

                        if (sp.pipelineId != lp.pipelineId) ++s_spBuildPacketMismatch;
                        if (sp.firstIndex  != lp.firstIndex)  ++s_spBuildPacketMismatch;
                        if (sp.indexCount  != lp.indexCount)  ++s_spBuildPacketMismatch;
                    }
                    }  // end StaticProp.BuildCompare

                    snapBuilt = true;
                }
            }

            bool useSnapshot;
            bool skipDispatch = false;
            if (retireLiveBuilder) {
                // Snapshot is sole owner. Dispatch unless STRUCTURALLY invalid.
                // snapshotInvalid == NOT built (guards failed: snap nullptr / ok!=1 /
                // count mismatch / malformed metadata). An empty-but-valid snapshot
                // (snapBuilt with totalCmds==0) is VALID → dispatch zero, NEVER fallback.
                //
                // A per-row guard failure during the snapshot build increments these
                // (and zeroes the bad slot) but still leaves snapBuilt=true. Treat any
                // such divergence as structurally invalid so we fall back to the live
                // build for the whole frame — matching the dual path's mismatch->fallback
                // safety. This also makes the spBuildFallback==0 merge gate cover it.
                const bool snapshotInvalid = !snapBuilt
                    || s_spBuildMetaMismatch   != 0u
                    || s_spBuildPacketMismatch != 0u;
                if (snapshotInvalid) {
                    // Stage 4 (count mismatch) already counted a fallback for this frame;
                    // avoid double-counting so spBuildFallback reflects frames, not events.
                    if (s_spBuildCountMismatch == 0u) ++s_spBuildFallback;
                    ZoneScopedN("StaticProp.LiveBuild.Fallback");  // rare fallback build, counted
                    v6LockstepViolations =
                        buildLiveV6Arrays(totalCmds, baseInstanceMapDisp, v6Packets, v6Meta);
                    s_v6FrameLockstepViolations = v6LockstepViolations;
                    useSnapshot = false;
                    if (v6LockstepViolations != 0u) {
                        // Fallback build itself is inconsistent — do not issue draws from a
                        // broken array this frame (matches dual-mode lockstep gating).
                        skipDispatch = true;
                    }
                } else {
                    useSnapshot = true;
                }
            } else {
                // Legacy dual path: use snapshot arrays only when compare clean.
                useSnapshot = snapBuilt
                    && s_spBuildPacketMismatch == 0
                    && s_spBuildMetaMismatch   == 0;

                if (snapBuilt && !useSnapshot) {
                    ++s_spBuildFallback;
                    if (!s_spBuildFirstFallbackLogged) {
                        s_spBuildFirstFallbackLogged = true;
                        std::fprintf(stderr,
                            "[RENDER_SNAPSHOT v3] frame=%u first-fallback"
                            " attempted=1 count_mismatch=%u pkt_mismatch=%u"
                            " meta_mismatch=%u fallback=1\n",
                            s_v6TotalFrameCount,
                            s_spBuildCountMismatch,
                            s_spBuildPacketMismatch,
                            s_spBuildMetaMismatch);
                    }
                }
            }

            const std::vector<RenderCore::DrawPacket>* pDispatchPackets =
                useSnapshot ? &s_snapV6Packets : &v6Packets;
            const std::vector<StaticPropDispatchMeta>* pDispatchMeta =
                useSnapshot ? &s_snapV6Meta    : &v6Meta;

            // FOLIAGE-STATICPROP-DEPTH-PREPASS-1 (B + C): run the depth prepass
            // over the SAME dispatch lists the color loop below draws, then flip
            // the color pass to GL_EQUAL + depthWrite-off. Gated/no-op by default
            // (flushDepthPrepassV6 returns false → color path byte-identical to
            // pre-change behavior, no re-applyPipeline issued).
            //
            // The prepass clobbers the bound program + depth state + color mask;
            // it restores the color mask itself. We re-apply the color pipeline
            // here (with the EQUAL/no-write override) so the color loop below runs
            // against the right program + depth state. SSBO slot 0 (instance
            // range) and the VAO/IBO survive (prepass does not rebind slot 0; the
            // color loop re-binds the OFF texture array via enteredOnGroup logic).
            if (!skipDispatch &&
                flushDepthPrepassV6(*pDispatchPackets, *pDispatchMeta,
                                    totalCmds, bc7Buckets, debugAddrMode_)) {
                // Color pass: keep ONLY the front-most fragment the prepass laid
                // (reverse-Z, so GEQUAL prepass stored the nearest depth). EQUAL +
                // no depth re-write. Object-ID (frag loc=2) is unaffected — it is a
                // color attachment write, not a depth-state field.
                RenderCore::PipelineDesc colorDesc =
                    RenderCore::getPipelineDesc(RenderCore::PipelineId::StaticPropOpaque);
                colorDesc.depthFunc        = RenderCore::DepthFunc::Equal;
                colorDesc.depthWriteEnable = false;
                pipeline_binder::applyPipeline(colorDesc, "StaticPropOpaque");  // re-test variant (Equal depth)
                // applyPipeline rebinds the program but NOT the VAO/IBO/SSBO/
                // texture; re-assert the geometry bindings the color loop expects
                // (the prepass left the OFF texture array bound, slot-2 bound, and
                // slot 0 untouched, so this is belt-and-suspenders).
                glBindVertexArray(s_sharedVao);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
            }

            // OBJECT-TERRAIN-COPLANAR-OFFSET-1: slope-scaled depth push so object
            // bodies win the coplanar tie over true-depth terrain (gate default OFF).
            const bool s_objPO = objPolyOffsetEnabled();
            if (s_objPO) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(objPolyOffsetFactor(), objPolyOffsetUnits());
            }
            if (!skipDispatch)
            for (uint32_t i = 0u; i < totalCmds; ++i) {
                const StaticPropDispatchMeta& m  = (*pDispatchMeta)[i];
                const RenderCore::DrawPacket&  dp = (*pDispatchPackets)[i];

                // v2.3 snap-cull: skip slots whose prev-frame instanceCount was zero.
                // Per-row slot identity check guards against any snapshot/live ordering drift.
                // On identity mismatch: don't skip — count it and fall through to current-frame logic.
                if (snapCullActive && i < snapN) {
                    const ExtractedStaticPropPacket& scRow = snap->staticPropPackets.data[i];
                    if (scRow.sortedSlot != i) {
                        // Slot identity mismatch — snapshot and live order diverged.
                        ++s_snapCullSlotMismatch;
                    } else if (scRow.instanceCount == 0u) {
                        // Prev-frame zero — safe to skip.
                        ++s_snapCullSkipped;
                        continue;
                    } else {
                        ++s_snapCullActive;
                    }
                }

                if (m.instanceCount == 0u) {
                    ++s_v6FrameZeroInstSkips;
                    continue;
                }

                if (bc7Buckets) {
                    // Bind this slot's bucket array (contiguous runs → at most
                    // one bind per bucket). s_slotBucketIndex is in sorted-slot
                    // order, same as the dispatch loop index i.
                    if (i < s_slotBucketIndex.size()) {
                        const int32_t b = static_cast<int32_t>(s_slotBucketIndex[i]);
                        if (b != curBucket &&
                            b >= 0 && b < static_cast<int32_t>(s_bucketArrays.size())) {
                            glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                            // STATICPROP-MATERIAL-ORM-1: bind the layer-aligned ORM
                            // sibling to kOrmTexUnit, then restore active unit 0.
                            if (s_ormSlotsEnabled &&
                                static_cast<size_t>(b) < s_ormBucketArrays.size()) {
                                glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                                glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                                glActiveTexture(GL_TEXTURE0);
                            }
                            curBucket = b;
                        }
                    }
                } else if (m.group == 1u && !enteredOnGroup) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                    enteredOnGroup = true;
                }

                glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(m.drawIDBase));

                glDrawElementsInstancedBaseVertexBaseInstance(
                    GL_TRIANGLES,
                    static_cast<GLsizei>(dp.indexCount),
                    GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(dp.firstIndex) * sizeof(uint32_t)),
                    static_cast<GLsizei>(m.instanceCount),
                    m.baseVertex,
                    m.baseInstance);

                if (s_v6TraceEnabled)
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V6] slot=%u pkt=%u type=%u group=%u"
                        " inst=%u base=%u drawID=%u first=%u count=%u baseV=%d\n",
                        m.sortedSlot, m.globalPacketIdx, m.typeId, m.group,
                        m.instanceCount, m.baseInstance, m.drawIDBase,
                        dp.firstIndex, dp.indexCount, m.baseVertex);

                ++s_v6FrameDrawsIssued;
            }

            if (s_objPO) glDisable(GL_POLYGON_OFFSET_FILL);  // OBJECT-TERRAIN-COPLANAR-OFFSET-1

            if (enteredOnGroup)
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

            {
                const GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    ++s_v6FrameGlErrors;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V6] event=gl_error frame=%u err=0x%x\n",
                        s_v6TotalFrameCount, static_cast<unsigned>(err));
                }
            }

            if ((s_v6TotalFrameCount % 600u) == 0u) {
                const uint32_t slotsConsidered = totalCmds;
                const int ok = (s_v6FrameSortedOob          == 0u &&
                                s_v6FramePacketOob           == 0u &&
                                s_v6FrameTypeOob             == 0u &&
                                s_v6FrameLockstepViolations  == 0u &&
                                s_v6FrameGlErrors            == 0u &&
                                (s_v6FrameDrawsIssued + s_v6FrameZeroInstSkips) == slotsConsidered)
                               ? 1 : 0;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V6] frame=%u event=dispatch_summary"
                    " slots_considered=%u draws_issued=%u zero_instance_skips=%u"
                    " sorted_oob=%u packet_oob=%u type_oob=%u lockstep_violations=%u"
                    " gl_errors=%u ok=%d\n",
                    s_v6TotalFrameCount, slotsConsidered,
                    s_v6FrameDrawsIssued, s_v6FrameZeroInstSkips,
                    s_v6FrameSortedOob, s_v6FramePacketOob, s_v6FrameTypeOob,
                    s_v6FrameLockstepViolations, s_v6FrameGlErrors, ok);
                if (s_snapshotBuildEnabled) {
                    std::fprintf(stderr,
                        "[RENDER_SNAPSHOT v3] frame=%u"
                        " attempted=%u count_mismatch=%u pkt_mismatch=%u"
                        " meta_mismatch=%u fallback=%u using_snapshot=%d\n",
                        s_v6TotalFrameCount,
                        s_spBuildAttempted,
                        s_spBuildCountMismatch,
                        s_spBuildPacketMismatch,
                        s_spBuildMetaMismatch,
                        s_spBuildFallback,
                        useSnapshot ? 1 : 0);
                }
            }

}

// DrawPacket v5 per-draw-call dispatch.
void GpuStaticPropBatcher::dispatchV5(StaticPropFlushFrame& f) {
    auto& fr_off_bytes_d = f.fr_off_bytes_d;
            s_v5FrameDrawsIssued     = 0u;
            s_v5FrameZeroInstSkips   = 0u;
            s_v5FrameSortedOob       = 0u;
            s_v5FramePacketOob       = 0u;
            s_v5FrameTypeOob         = 0u;
            s_v5FrameBaseInstMissing = 0u;
            s_v5FrameGlErrors        = 0u;
            ++s_v5TotalFrameCount;

            const uint32_t totalCmds = s_alphaOffCmdCount + s_alphaOnCmdCount;

            // Compute base-instance map pointer for this ring slot.
            // Arithmetic is in BYTES: multiply ring-slot index by bytes-per-frame,
            // byte-cast the void*, then reinterpret as uint32_t*.
            // Do NOT use index arithmetic on the void* base directly.
            const size_t fr_off_bi =
                static_cast<size_t>(s_coalesceFrameSlot) * s_baseInstanceByCmdBytesPerFrame;
            const uint32_t* baseInstanceMap = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(s_baseInstanceByCmdMap) + fr_off_bi);

            // Unconditionally bind SSBO slot 0 before the loop.
            // The prologue only binds inside if(s_alphaOffCmdCount > 0u).
            // If all props are alpha-ON (OFF count == 0), slot 0 is stale.
            // v5 only runs in global-pool mode (legacy mode disarms at gate-arm).
            {
                const size_t totalUsed = s_totalUsedBytesThisFrame;
                gpuBindSsboRange(0, s_coalesceInstanceSsbo,
                                 (long long)fr_off_bytes_d,
                                 (long long)(totalUsed > 0 ? totalUsed : sizeof(GpuStaticPropInstance)),
                                 "sp.coalesce.v5");
            }

            glActiveTexture(GL_TEXTURE0);
            // COMPRESSION-BC7-STATICPROP-2: gate ON binds per-bucket in-loop.
            const bool bc7Buckets = s_staticPropBc7Enabled && !s_slotBucketIndex.empty();
            int32_t curBucket = -1;
            if (!bc7Buckets) glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);

            bool enteredOnGroup = false;

            for (uint32_t i = 0u; i < totalCmds; ++i) {
                if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=sorted_oob\n", i);
                    ++s_v5FrameSortedOob;
                    continue;
                }
                const uint32_t globalPktIdx = s_sortedPacketOrder[i];

                if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=packet_oob\n", i);
                    ++s_v5FramePacketOob;
                    continue;
                }
                const GpuStaticPropPacket& pkt = s_packets[globalPktIdx];

                // Genuine OOB: owningTypeID outside the static type table.
                if (pkt.owningTypeID >= static_cast<uint32_t>(s_types.size())) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=type_oob\n", i);
                    ++s_v5FrameTypeOob;
                    continue;
                }
                // s_typeRanges only has snapshot (instance-bearing) types.
                // Missing = zero instances this frame; not an error.
                const auto typeIt = s_typeRanges.find(pkt.owningTypeID);
                const uint32_t instanceCount =
                    (typeIt != s_typeRanges.end()) ? typeIt->second.instanceCount : 0u;

                if (instanceCount == 0u) {
                    if (s_v5TraceEnabled)
                        std::fprintf(stderr, "[DRAW_PACKET_V5] event=skip slot=%u reason=zero_inst\n", i);
                    ++s_v5FrameZeroInstSkips;
                    continue;
                }
                const GLuint baseInstance = baseInstanceMap[i];

                // Group boundary: switch texture array when entering alpha-ON group.
                if (bc7Buckets) {
                    if (i < s_slotBucketIndex.size()) {
                        const int32_t b = static_cast<int32_t>(s_slotBucketIndex[i]);
                        if (b != curBucket &&
                            b >= 0 && b < static_cast<int32_t>(s_bucketArrays.size())) {
                            glBindTexture(GL_TEXTURE_2D_ARRAY, s_bucketArrays[b]);
                            // STATICPROP-MATERIAL-ORM-1: bind layer-aligned ORM sibling.
                            if (s_ormSlotsEnabled &&
                                static_cast<size_t>(b) < s_ormBucketArrays.size()) {
                                glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
                                glBindTexture(GL_TEXTURE_2D_ARRAY, s_ormBucketArrays[b]);
                                glActiveTexture(GL_TEXTURE0);
                            }
                            curBucket = b;
                        }
                    }
                } else if (i == s_alphaOffCmdCount) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOn);
                    enteredOnGroup = true;
                }

                // Per-draw uniform: u_drawIDBase = absolute sorted slot.
                // In multidraw: u_drawIDBase = group-start, gl_DrawID = within-group index.
                // In per-draw:  u_drawIDBase = i,           gl_DrawID = always 0.
                // Net shader index (gl_DrawID + u_drawIDBase) is identical in both paths.
                glUniform1i(s_locsCoalesce.drawIDBase, static_cast<GLint>(i));

                // firstIndex is uint32_t; multiply in uintptr_t space to avoid 32-bit overflow.
                glDrawElementsInstancedBaseVertexBaseInstance(
                    GL_TRIANGLES,
                    static_cast<GLsizei>(pkt.indexCount),
                    GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<uintptr_t>(pkt.firstIndex) * sizeof(uint32_t)),
                    static_cast<GLsizei>(instanceCount),
                    static_cast<GLint>(pkt.baseVertex),
                    baseInstance);

                if (s_v5TraceEnabled)
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] slot=%u type=%u inst=%u base_inst=%u draw_id_base=%u\n",
                        i, pkt.owningTypeID, instanceCount, static_cast<unsigned>(baseInstance), i);

                ++s_v5FrameDrawsIssued;
            }

            if (enteredOnGroup) {
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_texArrayOff);
            }

            {
                const GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    ++s_v5FrameGlErrors;
                    std::fprintf(stderr,
                        "[DRAW_PACKET_V5] event=gl_error frame=%u err=0x%x\n",
                        s_v5TotalFrameCount, static_cast<unsigned>(err));
                }
            }

            if ((s_v5TotalFrameCount % 600u) == 0u) {
                const int ok = (s_v5FrameSortedOob       == 0u &&
                                s_v5FramePacketOob        == 0u &&
                                s_v5FrameTypeOob          == 0u &&
                                s_v5FrameBaseInstMissing  == 0u &&
                                s_v5FrameGlErrors         == 0u) ? 1 : 0;
                std::fprintf(stderr,
                    "[DRAW_PACKET_V5] frame=%u event=dispatch_summary"
                    " slots_considered=%u draws_issued=%u zero_instance_skips=%u"
                    " sorted_oob=%u packet_oob=%u type_oob=%u base_instance_missing=%u"
                    " gl_errors=%u ok=%d\n",
                    s_v5TotalFrameCount,
                    s_alphaOffCmdCount + s_alphaOnCmdCount,
                    s_v5FrameDrawsIssued,
                    s_v5FrameZeroInstSkips,
                    s_v5FrameSortedOob,
                    s_v5FramePacketOob,
                    s_v5FrameTypeOob,
                    s_v5FrameBaseInstMissing,
                    s_v5FrameGlErrors,
                    ok);
            }

}

// Unbind the indirect buffer and ORM unit, restore the saved bindings and latch the first flush.
void GpuStaticPropBatcher::finishCoalesceDraw(StaticPropFlushFrame& f) {
    auto& guardSsbo4 = f.guardSsbo4; auto& guardTex2DArray = f.guardTex2DArray;
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

        // STATICPROP-MATERIAL-ORM-1: unbind the ORM unit and restore active unit 0
        // so we leave GL state exactly as the gate-OFF path does. Only touch the
        // ORM unit when the gate is ON (no extra state churn when OFF).
        if (s_ormSlotsEnabled) {
            glActiveTexture(GL_TEXTURE0 + kOrmTexUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            glActiveTexture(GL_TEXTURE0);
        }

        // 11.7.j — restore slot 4 + unit-0 GL_TEXTURE_BINDING_2D_ARRAY via the
        // guards' explicit .reset() (SSBO first, then texture, matching the
        // old bind-order). Active texture stays GL_TEXTURE0 (guard restores
        // it). Slot 15 restore lives at the Step 10.3 envelope in
        // compute_dispatch().
        guardSsbo4.reset();
        guardTex2DArray.reset();

        // 11.7.k — first-flush latch (per-mission). event=ready is logged
        // here, not at finalize, because finalize emits event=armed and
        // we want a separate ready signal that confirms the GPU actually
        // accepted the coalesce draw without disarm.
        if (!s_coalesceFirstFlushDone) {
            MC2_DIAGFE(stderr,
                "[COALESCE v1] event=ready buckets_off=%u buckets_on=%u "
                "cmds_off=%u cmds_on=%u\n",
                s_alphaOffCount, s_alphaOnCount,
                s_alphaOffCmdCount, s_alphaOnCmdCount);
            s_coalesceFirstFlushDone = true;
        }
}

// Legacy path: sync the instance / color SSBOs and set up the program.
void GpuStaticPropBatcher::legacyPrepare() {
        // GPU-SYNC-CONTRACT: the legacy instance/color SSBOs were CPU-written this
        // frame through a persistent GL_MAP_COHERENT_BIT mapping (instMapBase /
        // colMapBase). The ring's glClientWaitSync handles cross-frame WAR reuse, but
        // ordering THIS frame's CPU writes BEFORE the draws reads them still needs
        // GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT. Without it, NVIDIA reads stale instance
        // data each running frame -> props invisible during gameplay, reappearing only
        // when paused (the per-frame rewrite stops). The legacy path is the ACTIVE one
        // on NVIDIA (coalesce disarms there), so this is the live fix. AMD tolerated it.
        gpuSyncBarrier(GpuProducer::CpuCoherentWrite, GpuConsumer::InstancedDraw,
                       "staticprop_legacy_instance");

        // v1: applyPipeline() above bound the coalesce program (if available).
        // Restore the legacy program before issuing legacy uniform uploads and draws.
        glUseProgram(s_staticPropProgram);

        // Direct uniforms for the legacy path. Static props use the same
        // CPU-composed terrainMVP as terrain/terrain_overlay.vert: axisSwap *
        // worldToClip, row-major rewritten in gamecam.cpp, uploaded GL_FALSE.
        {
            const GLint locTerrainMVP = glGetUniformLocation(s_staticPropProgram, "u_worldToClipGL");
            const float* terrainMVP = gos_GetObjectFixBMVPMat4();
            if (locTerrainMVP >= 0 && terrainMVP)
                glUniformMatrix4fv(locTerrainMVP, 1, GL_FALSE, terrainMVP);
            const GLint locMVP = glGetUniformLocation(s_staticPropProgram, "u_mvp");
            const float* mm = gos_GetProj2ScreenMat4();
            if (locMVP >= 0 && mm) glUniformMatrix4fv(locMVP, 1, GL_TRUE, mm);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_tex"),           0);
            glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode"), debugAddrMode_);
            // FIXME(task-10): no clean per-scene global fog scalar source
            // available for static props; per-instance fog color is on v_fog.
            // 1.0 == "clear" per shader convention. Revisit for distance fog.
            glUniform1f(glGetUniformLocation(s_staticPropProgram, "u_fogValue"),      1.0f);
            // V-AMBIENT-STATIC-1: hemisphere ambient strength (legacy program).
            // Same semantics as coalesce site: 0.0 default -> byte-identical OFF.
            if (s_locsLegacy.ambientV1Strength >= 0)
                glUniform1f(s_locsLegacy.ambientV1Strength,
                            staticPropAmbientV1Enabled() ? 1.0f : 0.0f);
            // PROP-BASE-LIGHT-DIR-1: same semantics as the coalesce site.
            // 0.0 default -> byte-identical OFF.
            if (s_locsLegacy.propBaseLightDirStrength >= 0)
                glUniform1f(s_locsLegacy.propBaseLightDirStrength,
                            s_propBaseLightDirStrength);
            // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor (legacy
            // program). Frag applies it only to kFlagDecalFill (bit 3) fragments.
            if (s_locsLegacy.terrainDecalFill >= 0)
                glUniform1f(s_locsLegacy.terrainDecalFill, g_terrainDecalFill);
            // TERRAIN-DECAL-COLORBLEND-1: colormap atlas + UV params + blend
            // (legacy program). Same flag-gated no-op contract as coalesce.
            if (s_locsLegacy.terrainDecalColorBlend >= 0)
                glUniform1f(s_locsLegacy.terrainDecalColorBlend, g_terrainDecalColorBlend);
            // BUILDING-GLASS-1: dither-glass translucency toggle (legacy program).
            if (s_locsLegacy.glassDither >= 0)
                glUniform1f(s_locsLegacy.glassDither,
                            s_glassDitherEnabled ? 1.0f : 0.0f);
            if (g_terrainDecalColorBlend > 0.0f) {
                extern GLuint gos_terrain_indirect_getAtlasGLTex();
                extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
                extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
                extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
                glActiveTexture(GL_TEXTURE0 + kColormapTexUnit);
                glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getAtlasGLTex());
                glActiveTexture(GL_TEXTURE0);
                if (s_locsLegacy.terrainColormap >= 0)
                    glUniform1i(s_locsLegacy.terrainColormap, static_cast<GLint>(kColormapTexUnit));
                if (s_locsLegacy.decalAtlasTLX >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
                if (s_locsLegacy.decalAtlasTLY >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
                if (s_locsLegacy.decalAtlasOOW >= 0)
                    glUniform1f(s_locsLegacy.decalAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
            }
            // V-MATERIAL-DEBUG-1: per-frag material debug view (legacy program).
            // Default 0 = OFF; shader skips entire debug branch when 0.
            if (s_locsLegacy.debugMaterialMode >= 0)
                glUniform1i(s_locsLegacy.debugMaterialMode,
                            s_staticPropDebugMaterialMode);
            // LIGHTING-DEBUG-VIEWS-1B: unified lighting debug channel (legacy
            // program). Resolver -1 -> upload 0 -> pixel-invariant default.
            {
                extern int mc2LightingDebugMode();
                int lvm = mc2LightingDebugMode();
                glUniform1i(glGetUniformLocation(s_staticPropProgram, "u_lightingDebugView"),
                            lvm < 0 ? 0 : lvm);
            }
            // MC2_SHADER_PATH_TINT: solid-color path-id debug (default 0 = OFF).
            if (s_locsLegacy.pathTint >= 0)
                glUniform1i(s_locsLegacy.pathTint, mc2ShaderPathTint());
            // GREYBEARD-DIFFUSE-TEST: upload raw MC2 terrain sun for forced diffuse.
            if (s_locsLegacy.terrainSunMC2 >= 0) {
                extern void gos_GetTerrainLightDir(float*, float*, float*);
                float tsx = 0.0f, tsy = 0.0f, tsz = 1.0f;
                gos_GetTerrainLightDir(&tsx, &tsy, &tsz);
                glUniform3f(s_locsLegacy.terrainSunMC2, tsx, tsy, tsz);
            }
            // EDGE-LIGHT-EMISSION-1: stock hot-colour NIGHT tag state. Gate OFF ->
            // 0/0/0 uploaded -> get_base_light takes its daytime arms -> render is
            // byte-identical to the pre-fix build.
            if (s_locsLegacy.isNight >= 0 || s_locsLegacy.nightFactor >= 0) {
                bool  nIsNight = false;
                float nFactor  = 0.0f;
                // BEACON-FIXTURE-ID-1: the fixture gate also needs real night state.
                // Safe for non-fixtures: their scale stays 0.0, so get_base_light
                // keeps its daytime result (delta * 0 == 0).
                if (s_nightTagMode != 0 || s_fixtureLightsScale > 0.0f ||
                    s_fixtureGlowStrength > 0.0f) {
                    extern void gos_GetNightState(bool*, float*);
                    gos_GetNightState(&nIsNight, &nFactor);
                }
                if (s_locsLegacy.isNight >= 0)
                    glUniform1i(s_locsLegacy.isNight, nIsNight ? 1 : 0);
                if (s_locsLegacy.nightFactor >= 0)
                    glUniform1f(s_locsLegacy.nightFactor, nFactor);
                if (s_locsLegacy.nightEmissiveScale >= 0)
                    glUniform1f(s_locsLegacy.nightEmissiveScale,
                                (s_nightTagMode != 0) ? s_nightTagScale : 0.0f);
                if (s_locsLegacy.fixtureEmissiveScale >= 0)
                    glUniform1f(s_locsLegacy.fixtureEmissiveScale, s_fixtureLightsScale);
                // FIXTURE-GLOW-1: 0.0 -> v_fixtureGlow is vec3(0.0) -> byte-identical.
                if (s_locsLegacy.fixtureGlowStrength >= 0)
                    glUniform1f(s_locsLegacy.fixtureGlowStrength, s_fixtureGlowStrength);
            }
            // FAR-BACKDROP-LIT: upload gate + distance band + tint (legacy).
            // Mirror of the coalesce block above; OFF -> u_farBackdropLit=0 ->
            // byte-identical.
            if (s_locsLegacy.farBackdropLit >= 0)
                glUniform1i(s_locsLegacy.farBackdropLit, s_farBackdropLitEnabled ? 1 : 0);
            if (s_locsLegacy.farBackdropNear >= 0)
                glUniform1f(s_locsLegacy.farBackdropNear, s_farBackdropNear);
            if (s_locsLegacy.farBackdropFar >= 0)
                glUniform1f(s_locsLegacy.farBackdropFar, s_farBackdropFar);
            if (s_locsLegacy.farBackdropStrength >= 0)
                glUniform1f(s_locsLegacy.farBackdropStrength, s_farBackdropStrength);
            if (s_locsLegacy.farBackdropTint >= 0)
                glUniform3f(s_locsLegacy.farBackdropTint,
                            s_farBackdropTint[0], s_farBackdropTint[1], s_farBackdropTint[2]);
            // POOL-CONSUMERS-1 (batch-6 item 6): night light-pool consumption
            // (legacy program path; same gating as the coalesce site).
            gos_terrain_lighting::UploadObjectLightPoolUniforms(s_staticPropProgram);
            // V-IBL-STATIC-1: SH-L2 image-based ambient (legacy program). Same
            // semantics as coalesce site: strength 0.0 default -> byte-identical OFF.
            // V-IBL-STATIC-2: same per-mission source as coalesce site.
            // IBL-SUN-ROTATE-1: same accessor as the coalesce site; gate OFF
            // returns the unchanged shipped-table pointer.
            if (s_locsLegacy.iblSh >= 0)
                glUniform3fv(s_locsLegacy.iblSh, 9,
                             iblShCoeffsForUpload());
            if (s_locsLegacy.iblShStrength >= 0)
                glUniform1f(s_locsLegacy.iblShStrength,
                            iblShStrengthNightScaled());
            // V-MATERIAL-PBR-2: legacy program upload. Same gating as coalesce
            // site (env + ViewUniforms-disabled interlock). Default-OFF is
            // upload 0.0f -> shader short-circuit.
            if (s_locsLegacy.pbrV1Strength >= 0) {
                const float pbrStrength =
                    (pbrV1Enabled() && !s_viewUniformsDisabled)
                        ? g_pbrV1Strength : 0.0f;
                glUniform1f(s_locsLegacy.pbrV1Strength, pbrStrength);
            }
            // V-MATERIAL-PBR-2-TUNE-UI: legacy program override upload (mirror).
            if (s_locsLegacy.pbrV1RoughnessOverride >= 0) {
                const float pbrRough =
                    (g_pbrV1RoughnessOverrideEnabled && !s_viewUniformsDisabled)
                        ? g_pbrV1RoughnessOverrideValue : -1.0f;
                glUniform1f(s_locsLegacy.pbrV1RoughnessOverride, pbrRough);
            }
            // V-MATERIAL-PBR-2-DIAG: legacy program diag visualizer upload.
            if (s_locsLegacy.pbrV1DiagSunFound >= 0) {
                glUniform1i(s_locsLegacy.pbrV1DiagSunFound,
                            s_pbrV1DiagSunFound ? 1 : 0);
            }
        }
}

// One-shot per-type draw census.
void GpuStaticPropBatcher::legacyCensus() {
    // [SEAMPROBE] stage 8: one-shot per-type draw census (which types have a
    // range / instances / packets at first legacy draw). Gated on MC2_MODOVERRIDE_TRACE.
    {
        static bool s_seamDrawCensus = false;
        if (!s_seamDrawCensus && mc2::gate::get<"MC2_MODOVERRIDE_TRACE">()) {
            s_seamDrawCensus = true;
            for (uint32_t tid = 0; tid < s_types.size(); ++tid) {
                auto rit2 = s_typeRanges.find(tid);
                bool hasRange = (rit2 != s_typeRanges.end());
                uint32_t ic = hasRange ? rit2->second.instanceCount : 0;
                const GpuStaticPropType& ty = s_types[tid];
                if (ty.packetCount > 0 && (!hasRange || ic == 0)) {
                    fprintf(stderr, "[SEAMPROBE] draw-census typeID=%u src=%p packetCount=%u vertexCount=%u hasRange=%d instanceCount=%u (NOT DRAWN)\n",
                        tid, (void*)ty.source, ty.packetCount, ty.vertexCount, (int)hasRange, ic);
                }
            }
            fflush(stderr);
        }
    }
}

// Fence the ring slots and hand the parity slot to the dual-emit state.
void GpuStaticPropBatcher::fenceAndParitySlot() {
    s_fence[s_frameSlot] = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // Plan v3.8 Step 11.9 — coalesce fence insert after all draws issued.
    // Independent of legacy s_fence[]: tracks s_coalesceInstanceSsbo's
    // ring-slot lifecycle so the next visit (RING_FRAMES frames from now)
    // can wait on this fence before overwriting the slot's CPU writes.
    if (IsCoalesceEnabled()) {
        // Step 4.4 — use s_coalesceFrameSlot (coalesce ring), not s_frameSlot (legacy ring).
        s_coalesceFence[s_coalesceFrameSlot] = MC2_GL_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    // Stage 2.D.1: record this slot's parity-byte usage so the next visit
    // (RING_FRAMES frames from now) knows exactly how much to glGetBufferSubData.
    s_parityBytesUsedPerSlot[s_frameSlot] = s_parityBytesUsedThisFrame;
    // Stage 2.D.2: after the fence is inserted (GPU draw committed), advance
    // the dual-emit latch from Armed → WaitingForReadback. Pass s_frameSlot
    // so the compare only fires when the ring revisits this exact slot (after
    // glClientWaitSync ensures GPU writes are complete — RING_FRAMES later).
    if (gos_object_parity::IsDualEmitArmedAnyActor()) {
        gos_object_parity::AdvanceDualEmitToWaiting(s_frameSlot);
    }
}

// Restore the GL state saved at flush start.
void GpuStaticPropBatcher::releaseGlGuards(StaticPropFlushFrame& f) {
    auto& guardProgram = f.guardProgram; auto& guardVao = f.guardVao; auto& guardArrayBuf = f.guardArrayBuf; auto& guardElemBuf = f.guardElemBuf;
    auto& guardTexUnit0 = f.guardTexUnit0; auto& guardSsbo0 = f.guardSsbo0; auto& guardSsbo1 = f.guardSsbo1; auto& guardSsbo2 = f.guardSsbo2;
    auto& guardSsbo3 = f.guardSsbo3; auto& guardSsbo5 = f.guardSsbo5;
    // Restore GL state to EXACTLY what it was at flush start.
    // GLSTATE-GUARD-SITE5: explicit .reset() in the same order the hand-
    // rolled epilogue used to restore (SSBOs, then texture unit 0, then
    // program/VAO/buffer bindings) — must close here, BEFORE
    // gos_InvalidateRenderStateCache() below, not at this optional's natural
    // (function-end) scope exit. See the capture-site comment above.
    guardSsbo0.reset();
    guardSsbo1.reset();
    guardSsbo2.reset();
    guardSsbo3.reset();
    guardSsbo5.reset();
    guardTexUnit0.reset();
    guardVao.reset();
    guardArrayBuf.reset();
    guardElemBuf.reset();
    guardProgram.reset();
}

// Accumulate the counters, tick parity, clear the frame buckets and invalidate the render-state cache.
void GpuStaticPropBatcher::finishFlush() {
    // Pipeline state: no explicit restore needed — applyPipeline() set state
    // above, and gos_InvalidateRenderStateCache() below ensures the next
    // applyRenderStates() call re-applies from scratch regardless of what
    // applyPipeline left.
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/false);

    // Slice 2 (object-offload) — Stage 2.D.1: tick parity counters and emit
    // the 600-frame summary line if the cadence is hit. Internally a no-op
    // when MC2_OBJECT_PARITY_CHECK is unset.
    gos_object_parity::ParityFrameTick();

    //
    // Called AFTER ParityFrameTick so the summary's parity_compared_actors
    // sees the just-finalized count, and AFTER AdvanceDualEmitToWaiting so
    // the state machine has progressed for this frame. PickNextSampleIfReady:
    //   - rotates this frame's submitMultiShape observation list into the
    //     prior-frame pool (always),
    //   - if state == Done AND the pool is non-empty, picks one shape via
    //     round-robin and re-arms (state=Armed, armMode=SampledOnly,
    //     s_currentSampledShape=picked).
    // Default-off inside the sidecar; safe to call unconditionally.
    gos_object_parity::PickNextSampleIfReady();

    s_bucketsByType.clear();
    s_reinjectedThisFrame = false;
    s_lastUploadedSlot = 0xFFFFFFFFu;  // reset for next frame

    // RENDER_STATES v1: this path mutates GL_TEXTURE_WRAP_S/T (lines 1700-1701
    // above) on the bound texture object — persistent state that survives
    // function exit and is not tracked by applyRenderStates' cache. Without
    // invalidation, a subsequent applyRenderStates call would early-out on
    // matching tracked-slot values while wrap stays at REPEAT even when
    // gos_State_TextureAddress=Clamp was requested. CRITICAL-1 from the
    // 2026-05-08 adversarial review of the state-equality early-out.
    gos_InvalidateRenderStateCache();

    // v2.1: snapshot per-type instance counts for next-frame extraction.
    // s_typeRanges is module-static and holds this frame's counts.
    // Reading here (before s_bucketsByType.clear) gives prior-frame
    // semantics to batcher_getDrawSlotEntry() called from ExtractRenderSnapshot().
    s_typeInstanceCountPrevFrame.clear();
    for (const auto& kv : s_typeRanges)
        s_typeInstanceCountPrevFrame[kv.first] = kv.second.instanceCount;
}
