// GpuStaticPropBatcher: eligibility, persistent static instances, cached-instance submits, the batcher_* RenderCore seam accessors and the base-instance table.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"
#include "../../RenderCore/StaticPropOrigin.h"

// ---------------------------------------------------------------------------
// Slice 2, Stage 2.A — new method and free-function definitions
// ---------------------------------------------------------------------------

// GpuStaticPropBatcher::wasLastFailureLateRegistration (lines added below)
// Returns whether the most recent submitMultiShape() call returned false
// because of a late-registration miss. The flag s_lastSubmitWasLateReg is
// cleared at the TOP of every submitMultiShape() call (before any checks)
// so the window of confusion is exactly one submit.
bool GpuStaticPropBatcher::wasLastFailureLateRegistration() const {
    return s_lastSubmitWasLateReg;
}

// Slice 2 (Stage 2.D.3) — late-registration event-counter queries.
// Static methods so diagnostics can call them without a singleton
// hop. The counters live in the file-static block above; they are
// monotonic since process start.
uint64_t GpuStaticPropBatcher::getAllowedLateRegEventCount() {
    return s_lateReg_allowed_events;
}
uint64_t GpuStaticPropBatcher::getDisallowedLateRegEventCount() {
    return s_lateReg_disallowed_events;
}

// GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects
// Cheap, side-effect-free eligibility check used by the addRenderShape gate
// (tgl.cpp:2522 Stage 2.B) and by *Appearance::update (Stage 2.B) before
// calling submitMultiShape. Returns true iff submitMultiShape's pre-submit gates pass:
// geometry is finalized, registration has not failed fatally, and every SHAPE_NODE
// leaf under `multi` is registered in s_typeIndex.
// MUST NOT mutate any batcher state, log, or increment counters.
bool GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects(
        const TG_MultiShape* multi) const {
    if (!multi || !multi->usesTypeMaterials() || !s_geometryFinalized || s_fatalRegistrationFailure) return false;

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) return false;

    bool sawShapeNode = false;
    for (int i = 0; i < n; ++i) {
        const TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        const TG_Shape* child = rec.node.get();
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;  // skip helpers/bones
        if (!child->matchesTypeMaterials()) return false;
        sawShapeNode = true;
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(child->myType);
        if (s_typeIndex.find(ts->stableId()) == s_typeIndex.end()) return false;
    }
    return sawShapeNode;
}

void gos_GpuPropsCycleDebugMode() {
    auto& b = GpuStaticPropBatcher::instance();
    // 0..7 are the legacy bisection modes; 8 is the LODBUG probe (solid
    // magenta with alpha-test discard bypassed — see static_prop.frag).
    int next = (b.getDebugAddrMode() + 1) % 9;
    b.setDebugAddrMode(next);
}
int gos_GpuPropsGetDebugMode() {
    return GpuStaticPropBatcher::instance().getDebugAddrMode();
}

const std::vector<GpuStaticPropInstance>& GpuStaticPropBatcher::getLastBuiltBatch() const {
    return s_lastBuiltBatch;
}

void GpuStaticPropBatcher::submitCachedInstance(const GpuStaticPropInstance& inst) {
    if (inst.typeID >= s_types.size()) return;
    PerTypeBucket& bucket = s_bucketsByType[inst.typeID];

    // Diagnostic counters (submitted_instances_by_pop etc.) are NOT incremented
    // for registry-injected instances — they measure the dynamic compute path.
    GpuStaticPropInstance updated = inst;
    // Preserve the recipe's light-independent actor tint during replay.
    {
        // MC2_TREE_DIAG_TRACE diagnostic. Same format as submit() so a grep
        // diff (`grep "[TREE_DIAG]" log | sort`) lines up dyn-vs-static fields.
        // COUNTER-HONESTY-1: SAMPLE, not a census — see the submit() mirror.
        static int      s_staticPrinted = 0;
        static uint64_t s_staticSeen    = 0;
        if (s_treeDiagTrace) ++s_staticSeen;
        if (s_treeDiagTrace && s_staticPrinted == kTreeDiagSample) {
            ++s_staticPrinted;
            TREE_DIAG("path=stat sample=first%d SAMPLE-CAP-REACHED further path=stat records suppressed; "
                      "this stream is a SAMPLE not a census -- line count != replay count; "
                      "and it covers ALL prop populations, not just trees. NOTE: this path "
                      "carries no pop= -- submitCachedInstance replays a registry recipe and "
                      "is not told the population; do not infer one from the [TREE_DIAG] name",
                      kTreeDiagSample);
        }
        if (s_treeDiagTrace && s_staticPrinted < kTreeDiagSample) {
            ++s_staticPrinted;
            TREE_DIAG("path=stat sample=first%d n=%llu typeID=%u flags=0x%x lightIdx=%u colOff=%u tx=%.2f ty=%.2f tz=%.2f fog=%.3f,%.3f,%.3f,%.3f hi=%.3f,%.3f,%.3f,%.3f bucketInsts=%zu",
                kTreeDiagSample, static_cast<unsigned long long>(s_staticSeen),
                updated.typeID, updated.flags, updated.lightDataIndex, updated.actorTint,
                // Same offset fix as submit() — translation at [3]/[7]/[11], not [12..14].
                updated.modelMatrix[3], updated.modelMatrix[7], updated.modelMatrix[11],
                updated.fogRGB[0], updated.fogRGB[1], updated.fogRGB[2], updated.fogRGB[3],
                updated.aRGBHighlight[0], updated.aRGBHighlight[1], updated.aRGBHighlight[2], updated.aRGBHighlight[3],
                bucket.instances.size());
        }
    }
    bucket.instances.push_back(updated);
}

