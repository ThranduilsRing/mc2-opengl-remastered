// MC_TextureManager::renderLists phase 12: the legacy overlay pass (water loops, no-underlayer,
// shadow blobs, non-terrain alpha, vfx / hud submit).

#include "txmmgr_render_internal.h"

void MC_TextureManager::renderListsOverlaysWaterLoops (void)
{
    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
	{
	ZoneScopedN("RenderLists.TerrainAlphaWaterLoops");
	rlcost::Span _rl(rlcost::kWaterLoops);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Water);
    for(int states = 0; states < 2; ++states)
    {
        gos_SetRenderState( gos_State_AlphaTest, states);

        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
                // The legacy non-water terrain alpha/detail layer sits on the original
                // flat terrain plane and shows through displaced tess terrain as the
                // dark striped under-pattern. Keep water passes, but drop this layer.
                if (!(masterVertexNodes[i].flags & MC2_ISWATER) &&
                    !(masterVertexNodes[i].flags & MC2_ISWATERDETAIL)) {
                    masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
                    continue;
                }

                {
                    int waterMode = 0;
                    if (masterVertexNodes[i].flags & MC2_ISWATER) waterMode = 1;
                    else if (masterVertexNodes[i].flags & MC2_ISWATERDETAIL) waterMode = 2;
                    gos_SetRenderState(gos_State_Water, waterMode);
                }

                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Water);
	} // end RenderLists.TerrainAlphaWaterLoops
}

void MC_TextureManager::renderListsOverlaysNoUnderlayer (void)
{
	//<< sebi: added this section to draw objects which do not have terrain underlayer (those are added in quad.cpp, see (*) there )
	{ ZoneScopedN("Render.NoUnderlayer");
	  TracyGpuZone("Render.NoUnderlayer");
	  rlcost::Span _rl(rlcost::kNoUnderlayer);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
			!(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
			(masterVertexNodes[i].flags & MC2_GPUOVERLAY) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
	
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ZCompare, 0);
			gos_SetRenderState(gos_State_Overlay, 1);
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
			gos_SetRenderState(gos_State_Overlay, 0);
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
			
			//Reset the list to zero length to avoid drawing more then once!			
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	} // end ZoneScopedN("Render.NoUnderlayer")
	//<< sebi: end of added block
}

void MC_TextureManager::renderListsOverlaysShadowBlobs ([[maybe_unused]] const RenderListsFrame& st)
{
#ifdef MC2_VULKAN_ISLAND
	const bool vkLegacyOn = st.vkLegacyOn;
#endif
	// Cement overlays (MC2_ISCRATERS|MC2_ISTERRAIN) and decals (!MC2_ISTERRAIN|MC2_ISCRATERS)
	// are now drawn by gos_DrawTerrainOverlays() and gos_DrawDecals() before Render.Overlays.
	// The old Render.CraterOverlays and non-terrain crater loops are removed.

	{
	ZoneScopedN("RenderLists.ShadowBlobs");
	rlcost::Span _rl(rlcost::kShadowBlobs);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Blobs);
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState(	gos_State_ZWrite, 1);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}
	else
	{
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		// sebi: shadows do not draw in depth, we do not want z-fighting
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_ZCompare, 2);
	}

	//NEVER draw shadows in Software.
	if (Environment.Renderer != 3)
	{
		for (int i=0;i<nextAvailableVertexNode;i++)
		{
			 if	((masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
				(masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
				(masterVertexNodes[i].vertices))
			{
#ifdef MC2_VULKAN_ISLAND
				if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_SHADOW, 0); continue; }   // VK-PARITY-F1
#endif
				DWORD totalVertices = masterVertexNodes[i].numVertices;
				if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
				{
					totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
				}
			
				if (totalVertices && (totalVertices < MAX_SENDDOWN))
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
				}
				else if (totalVertices > MAX_SENDDOWN)
				{
					gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
					
					//Must divide up vertices into batches of 10,000 each to send down.
					// Somewhere around 20000 to 30000 it really gets screwy!!!
					long currentVertices = 0;
					while (currentVertices < totalVertices)
					{
						gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
						long tVertices = totalVertices - currentVertices;
						if (tVertices > MAX_SENDDOWN)
							tVertices = MAX_SENDDOWN;
						
						gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
						
						currentVertices += tVertices;
					}
				}
				
				//Reset the list to zero length to avoid drawing more then once!
				//Also comes in handy if gameLogic is not called.
				masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
			}
		}
	}
#ifdef MC2_VULKAN_ISLAND
	if (vkLegacyOn)
		vkLegacyRun();   // VK-PARITY-F1: queued legacy blob-shadow nodes -> shared GBuffer
#endif
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Blobs);
	} // end RenderLists.ShadowBlobs
}

