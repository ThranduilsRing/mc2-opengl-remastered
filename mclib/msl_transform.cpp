// Multi shape layer: the node-hierarchy walk (TransformMultiShape_BuildRecipe / _HierarchyOnly) and the GPU light-data cache.
// Declared in the class headers; shared names in msl_internal.h.
#include <atomic>
#include "msl_internal.h"
#include "sim_overlap.h"
#include "msl_node_walk.h"
//-------------------------------------------------------------------------------
// Walks the node hierarchy and publishes shapeToWorld / worldToShape per node. No vertex is touched: the GPU batchers
// transform and light, and the CPU fallback draw (TG_Shape::CpuDraw) projects from these matrices. Each walked leaf is
// stamped lastTurnTransformed = turn, the "this leaf was updated this frame" predicate the fallback draw and the cone
// glow test. Needs no camera, so it is safe during Mission::init.
long TG_MultiShape::TransformMultiShape_BuildRecipe (glm::vec3 *pos, glm::quat *rot)
{
    glm::mat4 	shapeOrigin;

    mc2::math::buildRotation(shapeOrigin, *rot);
    mc2::math::buildTranslation(shapeOrigin, *pos);

    BindMaterials();
    // Hierarchy order cached per shape (parents before children). It is rebuilt only when a parent link no longer matches the
    // snapshot the order was built from (shape compaction rebuilds the links).
    const auto parentIndexOf = [&](long n) {
        const _TG_ShapeRec* p = listOfShapes[n].parentNode;
        const long d = p ? static_cast<long>(p - listOfShapes) : -1L;
        gosASSERT(d < numTG_Shapes);
        return static_cast<int>(d);
    };
    bool orderOk = static_cast<long>(walkOrder_.size()) == numTG_Shapes && static_cast<long>(walkParent_.size()) == numTG_Shapes;
    for (long n = 0; orderOk && n < numTG_Shapes; ++n)
        orderOk = walkParent_[n] == parentIndexOf(n);
    if (!orderOk)
    {
        walkParent_.assign(numTG_Shapes, -1);
        walkOrder_.clear();
        walkOrder_.reserve(numTG_Shapes);
        std::vector<unsigned char> placed(numTG_Shapes, 0);
        for (long n = 0; n < numTG_Shapes; ++n) walkParent_[n] = parentIndexOf(n);
        for (long n = 0; n < numTG_Shapes; ++n)
        {
            int chain[MAX_NODES];
            long depth = 0;
            for (int c = static_cast<int>(n); c >= 0 && !placed[c]; c = walkParent_[c])
            {
                gosASSERT(depth < MAX_NODES);
                chain[depth++] = c;
            }
            while (depth > 0)
            {
                const int c = chain[--depth];
                placed[c] = 1;
                walkOrder_.push_back(c);
            }
        }
    }

    if (!walkOriginValid_ || std::memcmp(&walkOrigin_, &shapeOrigin, sizeof(shapeOrigin)) != 0)
    {
        walkOrigin_ = shapeOrigin;
        walkOriginValid_ = true;
        ++walkOriginGen_;
    }

    const long fNum = float2long(frameNum);
    for (const int idx : walkOrder_)
    {
        TG_ShapeRec& rec = listOfShapes[idx];
        if (rec.calcedThisFrame != turn)
        {
            const bool isRoot = rec.parentNode == nullptr;
            mc2::msl::NodeInput in;
            in.base = rec.baseRotation;
            if (rec.currentAnimation)
            {
                in.animated = true;
                if (rec.currentAnimation->quat)
                    in.animation = rec.currentAnimation->quat[fNum];
                in.translation = rec.currentAnimation->pos ? rec.currentAnimation->pos[fNum]
                    : (isRoot ? rec.node->myType->GetNodeCenter() : rec.node->myType->GetRelativeNodeCenter());
            }
            else
                in.translation = isRoot ? rec.node->myType->GetNodeCenter() : rec.node->myType->GetRelativeNodeCenter();

            const _TG_ShapeRec* parent = rec.parentNode;
            mc2::msl::evalNode(in, parent ? &parent->localShapeToWorld : nullptr, parent ? parent->walkKey.localGen : 0u,
                shapeOrigin, walkOriginGen_, walkGenCounter_, rec.walkKey, rec.localShapeToWorld, rec.shapeToWorld, rec.worldToShape);
            rec.calcedThisFrame = turn;
        }
        if (rec.node)
            rec.node->lastTurnTransformed = turn;
    }
    return(0);
}

