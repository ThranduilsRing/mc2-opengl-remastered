// Multi shape layer: the node-hierarchy walk (TransformMultiShape_BuildRecipe / _HierarchyOnly) and the GPU light-data cache.
// Declared in the class headers; shared names in msl_internal.h.
#include "msl_internal.h"
//-------------------------------------------------------------------------------
// Walks the node hierarchy and publishes shapeToWorld / worldToShape per node. No vertex is touched: the GPU batchers
// transform and light, and the CPU fallback draw (TG_Shape::CpuDraw) projects from these matrices. Each walked leaf is
// stamped lastTurnTransformed = turn, the "this leaf was updated this frame" predicate the fallback draw and the cone
// glow test. Needs no camera, so it is safe during Mission::init.
long TG_MultiShape::TransformMultiShape_BuildRecipe (glm::vec3 *pos, glm::quat *rot)
{
    glm::mat4 	shapeOrigin;
    glm::mat4 	localShapeOrigin;

    mc2::math::buildRotation(shapeOrigin, *rot);
    mc2::math::buildTranslation(shapeOrigin, *pos);

    BindMaterials();
    long i=0;

    TG_ShapeRecPtr childChain[MAX_NODES];

    {
    for (i=0;i<numTG_Shapes;i++)
    {
        //----------------------------------------------
        // Must set each transform!  Animating Textures!


        //-----------------------------------------------------------------
        // Heirarchy Animation Code.
        //
        // Simple, really.  For each shape in list, traverse back UP
        // the heirarchy and store the traversal pointers in a temp list.
        // Starting at the TOP of the heirarchy and for each shape, 
        // check if matrix set for that shape. If so, next node down.  If
        // not, copy above matrix into node and apply animation data.
        // Do this until at end of this heirarchy.
        long curChild = 0;
        childChain[curChild] = &listOfShapes[i];
        while (childChain[curChild]->parentNode)
        {
            curChild++;

            gosASSERT(curChild < MAX_NODES);

            childChain[curChild] = childChain[curChild-1]->parentNode;
        }

        long fNum = float2long(frameNum);

        glm::vec3 zero;
        zero.x = zero.y = zero.z = 0.0f;
        for (int j=curChild;j>=0;j--)
        {
            if (childChain[j]->calcedThisFrame != turn)
            {
                if (j == curChild)		//This is the ROOT Node.
                {
                    //----------------------------------------------------
                    // Top O the hierarchy.  Used passed in shapeMatrices
                    // Apply any animation data, if data is non-NULL
                    if (childChain[j]->currentAnimation)
                    {
                        //--------------------------------------
                        // Slerp between current and next frame
                        glm::quat slerpQuat;
                        slerpQuat.x = slerpQuat.y = slerpQuat.z = 0.0f;
                        slerpQuat.w = 1.0f;

                        if (childChain[j]->currentAnimation->quat)
                            slerpQuat = childChain[j]->currentAnimation->quat[fNum];

                        //--------------------------------------
                        //First Apply Animation to this local piece of heirarchy.
                        // If piece had base rotation, apply it.  Otherwise, no.
                        glm::quat totalRotation = slerpQuat;
                        if ((childChain[j]->baseRotation.w == 1.0f) && 
                                (childChain[j]->baseRotation.x == 0.0f) &&
                                (childChain[j]->baseRotation.y == 0.0f) && 
                                (childChain[j]->baseRotation.z == 0.0f))
                        {
                        }
                        else
                        {
                            totalRotation = mc2::math::composeQuat(slerpQuat, childChain[j]->baseRotation);
                            totalRotation = glm::normalize(totalRotation);
                        }

                        mc2::math::buildRotation(localShapeOrigin, totalRotation);

                        if (childChain[j]->currentAnimation->pos)
                            mc2::math::buildTranslation(localShapeOrigin, childChain[j]->currentAnimation->pos[fNum]);		//SPECIAL.  ROOT HAS ITS OWN OFFSETS!
                        else
                            mc2::math::buildTranslation(localShapeOrigin, childChain[j]->node->myType->GetNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        //------------------------------------------------------------------
                        //Then move the piece of the heirarchy into the frame of the parent
                        childChain[j]->shapeToWorld = childChain[j]->localShapeToWorld * shapeOrigin;

                        childChain[j]->worldToShape = glm::inverse(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                    else
                    {
                        mc2::math::buildRotation(localShapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
                        mc2::math::buildTranslation(localShapeOrigin, childChain[j]->node->myType->GetNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        childChain[j]->shapeToWorld = childChain[j]->localShapeToWorld * shapeOrigin;

                        childChain[j]->worldToShape = glm::inverse(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                }
                else
                {
                    //----------------------------------------------------------------------
                    // Not Top O the Heirarchy.  Figure out matrix based on Animation data.
                    if (childChain[j]->currentAnimation)
                    {
                        //--------------------------------------
                        // Slerp between current and next frame
                        glm::quat slerpQuat;
                        slerpQuat.x = slerpQuat.y = slerpQuat.z = 0.0f;
                        slerpQuat.w = 1.0f;

                        if (childChain[j]->currentAnimation->quat)
                            slerpQuat = childChain[j]->currentAnimation->quat[fNum];

                        //--------------------------------------
                        //First Apply Animation to this local piece of heirarchy.
                        // If piece had base rotation, apply it.  Otherwise, no.
                        glm::quat totalRotation = slerpQuat;
                        if ((childChain[j]->baseRotation.w == 1.0f) && 
                                (childChain[j]->baseRotation.x == 0.0f) &&
                                (childChain[j]->baseRotation.y == 0.0f) && 
                                (childChain[j]->baseRotation.z == 0.0f))
                        {
                        }
                        else
                        {
                            totalRotation = mc2::math::composeQuat(slerpQuat, childChain[j]->baseRotation);
                            totalRotation = glm::normalize(totalRotation);
                        }

                        mc2::math::buildRotation(localShapeOrigin, totalRotation);

                        if (childChain[j]->currentAnimation->pos)
                            mc2::math::buildTranslation(localShapeOrigin, childChain[j]->currentAnimation->pos[fNum]);
                        else
                            mc2::math::buildTranslation(localShapeOrigin, childChain[j]->node->myType->GetRelativeNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        //------------------------------------------------------------------
                        //Then move the piece of the heirarchy into the frame of the parent
                        childChain[j]->localShapeToWorld = localShapeOrigin * childChain[j+1]->localShapeToWorld;

                        //------------------------------------------------------------------
                        // Then deal with global translation.
                        childChain[j]->shapeToWorld = childChain[j]->localShapeToWorld * shapeOrigin;

                        childChain[j]->worldToShape = glm::inverse(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                    else
                    {
                        //--------------------------------------
                        // NO Animation if we are here.
                        //
                        // Apply Base Rotation.  If it is Zero, no problem!
                        glm::quat totalRotation = childChain[j]->baseRotation;
                        mc2::math::buildRotation(localShapeOrigin, totalRotation);
                        mc2::math::buildTranslation(localShapeOrigin, childChain[j]->node->myType->GetRelativeNodeCenter());

                        childChain[j]->localShapeToWorld = localShapeOrigin;

                        //------------------------------------------------------------------
                        //Then move the piece of the heirarchy into the frame of the parent
                        localShapeOrigin = childChain[j]->localShapeToWorld;
                        childChain[j]->localShapeToWorld = localShapeOrigin * childChain[j+1]->localShapeToWorld;

                        //------------------------------------------------------------------
                        // Then deal with global translation.
                        childChain[j]->shapeToWorld = childChain[j]->localShapeToWorld * shapeOrigin;

                        childChain[j]->worldToShape = glm::inverse(childChain[j]->shapeToWorld);
                        childChain[j]->calcedThisFrame = turn;
                    }
                }
            }
        }


        if (listOfShapes[i].node)
            listOfShapes[i].node->lastTurnTransformed = turn;
    }
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
        cachedFrame_         = g_mc2FrameCounter;
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
            cachedFrame_         = g_mc2FrameCounter;
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
    cachedFrame_         = g_mc2FrameCounter;
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
        cachedFrame_ == g_mc2FrameCounter &&
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
        cachedFrame_         = g_mc2FrameCounter;
    }
}
