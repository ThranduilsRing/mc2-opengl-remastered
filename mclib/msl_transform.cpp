// Multi shape layer: TG_MultiShape::TransformMultiShape (and its variants) and the GPU light-data cache.
// Declared in the class headers; shared names in msl_internal.h.
#include "msl_internal.h"
//-------------------------------------------------------------------------------
//This function does the actual transform math, clip checks and lighting math.
//The matrices passed in are the translation/rotation matrix for the shape and
//Its inverse.  Also will animate.
//Function returns -1 if all vertex screen positions are off screen.
//Function returns 0 if any one vertex screen position is off screen.
//Function returns 1 is all vertex screen positions are on screen.
// NOTE:  THIS IS NOT A RIGOROUS CLIP!!!!!!!!!

float yawRotation = 0.0f;

#ifdef LAB_ONLY
__int64 MCTimeTransformandLight 	= 0;
__int64 MCTimeAnimationandMatrix	= 0;
__int64 MCTimePerShapeTransform		= 0;
#endif

// Slice 2 (object-offload) — Stage 2.B: dispatch flag for the per-leaf
// MultiTransformShape call inside TransformMultiShape's body. Set true by
// TransformMultiShape_PositionsOnly, cleared after the inner call returns.
// File-static so the state is local to the per-frame transform dispatch.
// Single-threaded by construction (MC2 update is serial; no actor's update
// runs while another's TransformMultiShape is in flight).
static bool s_multiShapePositionsOnly = false;
// Track B: when true, TransformMultiShape populates shapeToWorld but skips the
// pool-allocating MultiTransformShape_* dispatch and the s_cameraOrigin read.
static bool s_buildRecipeOnly = false;

long TG_MultiShape::TransformMultiShape (glm::vec3 *pos, glm::quat *rot)
{
	::mc2_object_recon::Scope _recon_mShape_(
		&::mc2_object_recon::g_per_frame.mShape_total_ns,
		&::mc2_object_recon::g_per_frame.mShape_calls);
	// F3 CPU projection cost-baseline: per-multishape scope, aggregated
	// into the tgl_transform per-frame bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_tgl_scope(
	    ::mc2_cpu_proj_cost::BUCKET_TGL_TRANSFORM);
	::mc2_cpu_proj_cost::add_workload_tgl_transform(numTG_Shapes);
    //Profile T&L so I can break out GameLogic from T&L
#ifdef LAB_ONLY
    __int64 x;
    x=GetCycles();
