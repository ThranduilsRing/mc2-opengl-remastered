// TG shapes: TG_Shape::Render, per-poly selection and the render-shape PBR override.
// Declared in the class headers; shared names in tgl_internal.h.
#include "tgl_internal.h"
#include "Gates/gate.h"


struct TG_RenderShapePbrOverrideState {
	HGOSRENDERMATERIAL program = nullptr;
	DWORD normalTexture = 0;
	DWORD ormTexture = 0;
	DWORD materialSsbo = 0;
	float tileScale = 1.0f;
	float roughnessBias = 0.0f;
	float metallicInfluence = 0.0f;
};

static TG_RenderShapePbrOverrideState s_activeRenderShapePbrOverride;

void TG_SetRenderShapePbrOverride(HGOSRENDERMATERIAL program,
                                  DWORD normalTexture,
                                  DWORD ormTexture,
                                  DWORD materialSsbo,
                                  float tileScale,
                                  float roughnessBias,
                                  float metallicInfluence)
{
	s_activeRenderShapePbrOverride.program = program;
	s_activeRenderShapePbrOverride.normalTexture = normalTexture;
	s_activeRenderShapePbrOverride.ormTexture = ormTexture;
	s_activeRenderShapePbrOverride.materialSsbo = materialSsbo;
	s_activeRenderShapePbrOverride.tileScale = tileScale;
	s_activeRenderShapePbrOverride.roughnessBias = roughnessBias;
	s_activeRenderShapePbrOverride.metallicInfluence = metallicInfluence;
}

void TG_ClearRenderShapePbrOverride()
{
	s_activeRenderShapePbrOverride = TG_RenderShapePbrOverrideState();
}