uint64_t GpuStaticPropBatcher::persistentStaticGen() { return s_persistentStaticGen; }
void GpuStaticPropBatcher::setPersistentStaticGen(uint64_t g) { s_persistentStaticGen = g; }

uint32_t gos_ProbeStaticInstanceCount() {
    uint32_t n = 0; for (auto& [typeKey, insts] : s_persistentStaticStore) n += static_cast<uint32_t>(insts.size());
    return n;
}
bool gos_ProbeStaticInstanceWorld(uint32_t idx, float outXYZ[3]) {
    for (auto& [typeID, v] : s_persistentStaticStore) {
        if (idx < v.size()) {
            const float* m = v[idx].modelMatrix;
            const StaticPropOrigin o = staticPropOriginRender(m);
            outXYZ[0] = o.x; outXYZ[1] = o.y; outXYZ[2] = o.z;
            return true;
        }
        idx -= static_cast<uint32_t>(v.size());
    }
    return false;
}

void GpuStaticPropBatcher::clearPersistentStatic() {
    // Keep the map + per-type vector capacity; just reset sizes (rebuild refills).
    for (auto& [typeKey, insts] : s_persistentStaticStore) insts.clear();
    s_persistentStaticGen = 0xFFFFFFFFFFFFFFFFull;  // invalid until the rebuild sets it
}

// Append a contiguous run of recipes (one registry range) into the persistent
// static store, grouped by typeID. Called ONLY on a dirty generation (rare), so
// per-element push_back is fine. Actor tint is stored verbatim with the recipe.
void GpuStaticPropBatcher::appendPersistentStaticRange(const GpuStaticPropInstance* arr,
                                                       uint32_t count) {
    if (!arr || count == 0u) return;
    uint32_t i = 0u;
    while (i < count) {
        const uint32_t typeID = arr[i].typeID;
        if (typeID >= s_types.size()) { ++i; continue; }
        uint32_t j = i;
        while (j < count && arr[j].typeID == typeID) ++j;
        auto& store = s_persistentStaticStore[typeID];
        // NOTE: do NOT reserve(size + n) here — for single-leaf runs that sets
        // capacity to EXACTLY size+1 per call, defeating geometric growth (the 2a
        // reserve-O(N^2) regression). push_back grows geometrically; dirty-only.
        for (uint32_t k = i; k < j; ++k) store.push_back(arr[k]);
        i = j;
    }
}


bool GpuStaticPropBatcher::buildRecipeFromShape(
        TG_Shape* shape,
        const glm::mat4& shapeToWorld,
        uint32_t highlightARGB,
        uint32_t fogARGB,
        uint32_t flags,
        GpuStaticPropInstance* outRecipe) const {
    if (!shape || !outRecipe) return false;

    TG_TypeShape* typeShape = static_cast<TG_TypeShape*>(shape->myType);
    if (!typeShape) return false;

    auto it = s_typeIndex.find(typeShape->stableId());
    if (it == s_typeIndex.end()) return false;

    GpuStaticPropInstance inst{};
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    const auto& [typeKey, regTypeId] = *it;
    inst.typeID           = regTypeId;
    inst.actorTint = 0x0002ffffu;        // kNeutral|I=1 until the appearance supplies its light word
    inst.flags            = flags;
    inst.lightDataIndex   = 0xFFFFFFFFu; // sentinel; flush() patches per-frame
    inst.aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    inst.aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    inst.aRGBHighlight[2] = ((highlightARGB >>  0) & 0xFF) / 255.0f;
    inst.aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    inst.fogRGB[0] = ((fogARGB >> 16) & 0xFF) / 255.0f;
    inst.fogRGB[1] = ((fogARGB >>  8) & 0xFF) / 255.0f;
    inst.fogRGB[2] = ((fogARGB >>  0) & 0xFF) / 255.0f;
    inst.fogRGB[3] = ((fogARGB >> 24) & 0xFF) / 255.0f;

    *outRecipe = inst;
    return true;
}

