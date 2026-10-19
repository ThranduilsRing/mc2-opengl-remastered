// GpuStaticPropBatcher: eligibility, persistent static instances, cached-instance submits, the batcher_* RenderCore seam accessors and the base-instance table.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"


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
// calling submitMultiShape. Returns true iff g_useGpuObjects is enabled AND
// every SHAPE_NODE leaf under `multi` is registered in s_typeIndex.
// MUST NOT mutate any batcher state, log, or increment counters.
bool GpuStaticPropBatcher::isMultiShapeEligibleForGpuObjects(
        const TG_MultiShape* multi) const {
    if (!g_useGpuObjects) return false;
    if (!multi) return false;

    const int n = multi->numTG_Shapes;
    if (n <= 0 || !multi->listOfShapes) return false;

    for (int i = 0; i < n; ++i) {
        const TG_ShapeRec& rec = multi->listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        const TG_Shape* child = rec.node.get();
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;  // skip helpers/bones
        const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(child->myType);
        if (s_typeIndex.find(ts) == s_typeIndex.end()) return false;
    }
    return true;
}

// eligibleForGpuObjects — free function (declared in gos_static_prop_batcher.h)
// Per-leaf analog of isMultiShapeEligibleForGpuObjects. Called from the
// addRenderShape gate in tgl.cpp:2522 inside the bShadersDrawPathEnabled block.
// Returns true iff g_useGpuObjects is enabled AND `shape->myType` is a
// registered TG_TypeShape in s_typeIndex.
// Under default g_useGpuObjects=false this returns false unconditionally —
// the addRenderShape gate stays byte-for-byte equivalent in stock builds.
// MUST NOT mutate any batcher state.
bool eligibleForGpuObjects(TG_Shape* shape) {
    if (!g_useGpuObjects) return false;
    if (!shape) return false;
    if (!shape->myType) return false;
    if (shape->myType->GetNodeType() != SHAPE_NODE) return false;
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(shape->myType);
    return s_typeIndex.find(ts) != s_typeIndex.end();
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

    // firstColorOffset must be updated to the current bucket color position
    // so the GPU shader indexes colors correctly for this frame's layout.
    // Diagnostic counters (submitted_instances_by_pop etc.) are NOT incremented
    // for registry-injected instances — they measure the dynamic compute path.
    GpuStaticPropInstance updated = inst;
    // firstColorOffset is unused by any production shader (the colors fill is skipped).
    updated.firstColorOffset = 0u;
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
                updated.typeID, updated.flags, updated.lightDataIndex, updated.firstColorOffset,
                // Same offset fix as submit() — translation at [3]/[7]/[11], not [12..14].
                updated.modelMatrix[3], updated.modelMatrix[7], updated.modelMatrix[11],
                updated.fogRGB[0], updated.fogRGB[1], updated.fogRGB[2], updated.fogRGB[3],
                updated.aRGBHighlight[0], updated.aRGBHighlight[1], updated.aRGBHighlight[2], updated.aRGBHighlight[3],
                bucket.instances.size());
        }
    }
    bucket.instances.push_back(updated);
    // The per-instance Colors SSBO zero-fill is skipped: no production shader reads colors_ (addr-mode 4 reads v_argb).
}

uint64_t GpuStaticPropBatcher::persistentStaticGen() { return s_persistentStaticGen; }
void GpuStaticPropBatcher::setPersistentStaticGen(uint64_t g) { s_persistentStaticGen = g; }

// VANISH-PROBE-1: stable-order access to persistent static instance origins.
// Free functions (no header coupling) so code/gamecam.cpp can extern-declare
// them and project a real prop through worldToClipGL at the live camera angle.
uint32_t gos_ProbeStaticInstanceCount() {
    uint32_t n = 0; for (auto& kv : s_persistentStaticStore) n += static_cast<uint32_t>(kv.second.size());
    return n;
}
bool gos_ProbeStaticInstanceWorld(uint32_t idx, float outXYZ[3]) {
    // unordered_map iteration order is stable between calls within a frame (no
    // mutation), which is all the probe needs. modelMatrix is row-major data
    // uploaded GL_FALSE -> the prop vertex shader reads it column-major, so the
    // instance origin (modelMatrix * vec4(0,0,0,1)) is column 3 = m[12..14].
    for (auto& kv : s_persistentStaticStore) {
        const auto& v = kv.second;
        if (idx < v.size()) {
            const float* m = v[idx].modelMatrix;
            outXYZ[0] = m[12]; outXYZ[1] = m[13]; outXYZ[2] = m[14];
            return true;
        }
        idx -= static_cast<uint32_t>(v.size());
    }
    return false;
}

