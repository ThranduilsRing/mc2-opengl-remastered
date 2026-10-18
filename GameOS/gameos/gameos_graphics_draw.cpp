// gameos_graphics: gosMesh draw calls, render-state push/pop and apply, frame begin/end, material selection and the quad/line/point/triangle draw paths.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"
#include "utils/mc2log.h"


void gosRenderer::initRenderStates() {
    // RENDER_STATES v1: zero-init the entire arrays so the equality check has
    // a defined comparison for states that initRenderStates does not assign
    // explicitly (Terrain, Water, Overlay, IsHUD, Texture3). Without this,
    // uninitialized members at non-explicit indices could differ between
    // curStates_ and renderStates_ on every call and defeat the early-out.
    memset(&curStates_, 0, sizeof(curStates_));
    memset(&renderStates_, 0, sizeof(renderStates_));

	renderStates_[gos_State_Texture] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Texture2] = INVALID_TEXTURE_ID;
    renderStates_[gos_State_Texture3] = INVALID_TEXTURE_ID;
	renderStates_[gos_State_Filter] = gos_FilterNone;
	renderStates_[gos_State_ZCompare] = 1; 
    renderStates_[gos_State_ZWrite] = 1;
	renderStates_[gos_State_AlphaTest] = 0;
	renderStates_[gos_State_Perspective] = 1;
	renderStates_[gos_State_Specular] = 0;
	renderStates_[gos_State_Dither] = 0;
	renderStates_[gos_State_Clipping] = 0;	
	renderStates_[gos_State_WireframeMode] = 0;
	renderStates_[gos_State_AlphaMode] = gos_Alpha_OneZero;
	renderStates_[gos_State_TextureAddress] = gos_TextureWrap;
	renderStates_[gos_State_ShadeMode] = gos_ShadeGouraud;
	renderStates_[gos_State_TextureMapBlend] = gos_BlendModulateAlpha;
	renderStates_[gos_State_MipMapBias] = 0;
	renderStates_[gos_State_Fog]= 0;
	renderStates_[gos_State_MonoEnable] = 0;
	renderStates_[gos_State_Culling] = gos_Cull_None;
	renderStates_[gos_State_StencilEnable] = 0;
	renderStates_[gos_State_StencilFunc] = gos_Cmp_Never;
	renderStates_[gos_State_StencilRef] = 0;
	renderStates_[gos_State_StencilMask] = 0xffffffff;
	renderStates_[gos_State_StencilZFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilFail] = gos_Stencil_Keep;
	renderStates_[gos_State_StencilPass] = gos_Stencil_Keep;
	renderStates_[gos_State_Multitexture] = gos_Multitexture_None;
	renderStates_[gos_State_Ambient] = 0xffffff;
	renderStates_[gos_State_Lighting] = 0;
	renderStates_[gos_State_NormalizeNormals] = 0;
	renderStates_[gos_State_VertexBlend] = 0;

    applyRenderStates();
    renderStatesStackPointer = -1;
}

void gosRenderer::pushRenderStates()
{
    gosASSERT(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1);
    if(!(renderStatesStackPointer>=-1 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE - 1)) {
        return;
    }

    renderStatesStackPointer++;
    memcpy(&statesStack_[renderStatesStackPointer], &renderStates_, sizeof(renderStates_));
}

void gosRenderer::popRenderStates()
{
    gosASSERT(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE);
    
    if(!(renderStatesStackPointer>=0 && renderStatesStackPointer < RENDER_STATES_STACK_SIZE)) {
        return;
    }

    memcpy(&renderStates_, &statesStack_[renderStatesStackPointer], sizeof(renderStates_));
    renderStatesStackPointer--;
}

// RENDER_STATES v1: kill-switch (default-off → short-circuit path runs).
// MC2_RENDERSTATES_LEGACY=1 forces unconditional apply behavior.
static bool gos_renderStatesLegacyEnabled() {
    static const bool s = mc2::gate::get<"MC2_RENDERSTATES_LEGACY">();
    return s;
}