// (Removed dead #define TERRAIN_DEPTH_FUDGE (0.000f) 2026-05-16: zero
// consumers in this TU; a translation-unit-local trap if any future
// quad-style emit were added here. Bias single-sourced in
// mclib/terrain_depth_bias.h. VPL-deferred item 10.)
//-------------------------------------------------------------------------------
//This function takes the current listOfVisibleFaces and draws them using
//gos_DrawTriangle.
// [RENDER_CONTRACT:Pass=OpaqueObject id=TG_Shape_Render]
//   May also act as Pass=AlphaObject when material flags select alpha-test
//   or alpha-blend; see ShadowContract::castsDynamicShadow conjunction at
//   tgl.cpp:2700-2730. Phase 1 documents only; centralization is F4.
void TG_Shape::Render (float forceZ, bool isHudElement, BYTE alphaValue, bool isClamped, const Stuff::Matrix4D* shapeToClip, const Stuff::Matrix4D* shapeToWorld)
{
	const bool traceBuildingPbr = s_activeRenderShapePbrOverride.program && mc2::gate::get<"MC2_BUILDING_PBR_TRACE">();
	if (traceBuildingPbr) {
		static int s_pbrRenderTrace = 0;
		if (s_pbrRenderTrace < 64) {
			++s_pbrRenderTrace;
			fprintf(stderr,
				"[BUILDING_PBR_TRACE] TG_Shape::Render enter shape=%p verts=%ld visible=%ld last=%ld turn=%ld\n",
				this, numVertices, numVisibleFaces, lastTurnTransformed, turn);
			fflush(stderr);
		}
	}
	if (!renderTGLShapes)
		return;

	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
	{
		// Root/helper nodes have no CPU triangles to submit.
		return;
	}

	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows ||
		(/*(lastTurnTransformed != (turn-1)) &&*/ (lastTurnTransformed != turn)))
	{
		if (traceBuildingPbr) {
			fprintf(stderr,
				"[BUILDING_PBR_TRACE] TG_Shape::Render reject shape=%p lv=%p lc=%p lsv=%p lt=%p lvf=%p lvs=%p last=%ld turn=%ld\n",
				this, listOfVertices, listOfColors, listOfShadowTVertices, listOfTriangles,
				listOfVisibleFaces, listOfVisibleShadows, lastTurnTransformed, turn);
			fflush(stderr);
		}
		return;
	}

	// MECHOPAQUE-ORDER-FIX-2: enqueue-time OpaqueObject note removed — OpaqueObject is
	// observed at its DRAW site (txmmgr.cpp:3271 GpuMechBatcher_flush), not enqueue;
	// an enqueue note records before StaticProp's flush and lies about draw order.

	if (fogColor != 0xffffffff)
	{
		gos_SetRenderState( gos_State_Fog, fogColor); // sebi
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;

	gos_SetRenderState( gos_State_AlphaTest, theShape->alphaTestOn);
	gos_SetRenderState( gos_State_Filter, theShape->filterOn ? gos_FilterBiLinear : gos_FilterNone);

	DWORD lastTextureUsed = 0xffffffff;
	
	bool isNight = eye->getIsNight();
	float nightFactor = eye->getNightFactor();
	
	if (lightsOut)
	{
		isNight = false;
		nightFactor = 0.0f;
	}

	// GHOST-CONE-VERIFY-4: see MultiTransformShape — the cone leaf must honour
	// the fade-to-invisible alpha that hides a sensor-blip-only mover's mesh.
	if (isSpotlight && (!isNight
		|| mc2_cone_glow::cone_geom_hidden_by_alpha(alphaValue)))
		return;

	// Ensure depth write is on regardless of prior-pass state. This path uses
	// the legacy MLR queue (gos_DrawTriangles); the state propagates to the
	// batch flush in renderLists(). Depth comparison function left unchanged
	// pending A/B verification of vehicle forward-Z convention (render-hygiene-s1).
	gos_SetRenderState(gos_State_ZWrite, 1);

 	for (long j=0;j<numVisibleFaces;j++)
	{
		if (listOfVisibleFaces[j] < numTriangles)
		{
			TG_TypeTriangle triType = theShape->listOfTypeTriangles[listOfVisibleFaces[j]];
			TG_Triangle tri = listOfTriangles[listOfVisibleFaces[j]];

			gos_VERTEX gVertex[3];

			gVertex[0] = listOfVertices[triType.Vertices[0]];
			gVertex[1] = listOfVertices[triType.Vertices[1]];
			gVertex[2] = listOfVertices[triType.Vertices[2]];

			gVertex[0].u		= triType.uvdata.u0;
			gVertex[0].v		= triType.uvdata.v0;

			gVertex[0].argb		= tri.aRGBLight[0];

			gVertex[1].u		= triType.uvdata.u1;
			gVertex[1].v		= triType.uvdata.v1;

			gVertex[1].argb		= tri.aRGBLight[1];

			gVertex[2].u		= triType.uvdata.u2;
			gVertex[2].v		= triType.uvdata.v2;

			gVertex[2].argb		= tri.aRGBLight[2];

			DWORD addFlags = 0;
			if (isHudElement)		//We are a HUD Element like the compass.  Mark us as such.
			{
				addFlags |= MC2_ISCOMPASS;
			}
			
			if (isClamped)
			{
				addFlags |= MC2_ISTERRAIN;
			}

            if(theShape->alphaTestOn)
			{
				addFlags |= MC2_ALPHATEST;
			}

			if (drawOldWay)
			{
			
				if (theShape->listOfTextures && 
					(theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle != 0xffffffff))
				{
					if (theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle != lastTextureUsed)
					{
						gos_SetRenderState( gos_State_Texture, theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle );
						if (theShape->listOfTextures[triType.localTextureHandle].textureAlpha)
						{
							gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
							gos_SetRenderState( gos_State_AlphaTest, true);
						}
						
						lastTextureUsed = theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle;
					}
				}
				else
					gos_SetRenderState( gos_State_Texture, 0 );
	
				// ENCYCLO-3D-2 diagnostic: identify WHICH texture the preview
				// samples — node index, live GOS handle, and manager-side name.
				// Black previews + healthy verts means the answer is here:
				// handle 0 / 0xffffffff = untextured, valid handle with no
				// pixel data = cold source (smart-load deferred mount).
				if ( mc2::gate::get<"MC2_LOG_PREVIEW">() && g_mechPreviewRenderDepth > 0
					&& theShape->listOfTextures )
				{
					static DWORD s_seenNodes[64];
					static int   s_seenCount = 0;
					DWORD nodeIdx = theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex;
					bool seen = false;
					for ( int si = 0; si < s_seenCount; si++ )
						if ( s_seenNodes[si] == nodeIdx ) { seen = true; break; }
					if ( !seen && s_seenCount < 64 )
					{
						s_seenNodes[s_seenCount++] = nodeIdx;
						if ( FILE* f = fopen("preview_debug.log","a") )
						{
							const char* nm = mcTextureManager ? mcTextureManager->getTextureName(nodeIdx) : nullptr;
							fprintf(f,"[PREVIEW-TEX] local=%ld node=%lu gosHandle=%lu alpha=%d name=%s\n",
								(long)triType.localTextureHandle,
								(unsigned long)nodeIdx,
								(unsigned long)theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle,
								(int)theShape->listOfTextures[triType.localTextureHandle].textureAlpha,
								nm ? nm : "(null)");
							fclose(f);
						}
					}
				}
				// ENCYCLO-3D-1 diagnostic: dump a handful of transformed preview
				// verts (screen-space x/y + z) — separates "drawn offscreen"
				// from "z-rejected" from "degenerate". Throttled hard.
				if ( mc2::gate::get<"MC2_LOG_PREVIEW">() && g_mechPreviewRenderDepth > 0 )
				{
					static int s_vlog = 0;
					if ( (s_vlog++ % 500) < 3 )
					{
						if ( FILE* f = fopen("preview_debug.log","a") )
						{
							fprintf(f,"[PREVIEW] drawOldWay vtx (%.1f,%.1f,%.4f) (%.1f,%.1f,%.4f) (%.1f,%.1f,%.4f) argb=%08X\n",
								gVertex[0].x,gVertex[0].y,gVertex[0].z,
								gVertex[1].x,gVertex[1].y,gVertex[1].z,
								gVertex[2].x,gVertex[2].y,gVertex[2].z,
								gVertex[0].argb);
							fclose(f);
						}
					}
				}
				if ((gVertex[0].z >= 0.0f) &&
					(gVertex[0].z < 1.0f) &&
					(gVertex[1].z >= 0.0f) &&
					(gVertex[1].z < 1.0f) &&
					(gVertex[2].z >= 0.0f) &&
					(gVertex[2].z < 1.0f))
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					gos_DrawTriangles(gVertex, 3);
				}
				else if ( mc2::gate::get<"MC2_LOG_PREVIEW">() && g_mechPreviewRenderDepth > 0 )
				{
					static int s_zrej = 0;
					if ( (s_zrej++ % 500) == 0 )
					{
						if ( FILE* f = fopen("preview_debug.log","a") )
						{
							fprintf(f,"[PREVIEW] drawOldWay Z-REJECT z=(%.4f,%.4f,%.4f)\n",
								gVertex[0].z,gVertex[1].z,gVertex[2].z);
							fclose(f);
						}
					}
				}
			}
			else
			{
				if ((gVertex[0].z >= 0.0f) &&
					(gVertex[0].z < 1.0f) &&
					(gVertex[1].z >= 0.0f) &&  
					(gVertex[1].z < 1.0f) && 
					(gVertex[2].z >= 0.0f) &&  
					(gVertex[2].z < 1.0f))
				{
					if ((forceZ >= 0.0f) && (forceZ < 1.0f))
					{
						gVertex[0].z	= forceZ;
						gVertex[1].z	= forceZ;
						gVertex[2].z	= forceZ;
					}
 				
					if (isSpotlight)
					{
						mcTextureManager->addVertices(0xffffffff,gVertex,MC2_ISSPOTLGT);
					}
					else if (isWindow)
					{
						mcTextureManager->addVertices(0xffffffff, gVertex, MC2_DRAWALPHA);
					}
					else
					{
						//--------------------------------------------------
						// Any sorting we need to do occurs in here as well!
						// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
						if (theShape->listOfTextures[triType.localTextureHandle].textureAlpha || (alphaValue != 0xff))
						{
							mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,gVertex,MC2_DRAWALPHA | addFlags);
						}
						else
						{
							// [UI-PHASE1] carried from mc2r_ui_phase1: read by SimpleCamera
							// to tell "transformed but not drawn" apart from "not transformed".
							++g_previewDrawTris;
							mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,gVertex,MC2_DRAWSOLID | addFlags);
						}
					}
				}
			}
		}
	}


	if (theShape->ib_ && theShape->vb_) {
		// Collect shape for the dynamic shadow-map pass even when the renderable shape mixes
		// textured and alpha-marked materials. The shadow object pass is depth-only and does not
		// depend on the first material slot being fully opaque.
		// NOTE: 743efd6 added a `!firstTextureAlpha` clause here intending to suppress trees that
		// cast solid rectangular shadows. Trees are already excluded via `!alphaTestOn`
		// (TreeAppearance calls SetAlphaTest(true)), so the clause did nothing for trees but
		// suppressed every static prop whose first material is alpha-channel -- notably fences /
		// gates with perforated lattice textures. Reverted; alpha-cutout caster shader is the
		// proper long-term fix.
		const bool eligibleForDynamicShadow =
			!isSpotlight && !isWindow && !isHudElement && !isClamped &&
			!theShape->alphaTestOn && (alphaValue == 0xff) && shapeToWorld;
		if (eligibleForDynamicShadow) {
			addShadowShape(theShape->vb_, theShape->ib_, theShape->vdecl_, shapeToWorld->entries);
		}
	}

	// FIXME: this (listOfTypeTriangles[0]) is not correct if model has more than 1 texture!
	const TG_TypeTriangle firstTriType = theShape->listOfTypeTriangles[0];
	const bool firstTextureAlpha = theShape->listOfTextures[firstTriType.localTextureHandle].textureAlpha;
	const bool pbrOverrideActive = s_activeRenderShapePbrOverride.program != nullptr;
	if (!isSpotlight && !isWindow && (!firstTextureAlpha || pbrOverrideActive) && (alphaValue == 0xff))
	{
		DWORD addFlags = 0;
		if (isHudElement)
		{
			addFlags |= MC2_ISCOMPASS;
		}

		if (isClamped)
		{
			addFlags |= MC2_ISTERRAIN;
		}

        if(theShape->alphaTestOn)
		{
			addFlags |= MC2_ALPHATEST;
		}

		if (theShape->ib_ && theShape->vb_) {

			// FIXME: this is not correct if model has more than 1 texture!
			// TODO: split on per texture batches basis
			TG_TypeTriangle triType = firstTriType;

			Stuff::Matrix4D mvp;
			if ((forceZ >= 0.0f) && (forceZ < 1.0f))
			{
				// matrix that will make (x,y,w * forceZ, w) from (x,y,z,w), so that later after division by w, it will become just forceZ
				Stuff::Matrix4D forceZMatrix = Stuff::Matrix4D::Identity;
				forceZMatrix(2, 2) = 0.0f;
				forceZMatrix(3, 2) = forceZ;
				mvp.Multiply(cur_shape2clip, forceZMatrix);
			}
			else {
				mvp = cur_shape2clip;
			}

			TG_RenderShape rs;
			rs.ib_ = theShape->ib_;
			rs.vb_ = theShape->vb_;
			rs.vdecl_ = theShape->vdecl_;
            rs.mvp_ = mvp;
			rs.mw_ = *shapeToWorld;
			memcpy(rs.viewport_, cur_viewport, 4 * sizeof(float));
			{
				LcsScope<LcsBucket::C2> _lcs;
				rs.light_data_buffer_index_ = mcTextureManager->addLightDataStructure(&lightData_);
			}
            rs.isHudElement_ = isHudElement;

			const DWORD texNode = theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex;
			const DWORD renderFlags = MC2_DRAWSOLID | addFlags |
				((pbrOverrideActive && firstTextureAlpha) ? MC2_ALPHATEST : 0);
			if (pbrOverrideActive && firstTextureAlpha)
				mcTextureManager->addRenderShape(texNode, renderFlags);
			mcTextureManager->addRenderShape(texNode, &rs, renderFlags);
			if (traceBuildingPbr) {
				fprintf(stderr,
					"[BUILDING_PBR_TRACE] TG_Shape::Render enqueue shape=%p texNode=%lu localTex=%lu visible=%ld\n",
					this,
					(unsigned long)texNode,
					(unsigned long)triType.localTextureHandle,
					numVisibleFaces);
				fflush(stderr);
			}
		}
		else if (traceBuildingPbr) {
			fprintf(stderr, "[BUILDING_PBR_TRACE] TG_Shape::Render no buffers shape=%p ib=%p vb=%p\n",
				this, (void*)theShape->ib_, (void*)theShape->vb_);
			fflush(stderr);
		}
	}
	else if (traceBuildingPbr) {
		TG_TypeTriangle triType = theShape->listOfTypeTriangles[0];
		fprintf(stderr,
			"[BUILDING_PBR_TRACE] TG_Shape::Render skip indexed shape=%p spot=%d window=%d alpha=%d alphaValue=%u localTex=%lu\n",
			this, (int)isSpotlight, (int)isWindow,
			(int)theShape->listOfTextures[triType.localTextureHandle].textureAlpha,
			(unsigned)alphaValue, (unsigned long)triType.localTextureHandle);
		fflush(stderr);
	}

	gos_SetRenderState( gos_State_Fog, 0);
}	