long TG_MultiShape::TransformMultiShape_HierarchyOnly (glm::vec3 *pos, glm::quat *rot)
{
    return TransformMultiShape_BuildRecipe(pos, rot);
}


//-------------------------------------------------------------------------------
// Slice 2 (object-offload) — Stage 2.D.2 fix:
// Cache the GPU light-data index while worldLights[0]->aRGB is correct for
// THIS actor (during update(), immediately after SetLightList is called).
// By the time submitMultiShape() runs (during renderLists()), later actors
// will have overwritten worldLights[0]->aRGB for their own terrain positions.
// Calling GatherGpuObjectLightDataOnly() there produces the WRONG light color.
//
// This method locates the first SHAPE_NODE leaf (same heuristic as
// submitMultiShape's firstShapeNodeLeaf scan) and calls
// GatherGpuObjectLightDataOnly() to cache the dedup-cache index in
// cachedGpuLightIndex_. submitMultiShape then reads cachedGpuLightIndex_
// instead of calling GatherGpuObjectLightDataOnly() itself.
//-------------------------------------------------------------------------------
void TG_MultiShape::CacheGpuLightData()
{
    // PERF 2026-05-07: stripped hot Tracy zones CacheGpuLightData,
    // CacheGpuLightData findLeaf, and CacheGpuLightData GatherGpuObjectLightDataOnly.

    // Find first SHAPE_NODE leaf — same logic as submitMultiShape.
    TG_Shape* firstShapeNodeLeaf = nullptr;
    {
        for (int i = 0; i < numTG_Shapes; ++i) {
            TG_ShapeRec& rec = listOfShapes[i];
            if (!rec.processMe || !rec.node) continue;
            TG_Shape* child = rec.node.get();
            if (!child->myType) continue;
            if (child->myType->GetNodeType() != SHAPE_NODE) continue;
            firstShapeNodeLeaf = child;
            break;
        }
    }

    if (firstShapeNodeLeaf != nullptr) {
        cachedGpuLightIndex_ = firstShapeNodeLeaf->GatherGpuObjectLightDataOnly();
        cachedFrame_         = mc2::overlap::simulationFrame(g_mc2FrameCounter);
    }
}

// VEHICLE-LIGHTING-PATH-1: per-leaf gather for the legacy render path.
// See the header comment for why the first-leaf-only CacheGpuLightData() is
// insufficient for TG_MultiShape::Render (each TG_Shape::Render submits its own
// lightData_, mclib/tgl.cpp:3508). Same guard and same per-leaf gather call as
// CacheGpuLightData; the only difference is that it does not stop at the first
// SHAPE_NODE leaf. cachedGpuLightIndex_/cachedFrame_ are set from the FIRST
// leaf so any batcher-side reader keeps identical semantics.
void TG_MultiShape::CacheGpuLightDataAllLeaves()
{
    bool first = true;
    for (int i = 0; i < numTG_Shapes; ++i) {
        TG_ShapeRec& rec = listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node.get();
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;
        const uint32_t idx = child->GatherGpuObjectLightDataOnly();
        if (first) {
            first = false;
            cachedGpuLightIndex_ = idx;
            cachedFrame_         = mc2::overlap::simulationFrame(g_mc2FrameCounter);
        }
    }
}

// [LIGHTBAKE v1] First SHAPE_NODE leaf's durable post-decompose
// lightData_ (TG_Shape member; TG_MultiShape is friend of TG_Shape).
// Call AFTER CacheGpuLightData() has run this frame:
// GatherGpuObjectLightDataOnly -> addLightDataStructureWithPerActorColor
// wrote the post-template-copy + in-place decomposeFirstActiveLightColor
// struct into leaf->lightData_, which resetLightData does NOT touch
// (unlike the per-frame scratch slot). Same leaf scan as
// CacheGpuLightData for identity.
const TG_HWLightsData* TG_MultiShape::peekCachedLeafLightData()
{
    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < numTG_Shapes; ++i) {
        TG_ShapeRec& rec = listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node.get();
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;
        firstShapeNodeLeaf = child;
        break;
    }
    if (firstShapeNodeLeaf == nullptr) return nullptr;
    return &firstShapeNodeLeaf->lightData_;
}