void gosRenderer::applyRenderStates() {
    ZoneScopedN("ApplyRenderStates");

    rsCalls_++;

    // RENDER_STATES v1: state-equality early-out.
    // Auto-reset of Terrain/Water flags must run every call regardless (these are
    // single-frame "intent" bits cleared after read). gos_State_Overlay is copied,
    // not auto-reset — managed by renderLists. We therefore split the auto-reset
    // out so it always runs, then test the rest of state for equality.
    const bool legacy = gos_renderStatesLegacyEnabled();
    if (!legacy && stateCacheValid_) {
        bool equal = true;
        int mismatchK = -1;
        uint32_t mismatchCur = 0, mismatchRen = 0;
        // Iterate valid enum values [gos_State_Texture .. gos_MaxState). Index 0
        // is not a valid enum (gos_State_Texture==1) and is never written by
        // initRenderStates(), so its value is indeterminate and may differ
        // between curStates_ / renderStates_ — skip it.
        for (int k = gos_State_Texture; k < gos_MaxState; ++k) {
            if (k == gos_State_Texture || k == gos_State_Texture2 || k == gos_State_Texture3) {
                continue; // texture handles mutate per-frame; checked separately below
            }
            // Terrain/Water are auto-reset bits — renderStates_[k] is cleared after
            // each apply, so curStates_[k] will diverge if a caller set it; that is
            // exactly the signal we want to force a full apply through.
            if (curStates_[k] != renderStates_[k]) {
                equal = false;
                mismatchK = k;
                mismatchCur = curStates_[k];
                mismatchRen = renderStates_[k];
                break;
            }
        }
        // Optional diagnostic: print the first 8 mismatch reasons. Env-gated,
        // off by default per Debug Instrumentation Rule (keep, demote, don't
        // delete). MC2_RENDERSTATES_TRACE=1 to enable.
        static const bool s_rsTrace = mc2::gate::get<"MC2_RENDERSTATES_TRACE">();
        static int s_mismatchPrints = 0;
        if (s_rsTrace && !equal && s_mismatchPrints < 8) {
            s_mismatchPrints++;
            mc2::fmt::print_out("[RENDER_STATES v1] event=mismatch_first idx={:d} cur={:d} ren={:d}\n", mc2::fmt::as_i32(mismatchK), mc2::fmt::as_u32(mismatchCur), mc2::fmt::as_u32(mismatchRen));
            fflush(stdout);
        }
        if (equal) {
            // Resolved-handle parity: check the underlying GL texture id, not the
            // gos handle slot (handles mutate per-frame via tex_resolve).
            uint32_t tex_states[] = { gos_State_Texture, gos_State_Texture2, gos_State_Texture3 };
            for (int i = 0; i < 3 && equal; ++i) {
                DWORD h = renderStates_[tex_states[i]];
                gosTexture* tex = (h == INVALID_TEXTURE_ID) ? nullptr : this->getTexture(h);
                uint32_t glId = tex ? tex->getTextureId() : 0u;
                if (glId != cachedResolvedTexId_[i]) { equal = false; break; }
                if (curStates_[tex_states[i]] != h) { equal = false; break; }
            }
            if (equal) {
                // Skip GL work. Still run auto-reset bookkeeping so Terrain/Water
                // intent bits don't bleed into the next frame.
                curStates_[gos_State_Terrain] = renderStates_[gos_State_Terrain];
                renderStates_[gos_State_Terrain] = 0;
                curStates_[gos_State_Water]   = renderStates_[gos_State_Water];
                renderStates_[gos_State_Water]   = 0;
                curStates_[gos_State_Overlay] = renderStates_[gos_State_Overlay];
                rsSkipped_++;
                return;
            }
        }
    }

    rsApplied_++;

	switch (renderStates_[gos_State_Culling]) {
		case gos_Cull_None: glDisable(GL_CULL_FACE); break;
		case gos_Cull_CW:	
		case gos_Cull_CCW:
			glEnable(GL_CULL_FACE);
			// by default in OpenGL front face is CCW (could be changed by glFrontFace)
			glCullFace(renderStates_[gos_State_Culling] == gos_Cull_CW ? GL_BACK : GL_FRONT);
			break;
		default: gosASSERT(0 && "Wrong cull face value");
	}
	curStates_[gos_State_Culling] = renderStates_[gos_State_Culling];


	fog_color_ = uint32_to_vec4(renderStates_[gos_State_Fog]);
	curStates_[gos_State_Fog] = renderStates_[gos_State_Fog];

   switch(renderStates_[gos_State_ZWrite]) {
       case 0: glDepthMask(GL_FALSE); break;
       case 1: glDepthMask(GL_TRUE); break;
       default: gosASSERT(0 && "Wrong depth write value");
   }
   curStates_[gos_State_ZWrite] = renderStates_[gos_State_ZWrite];

   if(0 == renderStates_[gos_State_ZCompare]) {
       glDisable(GL_DEPTH_TEST);
   } else {
       glEnable(GL_DEPTH_TEST);
   }
   // Reverse-Z (U2): gos_State_ZCompare is the SCENE depth-compare bridge.
   // All gos_State_ZCompare writers (incl. txmmgr.cpp:2316/2327 and
   // objmgr.cpp:1826 legacy CPU blob-shadow quads, which depth-test
   // against the SCENE depth buffer) are scene-space; the ortho
   // shadow-MAP passes never route through gos_State_ZCompare (they set
   // glDepthFunc(GL_LESS) via the literal path in gameos_graphics.cpp
   // 4436/4580/4650 + gos_postprocess.cpp 1139/1163 under their own FBO).
   // So the scene remap applies globally here; no shadow carve-out.
   switch(renderStates_[gos_State_ZCompare]) {
       case 0: glDepthFunc(GL_ALWAYS);  break;   // unchanged
       case 1: glDepthFunc(GL_GEQUAL);  break;   // reverse-Z: was GL_LEQUAL
       case 2: glDepthFunc(GL_GREATER); break;   // reverse-Z: was GL_LESS
       default: gosASSERT(0 && "Wrong depth test value");
   }
   curStates_[gos_State_ZCompare] = renderStates_[gos_State_ZCompare];

   bool disable_blending = renderStates_[gos_State_AlphaMode] == gos_Alpha_OneZero;
   if(disable_blending) {
       glDisable(GL_BLEND);
   } else {
       glEnable(GL_BLEND);
   }
   switch(renderStates_[gos_State_AlphaMode]) {
       case gos_Alpha_OneZero:          glBlendFunc(GL_ONE, GL_ZERO); break;
       case gos_Alpha_OneOne:           glBlendFunc(GL_ONE, GL_ONE); break;
       case gos_Alpha_AlphaInvAlpha:    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_OneInvAlpha:      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
       case gos_Alpha_AlphaOne:         glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
       default: gosASSERT(0 && "Wrong alpha mode value");
   }
   curStates_[gos_State_AlphaMode] = renderStates_[gos_State_AlphaMode];

   #if 0 // now in shaders (this is not supported in CORE OpenGL profile
   bool enable_alpha_test = renderStates_[gos_State_AlphaTest] == 1;
   if(enable_alpha_test) {
       glEnable(GL_ALPHA_TEST);
       glAlphaFunc(GL_NOTEQUAL, 0.0f);
   } else {
       glDisable(GL_ALPHA_TEST);
   }
   #endif
   curStates_[gos_State_AlphaTest] = renderStates_[gos_State_AlphaTest];

   TexFilterMode filter = TFM_NONE;
   switch(renderStates_[gos_State_Filter]) {
       case gos_FilterNone: filter = TFM_NEAREST; break;
       case gos_FilterBiLinear : filter = TFM_LINEAR; break;
       case gos_FilterTriLinear: filter = TFM_LNEAR_MIPMAP_LINEAR; break;
   }
   // no mips for now, so ensure no invalid filters used
   //gosASSERT(filter == TFM_NEAREST || filter == TFM_LINEAR);
   // i do not know of any mipmaps that we are using
   if(filter == TFM_LNEAR_MIPMAP_LINEAR)
       filter = TFM_LINEAR;

   // in this case does not necessaily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_Filter] = renderStates_[gos_State_Filter];
  
   TexAddressMode address_mode = 
       renderStates_[gos_State_TextureAddress] == gos_TextureWrap ? TAM_REPEAT : TAM_CLAMP_TO_EDGE;
   // in this case does not necessarily mean, that state was set, because in OpenGL this is binded to texture (unless separate sampler state extension is used, which is not currently)
   curStates_[gos_State_TextureAddress] = renderStates_[gos_State_TextureAddress];

   uint32_t tex_states[] = { gos_State_Texture, gos_State_Texture2, gos_State_Texture3 };
   for(int i=0; i<sizeof(tex_states) / sizeof(tex_states[0]); ++i) {
       DWORD gosTextureHandle = renderStates_[tex_states[i]];

       glActiveTexture(GL_TEXTURE0 + i);

       gosTexture* tex = gosTextureHandle == INVALID_TEXTURE_ID ? nullptr : this->getTexture(gosTextureHandle);
       if(tex) {
           const uint32_t glId = tex->getTextureId();
           glBindTexture(GL_TEXTURE_2D, glId);
           setSamplerParams(tex->getTextureType(), address_mode, filter);
           cachedResolvedTexId_[i] = glId;

           gosTextureInfo texinfo;
           tex->getTextureInfo(&texinfo);
           if(renderStates_[gos_State_TextureMapBlend] == gos_BlendDecal && texinfo.format_ == gos_Texture_Alpha)
           {
               PAUSE((""));
           }

       } else {
           glBindTexture(GL_TEXTURE_2D, 0);
           cachedResolvedTexId_[i] = 0u;
       }
       curStates_[tex_states[i]] = gosTextureHandle;
   }

   ////////////////////////////////////////////////////////////////////////////////
   // RENDER_STATES v1: snapshot the full renderStates_ array into curStates_ so
   // the equality check has a coherent "last applied" reference for ALL state
   // dimensions. Many states (Perspective, Specular, ShadeMode, TextureMapBlend,
   // ...) aren't applied to GL at all (commented "now in shaders") but are still
   // mutated by callers per-shape — without this snapshot they'd never match
   // and the early-out would always fail.
   memcpy(&curStates_, &renderStates_, sizeof(curStates_));

   // Terrain tessellation flag — auto-reset to prevent bleed to non-terrain
   // draws. The snapshot above already copied the true value; clear renderStates_
   // AFTER so the next call sees curStates_[Terrain]=value, renderStates_=0 and
   // forces a full apply if a caller re-sets Terrain.
   renderStates_[gos_State_Terrain] = 0;
   renderStates_[gos_State_Water]   = 0;
   // Overlay is NOT auto-reset — managed by renderLists loop. Snapshot above
   // already mirrored its value.

   // RENDER_STATES v1: full-apply complete — cache is now coherent with GL.
   stateCacheValid_ = true;
}

