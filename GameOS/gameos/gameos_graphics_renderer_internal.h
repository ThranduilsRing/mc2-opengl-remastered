// gosRenderer and the types only it uses, moved verbatim from gameos_graphics.cpp (every field: the class was TU-internal,
// so any drift would be an ODR bug).
#ifndef GAMEOS_GRAPHICS_RENDERER_INTERNAL_H
#define GAMEOS_GRAPHICS_RENDERER_INTERNAL_H

// Include gameos_graphics_material_internal.h, not this file: it relies on the declarations that header makes before including it.

#include "debug_renderer.h"


// The renderer class and the types only it uses.


class gosRenderer {

    friend class gosShapeRenderer;

    using RenderState = uint32_t[gos_MaxState];
	static const std::string s_Foreground;

    public:
        gosRenderer(graphics::RenderWindowHandle win_h, int w, int h) {
            width_ = w;
            height_ = h;
            win_h_ = win_h;
            hudFlushed_ = false;
        }

        void initHeadless();   // --headless: state only, no device calls (gameos_graphics.cpp)

        uint32_t addTexture(gosTexture* texture) {
            gosASSERT(texture);
            textureList_.push_back(texture);
            return static_cast<uint32_t>((textureList_.size()-1));
        }

        uint32_t addFont(gosFont* font) {
            gosASSERT(font);
            fontList_.push_back(font);
            return static_cast<uint32_t>((fontList_.size()-1));
        }

		uint32_t addBuffer(gosBuffer* buffer) {
			gosASSERT(buffer);
			bufferList_.push_back(buffer);
			return static_cast<uint32_t>((bufferList_.size() - 1));
		}

		uint32_t addVertexDeclaration(gosVertexDeclaration* vdecl) {
			gosASSERT(vdecl);
			vertexDeclarationList_.push_back(vdecl);
			return static_cast<uint32_t>((vertexDeclarationList_.size() - 1));
		}

        // TODO: do same as with texture?
        void deleteFont(gosFont* font) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            
            struct equals_to {
                gosFont* fnt_;
                bool operator()(gosFont* fnt) {
                    return fnt == fnt_;
                }
            };

            equals_to eq;
            eq.fnt_ = font;