void GpuStaticPropBatcher::clearPersistentStatic() {
    // Keep the map + per-type vector capacity; just reset sizes (rebuild refills).
    for (auto& kv : s_persistentStaticStore) kv.second.clear();
    s_persistentStaticGen = 0xFFFFFFFFFFFFFFFFull;  // invalid until the rebuild sets it
}

// Append a contiguous run of recipes (one registry range) into the persistent
// static store, grouped by typeID. Called ONLY on a dirty generation (rare), so
// per-element push_back is fine. firstColorOffset is irrelevant (static carries 0
// colors, no shader reads it) — stored verbatim.
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

// Every frame: bulk-inject each persistent static block into its per-type bucket
// (one memcpy per type, NOT per leaf). Dynamic instances (if any) are appended by
// the dynamic submit path separately; static carries 0 colors so colors untouched.
void GpuStaticPropBatcher::reinjectPersistentStatic() {
    // M2a Task 4 DE-MERGE: under the population split, static props are drawn
    // from their own buffer + cmds (the static draw in flush()), so they must NOT
    // be merged into the dynamic bucket (else they double-draw). s_bucketsByType
    // becomes dynamic-only.
    if (staticPopSplitArmed()) return;
    // reinjPos/reinjCount (VK color feed) assume ONE reinject per frame; flag cleared at frame end.
    { static bool s_warned = false;
      if (s_reinjectedThisFrame && !s_warned) { s_warned = true;
          mc2::fmt::print_err("[PROP_REINJECT] WARNING reinjectPersistentStatic called twice in one frame; reinjPos/reinjCount guard invalid\n"); }
      s_reinjectedThisFrame = true; }
    for (auto& kv : s_persistentStaticStore) {
        const auto& block = kv.second;
        if (block.empty()) continue;
        PerTypeBucket& bucket = s_bucketsByType[kv.first];
        bucket.reinjPos   = static_cast<uint32_t>(bucket.instances.size());
        bucket.reinjCount = static_cast<uint32_t>(block.size());
        bucket.instances.insert(bucket.instances.end(), block.begin(), block.end());  // bulk
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

    auto it = s_typeIndex.find(typeShape);
    if (it == s_typeIndex.end()) return false;

    GpuStaticPropInstance inst{};
    // Matrix4D is a plain row-major Scalar[16] (see stuff/matrix.hpp). Copy
    // as-is; shader uploads the worldToClip uniform with GL_FALSE.
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    inst.typeID           = it->second;
    inst.firstColorOffset = 0u;          // placeholder; submitCachedInstance patches per-frame
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
    auto it = s_typeIndex.find(ts);
    if (it == s_typeIndex.end()) return false;
    if (outTypeId) *outTypeId = it->second;
    return true;
}

void batcher_queryTypeShape(const void* typeShape, int* outRegistered, int* outFailed) {
    const TG_TypeShape* ts = static_cast<const TG_TypeShape*>(typeShape);
    if (outRegistered) *outRegistered = (ts && s_typeIndex.count(ts)) ? 1 : 0;
    if (outFailed) {
        auto it = ts ? s_failedTypes.find(ts) : s_failedTypes.end();
        *outFailed = (it != s_failedTypes.end() && it->second) ? 1 : 0;
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
GLuint   batcher_getCmdToBucketSsbo()  { return s_cmdToBucketSsbo;  }

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
    return it->second.instanceCount;
}

// M1 FROZEN-STATIC-CULL-RECORDS: module-scope copy of the per-type global
// instance-pool base (alpha-group prefix-sum over s_sortedTypeOrder), published
// by batcher_prepareBaseInstanceTable(). global_slot(typeID, store_rank) ==
// s_baseInstanceForType[typeID] + store_rank, the exact binding-0 slot the draw
// indexes. Valid only after prepareBaseInstanceTable() runs in global-pool armed
// mode (empty under legacy/disarmed). The golden static cull-record build reads
// it to place each record at its instance-pool slot (record-index == pool-slot).
static std::vector<uint32_t> s_baseInstanceForType;

// ===========================================================================
// M2a POPULATION-SPLIT (gate MC2_STATIC_POP_SPLIT, requires G2). De-merge the
// persistent static instances out of the per-frame dynamic bucket so the
// static cull-record golden has a STABLE per-type base (the merged base shifts
// per-frame when dynamics share a static type's bucket -> the outOfRange 0->71
// defect). Tasks 1+2: static-only base table + own front-packed instance SSBO.
// Default-off => byte-identical (every new path gates on staticPopSplitArmed()).
// ===========================================================================

// Static-only per-type base (s_staticBaseInstanceForType, defined early near
// s_globalInstanceCap): prefix-sum over s_persistentStaticStore[t].size() in the
// SAME sorted+alpha order as s_baseInstanceForType, but counting ONLY persistent-
// static instances (no dynamics). The stable base the golden scatter must use.
// Dirty-gated on s_persistentStaticGen via s_staticBaseBuiltGen.
static void rebuildStaticBaseInstanceTableIfDirty() {
    if (s_staticBaseBuiltGen == s_persistentStaticGen) return;  // dirty-gated
    s_staticBaseInstanceForType.assign(s_types.size(), 0u);
    std::array<uint32_t, 2> groupCursor = {0u, 0u};
    for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
        const uint32_t typeID = s_sortedTypeOrder[i];
        if (typeID >= s_types.size()) continue;
        auto it = s_persistentStaticStore.find(typeID);
        const uint32_t cnt = (it != s_persistentStaticStore.end())
                                 ? static_cast<uint32_t>(it->second.size()) : 0u;
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

// StaticPopulation instance buffer (Task 2; state s_staticInstanceSsbo/Map/Bytes/
// FillGen defined early near s_globalInstanceCap): persistent-mapped, SINGLE
// region (static is frozen -> no triple-buffer ring). Sized ONCE at the global
// cap so growth never reallocates an immutable buffer mid-flight. Filled
// [0,total) once per dirty in static-base layout.
static bool ensureStaticInstanceCapacity() {
    if (s_staticInstanceSsbo) return s_staticInstanceMap != nullptr;
    const size_t want = static_cast<size_t>(s_globalInstanceCap)
                        * sizeof(GpuStaticPropInstance);
    // TIER2-EXCLUDED: dead-path
    glGenBuffers(1, &s_staticInstanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_staticInstanceSsbo);
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    while (glGetError() != GL_NO_ERROR) {}
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(want), nullptr, flags);
    if (glGetError() != GL_NO_ERROR) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glDeleteBuffers(1, &s_staticInstanceSsbo);
        s_staticInstanceSsbo = 0;
        mc2::fmt::print_err("[STATIC_POP_SPLIT] event=alloc_failed bytes={:d}\n", mc2::fmt::as_size(want));
        return false;
    }
    s_staticInstanceMap = MC2_GL_MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                           static_cast<GLsizeiptr>(want), flags);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    s_staticInstanceBytes = want;
    return s_staticInstanceMap != nullptr;
}