void gosRenderer::beginFrame()
{
    // RENDER_STATES v1: defensive cache invalidate at frame start. SDL window
    // swap, ImGui (if added), or any external GL hook between frames could
    // disturb GL state without the cache knowing. Cost is one redundant full
    // apply per frame at startup; the [RENDER_STATES v1] summary surfaces any
    // unexpected impact. MINOR-1 from the 2026-05-08 adversarial review.
    invalidateRenderStateCache();

    // Frame-boundary hygiene: IsHUD must be cleared by every callsite before the frame ends.
    // If it is still set here, a callsite leaked the bit across the frame boundary.
    if (renderStates_[gos_State_IsHUD] != 0) {
        MC2_LOG(Render, Debug, "[HUD] gos_State_IsHUD still set at frame start -- callsite leak");
        renderStates_[gos_State_IsHUD] = 0;
    }
    // HUD-SCALE-EXEMPT-LEAK-1: same hygiene for the scale-exempt bracket. The
    // cursor's z-order fix re-issues userInput->render() as a PostImGuiRender
    // callback, which runs AFTER flushHUDBatch's end-of-flush exempt reset —
    // its gos_SetHudScaleExempt(true) then leaked into the whole NEXT frame's
    // HUD recording, tagging every call exempt and silently no-opping the 85%
    // HUD shrink (observed: [HUDSCALE] apply skipExempt=100 scaled=0).
    s_hud_scale_exempt = false;
    hudBatch_.clear();
    hudFlushed_ = false;
    glBindVertexArray(gVAO);
    num_draw_calls_ = 0;

    TerrainPatchStream::beginFrame();
    // gos_terrain_indirect::BeginFrame() moved to endFrame() — see comment there.
}