            std::vector<gosFont*>::iterator it = 
                std::find_if(fontList_.begin(), fontList_.end(), eq);
            if(it != fontList_.end())
            {
                gosFont* font = *it;
                if(0 == gosFont::destroy(font))
                    fontList_.erase(it);
            }
        }

		bool deleteBuffer(gosBuffer* buffer) {
			std::vector<gosBuffer*>::iterator it = std::find(bufferList_.begin(), bufferList_.end(), buffer);
			if (it != bufferList_.end())
			{
				bufferList_.erase(it);
				return true;
			}
			return false;
		}

		bool deleteVertexDeclaration(gosVertexDeclaration* vdecl) {
			std::vector<gosVertexDeclaration*>::iterator it = std::find(vertexDeclarationList_.begin(), vertexDeclarationList_.end(), vdecl);
			if (it != vertexDeclarationList_.end())
			{
				vertexDeclarationList_.erase(it);
				return true;
			}
			return false;
		}

        gosFont* findFont(const char* font_id) {
            
            struct equals_to {
                const char* font_id_;
                bool operator()(const gosFont* fnt) {
                    return strcmp(fnt->getId(), font_id_)==0;
                }
            };

            equals_to eq;
            eq.font_id_ = font_id;

            std::vector<gosFont*>::iterator it = 
                std::find_if(fontList_.begin(), fontList_.end(), eq);
            if(it != fontList_.end())
                return *it;
            return nullptr;
        }

        gosTexture* getTexture(DWORD texture_id) {
            // TODO: return default texture
            if(texture_id == INVALID_TEXTURE_ID) {
                gosASSERT(0 && "Should not be requested");
                return nullptr;
            }
            gosASSERT(textureList_.size() > texture_id);
            gosASSERT(textureList_[texture_id] != 0);
            return textureList_[texture_id];
        }

        // Bound-check helper for gos_GetGLTextureId. Avoids the hard
        // assert in getTexture() for bogus handle values seen on
        // TG_TinyTexture entries that were never loaded (e.g. 0xFFFFFFFF).
        size_t getTextureListSize() const { return textureList_.size(); }

        void deleteTexture(DWORD texture_id) {
            // FIXME: bad use object list, with stable ids
            // to not waste space
            gosASSERT(textureList_.size() > texture_id);
            delete textureList_[texture_id];
            textureList_[texture_id] = nullptr;
        }

        gosTextAttribs& getTextAttributes() { return curTextAttribs_; }
        void setTextPos(int x, int y) { curTextPosX_ = x; curTextPosY_ = y; }
        void getTextPos(int& x, int& y) { x = curTextPosX_; y = curTextPosY_; }
        void setTextRegion(int Left, int Top, int Right, int Bottom) {
            curTextLeft_ = Left;
            curTextTop_ = Top;
            curTextRight_ = Right;
            curTextBottom_ = Bottom;
        }

        int getTextRegionWidth() { return curTextRight_ - curTextLeft_; }
        int getTextRegionHeight() { return curTextBottom_ - curTextTop_; }

        void setupViewport(bool FillZ, float ZBuffer, bool FillBG, DWORD BGColor, float top, float left, float bottom, float right, bool ClearStencil = 0, DWORD StencilValue = 0) {

            clearDepth_ = FillZ;
            clearDepthValue_ = ZBuffer;
            clearColor_ = FillBG;
            clearColorValue_ = BGColor;
            clearStencil_ = ClearStencil;
            clearStencilValue_ = StencilValue;
            viewportTop_ = top;
            viewportLeft_ = left;
            viewportBottom_ = bottom;
            viewportRight_ = right;
        }

        void getViewportTransform(float* viewMulX, float* viewMulY, float* viewAddX, float* viewAddY) {
            gosASSERT(viewMulX && viewMulY && viewAddX && viewAddY);
            *viewMulX = (viewportRight_ - viewportLeft_)*width_;
            *viewMulY = (viewportBottom_ - viewportTop_)*height_;
            *viewAddX = viewportLeft_ * width_;
            *viewAddY = viewportTop_ * height_;
        }

		void setRenderViewport(const vec4& vp) { render_viewport_ = vp; }
		vec4 getRenderViewport() { return render_viewport_; }

		const mat4& getProj2Screen() { return projection_; }
		int getWidth()  const { return width_; }
		int getHeight() const { return height_; }
        const vec4& getFogColor() const { return fog_color_; }

        void setRenderState(gos_RenderState RenderState, int Value) {
            renderStates_[RenderState] = Value;
        }

        int getRenderState(gos_RenderState RenderState) const {
            return renderStates_[RenderState];
        }

        void setScreenMode(DWORD width, DWORD height, DWORD bit_depth, bool GotoFullScreen, bool anti_alias,
                           DWORD physWidth = 0, DWORD physHeight = 0) {
            reqWidth = width;
            reqHeight = height;
            reqBitDepth = bit_depth;
            reqAntiAlias = anti_alias;
            reqGotoFullscreen = GotoFullScreen;
            // WINDOWED-8006-1 (issue #49): under the HUD-RES clamp the LOGICAL
            // canvas is 800x600 but the PHYSICAL window must not shrink to it —
            // that trapped windowed mode at an 800x600 window. physWidth==0 =
            // "keep the current OS window size" (the boot size from options.cfg);
            // nonzero = explicit physical resize.
            reqPhysWidth  = physWidth;
            reqPhysHeight = physHeight;
            pendingRequest = true;
        }

        void pushRenderStates();
        void popRenderStates();

        void applyRenderStates();

        void drawQuads(gos_VERTEX* vertices, int count);
        void drawLines(gos_VERTEX* vertices, int count);
        void flushHUDBatch();
        size_t beginHudPanel() const { return hudBatch_.size(); }
        void endHudPanel(size_t firstDraw, float scale);
        void drawPoints(gos_VERTEX* vertices, int count);
        void drawTris(gos_VERTEX* vertices, int count);
        void drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices);
        void drawText(const char* text);
        // Water fast path (Stage 2 of renderWater architectural slice).
        // Issues 1-2 instanced draws against the WaterRecipe + WaterFrame SSBOs.
        // Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.
        void renderWaterFastPath(
            unsigned int recordCount,
            unsigned int waterGosHandle,
            unsigned int waterDetailGosHandle,
            float waterElevation,
            float alphaDepth,
            unsigned int alphaEdgeByte,
            unsigned int alphaMiddleByte,
            unsigned int alphaDeepByte,
            float mapTopLeftX,
            float mapTopLeftY,
            float frameCos,
            float frameCosAlpha,
            float oneOverTF,
            float oneOverWaterTF,
            float cloudOffsetX,
            float cloudOffsetY,
            float sprayOffsetX,
            float sprayOffsetY,
            float maxMinUV);

        void beginFrame();
        void endFrame();

        void init();
        void destroy();
        void flush();

        void setNumDrawCallsToDraw(uint32_t num) { num_draw_calls_to_draw_ = num; }
        uint32_t getNumDrawCallsToDraw() { return num_draw_calls_to_draw_; }
        void setBreakOnDrawCall(bool b_break) { break_on_draw_call_ = b_break; }
        bool getBreakOnDrawCall() { return break_on_draw_call_; }
        void setBreakDrawCall(uint32_t num) { break_draw_call_num_ = num; }

        graphics::RenderWindowHandle getRenderWindowHandle() { return win_h_; }

        uint32_t getRenderState(gos_RenderState render_state) { return curStates_[render_state]; }


		void handleEvents();

        void setTerrainTessParams(float level, float near_dist, float far_dist) {
            terrain_tess_level_ = level;
            terrain_tess_dist_near_ = near_dist;
            terrain_tess_dist_far_ = far_dist;
        }
        void setTerrainPhongAlpha(float a) { terrain_phong_alpha_ = a; }
        void setTerrainDisplaceScale(float s) { terrain_displace_scale_ = s; }
        void setTerrainWireframe(bool w) { terrain_wireframe_ = w; }
        void setTerrainDebugMode(float mode) {
            terrain_debug_mode_ = mode;
        }
        void setTerrainMVP(const float* m) {
            memcpy(&terrain_mvp_, m, 16 * sizeof(float));
            terrain_mvp_valid_ = true;
            // Compute inverse VP for post-process depth reconstruction
            mat4 invVP = inverseMat4(terrain_mvp_);
            gosPostProcess* pp = getGosPostProcess();
            if (pp) {
                pp->setInverseViewProj(reinterpret_cast<const float*>(&invVP));
                pp->setViewProj(m);
            }
        }
        void setTerrainCameraPos(float x, float y, float z) {
            terrain_camera_pos_ = vec4(x, y, z, 1.0f);
        }
        // setTerrainViewport: stores (vmx,vmy,vax,vay) for EditorCamera.h shim.
        // gamecam.cpp does not call this (it reads viewport from gos_GetViewport
        // inline); this exists so the editor can set the params the same way.
        void setTerrainViewport(float vmx, float vmy, float vax, float vay) {
            terrain_viewport_ = vec4(vmx, vmy, vax, vay);
        }
        float getTerrainTessLevel() const { return terrain_tess_level_; }
        float getTerrainTessDistNear() const { return terrain_tess_dist_near_; }
        float getTerrainTessDistFar() const { return terrain_tess_dist_far_; }
        float getTerrainPhongAlpha() const { return terrain_phong_alpha_; }
        float getTerrainDisplaceScale() const { return terrain_displace_scale_; }
        float getTerrainDetailTiling() const { return terrain_detail_tiling_; }
        float getTerrainDetailStrength() const { return terrain_detail_strength_; }
        float getTerrainPOMScale() const { return terrain_pom_scale_; }
        bool getTerrainWireframe() const { return terrain_wireframe_; }
        bool isTerrainMVPValid() const { return terrain_mvp_valid_; }
        const mat4& getTerrainMVP() const { return terrain_mvp_; }
        // TERRAIN-SHORELINE-MASK-1: shared render-shader clock accessor (same
        // epoch + SmokeMode fixed-timestep override as the water fast-path's
        // "time" uniform, gameos_graphics.cpp:~3343). Exposed so gos_terrain_lod_chunk.cpp
        // can upload an identical f(worldPos,time)-only clock to the chunk frag
        // (camera-INDEPENDENT by construction — no view matrix involved).
        float getShaderClockSeconds() const {
            return SmokeMode::fixedTimestepEnabled()
                       ? static_cast<float>(SmokeMode::fixedClockSeconds())
                       : static_cast<float>((static_cast<double>((timing::get_wall_time_ms() - timeStart_)) / 1000.0));
        }
        const vec4& getTerrainCameraPos() const { return terrain_camera_pos_; }
        void setShadowMode(bool enabled) { shadow_mode_ = enabled; }
        bool getShadowMode() const { return shadow_mode_; }

        // Shadow softness (penumbra radius in texels)
        void setTerrainShadowSoftness(float s) { terrain_shadow_softness_ = s; }
        float getTerrainShadowSoftness() const { return terrain_shadow_softness_; }

        void setTerrainMatNormalBoost(float r, float g, float d, float c) {
            terrain_mat_normal_boost_[0] = r;
            terrain_mat_normal_boost_[1] = g;
            terrain_mat_normal_boost_[2] = d;
            terrain_mat_normal_boost_[3] = c;
        }
        void getTerrainMatNormalBoost(float* r, float* g, float* d, float* c) const {
            *r = terrain_mat_normal_boost_[0];
            *g = terrain_mat_normal_boost_[1];
            *d = terrain_mat_normal_boost_[2];
            *c = terrain_mat_normal_boost_[3];
        }
        void setTerrainMatTiling(float rock, float grass, float dirt, float concrete, float snow) {
            terrain_mat_tiling_[0] = rock;
            terrain_mat_tiling_[1] = grass;
            terrain_mat_tiling_[2] = dirt;
            terrain_mat_tiling_[3] = concrete;
            terrain_mat_tiling_snow_ = snow;
        }
        void getTerrainMatTiling(float* rock, float* grass, float* dirt, float* concrete, float* snow) const {
            *rock = terrain_mat_tiling_[0];
            *grass = terrain_mat_tiling_[1];
            *dirt = terrain_mat_tiling_[2];
            *concrete = terrain_mat_tiling_[3];
            *snow = terrain_mat_tiling_snow_;
        }
        void setTerrainClassGrass(const float* v) { memcpy(terrain_class_grass_, v, 4 * sizeof(float)); }
        void getTerrainClassGrass(float* v) const  { memcpy(v, terrain_class_grass_, 4 * sizeof(float)); }
        void setTerrainClassDirt(const float* v)   { memcpy(terrain_class_dirt_,  v, 4 * sizeof(float)); }
        void getTerrainClassDirt(float* v) const   { memcpy(v, terrain_class_dirt_,  4 * sizeof(float)); }
        void  setTerrainTintStrengthScale(float s) { terrain_tint_strength_scale_ = s; }
        float getTerrainTintStrengthScale() const   { return terrain_tint_strength_scale_; }
        // TERRAIN-CONTROLMAP-ALBEDO-1: default 0.0 -> shader's mix(x,1.0,0.0)==x
        // -> byte-identical when gate is OFF.
        void  setTerrainControlAlbedoStrength(float s) { terrain_control_albedo_strength_ = s; }
        float getTerrainControlAlbedoStrength() const   { return terrain_control_albedo_strength_; }
        // TERRAIN-MATERIAL-LIB-1: no setter existed prior (member only had an
        // env-read static initializer); added so the JSON reader can apply it
        // when MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN is unset (env still wins).
        void  setTerrainSnowBrightnessDampen(float v) { terrain_snow_brightness_dampen_ = v; }
        float getTerrainSnowBrightnessDampen() const  { return terrain_snow_brightness_dampen_; }
        void setTerrainTintRock(float r, float g, float b)  { terrain_tint_rock_[0]=r; terrain_tint_rock_[1]=g; terrain_tint_rock_[2]=b; }
        void getTerrainTintRock(float* r, float* g, float* b) const { *r=terrain_tint_rock_[0]; *g=terrain_tint_rock_[1]; *b=terrain_tint_rock_[2]; }
        void setTerrainTintGrass(float r, float g, float b)  { terrain_tint_grass_[0]=r; terrain_tint_grass_[1]=g; terrain_tint_grass_[2]=b; }
        void getTerrainTintGrass(float* r, float* g, float* b) const { *r=terrain_tint_grass_[0]; *g=terrain_tint_grass_[1]; *b=terrain_tint_grass_[2]; }
        void setTerrainTintDirt(float r, float g, float b)  { terrain_tint_dirt_[0]=r; terrain_tint_dirt_[1]=g; terrain_tint_dirt_[2]=b; }
        void getTerrainTintDirt(float* r, float* g, float* b) const { *r=terrain_tint_dirt_[0]; *g=terrain_tint_dirt_[1]; *b=terrain_tint_dirt_[2]; }
        void setTerrainTintConcrete(float r, float g, float b) { terrain_tint_concrete_[0]=r; terrain_tint_concrete_[1]=g; terrain_tint_concrete_[2]=b; }
        void getTerrainTintConcrete(float* r, float* g, float* b) const { *r=terrain_tint_concrete_[0]; *g=terrain_tint_concrete_[1]; *b=terrain_tint_concrete_[2]; }
        void setTerrainTintSnow(float r, float g, float b) { terrain_tint_snow_[0]=r; terrain_tint_snow_[1]=g; terrain_tint_snow_[2]=b; }
        void getTerrainTintSnow(float* r, float* g, float* b) const { *r=terrain_tint_snow_[0]; *g=terrain_tint_snow_[1]; *b=terrain_tint_snow_[2]; }
        void setTerrainMatRoughness(float rock, float grass, float dirt, float concrete) {
            terrain_mat_roughness_[0]=rock; terrain_mat_roughness_[1]=grass;
            terrain_mat_roughness_[2]=dirt; terrain_mat_roughness_[3]=concrete;
        }
        void getTerrainMatRoughness(float* rock, float* grass, float* dirt, float* concrete) const {
            *rock=terrain_mat_roughness_[0]; *grass=terrain_mat_roughness_[1];
            *dirt=terrain_mat_roughness_[2]; *concrete=terrain_mat_roughness_[3];
        }
        // POM-PERLAYER-HEIGHT-1: per-layer POM displacement-height multipliers
        // (rock/grass/dirt/concrete). Neutral 1.0 defaults; consumed by the
        // LOD-chunk frag only when MC2_TERRAIN_MATERIAL_LIB is on
        // (chunkSampleDisplacement branch).
        void setTerrainMatHeight(float rock, float grass, float dirt, float concrete) {
            terrain_mat_height_[0]=rock; terrain_mat_height_[1]=grass;
            terrain_mat_height_[2]=dirt; terrain_mat_height_[3]=concrete;
        }
        void getTerrainMatHeight(float* rock, float* grass, float* dirt, float* concrete) const {
            *rock=terrain_mat_height_[0]; *grass=terrain_mat_height_[1];
            *dirt=terrain_mat_height_[2]; *concrete=terrain_mat_height_[3];
        }
        void setTerrainMatAO(float rock, float grass, float dirt, float concrete) {
            terrain_mat_ao_[0]=rock; terrain_mat_ao_[1]=grass;
            terrain_mat_ao_[2]=dirt; terrain_mat_ao_[3]=concrete;
        }
        void getTerrainMatAO(float* rock, float* grass, float* dirt, float* concrete) const {
            *rock=terrain_mat_ao_[0]; *grass=terrain_mat_ao_[1];
            *dirt=terrain_mat_ao_[2]; *concrete=terrain_mat_ao_[3];
        }
        void  setTerrainNormalsFromHeightStrength(float s) { terrain_nfh_strength_ = s; }
        float getTerrainNormalsFromHeightStrength() const  { return terrain_nfh_strength_; }
        void  setTerrainCliffShadowFloor(float f) { terrain_cliff_shadow_floor_ = f; }
        float getTerrainCliffShadowFloor() const  { return terrain_cliff_shadow_floor_; }

        // Terrain draw killswitch
        void setTerrainDrawEnabled(bool e) { terrain_draw_enabled_ = e; }
        bool getTerrainDrawEnabled() const { return terrain_draw_enabled_; }
        float getTerrainDebugMode() const { return terrain_debug_mode_; }

        void setTerrainLightDir(float x, float y, float z) { terrain_light_dir_ = vec4(x, y, z, 0.0f); }
        const vec4& getTerrainLightDir() const { return terrain_light_dir_; }
        // CEMENT-NIGHT-LIGHT-1: mission flat-pad light (killswitch handled at
        // the gos_ setter — values arriving here are already gated).
        void setTerrainMissionFlatLight(float r, float g, float b) { terrain_mission_flat_light_ = vec4(r, g, b, 0.0f); }
        const vec4& getTerrainMissionFlatLight() const { return terrain_mission_flat_light_; }
        void setTerrainDetailParams(float tiling, float strength) { terrain_detail_tiling_ = tiling; terrain_detail_strength_ = strength; }
        // Slot `idx` takes the gos texture `handle` (INVALID_TEXTURE_ID clears it) and owns it: the texture it replaces is deleted.
        void setTerrainMaterialNormal(int idx, DWORD handle) {
            if (idx < 0 || idx >= 9) return;
            const bool valid = handle != INVALID_TEXTURE_ID && handle < textureList_.size() && textureList_[handle];
            if (terrain_mat_normal_handle_[idx] != INVALID_TEXTURE_ID && terrain_mat_normal_handle_[idx] != handle)
                deleteTexture(terrain_mat_normal_handle_[idx]);
            terrain_mat_normal_handle_[idx] = valid ? handle : INVALID_TEXTURE_ID;
            static uint64_t nextVersion = 0;
            terrain_normal_version_ = ++nextVersion;
        }
        DWORD getTerrainMaterialNormalHandle(int idx) const { return idx >= 0 && idx < 9 ? terrain_mat_normal_handle_[idx] : INVALID_TEXTURE_ID; }
        uint64_t getTerrainMaterialNormalVersion() const {
            for (int i = 0; i < 5; ++i)
                if (terrain_mat_normal_handle_[i] == INVALID_TEXTURE_ID) return 0;
            return terrain_normal_version_;
        }
        void setTerrainCellBombParams(float s, float j, float r) { terrain_cell_scale_ = s; terrain_cell_jitter_ = j; terrain_cell_rotation_ = r; }
        void setTerrainPOMParams(float scale, float, float) { terrain_pom_scale_ = scale; }
        void setTerrainWorldScale(float scale) { terrain_world_scale_ = scale; }

        void pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void drawTerrainOverlays();
        void drawDecals();
        // Slice A — draw the mission-static cement-overlay bake. Reproduces
        // drawTerrainOverlays()'s exact state/shader/uniforms/VAO but draws
        // the passed static VBO with per-overlayTexId ranges and does NOT
        // clear (mirrors DrawMineStatic). draws/drawCount are forwarded from
        // gos_terrain_indirect::DrawDecalStatic via the bridge.
        bool drawDecalStaticBatch(unsigned int vboGL,
                                  const struct GosDecalStaticDraw* draws,
                                  int drawCount);

        // META-FIX design D (MC2_RES_ASSERT): fresh SDL drawable probe for the
        // per-frame audit (gameosmain.cpp mc2resFrameAudit) — the cached
        // Environment.drawableWidth/Height only refreshes in handleEvents.
        // Read-only; false when the window does not exist yet.
        bool probeDrawableSize(int* w, int* h) {
            if (!win_h_)
                return false;
            graphics::get_drawable_size(win_h_, w, h);
            return true;
        }

        // RENDER_STATES v1: external invalidation hook (public).
        //
        // LOAD-BEARING CONTRACT — DO NOT REMOVE INVALIDATION CALLS.
        // Any code path that mutates GL state outside applyRenderStates'
        // tracked-slot set (program/depth/blend/cull/sampler/texture-bind,
        // including per-texture-object wrap/filter via glTexParameteri) MUST
        // call this hook AT THE END of the path so the next applyRenderStates
        // re-applies fully. Cache is consulted on the next call without
        // further bookkeeping; missed invalidation = silent visual corruption.
        // Origin: MAJOR-3 from 2026-05-08 adversarial review of the cache;
        // CRITICAL-1 was caller-side coverage gap of this exact contract.
        // Current invalidation sites (grep gos_InvalidateRenderStateCache /
        // invalidateRenderStateCache): gos_terrain_bridge_drawSingleBucket,
        // renderWaterFastPath, gos_terrain_bridge_drawIndirect,
        // endShadowPrePass, endDynamicShadowPass, gosPostProcess::endScene,
        // GpuStaticPropBatcher::flush, drawTerrainOverlays, drawDecals,
        // gos_ForceApplyRenderStates, gosRenderer::beginFrame (defensive).
        // When adding a new fast path: invalidate or break the cache contract.
        void invalidateRenderStateCache() { stateCacheValid_ = false; }

    private:

        bool beforeDrawCall();
        void afterDrawCall();

        // The debug-line queue lives and dies with the render context (see debug_renderer.h).
        DebugRenderer::Queue debugQueue_;

        int width_;
        int height_;
        graphics::RenderWindowHandle win_h_;

        // fits vertices into viewport
        mat4 projection_;

		vec4 fog_color_;

        void initRenderStates();

        std::vector<gosTexture*> textureList_;
        std::vector<gosFont*> fontList_;
        std::vector<gosBuffer*> bufferList_;
        std::vector<gosVertexDeclaration*> vertexDeclarationList_;

        DWORD reqWidth;
        DWORD reqPhysWidth = 0;   // WINDOWED-8006-1: physical window size (logical may be clamped)
        DWORD reqPhysHeight = 0;
        DWORD reqHeight;
        DWORD reqBitDepth;
        DWORD reqAntiAlias;
        bool reqGotoFullscreen;
        bool pendingRequest;

        RenderState curStates_;
        RenderState renderStates_;

        // RENDER_STATES v1: state-equality early-out cache for applyRenderStates().
        // stateCacheValid_=false forces a full apply on the next call (used after
        // any external GL state disturbance — bridges, post-process, shadow draws).
        // cachedResolvedTexId_[u] tracks the resolved GL texture id we last bound
        // on unit u; texture handles mutate per-frame (see memory note
        // mc2_texture_handle_is_live.md), so an unchanged gos_State_TextureN
        // does NOT imply the underlying GL texture is the same.
        bool stateCacheValid_ = false;
        uint32_t cachedResolvedTexId_[3] = {0u, 0u, 0u};
        // 600-frame summary counters
        uint32_t rsCalls_ = 0;
        uint32_t rsSkipped_ = 0;
        uint32_t rsApplied_ = 0;
        uint32_t rsFrames_ = 0;

        static const int RENDER_STATES_STACK_SIZE = 16;
        int renderStatesStackPointer;
        RenderState statesStack_[RENDER_STATES_STACK_SIZE];
        //

        gosTextAttribs curTextAttribs_;

        int curTextPosX_;
        int curTextPosY_;

        int curTextLeft_;
        int curTextTop_;
        int curTextRight_;
        int curTextBottom_;
        //
        
        bool clearDepth_;
        float clearDepthValue_;
        bool clearColor_;
        DWORD clearColorValue_;
        bool clearStencil_;
        DWORD clearStencilValue_;
        float viewportTop_;
        float viewportLeft_;
        float viewportBottom_;
        float viewportRight_;
        //

		vec4 render_viewport_;
        
        gosMesh* text_;

        //
        uint32_t num_draw_calls_;
        uint32_t num_draw_calls_to_draw_;
        bool break_on_draw_call_;
        uint32_t break_draw_call_num_;

        std::vector<HudDrawCall> hudBatch_;
        bool                     hudFlushed_;

        float terrain_tess_level_ = 4.0f;          // base inner/outer tessellation factor
        float terrain_tess_dist_near_ = 200.0f;    // full tess below this distance
        float terrain_tess_dist_far_ = 2000.0f;    // tess=1 beyond this distance
        float terrain_phong_alpha_ = 0.5f;         // Phong smoothing strength
        float terrain_displace_scale_ = 2.0f;      // displacement amplitude (dirt-only in TES)
        bool terrain_wireframe_ = false;            // wireframe overlay toggle
        float terrain_debug_mode_ = 0.0f;          // 0=off, 1..9 fragment debug modes (see GraphicsOptionsWindow kTerrainModes), -1 tess-alive probe. Env MC2_TERRAIN_DEBUG_MODE overrides at upload.

        // Terrain tessellation MVP (world-to-NDC)
        mat4 terrain_mvp_;
        bool terrain_mvp_valid_ = false;
        vec4 terrain_camera_pos_;  // MC2 world space camera position for TCS LOD
        vec4 terrain_viewport_;    // (vmx, vmy, vax, vay) for EditorCamera shim

        // GPU-driven dynamic sun shadow -- depth-only instanced programs (Phase 1).
        // Registered at init; called by Tasks 3+ to draw GPU-batched mechs/buildings
        // into the dynamic shadow FBO.
        bool shadow_mode_ = false;
        float terrain_shadow_softness_ = 2.5f;
        bool terrain_draw_enabled_ = true;

        // Terrain splatting textures (from main source, needed for material normal maps)
        vec4 terrain_light_dir_;
        // CEMENT-NIGHT-LIGHT-1: mission flat-pad light colour (eye->getLightRed/
        // Green/Blue(flatIntensity)/255, flatIntensity = clamp(lightDir.z,0,1)) —
        // the legacy per-vertex value burned into the colormap by burnInShadows
        // (terrtxm2.cpp). Cement-atlas / overlay texels are RAW (unburned), so
        // the shaders multiply this in. Default (1,1,1) = day-neutral so paths
        // that never call the setter (menus, editor) are unchanged.
        vec4 terrain_mission_flat_light_ = vec4(1.0f, 1.0f, 1.0f, 0.0f);
        float terrain_detail_tiling_ = 1.0f;
        float terrain_detail_strength_ = 1.0f;  // global detail-normal multiplier; was 4.0 (too loud, carpet at distance)
        uint64_t terrain_normal_version_ = 0;
        DWORD  terrain_mat_normal_handle_[9] = {INVALID_TEXTURE_ID, INVALID_TEXTURE_ID, INVALID_TEXTURE_ID, INVALID_TEXTURE_ID,
                                                INVALID_TEXTURE_ID, INVALID_TEXTURE_ID, INVALID_TEXTURE_ID, INVALID_TEXTURE_ID,
                                                INVALID_TEXTURE_ID};   // the gos textures behind terrain_mat_normal_
        float terrain_cell_scale_ = 8.0f;
        float terrain_cell_jitter_ = 0.8f;
        float terrain_cell_rotation_ = 1.0f;
        float terrain_pom_scale_ = 0.02f;
        float terrain_world_scale_ = 15360.0f;
        // Per-material normal boost: [rock, grass, dirt, concrete]; matches shader const default.
        float terrain_mat_normal_boost_[4] = { 0.9f, 0.5f, 1.1f, 2.5f };  // grass 1.1->0.5: detail normal was too loud, formed tiling carpet at distance
        // Per-material UV tiling: [rock, grass, dirt, concrete]; snow separate.
        // Grass default lowered 12→2 to reduce excessive normal-map repetition.
        float terrain_mat_tiling_[4] = { 3.0f, 2.0f, 1.0f, 6.0f };
        float terrain_mat_tiling_snow_ = 1.0f;
        // TERRAIN-CLASSIFY-TUNING-1: colormap RGB channel-delta classifier thresholds.
        //   grass = (gMinusRLo, gMinusRHi, gBrightLo, gBrightHi)
        //   dirt  = (rMinusGLo, rMinusGHi, rBrightLo, rBrightHi)
        // Sand_M24 profile widens the dirt gate at mission start (see mclib/terrain.cpp).
        float terrain_class_grass_[4] = { -0.02f, 0.06f, 0.22f, 0.40f };
        float terrain_class_dirt_[4]  = { -0.02f, 0.06f, 0.22f, 0.45f };
        float terrain_tint_strength_scale_ = 1.0f;  // 0=colormap passthrough, 1=full tint
        // TERRAIN-CONTROLMAP-ALBEDO-1: default 0.0 -- gate OFF uploads this
        // verbatim, and the frag's mix(x,1.0,0.0)==x is algebraically the
        // pre-slice expression (byte-identical). Gate ON + JSON/env authors
        // this toward 1.0 so authored control-map weights fully repaint albedo.
        float terrain_control_albedo_strength_ = 0.0f;
        // Snow brightness dampen: <1 darkens detected-snow fragments. Default 0.78
        // (visibly turned down); MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN overrides.
        float terrain_snow_brightness_dampen_ = mc2::gate::get<"MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN">();
        // TERRAIN-TINT-UI-1: material base tint colors (defaults match shader)
        float terrain_tint_rock_[3]  = { 0.36f, 0.37f, 0.40f };
        float terrain_tint_grass_[3] = { 0.35f, 0.42f, 0.25f };
        float terrain_tint_dirt_[3]  = { 0.48f, 0.42f, 0.33f };
        // TERRAIN-MATERIAL-LIB-1: the two tint constants that were frag literals
        // (terrain_lod_chunk.frag:715-716 / gos_terrain.frag:787-788) promoted to
        // uniforms so terrain_materials.json can cover them. Defaults are the
        // EXACT literal values -- byte-identity requirement.
        float terrain_tint_concrete_[3] = { 0.55f, 0.53f, 0.50f };
        float terrain_tint_snow_[3]     = { 0.75f, 0.78f, 0.84f };
        // Per-layer roughness/AO scalars (rock, grass, dirt, concrete). Neutral
        // 1.0 defaults so u_useMaterialLib==0 (gate OFF) skips the branch that
        // would consume them, and gate-ON with defaults reproduces current
        // lighting exactly (dot(weights, 1.0) == 1.0).
        float terrain_mat_roughness_[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float terrain_mat_ao_[4]        = { 1.0f, 1.0f, 1.0f, 1.0f };
        float terrain_mat_height_[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
        // Per-frame multiplier on the macro-slope tilt of the chunk terrain normal.
        // 1.0 = full slope tilt, 0.0 = no slope contribution. Slider range 0..1.5.
        float terrain_nfh_strength_ = 1.0f;
        // CLIFF SHADOW FLOOR: lifts shadow-side steep terrain faces off near-black.
        // Slider 0..0.6; default 0.0 = byte-identical (no-op). Effective only when
        // env gate MC2_TERRAIN_CLIFF_SHADOW_FLOOR is set to a non-zero float.
        float terrain_cliff_shadow_floor_ = 0.0f;

        // ── World-space overlay batches ────────────────────────────────────────
        // Both terrain overlays (cement perimeter) and decals (footprints)
        // use the same vertex layout (WorldOverlayVert, 28 bytes) and VAO setup.
        // Per-draw texture grouping: multiple addTriangle calls within a frame with
        // different texHandle values are batched into separate draw entries.
        struct OverlayBatchEntry_ {
            unsigned int texHandle;
            unsigned int firstVert;    // index into verts vector
            unsigned int vertCount;
        };
        struct OverlayBatch_ {
            std::vector<WorldOverlayVert> verts;
            std::vector<OverlayBatchEntry_> draws;
        };

        OverlayBatch_ terrainOverlayBatch_;
        OverlayBatch_ decalBatch_;

        uint64_t timeStart_ = 0;          // shared epoch for all time-animated shaders

        void pushToOverlayBatch_(OverlayBatch_& b, const WorldOverlayVert* v3, unsigned int texHandle);
};

#endif // GAMEOS_GRAPHICS_RENDERER_INTERNAL_H