static void fillStaticInstanceBufferIfDirty() {
    if (s_staticInstanceFillGen == s_persistentStaticGen) return;
    rebuildStaticBaseInstanceTableIfDirty();
    size_t total = 0;
    for (auto& kv : s_persistentStaticStore) total += kv.second.size();
    if (total > static_cast<size_t>(s_globalInstanceCap)) {
        mc2::fmt::print_err("[STATIC_POP_SPLIT] event=overflow total={:d} cap={:d}\n", mc2::fmt::as_size(total), mc2::fmt::as_u32(s_globalInstanceCap));
        return;  // never write past the cap
    }
    if (!ensureStaticInstanceCapacity()) return;
    // Criterion 2: the buffer is overwritten in place — wait for the prior
    // static draw to drain before clobbering it (dirties are rare, so the stall
    // is rare; GPU in-order execution makes one wait cover all earlier frames).
    if (s_staticDrawFence) {
        glClientWaitSync(s_staticDrawFence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_staticDrawFence);
        s_staticDrawFence = nullptr;
    }
    auto* dst = static_cast<GpuStaticPropInstance*>(s_staticInstanceMap);
    for (auto& kv : s_persistentStaticStore) {
        const uint32_t typeID = kv.first;
        if (typeID >= s_staticBaseInstanceForType.size()) continue;
        const uint32_t base = s_staticBaseInstanceForType[typeID];
        const auto& vec = kv.second;
        for (uint32_t r = 0; r < vec.size(); ++r) dst[base + r] = vec[r];
    }
    s_staticInstanceFillGen = s_persistentStaticGen;
}

