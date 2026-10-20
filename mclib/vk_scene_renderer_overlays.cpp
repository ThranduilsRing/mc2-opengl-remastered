#include "vk_scene_renderer.h"
// mc2::VkSceneRenderer::renderLists phase 12: the legacy overlay pass (water loops, no-underlayer,
// shadow blobs, non-terrain alpha, vfx / hud submit).

#include "vk_scene_renderer_internal.h"

void mc2::VkSceneRenderer::renderListsOverlaysWaterLoops (RenderSubmissions& submissions)
{
    // sebi: split in 2 parts, first draw objects which have alpha test off, then with alpha test on
	{
	ZoneScopedN("RenderLists.TerrainAlphaWaterLoops");
    for(int states = 0; states < 2; ++states)
    {
        gos_SetRenderState( gos_State_AlphaTest, states);

        for (int i=0;i<submissions.nextAvailableVertexNode;i++)
        {
            if ((submissions.masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    (submissions.masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (submissions.masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (submissions.masterVertexNodes[i].vertices))
            {
                // The legacy non-water terrain alpha/detail layer sits on the original
                // flat terrain plane and shows through displaced tess terrain as the
                // dark striped under-pattern. Keep water passes, but drop this layer.
                if (!(submissions.masterVertexNodes[i].flags & MC2_ISWATER) &&
                    !(submissions.masterVertexNodes[i].flags & MC2_ISWATERDETAIL)) {
                    submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
                    continue;
                }

                {
                    int waterMode = 0;
                    if (submissions.masterVertexNodes[i].flags & MC2_ISWATER) waterMode = 1;
                    else if (submissions.masterVertexNodes[i].flags & MC2_ISWATERDETAIL) waterMode = 2;
                    gos_SetRenderState(gos_State_Water, waterMode);
                }

                submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
            }
        }
    }
    //reset alpha test at the end
    gos_SetRenderState( gos_State_AlphaTest, 0);
	} // end RenderLists.TerrainAlphaWaterLoops
}

void mc2::VkSceneRenderer::renderListsOverlaysNoUnderlayer (RenderSubmissions& submissions)
{
	//<< sebi: added this section to draw objects which do not have terrain underlayer (those are added in quad.cpp, see (*) there )
	{ ZoneScopedN("Render.NoUnderlayer");
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	for (int i=0;i<submissions.nextAvailableVertexNode;i++)
	{
		if ((submissions.masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
			!(submissions.masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
			(submissions.masterVertexNodes[i].flags & MC2_GPUOVERLAY) &&
			(submissions.masterVertexNodes[i].vertices))
		{
			submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
		}
	}
	} // end ZoneScopedN("Render.NoUnderlayer")
	//<< sebi: end of added block
}

void mc2::VkSceneRenderer::renderListsOverlaysShadowBlobs (RenderSubmissions& submissions)
{
	// Cement overlays (MC2_ISCRATERS|MC2_ISTERRAIN) and decals (!MC2_ISTERRAIN|MC2_ISCRATERS)
	// are now drawn by gos_DrawTerrainOverlays() and gos_DrawDecals() before Render.Overlays.
	// The old Render.CraterOverlays and non-terrain crater loops are removed.

	{
	ZoneScopedN("RenderLists.ShadowBlobs");
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
		for (int i=0;i<submissions.nextAvailableVertexNode;i++)
		{
			 if	((submissions.masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
				(submissions.masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
				(submissions.masterVertexNodes[i].vertices))
			{
#ifdef MC2_VULKAN_ISLAND
				vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_SHADOW, 0);
#endif
				submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
			}
		}
	}
#ifdef MC2_VULKAN_ISLAND
	vkLegacyRun(submissions);   // VK-PARITY-F1: queued legacy blob-shadow nodes -> shared GBuffer
#endif
	} // end RenderLists.ShadowBlobs
}

void mc2::VkSceneRenderer::renderListsOverlaysNonTerrainAlpha (RenderSubmissions& submissions)
{
	{
	ZoneScopedN("RenderLists.NonTerrainAlphaLoops");
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
        for (int i=0;i<submissions.nextAvailableVertexNode;i++)
        {
            if (!(submissions.masterVertexNodes[i].flags & MC2_ISTERRAIN) &&
                    !(submissions.masterVertexNodes[i].flags & MC2_ISSHADOWS) &&
                    !(submissions.masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
                    (submissions.masterVertexNodes[i].flags & MC2_DRAWALPHA) &&
                    (submissions.masterVertexNodes[i].flags & MC2_ALPHATEST)==states*MC2_ALPHATEST &&
                    (submissions.masterVertexNodes[i].vertices))
            {
#ifdef MC2_VULKAN_ISLAND
                vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_ALPHA, static_cast<uint32_t>(states));
#endif
                submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
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
	} // end RenderLists.NonTerrainAlphaLoops
}

void mc2::VkSceneRenderer::renderListsOverlaysVfxHudSubmit (RenderSubmissions& submissions)
{
	{
	ZoneScopedN("RenderLists.VfxHudSubmit");
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
				
	for (int i=0;i<submissions.nextAvailableVertexNode;i++)
	{
		if ((submissions.masterVertexNodes[i].flags & MC2_ISEFFECTS) &&
			(submissions.masterVertexNodes[i].vertices))
		{
#ifdef MC2_VULKAN_ISLAND
			vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_ADD, 0);
#endif
			submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
		}
	}
	
	// Spotlight cones are blended/additive: no depth writes during the draw.
	// Downstream state is unchanged -- the post-loop reset below already
	// sets ZWrite 0 (legacy behavior).
	gos_SetRenderState(	gos_State_ZWrite, 0);

	for (int i=0;i<submissions.nextAvailableVertexNode;i++)
	{
		if ((submissions.masterVertexNodes[i].flags & MC2_ISSPOTLGT) &&
			(submissions.masterVertexNodes[i].vertices))
		{
#ifdef MC2_VULKAN_ISLAND
			vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_ADD, 0);
#endif
			submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
		}
	}
	
#ifdef MC2_VULKAN_ISLAND
	vkLegacyRun(submissions);   // VK-PARITY-F1: queued legacy alpha + additive effect / spotlight-cone nodes
#endif
	gos_SetRenderState( gos_State_ZWrite, 0);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_Perspective, 1);
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_AlphaTest, 1);
	
	for (int i=0;i<submissions.nextAvailableVertexNode;i++)
	{
		if ((submissions.masterVertexNodes[i].flags & MC2_ISCOMPASS) &&
			(submissions.masterVertexNodes[i].vertices))
		{
			submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
		}
	}
	
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
	
	for (int i=0;i<submissions.nextAvailableVertexNode;i++)
	{
		if ((submissions.masterVertexNodes[i].flags & MC2_ISHUDLMNT) &&
			(submissions.masterVertexNodes[i].vertices))
		{
			submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
		}
	}

	//Must turn zCompare back on for FXs
	gos_SetRenderState( gos_State_ZCompare, 1 );

	// Reset terrain extra buffer after rendering — will be re-filled during next frame's TerrainQuad::draw() calls
	} // end RenderLists.VfxHudSubmit
}

void mc2::VkSceneRenderer::renderListsOverlaysOld (RenderSubmissions& submissions)
{
	{
		ZoneScopedN("Render.Overlays");
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
	
	renderListsOverlaysWaterLoops(submissions);
	renderListsOverlaysNoUnderlayer(submissions);
	renderListsOverlaysShadowBlobs(submissions);
	renderListsOverlaysNonTerrainAlpha(submissions);
	renderListsOverlaysVfxHudSubmit(submissions);
	} // end Render.Overlays zone
}