// ---------------------------------------------------------------------------
// C1b free-function accessors (Track C slice C1b).
// Expose immutable per-type geometry for gpu_cull_compute.cpp's indirect buffer
// construction. Valid only after finalizeGeometry().
// ---------------------------------------------------------------------------

uint32_t batcher_getTypeCount() {
    return static_cast<uint32_t>(s_types.size());
}

// [PLACE_TYPE_TEST] DIAG: is this leaf TG_TypeShape* registered (in the finalized
// VBO via s_typeIndex), and/or did it hit the late-register drop (s_failedTypes)?
// Lets the placement site report whether the EXACT shape it draws was primed.
// EDITOR-STATIC-RECIPE-FROM-TYPE-1: resolve a leaf TG_TypeShape* to its typeID
// in the finalized type table. Lets the editor re-point a runtime placement's
// recipe at the canonical (primed, geometry-bearing) TYPE leaf instead of the
// post-finalize CreateFrom instance copy. Returns false if not registered.
bool batcher_typeIdForTypeShape(const void* typeShape, uint32_t* outTypeId) {
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(typeShape);
    if (!ts) return false;
    auto it = s_typeIndex.find(ts->stableId());
    if (it == s_typeIndex.end()) return false;
    const auto& [typeKey, regTypeId] = *it;
    if (outTypeId) *outTypeId = regTypeId;
    return true;
}

void batcher_queryTypeShape(const void* typeShape, int* outRegistered, int* outFailed) {
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(typeShape);
    if (outRegistered) *outRegistered = (ts && s_typeIndex.count(ts->stableId())) ? 1 : 0;
    if (outFailed) {
        auto it = ts ? s_failedTypes.find(ts->stableId()) : s_failedTypes.end();
        bool failed = false;
        if (it != s_failedTypes.end()) {
            const auto& [failKey, failFlag] = *it;
            failed = failFlag;
        }
        *outFailed = failed ? 1 : 0;
    }
}

bool batcher_getTypeDrawInfo(uint32_t  typeID,
                              uint32_t* outIndexCount,
                              uint32_t* outFirstIndex,
                              int32_t*  outBaseVertex,
                              uint32_t* outInstanceCap) {
    if (!s_geometryFinalized) return false;
    if (typeID >= s_types.size()) return false;

    const GpuStaticPropType& type = s_types[typeID];

    // Sum index count across all packets for this type (for correct
    // glMultiDrawElementsIndirect — each bucket covers all packets).
    // For C1b's first-pass implementation we use a single draw command
    // per type covering ALL packets as one contiguous draw. This works
    // when all packets for a type are contiguous in the IBO, which is
    // guaranteed by registerType()'s sequential append.
    uint32_t totalIndexCount = 0;
    uint32_t firstPktFirstIndex = 0;
    int32_t  firstPktBaseVertex = 0;
    for (uint32_t p = 0; p < type.packetCount; ++p) {
        const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
        totalIndexCount += pkt.indexCount;
        if (p == 0) {
            firstPktFirstIndex = pkt.firstIndex;
            firstPktBaseVertex = pkt.baseVertex;
        }
    }

    if (outIndexCount)  *outIndexCount  = totalIndexCount;
    if (outFirstIndex)  *outFirstIndex  = firstPktFirstIndex;
    if (outBaseVertex)  *outBaseVertex  = firstPktBaseVertex;
    // Per-bucket capacity: s_instanceCapacity is the per-frame total;
    // use it as the upper bound for each bucket (conservative — sums to
    // N*s_instanceCapacity for the visibleIds[] buffer, which is large but
    // safe; actual peak is at most s_instanceCapacity total).
    if (outInstanceCap) *outInstanceCap = static_cast<uint32_t>(s_instanceCapacity > 0
                                            ? s_instanceCapacity : INITIAL_INSTANCES_PER_FRAME);
    return true;
}

// ---------------------------------------------------------------------------
// §5.6b accessor bodies (plan v3.8 Step 1.2a — deferred from Step group 1
// because the bodies reference Step 2.1 file-scope statics).
// All return safe sentinels (0 / nullptr / empty / false) before
// finalizeGeometry() runs so a caller-side null-deref or stale-handle bind
// is impossible during init ordering.
// ---------------------------------------------------------------------------

const uint32_t* batcher_getSortedTypeOrder() {
    return (s_geometryFinalized && !s_sortedTypeOrder.empty())
           ? s_sortedTypeOrder.data()
           : nullptr;
}

uint32_t batcher_getAlphaOffCount() { return s_alphaOffCount; }
uint32_t batcher_getAlphaOnCount()  { return s_alphaOnCount;  }

const uint32_t* batcher_getSortedPacketOrder() {
    return (s_geometryFinalized && !s_sortedPacketOrder.empty())
           ? s_sortedPacketOrder.data()
           : nullptr;
}
uint32_t batcher_getSortedPacketCount() {
    return static_cast<uint32_t>(s_sortedPacketOrder.size());
}
uint32_t batcher_getAlphaOffCmdCount() { return s_alphaOffCmdCount; }
uint32_t batcher_getAlphaOnCmdCount()  { return s_alphaOnCmdCount;  }