#endif

    glm::mat4 	shapeOrigin;
    glm::mat4	shadowOrigin;
    glm::mat4 	localShapeOrigin;

    mc2::math::buildRotation(shapeOrigin, *rot);
    mc2::math::buildTranslation(shapeOrigin, *pos);

    mc2::math::Euler angles = mc2::math::eulerFromQuat(*rot);
    yawRotation = angles.yaw;

    mc2::math::buildRotation(shadowOrigin, mc2::math::Euler(-angles.pitch,0.0f,0.0f));
    mc2::math::buildTranslation(shadowOrigin, *pos);

    BindMaterials();
    long i=0;
    glm::vec3 camPosition;
    // Guard null — s_cameraOrigin is unset during Mission::init (before first Camera::update).
    // s_buildRecipeOnly skips per-vertex dispatch so camPosition is never used in that mode.
    if (TG_Shape::s_cameraOrigin)
        camPosition = mc2::math::getTranslation(*TG_Shape::s_cameraOrigin);
    else
        { camPosition.x = camPosition.y = camPosition.z = 0.0f; }

    glm::mat4  shapeToClip, rootShapeToClip;
    glm::vec3 backFacePoint;

    TG_ShapeRecPtr childChain[MAX_NODES];

    // D-gpu-pose-instrument: PerShapeLoop wraps the per-shape iteration.
    // Per-mech-per-call (fires once per TransformMultiShape invocation),
    // not per-leaf-iteration. Captures the WHOLE loop including hierarchy
    // walk + per-leaf dispatch + MultiTransformShadows. Future GPU-compute
    // target.
    { ZoneScopedN("TG.MultiShape.PerShapeLoop");
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

        // GPU recipes consume the hierarchy matrices, not CPU leaf lighting,
        // projection or shadow scratch. Avoid the per-node world-light scan.
        if (s_buildRecipeOnly)
            continue;

        if (useFaceLighting || useVertexLighting)
        {
            //----------------------------------------------------
            // Setup Lighting here.
            if (Environment.Renderer != 3)
            {
                for (long iLight=0;iLight<TG_Shape::s_numLights;iLight++)
                {
                    if ((TG_Shape::s_listOfLights[iLight] != nullptr) && (TG_Shape::s_listOfLights[iLight]->active))
                    {
                        switch (TG_Shape::s_listOfLights[iLight]->lightType)
                        {
                            case TG_LIGHT_AMBIENT:
                                {
                                    //No Setup needed for Ambient light
                                }
                                break;

                            case TG_LIGHT_INFINITE:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != nullptr)
                                    {
                                        TG_Shape::s_lightToShape[iLight] = TG_Shape::s_listOfLights[iLight]->lightToWorld * listOfShapes[i].worldToShape;
                                        glm::vec3 uVec;
                                        uVec = mc2::math::localForwardInWorld(TG_Shape::s_lightToShape[iLight]);
                                        TG_Shape::s_lightDir[iLight] = uVec;

                                        if (listOfShapes[i].parentNode == nullptr)
                                        {
                                            TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                            //if (angles.yaw != 0.0f )
                                            //	RotateLight(TG_Shape::s_rootLightDir[iLight],-angles.yaw);
                                        }
                                    }
                                }
                                break;

                            case TG_LIGHT_INFINITEWITHFALLOFF:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != nullptr)
                                    {
                                        TG_Shape::s_lightToShape[iLight] = TG_Shape::s_listOfLights[iLight]->lightToWorld * listOfShapes[i].worldToShape;
                                        glm::vec3 uVec;
                                        uVec = mc2::math::localForwardInWorld(TG_Shape::s_lightToShape[iLight]);
                                        TG_Shape::s_lightDir[iLight] = uVec;

                                        if (listOfShapes[i].parentNode == nullptr)
                                        {
                                            TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                        }
                                    }
                                }
                                break;

                            case TG_LIGHT_POINT:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != nullptr)
                                    {
                                        glm::vec3 lightPos;
                                        lightPos = TG_Shape::s_listOfLights[iLight]->direction;

                                        glm::vec3 shapePosition;
                                        shapePosition = mc2::math::getTranslation(listOfShapes[i].shapeToWorld);

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_lightDir[iLight] = shapePosition;
                                        //if (angles.yaw != 0.0f )
                                        //	RotateLight(TG_Shape::s_lightDir[iLight],-angles.yaw);
                                    }

                                    if (listOfShapes[i].parentNode == nullptr)
                                    {
                                        TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                    }
                                }
                                break;

                            case TG_LIGHT_TERRAIN:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != nullptr)
                                    {
                                        glm::vec3 lightPos;
                                        lightPos = TG_Shape::s_listOfLights[iLight]->direction;

                                        glm::vec3 shapePosition;
                                        shapePosition = mc2::math::getTranslation(listOfShapes[i].shapeToWorld);

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_lightDir[iLight] = shapePosition;
                                    }

                                    if (listOfShapes[i].parentNode == nullptr)
                                    {
                                        TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_lightDir[iLight];
                                    }
                                }
                                break;

                            case TG_LIGHT_SPOT:
                                {
                                    if (TG_Shape::s_listOfLights[iLight] != nullptr)
                                    {
                                        glm::vec3 lightPos;
                                        lightPos = TG_Shape::s_listOfLights[iLight]->direction;

                                        glm::vec3 shapePosition;
                                        shapePosition = mc2::math::getTranslation(listOfShapes[i].shapeToWorld);

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_lightDir[iLight] = shapePosition;
                                        //if (angles.yaw != 0.0f )
                                        //	RotateLight(TG_Shape::s_lightDir[iLight],-angles.yaw);

                                        lightPos = TG_Shape::s_listOfLights[iLight]->spotDir;
                                        shapePosition = mc2::math::getTranslation(listOfShapes[i].shapeToWorld);

                                        shapePosition -= lightPos;
                                        shapePosition.y = 0.0f;
                                        TG_Shape::s_spotDir[iLight] = shapePosition;
                                        //if (angles.yaw != 0.0f )
                                        //	RotateLight(TG_Shape::s_spotDir[iLight],-angles.yaw);

                                        if (listOfShapes[i].parentNode == nullptr)
                                        {
                                            TG_Shape::s_rootLightDir[iLight] = TG_Shape::s_spotDir[iLight];
                                        }
                                    }
                                }
                                break;
                        }
                    }
                }
            }
        }

        shapeToClip = listOfShapes[i].shapeToWorld * TG_Shape::s_worldToClip;
        backFacePoint = mc2::math::transformPoint(camPosition, listOfShapes[i].worldToShape);

#ifdef LAB_ONLY
        x=GetCycles()-x;
        MCTimeAnimationandMatrix += x;
        x=GetCycles();