void MC_TextureManager::renderListsOverlaysNonTerrainAlpha ([[maybe_unused]] const RenderListsFrame& st)
{
#ifdef MC2_VULKAN_ISLAND
	const bool vkLegacyOn = st.vkLegacyOn;
#endif
	{
	ZoneScopedN("RenderLists.NonTerrainAlphaLoops");
	rlcost::Span _rl(rlcost::kAlphaLoops);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_AlphaVfx);
	gos_SetRenderState( gos_State_ZCompare, 1);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		// Blended alpha must not write depth: these are sorted-alpha nodes,
		// not the opaque/masked prepass. Applies to both the pure-blend
		// (states=0) and alpha-test cutout (states=1) loops, conservatively.
		gos_SetRenderState(	gos_State_ZWrite, 0);
	}

    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
    for(int states = 0; states < 2; ++states)
    {
        gos_SetRenderState( gos_State_AlphaTest, states);
        for (int i=0;i<nextAvailableVertexNode;i++)
        {
            if (!(masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    !(masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
                    !(masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
                    (masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (masterVertexNodes[i].vertices))
            {
#ifdef MC2_VULKAN_ISLAND
                if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_ALPHA, (uint32_t)states); continue; }   // VK-PARITY-F1
#endif
                DWORD totalVertices = masterVertexNodes[i].numVertices;
                if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
                {
                    totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
                }

                if (totalVertices && (totalVertices < MAX_SENDDOWN))
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
                    gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
                }
                else if (totalVertices > MAX_SENDDOWN)
                {
                    gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());

                    //Must divide up vertices into batches of 10,000 each to send down.
                    // Somewhere around 20000 to 30000 it really gets screwy!!!
                    long currentVertices = 0;
                    while (currentVertices < totalVertices)
                    {
                        gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
                        long tVertices = totalVertices - currentVertices;
                        if (tVertices > MAX_SENDDOWN)
                            tVertices = MAX_SENDDOWN;

                        gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );

                        currentVertices += tVertices;
                    }
                }

                //Reset the list to zero length to avoid drawing more then once!
                //Also comes in handy if gameLogic is not called.
                masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);
	if (Environment.Renderer != 3)
	{
		// Restore legacy depth-write state for downstream passes (they
		// inherit render state from this block).
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_AlphaVfx);
	} // end RenderLists.NonTerrainAlphaLoops
}

void MC_TextureManager::renderListsOverlaysVfxHudSubmit ([[maybe_unused]] const RenderListsFrame& st)
{
#ifdef MC2_VULKAN_ISLAND
	const bool vkLegacyOn = st.vkLegacyOn;
#endif
	{
	ZoneScopedN("RenderLists.VfxHudSubmit");
	rlcost::Span _rl(rlcost::kVfxHud);
	gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Hud);
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState( gos_State_Fog, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneOne);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulateAlpha);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_MonoEnable, 0);
	}
				
	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISEFFECTS) &&
			(masterVertexNodes[i].vertices))
		{
#ifdef MC2_VULKAN_ISLAND
			if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_ADD, 0); continue; }   // VK-PARITY-F1
#endif
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	// Spotlight cones are blended/additive: no depth writes during the draw.
	// Downstream state is unchanged -- the post-loop reset below already
	// sets ZWrite 0 (legacy behavior).
	gos_SetRenderState(	gos_State_ZWrite, 0);

	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISSPOTLGT) &&
			(masterVertexNodes[i].vertices))
		{
#ifdef MC2_VULKAN_ISLAND
			if (vkLegacyOn) { vkLegacySubmitNode(i, MC2_LEGACY_PASS_ADD, 0); continue; }   // VK-PARITY-F1
#endif
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
#ifdef MC2_VULKAN_ISLAND
	if (vkLegacyOn)
		vkLegacyRun();   // VK-PARITY-F1: queued legacy alpha + additive effect / spotlight-cone nodes
#endif
	gos_SetRenderState( gos_State_ZWrite, 0);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_Perspective, 1);
 	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_AlphaTest, 1);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
	
 	for (int i=0;i<nextAvailableVertexNode;i++)
	{
		if ((masterVertexNodes[i].flags & MC2_ISHUDLMNT) &&
			(masterVertexNodes[i].vertices))
		{
			DWORD totalVertices = masterVertexNodes[i].numVertices;
			if (masterVertexNodes[i].currentVertex != (masterVertexNodes[i].vertices + masterVertexNodes[i].numVertices))
			{
				totalVertices = masterVertexNodes[i].currentVertex - masterVertexNodes[i].vertices;
			}
			
			if (totalVertices && (totalVertices < MAX_SENDDOWN))
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				gos_RenderIndexedArray( masterVertexNodes[i].vertices, totalVertices, indexArray, totalVertices );
			}
			else if (totalVertices > MAX_SENDDOWN)
			{
				gos_SetRenderState( gos_State_Texture, masterTextureNodes[masterVertexNodes[i].textureIndex].get_gosTextureHandle());
				
				//Must divide up vertices into batches of 10,000 each to send down.
				// Somewhere around 20000 to 30000 it really gets screwy!!!
				long currentVertices = 0;
				while (currentVertices < totalVertices)
				{
					gos_VERTEX *v = masterVertexNodes[i].vertices + currentVertices;
					long tVertices = totalVertices - currentVertices;
					if (tVertices > MAX_SENDDOWN)
						tVertices = MAX_SENDDOWN;
					
					gos_RenderIndexedArray(v, tVertices, indexArray, tVertices );
					
					currentVertices += tVertices;
				}
			}
	
			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterVertexNodes[i].currentVertex = masterVertexNodes[i].vertices;
		}
	}

	//Must turn zCompare back on for FXs
	gos_SetRenderState( gos_State_ZCompare, 1 );

	// Reset terrain extra buffer after rendering — will be re-filled during next frame's TerrainQuad::draw() calls
	gos_TerrainExtraReset();
	gos_render_pass_timer::End(gos_render_pass_timer::Pass_Hud);
	} // end RenderLists.VfxHudSubmit
}

void MC_TextureManager::renderListsOverlaysOld (const RenderListsFrame& st)
{
	{
		ZoneScopedN("Render.Overlays"); GosGlLostDrawsScope _lost_("overlays.overlays_old");
		TracyGpuZone("Render.Overlays");
	if (Environment.Renderer == 3)
	{
		//Do NOT draw the water as transparent in software
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
	}
	
	renderListsOverlaysWaterLoops();
	renderListsOverlaysNoUnderlayer();
	renderListsOverlaysShadowBlobs(st);
	renderListsOverlaysNonTerrainAlpha(st);
	renderListsOverlaysVfxHudSubmit(st);
	} // end Render.Overlays zone
}