bool batcher_getPacketDrawInfo(uint32_t globalPacketIdx,
                                uint32_t* outIndexCount,
                                uint32_t* outFirstIndex,
                                int32_t*  outBaseVertex,
                                uint32_t* outOwningTypeID) {
    if (!s_geometryFinalized) return false;
    if (globalPacketIdx >= s_packets.size()) return false;
    const auto& pkt = s_packets[globalPacketIdx];
    if (outIndexCount)   *outIndexCount   = pkt.indexCount;
    if (outFirstIndex)   *outFirstIndex   = pkt.firstIndex;
    if (outBaseVertex)   *outBaseVertex   = pkt.baseVertex;
    if (outOwningTypeID) *outOwningTypeID = pkt.owningTypeID;
    return true;
}

bool batcher_getPacketMaterialFlags(uint32_t globalPacketIdx,
                                     uint32_t* outMaterialFlags) {
    if (!s_geometryFinalized) return false;
    if (globalPacketIdx >= s_packets.size()) return false;
    if (outMaterialFlags) *outMaterialFlags = s_packets[globalPacketIdx].materialFlags;
    return true;
}

uint32_t batcher_getInstanceCap(uint32_t typeID) {
    if (!s_geometryFinalized || typeID >= s_types.size()) return 0;
    return s_types[typeID].instanceCap;
}

// Slice-A cut-off upper-bound oracle: per-type LIVE instance count from the
// last upload (= the frozen draw-pool span the indirect draw renders from).
// s_typeRanges is rebuilt every upload (uploadAllBucketsIfNeeded :4538) with
// instanceCount = bucket.instances.size(); reading it gives an authoritative,
// timing-stable denominator (no per-frame bucket clear race). For frozen static
// props it equals this frame's pool span.
uint32_t batcher_getTypeUploadedInstanceCount(uint32_t typeID) {
    auto it = s_typeRanges.find(typeID);
    if (it == s_typeRanges.end()) return 0u;
    const auto& [rangeKey, range] = *it;
    return range.instanceCount;
}

// M1 FROZEN-STATIC-CULL-RECORDS: module-scope copy of the per-type global
// instance-pool base (alpha-group prefix-sum over s_sortedTypeOrder), published
// by batcher_prepareBaseInstanceTable(). global_slot(typeID, store_rank) ==
// s_baseInstanceForType[typeID] + store_rank, the exact binding-0 slot the draw
// indexes. Valid only after prepareBaseInstanceTable() runs in global-pool armed
// mode (empty under legacy/disarmed). The golden static cull-record build reads
// it to place each record at its instance-pool slot (record-index == pool-slot).
static std::vector<uint32_t> s_baseInstanceForType;

// Static-only per-type base (s_staticBaseInstanceForType, defined early near
// s_globalInstanceCap): prefix-sum over s_persistentStaticStore[t].size() in the
// SAME sorted+alpha order as s_baseInstanceForType, but counting ONLY persistent-
// static instances (no dynamics). The stable base the golden scatter must use.
// Dirty-gated on s_persistentStaticGen via s_staticBaseBuiltGen.
static void rebuildStaticBaseInstanceTableIfDirty() {
    if (s_staticBaseBuiltGen == s_persistentStaticGen) return;  // dirty-gated
    s_staticBaseInstanceForType.assign(s_types.size(), 0u);
    std::array<uint32_t, 2> groupCursor = {0u, 0u};
    for (unsigned int typeID : s_sortedTypeOrder) {
        if (typeID >= s_types.size()) continue;
        auto it = s_persistentStaticStore.find(typeID);
        uint32_t cnt = 0u;
        if (it != s_persistentStaticStore.end()) {
            const auto& [storeKey, storeInsts] = *it;
            cnt = static_cast<uint32_t>(storeInsts.size());
        }
        const uint32_t group = s_types[typeID].alphaClass;
        s_staticBaseInstanceForType[typeID] = groupCursor[group];
        groupCursor[group] += cnt;
    }
    const uint32_t offGroupCount = groupCursor[0];
    for (uint32_t typeID : s_sortedTypeOrder) {
        if (typeID < s_types.size() && s_types[typeID].alphaClass == 1u)
            s_staticBaseInstanceForType[typeID] += offGroupCount;
    }
    s_staticBaseBuiltGen = s_persistentStaticGen;
}

uint32_t batcher_getStaticBaseInstanceForType(uint32_t typeID) {
    return (typeID < s_staticBaseInstanceForType.size())
               ? s_staticBaseInstanceForType[typeID] : 0u;
}