GLuint batcher_getStaticInstanceSsbo() { return s_staticInstanceSsbo; }

// Task 3: build the StaticPopulation indirect cmds, one per sorted packet (same
// [alpha-OFF | alpha-ON] order as s_sortedPacketOrder, so the Task-4 draw splits
// at s_alphaOffCmdCount exactly like the dynamic draw). count/firstIndex/
// baseVertex are geometry-static; baseInstance = the type's STABLE static-only
// base; instanceCount = the type's full static population (first-N draw — NOT a
// GPU cull count; visibleIds authority arrives in M2b). A packet whose type has
// 0 static instances gets instanceCount=0 -> harmless no-op sub-draw. Dirty-
// gated on s_persistentStaticGen. Built but NOT drawn until Task 4.
static void rebuildStaticDrawCmdsIfDirty() {
    if (s_staticCmdBuiltGen == s_persistentStaticGen) return;
    rebuildStaticBaseInstanceTableIfDirty();
    const uint32_t pktCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
    s_staticDrawCmds.assign(pktCount, StaticDrawCmd{});
    for (uint32_t i = 0; i < pktCount; ++i) {
        const uint32_t gp = s_sortedPacketOrder[i];
        if (gp >= s_packets.size()) continue;
        const GpuStaticPropPacket& pkt = s_packets[gp];
        const uint32_t tid = pkt.owningTypeID;
        auto it = s_persistentStaticStore.find(tid);
        const uint32_t cnt = (it != s_persistentStaticStore.end())
                                 ? static_cast<uint32_t>(it->second.size()) : 0u;
        StaticDrawCmd& c = s_staticDrawCmds[i];
        c.count         = pkt.indexCount;
        c.instanceCount = cnt;
        c.firstIndex    = pkt.firstIndex;
        c.baseVertex    = pkt.baseVertex;
        c.baseInstance  = batcher_getStaticBaseInstanceForType(tid);
    }
    if (pktCount > 0u) {
        if (!s_staticIndirectCmdBuf) glGenBuffers(1, &s_staticIndirectCmdBuf);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, s_staticIndirectCmdBuf);
        MC2_GL_BufferData(GL_DRAW_INDIRECT_BUFFER,
                     static_cast<GLsizeiptr>(pktCount * sizeof(StaticDrawCmd)),
                     s_staticDrawCmds.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }
    s_staticCmdBuiltGen = s_persistentStaticGen;

    // Diag (env-gated): dump the static cmd summary so Task 3 can be validated
    // before the draw exists. Sum of instanceCount over OFF/ON groups must equal
    // the persistent store's total instance count; non-zero cmd count == sorted packet count.
    static const bool s_cmdDiag = mc2::gate::get<"MC2_STATIC_POP_SPLIT_CMD_DIAG">();
    if (s_cmdDiag) {
        // maxBaseInstance+its instanceCount must stay within the static pool
        // [0,storeTotal); nonZeroCmds = packets whose type has static instances.
        uint32_t nonZero = 0, maxEnd = 0;
        for (const auto& c : s_staticDrawCmds) {
            if (c.instanceCount) ++nonZero;
            const uint32_t end = c.baseInstance + c.instanceCount;
            if (end > maxEnd) maxEnd = end;
        }
        uint64_t storeTotal = 0;
        for (auto& kv : s_persistentStaticStore) storeTotal += kv.second.size();
        mc2::fmt::print_err("[STATIC_POP_SPLIT_CMD] pkts={:d} nonZeroCmds={:d} offCmds={:d} maxBaseInst+count={:d} storeTotal={:d} (maxEnd<=storeTotal expected)\n", mc2::fmt::as_u32(pktCount), mc2::fmt::as_u32(nonZero), mc2::fmt::as_u32(s_alphaOffCmdCount), mc2::fmt::as_u32(maxEnd), mc2::fmt::as_u64(static_cast<unsigned long long>(storeTotal)));
        std::fflush(stderr);
    }
}

GLuint batcher_getStaticIndirectCmdBuf() { return s_staticIndirectCmdBuf; }

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

