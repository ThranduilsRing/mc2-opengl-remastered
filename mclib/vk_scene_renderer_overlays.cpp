#include "vk_scene_renderer.h"
// Legacy overlay state blocks and emit bodies are dispatched by a pure schedule.
#include "vk_scene_renderer_internal.h"
#include "vk_scene_renderer_overlays_algo.h"

void mc2::VkSceneRenderer::renderListsOverlaysOld (RenderSubmissions& submissions)
{
    ZoneScopedN("Render.Overlays");
    using namespace mc2_overlay_algo;
    constexpr Masks masks{MC2_ISTERRAIN, MC2_DRAWALPHA, MC2_ISSHADOWS,
        MC2_ISEFFECTS, MC2_ISCOMPASS, MC2_ISSPOTLGT, MC2_ISHUDLMNT,
        MC2_ALPHATEST, MC2_ISWATER, MC2_ISWATERDETAIL, MC2_GPUOVERLAY};
    Schedule(submissions.nextAvailableVertexNode, Environment.Renderer == 3, masks,
        [&](int i) {
            const auto& n = submissions.masterVertexNodes[i];
            return Node{static_cast<uint32_t>(n.flags), n.vertices != nullptr,
                static_cast<uint32_t>(n.numVertices),
                n.vertices ? static_cast<uint32_t>(n.currentVertex - n.vertices) : 0u};
        },
        [&](const Event& event) {
            const int i = event.node;
            const int states = event.value;
            switch (event.op) {
            case Op::RenderStateBlock:
                switch (static_cast<Block>(event.value)) {
                case Block::Initial: {
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

                    break;
                }
                case Block::WaterNode: {
                {
                    int waterMode = 0;
                    if (submissions.masterVertexNodes[i].flags & MC2_ISWATER) waterMode = 1;
                    else if (submissions.masterVertexNodes[i].flags & MC2_ISWATERDETAIL) waterMode = 2;
                    gos_SetRenderState(gos_State_Water, waterMode);
                }
                    break;
                }
                case Block::NoUnderlayer: {
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

                    break;
                }
                case Block::Shadow: {
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

                    break;
                }
                case Block::Alpha: {
	gos_SetRenderState( gos_State_ZCompare, 1);
	if (Environment.Renderer != 3)
	{
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		// Blended alpha must not write depth: these are sorted-alpha nodes,
		// not the opaque/masked prepass. Applies to both the pure-blend
		// (states=0) and alpha-test cutout (states=1) loops, conservatively.
		gos_SetRenderState(	gos_State_ZWrite, 0);
	}

                    break;
                }
                case Block::AlphaRestore: {
	if (Environment.Renderer != 3)
	{
		// Restore legacy depth-write state for downstream passes (they
		// inherit render state from this block).
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
                    break;
                }
                case Block::Effects: {
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
                    break;
                }
                case Block::Spotlight: {
	gos_SetRenderState(	gos_State_ZWrite, 0);

                    break;
                }
                case Block::Compass: {
	gos_SetRenderState( gos_State_ZWrite, 0);
	gos_SetRenderState( gos_State_ZCompare, 0);
	gos_SetRenderState( gos_State_Perspective, 1);
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_AlphaTest, 1);
	
                    break;
                }
                case Block::Hud: {
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);

                    break;
                }
                case Block::End: {
	gos_SetRenderState( gos_State_ZCompare, 1 );

                    break;
                }
                }
                break;
            case Op::AlphaTest:
                gos_SetRenderState( gos_State_AlphaTest, states);
                break;
            case Op::Submit:
                switch (event.pass) {
                case Pass::Shadow:
                vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_SHADOW, 0);
                    break;
                case Pass::Alpha:
                vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_ALPHA, static_cast<uint32_t>(states));
                    break;
                case Pass::Effects:
                case Pass::Spotlight:
                vkLegacySubmitNode(submissions, i, MC2_LEGACY_PASS_ADD, 0);
                    break;
                default: break; // Reset-only passes never submit.
                }
                break;
            case Op::ResetVertex:
                submissions.masterVertexNodes[i].currentVertex = submissions.masterVertexNodes[i].vertices;
                break;
            case Op::Flush:
                vkLegacyRun(submissions);
                break;
            case Op::PassBegin:
            case Op::PassEnd:
                break; // Ordering markers; state changes have their own events.
            }
        });
}