// Task 3: build the StaticPopulation indirect cmds, one per sorted packet (same
// [alpha-OFF | alpha-ON] order as s_sortedPacketOrder, so the Task-4 draw splits
// at s_alphaOffCmdCount exactly like the dynamic draw). count/firstIndex/
// baseVertex are geometry-static; baseInstance = the type's STABLE static-only
// base; instanceCount = the type's full static population (first-N draw — NOT a
// GPU cull count; visibleIds authority arrives in M2b). A packet whose type has
// 0 static instances gets instanceCount=0 -> harmless no-op sub-draw. Dirty-
// gated on s_persistentStaticGen. Built but NOT drawn until Task 4.

uint32_t batcher_getBaseInstanceForType(uint32_t typeID) {
    // M2a: under the population split, the golden scatter must use the STABLE
    // static-only base (dynamics no longer inflate it) -> outOfRange -> 0.
    if (staticPopSplitArmed())
        return batcher_getStaticBaseInstanceForType(typeID);
    return (typeID < s_baseInstanceForType.size()) ? s_baseInstanceForType[typeID] : 0u;
}

// True when the per-type base table is valid this frame (global-pool armed and
// prepareBaseInstanceTable has run). The golden build must gate on this.
bool batcher_isBaseInstanceTableReady() {
    return !s_globalPoolLegacy && batcher_isCoalesceArmed()
           && !s_baseInstanceForType.empty();
}

// COMPRESSION-BC7-STATICPROP-2: under the gate the two named handles are 0
// (the build produced per-bucket arrays instead). Fall back to the first
// alpha-OFF / first alpha-ON bucket array so the ImGui GBuffer preview still
// shows a representative array. Gate OFF: returns the original handles.

size_t batcher_getCoalescePerFrameInstanceBytes() {
    return s_coalescePerFrameInstanceBytes;
}

bool batcher_isCoalesceLayoutReady() { return s_coalesceLayoutReady; }
bool batcher_isCoalesceArmed()       { return s_coalesceArmed;       }

// MaterialGpu-3: look up entry from the per-mission material table.
// Returns false (and leaves *out untouched) when the gate is OFF, the table
// is empty, or index is out of range.
bool batcher_getMaterialGpuEntry(uint32_t index, RenderCore::MaterialGpu* out) {
    if (!s_materialGpuEnabled) return false;
    if (!out) return false;
    if (index >= static_cast<uint32_t>(s_materialGpuTable.size())) return false;
    *out = s_materialGpuTable[index];
    return true;
}

bool batcher_getPacketTexArrayLayer(uint32_t globalPacketIdx, int32_t* out) {
    if (!out) return false;
    *out = -1;
    if (globalPacketIdx >= static_cast<uint32_t>(s_packetTexArrayLayer.size()))
        return false;
    *out = s_packetTexArrayLayer[globalPacketIdx];
    return true;
}

// V-MATERIAL-STATIC-0: read-only inventory accessors.
uint32_t batcher_getStaticPropMaterialInventoryCount() {
    return static_cast<uint32_t>(s_materialInventory.size());
}

bool batcher_getStaticPropMaterialInventoryEntry(
        uint32_t idx, StaticPropMaterialInventoryEntry* out) {
    if (!out) return false;
    if (idx >= static_cast<uint32_t>(s_materialInventory.size())) return false;
    *out = s_materialInventory[idx];
    return true;
}

uint32_t batcher_getDrawSlotCount() {
    return static_cast<uint32_t>(s_sortedPacketOrder.size());
}

bool batcher_getDrawSlotEntry(uint32_t slot, ExtractedStaticPropPacket* out) {
    if (!out) return false;
    if (slot >= static_cast<uint32_t>(s_sortedPacketOrder.size())) return false;

    const uint32_t globalPktIdx = s_sortedPacketOrder[slot];
    if (globalPktIdx >= static_cast<uint32_t>(s_packets.size())) return false;

    out->sortedSlot      = slot;
    out->globalPacketIdx = globalPktIdx;
    out->typeId          = s_packets[globalPktIdx].owningTypeID;
    out->pipelineId      = static_cast<uint32_t>(
        slot < static_cast<uint32_t>(s_alphaOffCmdCount)
            ? RenderCore::PipelineId::StaticPropOpaque
            : RenderCore::PipelineId::StaticPropAlphaTest);

    // materialIdx from per-slot sidecar; sentinel if sidecar was not valid at finalize time
    if (s_materialGpuSidecarValid &&
        slot < static_cast<uint32_t>(s_packetMaterialIdx.size())) {
        out->materialIdx = s_packetMaterialIdx[slot];
    } else {
        out->materialIdx = 0xFFFFFFFFu;
    }

    // instanceCount from previous frame's flush() snapshot
    const auto it = s_typeInstanceCountPrevFrame.find(out->typeId);
    out->instanceCount = 0u;
    if (it != s_typeInstanceCountPrevFrame.end()) {
        const auto& [prevKey, prevCount] = *it;
        out->instanceCount = prevCount;
    }

    // texArrayLayer from per-packet sidecar (v2.2). Sentinel -1 if not available.
    {
        int32_t layer = -1;
        batcher_getPacketTexArrayLayer(globalPktIdx, &layer);
        out->texArrayLayer = layer;
    }

    return true;
}