extern bool InEditor;
//------------------------------------------------------------------------------------------------------------
// This function checks each visible triangle against the mouse position and returns if we are over ANY poly!
bool TG_Shape::PerPolySelect (float mouseX, float mouseY)
{
	if (!InEditor)
	{
		if (!listOfVertices ||
			!listOfColors ||
			!listOfShadowTVertices ||
			!listOfTriangles ||
			!listOfVisibleFaces ||
			!listOfVisibleShadows ||
			((lastTurnTransformed != (turn-1)) /*&& (lastTurnTransformed != turn)*/))
			return false;
	}
	else
	{
		if (!listOfVertices ||
			!listOfColors ||
			!listOfShadowTVertices ||
			!listOfTriangles ||
			!listOfVisibleFaces ||
			!listOfVisibleShadows ||
			((lastTurnTransformed != (turn-1)) && (lastTurnTransformed != turn)))
			return false;
	}

	// Cheap screen-space AABB reject before the per-triangle hit test.
	// listOfVertices[] is already this frame's screen-space transform (the
	// guard above proved lastTurnTransformed==turn-1), so this reuses work
	// instead of re-projecting like the reverted objmgr rect pre-filter did.
	// In a dense cluster ~700 statics/frame run PerPolySelect; 699 of them do
	// NOT contain the cursor. For those an O(numVertices) min/max compare beats
	// the O(numVisibleFaces) point-in-triangle loop (~13 mults/face). The bbox
	// spans ALL transformed verts (over-inclusive) so it can never reject a
	// candidate the cursor is actually over. (FindTerrainObj 1.5ms hotspot;
	// see HANDOFF_2026_06_10_pick_path_hotspots_recon.)
	{
		float minX = listOfVertices[0].x, maxX = minX;
		float minY = listOfVertices[0].y, maxY = minY;
		for (long j=1;j<numVertices;j++)
		{
			float x = listOfVertices[j].x, y = listOfVertices[j].y;
			if (x < minX) minX = x; else if (x > maxX) maxX = x;
			if (y < minY) minY = y; else if (y > maxY) maxY = y;
		}
		if (mouseX < minX || mouseX > maxX || mouseY < minY || mouseY > maxY)
			return false;
	}

	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;
 	for (long j=0;j<numVisibleFaces;j++)
	{
		if (listOfVisibleFaces[j] < numTriangles)
		{
			TG_TypeTriangle triType = theShape->listOfTypeTriangles[listOfVisibleFaces[j]];
			Stuff::Point3D v0,v1,v2;
			v0.x = listOfVertices[triType.Vertices[0]].x;
			v0.y = listOfVertices[triType.Vertices[0]].y;
			
			v1.x = listOfVertices[triType.Vertices[1]].x;
			v1.y = listOfVertices[triType.Vertices[1]].y;
			
			v2.x = listOfVertices[triType.Vertices[2]].x;
			v2.y = listOfVertices[triType.Vertices[2]].y;
			v0.z = v1.z = v2.z = 0.0f;
			
			//
			//Theorem:
			// Given the sides of the triangle defined by the lines v0v1, v1v2 and v2v0
			// in the form Ax + By + C = 0
			//
			// the point mousex, mousey lies inside the triangle if and only if
			//
			//  	A0 * mouseX + B0 * mouseY + C0 = D0
			//		A1 * mouseX * B1 * mouseY + c1 = D1
			//		A2 * mouseX + B2 * mouseY + c2 = D2
			//
			// Dx is the same sign for each line as the correct sign for clockwise or counterclockwise vertices!
			//
			Stuff::Vector3D line1;
			Stuff::Vector3D line2;
			line1.Subtract(v0,v1);
			line2.Subtract(v1,v2);
			
			float order = line2.x * line1.y - line1.x * line2.y;
			order = sign2(order);
			
			float A0 = -(v0.y - v1.y);
			float B0 = (v0.x - v1.x);
			float C0 = -B0*(v0.y) - A0*(v0.x);
			float D0 = A0 * mouseX + B0 * mouseY + C0;
			
			float A1 = -(v1.y - v2.y);
			float B1 = (v1.x - v2.x);
			float C1 = -B1*(v1.y) - A1*(v1.x);
			float D1 = A1 * mouseX + B1 * mouseY + C1;
 			
 			float A2 = -(v2.y - v0.y);
			float B2 = (v2.x - v0.x);
			float C2 = -B2*(v2.y) - A2*(v2.x);
			float D2 = A2 * mouseX + B2 * mouseY + C2;
			
			if ((sign2(D0) == order) && (sign2(D0) == sign2(D1)) && (sign2(D0) == sign2(D2)))
				return true;
    	}
	}
	
	return false;
}

constexpr unsigned int SHADOW_HARDWARE_BLACK = 0xff000000;
constexpr unsigned int SHADOW_SOFTWARE_BLACK = 0xff000000;
constexpr float FLOAT_SHADOW_HARDWARE_BLACK = 128.0f;