// [LIGHTBAKE v1] Re-emit a mission-load-baked constant into a per-frame
// slot WITHOUT GatherLights/decompose/template recompute. Per-frame slot
// WRITE stays (Shape-C O(1) consumer); the RECOMPUTE is what dies. Sets
// cachedGpuLightIndex_/cachedFrame_ so the existing
// staticReg.lightDataIndex = getCachedGpuLightIndex() capture + batcher
// read are unchanged.
void TG_MultiShape::EmitBakedGpuLightData(int32_t recipeIndex, const TG_HWLightsData& baked)
{
    // [LIGHTBAKE v2] THE substitutive edit (adversarial-review C1 locus):
    // the static recipe's permanent light slot IS recipeIndex (the CPU
    // mirror lives at lightData_[recipeIndex], written once by
    // mc2WriteStaticLightSlot, re-shipped every frame by the unchanged
    // whole-buffer upload). So this is a pure pointer assignment -- NO
    // mc2SubmitBakedLightSlot, NO addLightDataStructure, NO 1792B FNV,
    // NO 1792B memcmp. This is what takes `addLightDataStructure scan`
    // -> ~0 for the static class (the ~1840 calls/~1ms/frame survivor).
    (void)baked;  // identity is recipeIndex; the struct already persists in the slot
    cachedGpuLightIndex_ = static_cast<uint32_t>(recipeIndex);
    cachedFrame_         = mc2::overlap::simulationFrame(g_mc2FrameCounter);
}

void TG_MultiShape::ResubmitCachedGpuLightData()
{
    // FRAME-JOBS-2D invariant: this function must never run on worker threads.
    // g_workerResubmitCalls > 0 means split boundary is broken — touchWorkerPrepass
    // called into the light-data path instead of touchSerialCommit.
    if (isFrameJobsWorkerThread())
        g_workerResubmitCalls.fetch_add(1, std::memory_order_relaxed);

    // [LIGHTBRIDGE v1] C6 repoint: if this multi already populated a VALID
    // slot THIS frame, that slot is still live (per-frame reset by
    // resetLightData/clearArrays at frame start) — return it and skip the
    // direct addLightDataStructure 1792B FNV + 1792B memcmp entirely. The
    // cachedGpuLightIndex_ != sentinel guard is load-bearing: the
    // registration path (gos_static_prop_registry.cpp) sets cachedFrame_
    // without a valid index. Kill-switch OFF -> legacy resubmit bit-for-bit.
    extern bool mc2LightBridgeRepointEnabled();
    if (mc2LightBridgeRepointEnabled() &&
        cachedFrame_ == mc2::overlap::simulationFrame(g_mc2FrameCounter) &&
        cachedGpuLightIndex_ != 0xFFFFFFFFu) {
        static bool s_c6Logged = false;
        if (!s_c6Logged) {
            s_c6Logged = true;
            MC2_DIAGF("[LIGHTBRIDGE v1] event=c6_fastpath_hit\n");
            fflush(stdout);
        }
        return;
    }

    TG_Shape* firstShapeNodeLeaf = nullptr;
    for (int i = 0; i < numTG_Shapes; ++i) {
        TG_ShapeRec& rec = listOfShapes[i];
        if (!rec.processMe || !rec.node) continue;
        TG_Shape* child = rec.node.get();
        if (!child->myType) continue;
        if (child->myType->GetNodeType() != SHAPE_NODE) continue;
        firstShapeNodeLeaf = child;
        break;
    }

    if (firstShapeNodeLeaf != nullptr) {
        cachedGpuLightIndex_ = firstShapeNodeLeaf->ResubmitCachedLightData();
        cachedFrame_         = mc2::overlap::simulationFrame(g_mc2FrameCounter);
    }
}