// Lazy-eval gate for the dev-only shader hot-reload sweep. Default OFF in
// shipping; set MC2_SHADER_HOT_RELOAD=1 to opt in for shader iteration.
// Pattern matches gos_terrain_indirect::IsEnabled() — static-init-order safe.
// Diagnostic 2026-05-07: the unconditional sweep showed 1.82 ms self-time
// inside Camera.UpdateRenderers/gos_RendererEndFrame in Tracy because
// last_check_time was never updated, so once 500 ms elapsed checkReload()
// ran every frame over materialList_.
static bool gos_ShaderHotReloadEnabled() {
    static const bool s = mc2::gate::get<"MC2_SHADER_HOT_RELOAD">();
    return s;
}

void gosRenderer::endFrame()
{
    // Clear the GPU-terrain arm AFTER renderLists() has consumed it this frame.
    // Must be end-of-frame, not begin-of-frame: DoGameLogic() (which calls
    // Terrain::geometry() → ComputePreflight() → arm) runs BEFORE draw_screen()
    // (which calls beginFrame() then renderLists()). Placing the reset in
    // beginFrame() wiped the arm before renderLists() could see it, causing
    // permanent black terrain. Menu / mech-bay frames never call ComputePreflight(),
    // so they never set the arm; the end-of-frame clear is a no-op for them.
    gos_terrain_indirect::BeginFrame();

    rsFrames_++;
    if (rsFrames_ >= 600) {
        MC2_DIAGF("[RENDER_STATES v1] event=summary frames=%u calls=%u skipped_count=%u applied_count=%u\n",
               rsFrames_, rsCalls_, rsSkipped_, rsApplied_);
        fflush(stdout);
        rsFrames_ = 0;
        rsCalls_ = 0;
        rsSkipped_ = 0;
        rsApplied_ = 0;
    }

    if (!gos_ShaderHotReloadEnabled())
        return;

    static uint64_t last_check_time = timing::get_wall_time_ms();
    const uint64_t now = timing::get_wall_time_ms();
    if(now - last_check_time > 500)
    {
        for(int i=0; i< materialList_.size(); ++i)
        {
            materialList_[i]->checkReload();
        }
        // Originally absent: without reassigning last_check_time the
        // condition stayed true every frame after the first 500 ms,
        // turning a 2 Hz cadence into a per-frame O(materialList_.size())
        // sweep. Diagnosed via [RENDER_STATES v1] / Tracy 2026-05-07.
        last_check_time = now;
    }
}