void batcher_compareSnapshotPackets(RenderSnapshot* snap) {
    if (!snap) return;

    const uint32_t liveCount = batcher_getDrawSlotCount();
    snap->spCompareSnapshotCount = static_cast<uint32_t>(snap->staticPropPackets.count);
    snap->spCompareLiveCount     = liveCount;

    // Fill live_count even when snapshot data is null (e.g. arena overflow).
    if (!snap->staticPropPackets.data) {
        if (liveCount != 0u) snap->spCountMismatch = 1u;
        return;
    }

    if (snap->staticPropPackets.count != liveCount) {
        snap->spCountMismatch = 1u;
        return;  // per-slot compare is meaningless when counts diverge
    }

    const uint32_t n = liveCount;
    for (uint32_t i = 0u; i < n; ++i) {
        const ExtractedStaticPropPacket& row = snap->staticPropPackets.data[i];

        // sortedSlot must equal i (guaranteed by ok=1 invariant, but verify explicitly)
        if (row.sortedSlot != i) {
            ++snap->spSortedSlotMismatch;
        }

        // globalPacketIdx: three-guard pattern mirrors v6 builder guards.
        // Guard 1: i must be a valid index into s_sortedPacketOrder.
        if (i >= static_cast<uint32_t>(s_sortedPacketOrder.size())) {
            ++snap->spGlobalPacketMismatch;
            continue;
        }
        const uint32_t livePktIdx = s_sortedPacketOrder[i];
        // Guard 2: livePktIdx must be a valid index into s_packets.
        if (livePktIdx >= static_cast<uint32_t>(s_packets.size())) {
            ++snap->spGlobalPacketMismatch;
            continue;
        }
        if (row.globalPacketIdx != livePktIdx) {
            ++snap->spGlobalPacketMismatch;
        }

        // pipelineId must use real RenderCore::PipelineId enum values — NOT raw 0/1.
        const uint32_t expectedPipeline = static_cast<uint32_t>(
            i < s_alphaOffCmdCount
                ? RenderCore::PipelineId::StaticPropOpaque
                : RenderCore::PipelineId::StaticPropAlphaTest);
        if (row.pipelineId != expectedPipeline) {
            ++snap->spPipelineMismatch;
        }

        // materialIdx from sidecar (only check when sidecar was valid at finalize time)
        if (s_materialGpuSidecarValid &&
            i < static_cast<uint32_t>(s_packetMaterialIdx.size())) {
            if (row.materialIdx != s_packetMaterialIdx[i]) {
                ++snap->spMaterialIdxMismatch;
            }
        }

        // instanceCount: prev-frame snapshot vs current-frame s_typeRanges.
        // Informational only — different-frame authority by design; NOT in ok gate.
        {
            const auto typeIt = s_typeRanges.find(row.typeId);
            uint32_t currentInstCount = 0u;
            if (typeIt != s_typeRanges.end()) {
                const auto& [rangeKey, range] = *typeIt;
                currentInstCount = range.instanceCount;
            }
            if (row.instanceCount != currentInstCount) {
                ++snap->spInstanceCountMismatch;
            }
        }

        // texArrayLayer vs MaterialGpu.albedoTex cross-authority check.
        // Only compare when both materialIdx and texArrayLayer are valid.
        // A missing MaterialGpu entry when materialIdx is valid is itself a mismatch.
        if (row.materialIdx != 0xFFFFFFFFu && row.texArrayLayer != -1) {
            RenderCore::MaterialGpu mat{};
            if (!batcher_getMaterialGpuEntry(row.materialIdx, &mat)) {
                // materialIdx valid but entry missing — structural failure.
                ++snap->spTexLayerMismatch;
            } else if (mat.albedoTex != static_cast<uint32_t>(row.texArrayLayer)) {
                ++snap->spTexLayerMismatch;
            }
        }
    }
}

void batcher_getSnapshotBuildStats(uint32_t* attempted, uint32_t* countMismatch,
                                   uint32_t* packetMismatch, uint32_t* metaMismatch,
                                   uint32_t* fallback)
{
    if (attempted)      *attempted      = s_spBuildAttempted;
    if (countMismatch)  *countMismatch  = s_spBuildCountMismatch;
    if (packetMismatch) *packetMismatch = s_spBuildPacketMismatch;
    if (metaMismatch)   *metaMismatch   = s_spBuildMetaMismatch;
    if (fallback)       *fallback       = s_spBuildFallback;
}

uint32_t batcher_getPerTypePeakCount(uint32_t typeID) {
    return typeID < s_perTypePeak.size() ? s_perTypePeak[typeID] : 0u;
}