#endif

        // Slice 2 (object-offload) — Stage 2.B: dispatch on the file-static
        // flag set by TransformMultiShape_PositionsOnly. The positions-only
        // variant strips the per-vertex / per-face lighting kernels; slice 1
        // batcher provides GPU vertex lighting via the calc_light() kernel
        // finished in lighting.hglsl (Stage 2.C). Default branch (flag false)
        // is the unchanged legacy call.
        // D-gpu-pose-instrument: PerLeaf zone wraps the per-shape dispatch.
        // Per-leaf-iteration (fires N times per loop). Per-leaf work is
        // ~500ns (pool alloc + per-vertex projection + per-face cull),
        // above the 100ns floor; aggregate Tracy overhead <1% frame budget.
        { ZoneScopedN("TG.MultiShape.PerLeaf");
        if (s_multiShapePositionsOnly)
        {
            listOfShapes[i].node->MultiTransformShape_PositionsOnly(&shapeToClip,&backFacePoint,listOfShapes[i].parentNode,isHudElement,alphaValue,isClamped);
        }
        else
        {
            listOfShapes[i].node->MultiTransformShape(&shapeToClip,&backFacePoint,listOfShapes[i].parentNode,isHudElement,alphaValue,isClamped);
        }
        } // end PerLeaf zone

        // D-gpu-pose-instrument: ShadowProj wraps the MultiTransformShadows
        // dispatch (per-light × per-vertex shadow projection). Per-leaf-
        // iteration; only fires when useShadows && d_useShadows.
        if (useShadows && d_useShadows)
        {
            ZoneScopedN("TG.MultiShape.ShadowProj");
            listOfShapes[i].node->MultiTransformShadows(pos, &(listOfShapes[i].shapeToWorld),yawRotation);
        }

#ifdef LAB_ONLY
        x=GetCycles()-x;
        MCTimePerShapeTransform += x;
        x=GetCycles();
#endif
    }
    } // end PerShapeLoop zone

#ifdef LAB_ONLY
    MCTimeTransformandLight = MCTimeAnimationandMatrix + MCTimePerShapeTransform;
#endif
    return(0);
}

//-------------------------------------------------------------------------------
// Slice 2 (object-offload) — Stage 2.B
// TransformMultiShape_PositionsOnly: thin wrapper that flips the file-static
// per-leaf dispatch flag, calls the existing TransformMultiShape (so the
// heirarchy animation, shapeToClip computation, and TG_Shape::s_* state setup
// all run unchanged), then clears the flag. The flag is read at the per-leaf
// MultiTransformShape call site near the bottom of TransformMultiShape's body.
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_PositionsOnly (glm::vec3 *pos, glm::quat *rot)
{
    s_multiShapePositionsOnly = true;
    long result = TransformMultiShape(pos, rot);
    s_multiShapePositionsOnly = false;
    return result;
}

//-------------------------------------------------------------------------------
// Track B (widen registry): recipe-build-only variant.
// Runs the hierarchy traversal to populate listOfShapes[i].shapeToWorld per
// leaf without touching TGL vertex/face/color pools and without requiring
// TG_Shape::s_cameraOrigin to be non-null. Called from BldgAppearance/
// TreeAppearance::registerStatic during Mission::init before Camera::update.
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_BuildRecipe (glm::vec3 *pos, glm::quat *rot)
{
    s_buildRecipeOnly = true;
    long result = TransformMultiShape(pos, rot);
    s_buildRecipeOnly = false;
    return result;
}

//-------------------------------------------------------------------------------
// Slice D-leaf-skip-v2 (2026-05-09): thin runtime wrapper that reuses the
// s_buildRecipeOnly mechanism for the GPU mech body callsite. Skip per-leaf
// dispatch + MultiTransformShadows; preserve hierarchy walk. Recon proved
// per-leaf state on mechShape has zero practical consumer in modern + GPU
// mech mode (Slice A bypasses Render(true); RenderShadows unreachable on
// tessellation; PerPolySelect-on-mechs theoretical via fallback findObjectByMouse
// but findMoverByMouse rect-only test catches all real mech selection).
//-------------------------------------------------------------------------------
long TG_MultiShape::TransformMultiShape_HierarchyOnly (glm::vec3 *pos, glm::quat *rot)
{
    // ANIM-HIERARCHYONLY-DEDUP-1: this was a byte-identical copy of
    // TransformMultiShape_BuildRecipe (same s_buildRecipeOnly mechanism). Kept as a
    // distinct entry point for the GPU-mech-body callsite (semantic name) but now
    // DELEGATES to the single implementation — no duplicated flag dance. Byte-identical.
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
//
// Only active when g_useGpuObjects is true; no-op otherwise so non-GPU
// paths have zero overhead.
//-------------------------------------------------------------------------------
void TG_MultiShape::CacheGpuLightData()
{
    // PERF 2026-05-07: stripped hot Tracy zones CacheGpuLightData,
    // CacheGpuLightData findLeaf, and CacheGpuLightData GatherGpuObjectLightDataOnly.
    //
    // Slice B1 (2026-05-09): also honor g_useGpuMechs so an operator
    // running MC2_GPU_OBJECTS=0 MC2_GPU_MECHS=1 still gets the cache
    // refresh for mechs. Adversarial review MAJOR-1.
    if (!g_useGpuObjects && !g_useGpuMechs)
        return;

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
    if (!g_useGpuObjects && !g_useGpuMechs)
        return;

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

    // Slice B1 (2026-05-09): see CacheGpuLightData rationale.
    if (!g_useGpuObjects && !g_useGpuMechs)
        return;

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