void gosRenderer::handleEvents()
{
    if(pendingRequest) {
        ZoneScopedN("gosRenderer::handleEvents pendingRequest");

        width_ = reqWidth;
        height_ = reqHeight;

        // x = 1/w; x =2*x - 1;
        // y = 1/h; y= 1- y; y =2*y - 1;
        // z = z;
        projection_ = mat4(2.0f / (float)width_, 0, 0.0f, -1.0f,
                0, -2.0f / (float)height_, 0.0f, 1.0f,
                0, 0, 1.0f, 0.0f,
                0, 0, 0.0f, 1.0f);

        // WINDOWED-8006-1: only resize the OS window on an EXPLICIT physical
        // request. reqPhysWidth==0 (the HUD-RES-clamped path) keeps the boot
        // window size — logical canvas lives in width_/height_/projection_.
        bool windowOk = true;
        if (reqPhysWidth > 0 && reqPhysHeight > 0)
        {
            ZoneScopedN("gosRenderer::handleEvents resizeWindow");
            windowOk = graphics::resize_window(win_h_, (int)reqPhysWidth, (int)reqPhysHeight);
        }
        {
        if(windowOk)
		{
            { ZoneScopedN("gosRenderer::handleEvents fullscreen"); graphics::set_window_fullscreen(win_h_, reqGotoFullscreen); }

            // NOFOCUS-HARDEN-1: this is the SetScreenMode re-issue path
            // (prefs.applyPrefs() replays options.cfg during boot). Under
            // MC2_WINDOW_OFFSCREEN re-assert the offscreen origin here, in the
            // same synchronous step that changed the mode, so the window is
            // never observable at a visible position between the two.
            graphics::enforce_window_offscreen(win_h_);

            Environment.screenWidth = width_;
            Environment.screenHeight = height_;

			{ ZoneScopedN("gosRenderer::handleEvents drawableSize"); graphics::get_drawable_size(win_h_, &Environment.drawableWidth, &Environment.drawableHeight); }

#ifdef MC2_IMGUI
            // Keep ImGui DisplaySize and FramebufferScale in sync with the
            // actual window after a resolution change.  Without this,
            // io.DisplaySize stays at the startup value (800x600) and all
            // ImGui layout, hit-testing, and text rendering use stale dimensions.
            GuiRuntime::NotifyResize(
                Environment.drawableWidth,
                Environment.drawableHeight,
                Environment.screenWidth,
                Environment.screenHeight);
#endif

        }
        }
        pendingRequest = false;
    }
}