bool batcher_isGlobalPoolLegacy() { return s_globalPoolLegacy; }
uint32_t batcher_getGlobalInstanceCap() { return s_globalInstanceCap; }
uint32_t batcher_getCoalesceFrameSlot() { return s_coalesceFrameSlot; }  // diagnostic

void batcher_getStaticPropOpaqueDebugState(StaticPropOpaqueDebugState* out) {
    if (!out) return;
    out->snapshotDispatchDefault     = true;
    out->legacyDispatch              = false;
    out->materialGpuEnabled          = s_materialGpuEnabled;
    out->materialGpuSample           = s_materialGpuSampleEnabled;
    out->iblShEnabled                = iblShEnabled();
    out->iblShStrength               = g_iblShStrength;
    out->iblShSet                    = s_currentShSet ? s_currentShSet->name : "default";
    out->pbrEnabled                  = pbrV1Enabled();
    out->pbrStrength                 = g_pbrV1Strength;
    out->pbrRoughnessOverrideEnabled = g_pbrV1RoughnessOverrideEnabled;
    out->pbrRoughnessOverride        = g_pbrV1RoughnessOverrideValue;
    out->debugMaterialMode           = s_staticPropDebugMaterialMode;
    out->spV6DrawCalls               = s_v6FrameDrawsIssued;
    out->spAlphaOffPackets           = s_alphaOffCmdCount;
    out->materialGpuTableSize        = static_cast<uint32_t>(s_materialGpuTable.size());
    out->materialInventorySize       = static_cast<uint32_t>(s_materialInventory.size());
}

uint32_t batcher_getStaticPropTypeDescCount() {
    return static_cast<uint32_t>(s_typeDescTable.size());
}

bool batcher_getStaticPropTypeDesc(uint32_t typeId, RenderCore::StaticPropTypeDesc* out) {
    if (!out) return false;
    if (typeId >= static_cast<uint32_t>(s_typeDescTable.size())) return false;
    *out = s_typeDescTable[typeId];
    return true;
}

const RenderCore::StaticPropTypeDesc* batcher_getStaticPropTypeDescTable(uint32_t* outCount) {
    if (!outCount) return nullptr;
    *outCount = static_cast<uint32_t>(s_typeDescTable.size());
    if (s_typeDescTable.empty()) return nullptr;
    return s_typeDescTable.data();
}

void batcher_buildCandidateLog() {
    static const bool gate    = false;
    static const bool verbose = false;
    if (!gate) return;

    if (s_typeDescTable.empty()) {
        mc2::fmt::print_err("[DRAW_CAND v0] table_empty\n");
        return;
    }

    uint32_t expected = 0u;
    for (const auto& d : s_typeDescTable) { expected += d.packetCount; }

    static bool s_detailDone = false;
    const bool doDetail = verbose || !s_detailDone;

    uint32_t emitted = 0u;
    uint32_t emptyTypes = 0u;
    uint32_t boundsErrors = 0u;
    for (const RenderCore::StaticPropTypeDesc& desc : s_typeDescTable) {
        if (desc.packetCount == 0u) { ++emptyTypes; continue; }
        for (uint32_t p = 0u; p < desc.packetCount; ++p) {
            const uint32_t pktIdx = desc.firstPacket + p;
            if (pktIdx >= static_cast<uint32_t>(s_packets.size())) {
                if (doDetail) {
                    mc2::fmt::print_err("[DRAW_CAND v0 detail] typeId={:d} pkt={:d} BOUNDS_OVERFLOW\n", mc2::fmt::as_u32(desc.typeId), mc2::fmt::as_u32(p));
                }
                ++boundsErrors;
                continue;
            }
            if (doDetail) {
                const GpuStaticPropPacket& pkt = s_packets[pktIdx];
                mc2::fmt::print_err("[DRAW_CAND v0 detail] typeId={:d} pkt={:d} firstIndex={:d} indexCount={:d} baseVertex={:d} alphaClass={:d}\n", mc2::fmt::as_u32(desc.typeId), mc2::fmt::as_u32(p), mc2::fmt::as_u32(pkt.firstIndex), mc2::fmt::as_u32(pkt.indexCount), mc2::fmt::as_i32(pkt.baseVertex), mc2::fmt::as_u32(desc.alphaClass));
            }
            ++emitted;
        }
    }
    s_detailDone = true;

    // Summary line emitted every frame.
    // OK gate: emitted==expected (all reachable packets walked), boundsErrors==0.
    // emptyTypes is informational; a type may be registered with no geometry.
    mc2::fmt::print_err("[DRAW_CAND v0] emitted={:d} expected={:d} emptyTypes={:d} boundsErrors={:d}{}\n",
        mc2::fmt::as_u32(emitted), mc2::fmt::as_u32(expected), mc2::fmt::as_u32(emptyTypes), mc2::fmt::as_u32(boundsErrors),
        mc2::fmt::safe((emitted == expected && boundsErrors == 0) ? " OK" : " MISMATCH"));
}