GLuint batcher_getCoalesceInstanceSsbo() { return s_coalesceInstanceSsbo; }
GLuint batcher_getPerDrawSsbo()          { return s_perDrawSsbo;          }
// COMPRESSION-BC7-STATICPROP-2: under the gate the two named handles are 0
// (the build produced per-bucket arrays instead). Fall back to the first
// alpha-OFF / first alpha-ON bucket array so the ImGui GBuffer preview still
// shows a representative array. Gate OFF: returns the original handles.
GLuint batcher_getTexArrayOff() {
    if (s_texArrayOff) return s_texArrayOff;
    for (size_t b = 0; b < s_bucketInfo.size(); ++b)
        if (s_bucketInfo[b].group == 0u) return s_bucketInfo[b].glArray;
    return 0;
}
GLuint batcher_getTexArrayOn() {
    if (s_texArrayOn) return s_texArrayOn;
    for (size_t b = 0; b < s_bucketInfo.size(); ++b)
        if (s_bucketInfo[b].group == 1u) return s_bucketInfo[b].glArray;
    return 0;
}
GLuint batcher_getPermutationSsbo()      { return s_permutationSsbo;      }

size_t batcher_getCoalescePerFrameInstanceBytes() {
    // SSBO-BIND-ALIGN: this is the per-frame RING STRIDE; fr_off_bytes_d =
    // s_coalesceFrameSlot * this is the glBindBufferRange offset. The std430
    // GpuStaticPropInstance stride is 112 bytes, so cap*112 is a multiple of 256
    // (NVIDIA's GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT) only when cap % 16 == 0.
    // Otherwise slots 1+ bind at a misaligned offset -> NVIDIA rejects with
    // GL_INVALID_VALUE -> the instance SSBO never binds -> static-prop trees
    // invisible during gameplay, reappearing only when paused on slot 0 (offset 0,
    // the one aligned slot). AMD tolerated it. Pad the per-frame stride up to the
    // alignment; the allocation (Step 5.9) pads identically so offsets never
    // overrun the ring.
    size_t raw = s_globalPoolLegacy
        ? (s_offGroupTotalBytes + s_onGroupTotalBytes)
        : (static_cast<size_t>(s_globalInstanceCap) * sizeof(GpuStaticPropInstance));
    return static_cast<size_t>(gpuAlignUp(static_cast<unsigned long long>(raw),
                              static_cast<unsigned long long>(gpuSsboOffsetAlignment())));
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
    out->instanceCount = (it != s_typeInstanceCountPrevFrame.end()) ? it->second : 0u;

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
            const uint32_t currentInstCount =
                (typeIt != s_typeRanges.end()) ? typeIt->second.instanceCount : 0u;
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
    out->snapshotDispatchDefault     = !s_v6Disarmed;
    out->legacyDispatch              = s_v6Disarmed;
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
    static const bool gate    = mc2::gate::get<"MC2_TYPE_TABLE_CAND_LOG">();
    static const bool verbose = mc2::gate::get<"MC2_TYPE_TABLE_CAND_VERBOSE">();
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

// Saved slot-16 GL buffer object. Slot 16 is reserved to this feature.
static GLint s_savedSsbo16 = 0;

void batcher_bindBaseInstanceByCmdSsboForPatch() {
    if (s_globalPoolLegacy || !s_baseInstanceByCmdSsbo) return;
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, BASE_INSTANCE_SSBO_BINDING, &s_savedSsbo16);
    const GLintptr off = static_cast<GLintptr>(
        s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame);
    gpuBindSsboRange(BASE_INSTANCE_SSBO_BINDING, s_baseInstanceByCmdSsbo,
                     static_cast<long long>(off), static_cast<long long>(s_baseInstanceByCmdBytesPerFrame),
                     "sp.baseInstanceByCmd");
}
void batcher_unbindBaseInstanceByCmdSsboForPatch() {
    if (s_globalPoolLegacy || !s_baseInstanceByCmdSsbo) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, BASE_INSTANCE_SSBO_BINDING, (GLuint)s_savedSsbo16);
    s_savedSsbo16 = 0;
}

// Env probe for GPU-vs-CPU count diagnostic (Step 4.11).
static const bool s_coalesceGpuVsCpuTrace = mc2::gate::get<"MC2_COALESCE_GPU_VS_CPU_COUNT_TRACE">();