bool gosRenderer::beforeDrawCall()
{
    num_draw_calls_++;
    if(break_draw_call_num_ == num_draw_calls_ && break_on_draw_call_) {
        PAUSE(("Draw call %d break\n", num_draw_calls_ - 1));
    }

    return (num_draw_calls_ > num_draw_calls_to_draw_) && num_draw_calls_to_draw_ != 0;
}

void gosRenderer::afterDrawCall()
{
}

gosRenderMaterial* gosRenderer::selectBasicRenderMaterial(const RenderState& rs) const
{
	ZoneScopedN("SelectBasicMaterial");
	const auto& sh_var = rs[gos_State_Texture]!=0 ?
		materialDB_.find("gos_tex_vertex")->second :
		materialDB_.find("gos_vertex")->second;
    uint32_t flags = rs[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;
    if (rs[gos_State_Overlay])
        flags |= SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::IS_OVERLAY);

    if(sh_var.count(flags))
        return sh_var.at(flags);
    else
    {
        STOP(("Trying to get variation which does not exist: shader: %s flags: %d\n", "basic", flags));
        return nullptr;
    }
}

gosRenderMaterial* gosRenderer::selectLightedRenderMaterial(const RenderState& rs) const
{
	const auto& sh_var = rs[gos_State_Texture]!=0 ?
		materialDB_.find("gos_tex_vertex_lighted")->second :
		materialDB_.find("gos_vertex_lighted")->second;
    uint32_t flags = rs[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;

    if(sh_var.count(flags))
        return sh_var.at(flags);
    else
    {
        STOP(("Trying to get variation which does not exist: shader: %s flags: %d\n", "lighted", flags));
        return nullptr;
    }
}

void gosRenderer::drawQuads(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawQuads");
    gosASSERT(vertices);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            MC2_LOG(Render, Debug, "[HUD] Late drawQuads discarded (after flushHUDBatch)");
            return;
        }
        HudDrawCall call;
        call.kind = kHudQuadBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        call.scaleExempt = s_hud_scale_exempt;
        call.canvasExempt = (s_hud_canvas_exempt_mode < 0)
            ? s_hud_scale_exempt : (s_hud_canvas_exempt_mode != 0);
        hudBatch_.push_back(std::move(call));
        return;
    }

    // World geometry reaches Vulkan through the typed scene feeds.
}