// Env probe for GPU-vs-CPU count diagnostic (Step 4.11).

void batcher_prepareBaseInstanceTable() {
    // Legacy mode: coalesce ring mirrors legacy ring (s_coalesceFrameSlot = s_frameSlot
    // at the :2644 advance site). Nothing to do here.
    if (s_globalPoolLegacy) return;

    // CRIT-2 v2 fix: advance coalesce ring slot BEFORE any guards so flush()'s
    // memcpy + draw always read a fresh, well-defined slot.
    s_coalesceFrameSlot = (s_coalesceFrameSlot + 1) % RING_FRAMES;

    // Prefix-sum: compute baseInstance for each type in sorted order.
    std::array<uint32_t, 2> groupCursor = {0u, 0u};
    static std::vector<uint32_t> baseInstanceForType;
    baseInstanceForType.assign(s_types.size(), 0u);
    for (unsigned int typeID : s_sortedTypeOrder) {
        if (typeID >= s_types.size()) continue;
        auto kvIt = s_bucketsByType.find(typeID);
        if (kvIt == s_bucketsByType.end()) continue;
        const uint32_t group = s_types[typeID].alphaClass;
        baseInstanceForType[typeID] = groupCursor[group];
        const auto& [bucketKey, bucket] = *kvIt;
        groupCursor[group] += static_cast<uint32_t>(bucket.instances.size());
    }
    const uint32_t offGroupCount = groupCursor[0];
    const uint32_t totalCount    = offGroupCount + groupCursor[1];

    if (totalCount > s_globalInstanceCap) {
        MC2_DIAGFE(stderr, "[COALESCE v1] event=disarmed reason=global_pool_overflow "
                     "used=%u cap=%u\n", totalCount, s_globalInstanceCap);
        std::fflush(stderr);
        s_coalesceArmed = false;
        return;
    }

    // Offset alpha-ON types by alpha-OFF group total.
    for (uint32_t typeID : s_sortedTypeOrder) {
        if (typeID >= s_types.size()) continue;
        if (s_types[typeID].alphaClass == 1u) {
            baseInstanceForType[typeID] += offGroupCount;
        }
    }

    // M1: publish the finalized per-type base for the golden static cull-record
    // scatter (batcher_getBaseInstanceForType). This is the exact binding-0 slot
    // layout the draw uses, so a record placed at base[t]+rank aligns 1:1 with
    // the instance the draw renders.
    s_baseInstanceForType = baseInstanceForType;

    // Rebuild the population split's static instance table only when its store changes.
    if (staticPopSplitArmed()) {
        rebuildStaticBaseInstanceTableIfDirty();
    }

    // Stash group byte/count boundaries for flush() memcpy and draw path.
    s_offGroupCountThisFrame  = offGroupCount;
    s_offGroupBytesThisFrame  = offGroupCount * sizeof(GpuStaticPropInstance);
    s_onGroupBytesThisFrame   = (totalCount - offGroupCount) * sizeof(GpuStaticPropInstance);
    s_totalUsedBytesThisFrame = totalCount * sizeof(GpuStaticPropInstance);
}

// PROP-RECIPE-BAKE-1 (diagnostic, read-only). Resolve a batcher typeID to its
// source shape node name and its alpha class. Used by the [PROPROW v1] probe in
// gos_static_prop_registry.cpp to answer the load-bearing question for the
// dark-prop defect: are the prop types that bake a SUNLESS light row (nL=1,
// sun=0 -- the TreeAppearance treatment, which deactivates worldLights[0]) real
// alpha-test foliage, where that treatment is correct, or opaque scenery props
// where it is wrong? alphaClass is the same foliage-ness discriminator
// FOLIAGE-ALPHAFLAG-1 uses (0 = alpha-OFF = opaque = NOT foliage).
// Global scope on purpose: declaring it inside the registry's flush() would
// namespace it into GpuStaticPropRegistry:: and fail to link.
bool mc2PeekStaticPropTypeInfo(uint32_t typeID, const char** outName, int* outAlphaClass)
{
    if (outName) *outName = "-";
    if (outAlphaClass) *outAlphaClass = -1;
    if (typeID >= s_types.size()) return false;
    const GpuStaticPropType& t = s_types[typeID];
    if (outName && t.source)
        *outName = const_cast<TG_TypeShape*>(t.source)->getNodeId();
    if (outAlphaClass) *outAlphaClass = static_cast<int>(t.alphaClass);
    return true;
}

bool mc2PeekStaticPropTreeCanopy(uint32_t typeID, float out[3])
{
    if (typeID >= s_types.size() || s_types[typeID].treeCanopy[1] <= 0.0f) return false;
    for (int i = 0; i < 3; ++i) out[i] = s_types[typeID].treeCanopy[i];
    return true;
}