void batcher_prepareBaseInstanceTable() {
    // Legacy mode: coalesce ring mirrors legacy ring (s_coalesceFrameSlot = s_frameSlot
    // at the :2644 advance site). Nothing to do here.
    if (s_globalPoolLegacy) return;

    // CRIT-2 v2 fix: advance coalesce ring slot BEFORE any guards so flush()'s
    // memcpy + draw always read a fresh, well-defined slot.
    s_coalesceFrameSlot = (s_coalesceFrameSlot + 1) % RING_FRAMES;

    // Wait on the fence for the slot we just took (from RING_FRAMES frames ago).
    if (s_coalesceFence[s_coalesceFrameSlot]) {
        glClientWaitSync(s_coalesceFence[s_coalesceFrameSlot],
                         GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_coalesceFence[s_coalesceFrameSlot]);
        s_coalesceFence[s_coalesceFrameSlot] = nullptr;
    }

    // Step 4.11 — GPU-vs-CPU count diagnostic (env-gated). Must be after fence
    // wait (prior frame's compute is now complete — fence covered patch dispatch).
    if (s_coalesceGpuVsCpuTrace) {
        static std::vector<uint32_t> s_priorFrameCpuCount;
        const GLuint bucketsBuf = gpu_cull::compute_getBucketCountsBuf();
        if (bucketsBuf && !s_types.empty()) {
            std::vector<uint32_t> bucketCountGpu(s_types.size() + 1, 0u);
            glGetNamedBufferSubData(bucketsBuf, 0,
                static_cast<GLsizeiptr>((s_types.size() + 1) * sizeof(uint32_t)),
                bucketCountGpu.data());
            const size_t cmpN = std::min(s_priorFrameCpuCount.size(), bucketCountGpu.size() - 1);
            for (uint32_t t = 0; t < static_cast<uint32_t>(cmpN); ++t) {
                if (bucketCountGpu[t] > s_priorFrameCpuCount[t]) {
                    MC2_DIAGFE(stderr,
                        "[COALESCE v1] event=gpu_count_exceeds_cpu type=%u gpu=%u cpu=%u\n",
                        t, bucketCountGpu[t], s_priorFrameCpuCount[t]);
                    std::fflush(stderr);
                }
            }
        }
        // Stash this frame's CPU bucket sizes for comparison RING_FRAMES frames from now.
        s_priorFrameCpuCount.assign(s_types.size(), 0u);
        for (auto& kv : s_bucketsByType) {
            if (kv.first < s_priorFrameCpuCount.size())
                s_priorFrameCpuCount[kv.first] = static_cast<uint32_t>(kv.second.instances.size());
        }
    }

    if (!s_baseInstanceByCmdMap) return;
    if (!batcher_isCoalesceArmed()) return;

    // Prefix-sum: compute baseInstance for each type in sorted order.
    std::array<uint32_t, 2> groupCursor = {0u, 0u};
    static std::vector<uint32_t> baseInstanceForType;
    baseInstanceForType.assign(s_types.size(), 0u);
    for (uint32_t i = 0; i < s_sortedTypeOrder.size(); ++i) {
        const uint32_t typeID = s_sortedTypeOrder[i];
        if (typeID >= s_types.size()) continue;
        auto kvIt = s_bucketsByType.find(typeID);
        if (kvIt == s_bucketsByType.end()) continue;
        const uint32_t group = s_types[typeID].alphaClass;
        baseInstanceForType[typeID] = groupCursor[group];
        groupCursor[group] += static_cast<uint32_t>(kvIt->second.instances.size());
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

    // M2a POPULATION-SPLIT: rebuild the static-only base table + fill the static
    // instance buffer when the persistent store changed (both dirty-gated on
    // s_persistentStaticGen). The base table must be ready before the golden
    // scatter (buildStaticPrefixGolden) reads it later this frame. Default-off
    // => not called => byte-identical.
    if (staticPopSplitArmed()) {
        rebuildStaticBaseInstanceTableIfDirty();
        fillStaticInstanceBufferIfDirty();
        rebuildStaticDrawCmdsIfDirty();   // Task 3: own indirect cmds (not drawn until Task 4)
    }

    const uint32_t pktCount = static_cast<uint32_t>(s_sortedPacketOrder.size());
    const size_t fr_off = s_coalesceFrameSlot * s_baseInstanceByCmdBytesPerFrame;
    uint32_t* dst = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(s_baseInstanceByCmdMap) + fr_off);
    for (uint32_t i = 0; i < pktCount; ++i) {
        const uint32_t globalPktIdx = s_sortedPacketOrder[i];
        const uint32_t typeID       = s_packets[globalPktIdx].owningTypeID;
        dst[i] = (typeID < baseInstanceForType.size()) ? baseInstanceForType[typeID] : 0u;
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