void gosRenderer::drawLines(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawLines");
    gosASSERT(vertices);

    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            MC2_LOG(Render, Debug, "[HUD] Late drawLines discarded (after flushHUDBatch)");
            return;
        }
        HudDrawCall call;
        call.kind = kHudLineBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        call.scaleExempt = s_hud_scale_exempt;
        call.canvasExempt = (s_hud_canvas_exempt_mode < 0)
            ? s_hud_scale_exempt : (s_hud_canvas_exempt_mode != 0);
        hudBatch_.push_back(std::move(call));
        return;
    }

    // World geometry reaches Vulkan through the typed scene feeds.
}

void gosRenderer::drawPoints(gos_VERTEX* vertices, int count) {
    // The legacy point rasterizer wrote only the discarded GL scene.
    gosASSERT(vertices);
}


// VK-VFX-COALESCE-1: consecutive-same-state HUD tri-batch merge in drawTris()
// below. MLRSortByOrder emits gosFX geometry (weapon tracers, muzzle flashes,
// impacts) one triangle at a time via gos_DrawTriangles -- under a weapon
// volley this pushed one HudDrawCall PER TRIANGLE (measured peak 251 hudBatch_
// entries/frame vs a 14-entry quiet floor). The merge is unconditional: its
// A/B (mc2_01, forced multi-mech fire) collapsed peak entries 349 -> 109 with
// [VK_HUD_AUDIT v1] vertex totals bit-identical to the per-triangle path
// (VK-ARC-STATE.md 2026-07-16).

void gosRenderer::drawTris(gos_VERTEX* vertices, int count) {
    ZoneScopedN("DrawTris");
    gosASSERT(vertices);

    gosASSERT((count % 3) == 0);

    if ( mc2::gate::get<"MC2_LOG_PREVIEW">() ) {
        GLint curFbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &curFbo);
        FILE* f = fopen("preview_debug.log","a");
        if (f) { mc2::fmt::print(f, "[PREVIEW] drawTris count={:d} isHUD={:d} curFbo={:d}\n", mc2::fmt::as_i32(count), mc2::fmt::as_i32((int)renderStates_[gos_State_IsHUD]), mc2::fmt::as_i32(curFbo)); fflush(f); fclose(f); }
    }
    if (renderStates_[gos_State_IsHUD]) {
        if (hudFlushed_) {
            MC2_LOG(Render, Debug, "[HUD] Late drawTris discarded (after flushHUDBatch)");
            return;
        }

        // VK-VFX-COALESCE-1: glue this triangle onto hudBatch_.back() instead of
        // pushing a new HudDrawCall when the back() entry is a compatible
        // kHudTriBatch (same render state / projection / scale-exemption).
        // Paint order is preserved EXACTLY: MLRSortByOrder already emits
        // triangles in depth-sorted order one at a time (SetDifferences forces
        // a state change -> a new hudBatch_ entry, same as before), so this
        // only glues ADJACENT same-state records together -- it never reorders.
        // flushHUDBatch/recordPendingHudColorProof already replay a variable-
        // length triangle-list per entry, so the merge is transparent downstream.
        //
        // HUD-SHRINK HAZARD: flushHUDBatch's bottom-band shrink transform
        // (s_hud_scale/s_hud_scale_active, ACTIVE by default in every mission --
        // s_hud_scale=0.85) makes ONE shrink/no-shrink decision per hudBatch_
        // entry from that entry's own vertex centroid. Two same-state triangles
        // adjacent in the GLOBAL depth sort can belong to different effects at
        // different screen positions (e.g. two mechs' lasers sharing one
        // texture/blend state) and independently straddle the 60%-height band.
        // A blind append would recompute ONE aggregate centroid for the merged
        // batch, which can flip the shrink decision for triangles that would
        // have decided the other way alone -- a real pixel shift, not just a
        // draw-count change. Guard: only merge when the incoming triangle's own
        // centroid falls on the SAME side of the band as the side already
        // established by the batch's FIRST triangle (back().vertices[0..3),
        // append-only so that snapshot never changes) -- mean of same-side
        // values stays same-side, so the merged decision stays bit-identical to
        // what every constituent triangle would have gotten on its own. When
        // the shrink test can't apply to this draw (out of mission, scale==1,
        // or scaleExempt) there is no per-vertex hazard and same-state adjacent
        // triangles may always merge.
        if (count == 3 && !hudBatch_.empty()) {
            HudDrawCall& back = hudBatch_.back();
            if (back.kind == kHudTriBatch &&
                !back.vertices.empty() &&
                back.scaleExempt == s_hud_scale_exempt &&
                memcmp(back.stateSnapshot, renderStates_, sizeof(back.stateSnapshot)) == 0 &&
                memcmp(&back.projection, &projection_, sizeof(back.projection)) == 0)
            {
                bool sameSideOk = true;
                if (s_hud_scale_active && s_hud_scale < 0.999f && !s_hud_scale_exempt) {
                    const float bottomBand = (float)height_ * 0.60f;
                    const float cyBack = (back.vertices[0].y + back.vertices[1].y + back.vertices[2].y) / 3.0f;
                    const float cyNew  = (vertices[0].y + vertices[1].y + vertices[2].y) / 3.0f;
                    sameSideOk = (cyBack >= bottomBand) == (cyNew >= bottomBand);
                }
                if (sameSideOk) {
                    back.vertices.insert(back.vertices.end(), vertices, vertices + count);
                    return;
                }
            }
        }

        HudDrawCall call;
        call.kind = kHudTriBatch;
        call.vertices.assign(vertices, vertices + count);
        memcpy(call.stateSnapshot, renderStates_, sizeof(call.stateSnapshot));
        call.projection = projection_;
        call.fontTexId = 0;
        call.foregroundColor = 0;
        call.scaleExempt = s_hud_scale_exempt;
        call.canvasExempt = (s_hud_canvas_exempt_mode < 0)
            ? s_hud_scale_exempt : (s_hud_canvas_exempt_mode != 0);
        hudBatch_.push_back(std::move(call));
        return;
    }

    // World geometry reaches Vulkan through the typed scene feeds.
}

void gosRenderer::terrainOverrideThinMVP(const float* mvp4x4)
{
    if (!mvp4x4) return;
    const auto& tl = thinTerrainLocs_;
    if (tl.terrainMVP < 0) return;
    // GL_FALSE is correct for terrainMVP (memory/terrain_mvp_gl_false.md) —
    // matches the upload at terrainBindThinUniformsForPatchStream above.
    glUniformMatrix4fv(tl.terrainMVP, 1, GL_FALSE, mvp4x4);
}

// TERRAIN-DEADCODE-DELETE-1: gosRenderer::terrainDrawIndexedPatches deleted.
// The legacy tess-solid terrain funnel is retired — terrain renders via the LOD-chunk path.
// The sole call site at gameos_graphics.cpp (the :7291 if-branch) is now a skip+tripwire.
// Helpers called by this function (cacheTerrainUniformLocations, terrainBindThinUniformsForPatchStream,
// bindTerrainHeightTexUniforms) remain in use by the shadow pre-pass and bridge paths.

void gosRenderer::drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices) {
    ZoneScopedN("DrawIndexedTris.Basic");
    gosASSERT(vertices && indices);

    gosASSERT((num_indices % 3) == 0);

    // VK-FIELD-VFX-8: MLR's indexed tri-list path (SortData::DrawTriIndexedList ->
    // gos_RenderIndexedArray) never honoured gos_State_IsHUD, so gosFX effect geometry
    // flushed inside the gamecam clipper RenderNow scope (IsHUD set under MC2_VK_VFX)
    // rasterized into the GL scene buffer the VK present discards
    // (scene.gosfx_clipper lost-draw samples). Expand to a plain triangle list and
    // route through drawTris' IsHUD batching, same as the non-indexed effect draws.
    if (renderStates_[gos_State_IsHUD] && num_indices > 0) {
        std::vector<gos_VERTEX> expanded;
        expanded.reserve((size_t)num_indices);
        for (int i = 0; i < num_indices; ++i)
            expanded.push_back(vertices[indices[i]]);
        drawTris(expanded.data(), (int)expanded.size());
        return;
    }

    // World geometry reaches Vulkan through the typed scene feeds.
}
