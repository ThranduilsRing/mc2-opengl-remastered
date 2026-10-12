// gosRenderer and the types only it uses, moved verbatim from gameos_graphics.cpp (every field: the class was TU-internal,
// so any drift would be an ODR bug).
#ifndef GAMEOS_GRAPHICS_RENDERER_INTERNAL_H
#define GAMEOS_GRAPHICS_RENDERER_INTERNAL_H

// Include gameos_graphics_material_internal.h, not this file: it relies on the declarations that header makes before including it.


// The renderer class and the types only it uses.


class gosRenderer {

    friend class gosShapeRenderer;

    typedef uint32_t RenderState[gos_MaxState];
	static const std::string s_Foreground;

    public:
        gosRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {
            width_ = w;
            height_ = h;
            ctx_h_ = ctx_h;
            win_h_ = win_h;
            hudFlushed_ = false;
        }

        uint32_t addTexture(gosTexture* texture) {
            gosASSERT(texture);
            textureList_.push_back(texture);
            return (uint32_t)(textureList_.size()-1);
        }

        uint32_t addFont(gosFont* font) {
            gosASSERT(font);
            fontList_.push_back(font);
            return (uint32_t)(fontList_.size()-1);
        }

		uint32_t addBuffer(gosBuffer* buffer) {
			gosASSERT(buffer);
			bufferList_.push_back(buffer);
			return (uint32_t)(bufferList_.size() - 1);
		}

		uint32_t addVertexDeclaration(gosVertexDeclaration* vdecl) {
			gosASSERT(vdecl);
			vertexDeclarationList_.push_back(vdecl);
			return (uint32_t)(vertexDeclarationList_.size() - 1);
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
            textureList_[texture_id] = 0;
        }

        uint32_t getFlagsFromStates()
        {
            return curStates_[gos_State_AlphaTest] ? SHADER_FLAG_INDEX_TO_MASK(gosGLOBAL_SHADER_FLAGS::ALPHA_TEST) : 0;
        }

        gosRenderMaterial* getRenderMaterial(const char* name) {

            uint32_t flags = getFlagsFromStates();
            return materialDB_[name][flags];
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
        void replayTextQuads(const HudDrawCall& call);
        void drawPoints(gos_VERTEX* vertices, int count);
        void drawTris(gos_VERTEX* vertices, int count);
        void drawIndexedTris(gos_VERTEX* vertices, int num_vertices, WORD* indices, int num_indices);
		void drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl, const float* mvp);
		void drawIndexedTris(HGOSBUFFER ib, HGOSBUFFER vb, HGOSVERTEXDECLARATION vdecl);
        void drawText(const char* text);
        void terrainBindUniformsForPatchStream(gosRenderMaterial* material);
        // Returns ssboRecordBase uniform location in the thin program, or -1.
        // If overrideProg is non-null, binds uniforms to that program instead
        // (B4 Stage 1b: mask-SOLID shader shares all uniform names with thin
        // shader so this is safe).
        int terrainBindThinUniformsForPatchStream(glsl_program* overrideProg = nullptr);
        // Fix A (2026-05-14): re-upload an externally-provided terrainMVP over
        // the one just bound by terrainBindThinUniformsForPatchStream.  Used by
        // the indirect bridge to align the VS projection MVP with the MVP that
        // compute used when writing the thin records being drawn this frame.
        // Uses the already-cached thinTerrainLocs_.terrainMVP — no extra
        // glGetUniformLocation.  Must be called AFTER terrainBindThinUniformsForPatchStream
        // (which caches the location) and while the thin program is bound.
        void terrainOverrideThinMVP(const float* mvp4x4);
        glsl_program* getThinTerrainProgram()      const { return thin_terrain_prog_;        }
        glsl_program* getTerrainSurfaceProgram()   const { return terrain_surface_prog_;     }
        glsl_program* getWaterFastProgram()        const { return water_fast_prog_;           }
        glsl_program* getMineStaticProgram()       const { return mine_static_prog_;          }  // PR2c Stage 2c
        glsl_program* getTerrainOverlayProgram()   const { return overlayProg_;                }
        glsl_program* getMaskSolidProgram()        const { return mask_solid_prog_;           }  // B4 Stage 1b — mask-SOLID draw
        glsl_program* getMaskWaterProgram()        const { return mask_water_prog_;           }  // B4 Stage 1c — mask-water draw
        glsl_program* getShadowMechProg()          const { return shadow_mech_prog_;          }
        glsl_program* getShadowStaticPropProg()    const { return shadow_static_prop_prog_;   }

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

        graphics::RenderContextHandle getRenderContextHandle() { return ctx_h_; }

        uint32_t getRenderState(gos_RenderState render_state) { return curStates_[render_state]; }

        gosRenderMaterial* selectBasicRenderMaterial(const RenderState& rs) const ;
        gosRenderMaterial* selectLightedRenderMaterial(const RenderState& rs) const ;

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
                pp->setInverseViewProj((const float*)&invVP);
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
        void terrainExtraReset() { terrain_extra_count_ = 0; terrain_extra_draw_offset_ = 0; terrain_batch_extras_ = nullptr; terrain_batch_extras_count_ = 0; }
        void terrainExtraAdd(const gos_TERRAIN_EXTRA* data, int count) {
            if (terrain_extra_count_ + count <= terrain_extra_capacity_) {
                memcpy(terrain_extra_data_ + terrain_extra_count_, data, sizeof(gos_TERRAIN_EXTRA) * count);
                terrain_extra_count_ += count;
            }
        }
        void setTerrainBatchExtras(const gos_TERRAIN_EXTRA* extras, int count) {
            terrain_batch_extras_ = extras;
            terrain_batch_extras_count_ = count;
        }
        int getTerrainExtraCount() const { return terrain_extra_count_; }
        float getTerrainTessLevel() const { return terrain_tess_level_; }
        float getTerrainTessDistNear() const { return terrain_tess_dist_near_; }
        float getTerrainTessDistFar() const { return terrain_tess_dist_far_; }
        float getTerrainPhongAlpha() const { return terrain_phong_alpha_; }
        float getTerrainDisplaceScale() const { return terrain_displace_scale_; }
        float getTerrainDetailTiling() const { return terrain_detail_tiling_; }
        float getTerrainDetailStrength() const { return terrain_detail_strength_; }
        float getTerrainPOMScale() const { return terrain_pom_scale_; }
        bool getTerrainWireframe() const { return terrain_wireframe_; }
        gos_TERRAIN_EXTRA* getTerrainExtraData() const { return terrain_extra_data_; }
        bool isTerrainMVPValid() const { return terrain_mvp_valid_; }
        const mat4& getTerrainMVP() const { return terrain_mvp_; }
        // TERRAIN-SHORELINE-MASK-1: shared render-shader clock accessor (same
        // epoch + SmokeMode fixed-timestep override as the water fast-path's
        // "time" uniform, gameos_graphics.cpp:~3343). Exposed so gos_terrain_lod_chunk.cpp
        // can upload an identical f(worldPos,time)-only clock to the chunk frag
        // (camera-INDEPENDENT by construction — no view matrix involved).
        float getShaderClockSeconds() const {
            return SmokeMode::fixedTimestepEnabled()
                       ? (float)SmokeMode::fixedClockSeconds()
                       : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0);
        }
        gosRenderMaterial* getTerrainMaterial() const { return terrain_material_; }
        const vec4& getTerrainCameraPos() const { return terrain_camera_pos_; }
        void setShadowMode(bool enabled) { shadow_mode_ = enabled; }
        bool getShadowMode() const { return shadow_mode_; }
        gosRenderMaterial* getShadowTerrainMaterial() const { return shadow_terrain_material_; }

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

        // F1 Stage A-pre Task 7b: store probe-only worldToClipGL for upload at
        // draw time inside terrainBindUniformsForPatchStream (terrainDrawIndexedPatches retired).
        void setWorldToClipGLProbeMatrix(const float* M16) {
            memcpy(probeWorldToClipGL_, M16, 16 * sizeof(float));
            probeWorldToClipGLValid_ = true;
        }

        // Shadow pre-pass (separate pass over all terrain batches before shading)
        void beginShadowPrePass(bool clearDepth = true);
        void drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
            WORD* indices, int numIndices,
            const gos_TERRAIN_EXTRA* extras, int extraCount);
        void drawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
            HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4);
        void endShadowPrePass();

        // Dynamic object shadow pass (camera-centered, per-frame)
        void beginDynamicShadowPass();
        void endDynamicShadowPass();

        void setTerrainLightDir(float x, float y, float z) { terrain_light_dir_ = vec4(x, y, z, 0.0f); }
        const vec4& getTerrainLightDir() const { return terrain_light_dir_; }
        // CEMENT-NIGHT-LIGHT-1: mission flat-pad light (killswitch handled at
        // the gos_ setter — values arriving here are already gated).
        void setTerrainMissionFlatLight(float r, float g, float b) { terrain_mission_flat_light_ = vec4(r, g, b, 0.0f); }
        const vec4& getTerrainMissionFlatLight() const { return terrain_mission_flat_light_; }
        void setTerrainDetailParams(float tiling, float strength) { terrain_detail_tiling_ = tiling; terrain_detail_strength_ = strength; }
        void setTerrainMaterialNormal(int idx, GLuint texId) {
            if (idx >= 0 && idx < 9) {
                terrain_mat_normal_[idx] = texId;
                terrain_normal_array_dirty_ = true;
            }
        }
        void buildTerrainNormalArray() {
            // Require slots 0-4 (rock/grass/dirt/concrete/snow). Slots 5-8 are optional.
            for (int i = 0; i < 5; ++i) {
                if (terrain_mat_normal_[i] == 0) {
                    static bool s_warned = false;
                    if (!s_warned) {
                        s_warned = true;
                        MC2_LOG(Render, Debug, "buildTerrainNormalArray: slot {:d} is zero -- deferring build until slots 0-4 are set", mc2::fmt::as_i32(i));
                    }
                    return;
                }
            }

            GlPixelStoreGuard guard;  // saves and restores pixel-store, PBO, active tex, bindings

            GLint refW = 0, refH = 0, refFmt = 0;
            GLint baseLevel = 0, maxLevel = 0;
            glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[0]);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &refW);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &refH);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &refFmt);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &baseLevel);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,  &maxLevel);
            glBindTexture(GL_TEXTURE_2D, 0);

            if (refW <= 0 || refH <= 0)
                STOP(("buildTerrainNormalArray: slot 0 has invalid size %dx%d", refW, refH));

            for (int i = 1; i < 9; ++i) {
                if (terrain_mat_normal_[i] == 0) continue;  // optional slot absent
                GLint wi = 0, hi = 0, fmti = 0;
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[i]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,           &wi);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,          &hi);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmti);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (wi != refW || hi != refH)
                    STOP(("buildTerrainNormalArray: slot %d size %dx%d != slot 0 size %dx%d",
                          i, wi, hi, refW, refH));
                if (fmti != refFmt)
                    STOP(("buildTerrainNormalArray: slot %d internal format 0x%X != slot 0 format 0x%X",
                          i, fmti, refFmt));
            }

            int numLevels = maxLevel - baseLevel + 1;
            if (numLevels < 1) numLevels = 1;
            {
                int maxPossible = 0, d = (refW > refH ? refW : refH);
                while (d > 0) { ++maxPossible; d >>= 1; }
                if (numLevels > maxPossible) numLevels = maxPossible;
            }

            if (terrain_normal_array_tex_ != 0) {
                glDeleteTextures(1, &terrain_normal_array_tex_);
                terrain_normal_array_tex_ = 0;
            }

            // Allocate 9-layer GL_TEXTURE_2D_ARRAY (slots 0-4 required, 5-8 optional).
            // glGetTexImage always decompresses if the source is compressed; we
            // receive RGBA8 regardless of the source internal format.
            // TEX-CLASS: asset-pool -- terrain per-material normal-map 2D_ARRAY (content)
            glGenTextures(1, &terrain_normal_array_tex_);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8,
                         refW, refH, 9 /*layers*/,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

            // Copy all mip levels from each source texture into the array.
            // Normal-map mips are authored — do not use glGenerateMipmap here
            // (averaging would shorten normals; hardware-generated mips differ
            // from the source importer's output).
            // Missing optional slots (terrain_mat_normal_[i] == 0) get a flat
            // neutral normal (128,128,255,255) so sampling them is always valid.
            std::vector<uint8_t> pixels;
            int levelsCopied = 0;
            for (int level = baseLevel; level < baseLevel + numLevels; ++level) {
                GLint mipW = 0, mipH = 0;
                glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[0]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH,  &mipW);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &mipH);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (mipW <= 0 || mipH <= 0) break;
                ++levelsCopied;

                glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
                glTexImage3D(GL_TEXTURE_2D_ARRAY, level, GL_RGBA8,
                             mipW, mipH, 9, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

                const size_t mipBytes = (size_t)mipW * mipH * 4;
                pixels.resize(mipBytes);

                for (int layer = 0; layer < 9; ++layer) {
                    if (terrain_mat_normal_[layer] != 0) {
                        glBindTexture(GL_TEXTURE_2D, terrain_mat_normal_[layer]);
                        glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                        glBindTexture(GL_TEXTURE_2D, 0);
                    } else {
                        // Flat neutral normal (128,128,255,255) for absent optional slot.
                        auto* p32 = reinterpret_cast<uint32_t*>(pixels.data());
                        std::fill(p32, p32 + mipW * mipH, 0xFFFF8080u);
                    }

                    glBindTexture(GL_TEXTURE_2D_ARRAY, terrain_normal_array_tex_);
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level,
                                    0, 0, layer,
                                    mipW, mipH, 1,
                                    GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                }
            }

            // Sampling params. CRITICAL: MAX_LEVEL must match the levels ACTUALLY
            // copied, not the pre-computed numLevels. The copy loop breaks early
            // when a source mip is absent; if we declare more levels than exist,
            // the array is mip-INCOMPLETE -> every sample returns black under a
            // mipmap min filter (the chunk material-detail "all black" bug). If
            // only level 0 exists, drop to a non-mip filter so it stays complete.
            if (levelsCopied < 1) levelsCopied = 1;
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, baseLevel);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,  baseLevel + levelsCopied - 1);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                            (levelsCopied > 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

            terrain_normal_array_dirty_ = false;
        }
        // Phase 10 Step 5: expose the merged material normal sampler2DArray so the
        // terrain LOD chunk path (a bolt-on draw) can bind the SAME texture the
        // legacy terrain uses. Builds lazily if dirty/never-built; returns 0 until
        // required slots 0-4 are populated (slots 5-8 optional). GL context assumed
        // live (call from a draw, like the legacy bind sites).
        GLuint getTerrainNormalArrayTexEnsureBuilt() {
            if (terrain_normal_array_dirty_ || terrain_normal_array_tex_ == 0)
                buildTerrainNormalArray();
            return terrain_normal_array_tex_;
        }
        void setTerrainCellBombParams(float s, float j, float r) { terrain_cell_scale_ = s; terrain_cell_jitter_ = j; terrain_cell_rotation_ = r; }
        void setTerrainPOMParams(float scale, float, float) { terrain_pom_scale_ = scale; }
        void setTerrainWorldScale(float scale) { terrain_world_scale_ = scale; }

        void pushTerrainOverlayTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void pushDecalTri(const WorldOverlayVert* verts3, unsigned int texHandle);
        void drawTerrainOverlays();
        void drawDecals();
        // APPLY-STATE-TERRAINDECAL-1: first top-level scene-pass apply-state.
        // When MC2_FRAMEGRAPH_EXECUTOR is ON, the executor pre-applies the
        // TerrainDecal pipeline (the sole entry render-state of drawDecals) and
        // sets decalStateAppliedByExecutor_; drawDecals() then skips its own
        // applyPipeline (one-shot reset). Gate OFF -> flag stays false -> body
        // applies as before -> byte-identical.
        void executorApplyTerrainDecalState();
        bool decalStateAppliedByExecutor_ = false;
        // APPLY-STATE-TERRAINOVERLAY-1: second top-level scene-pass apply-state.
        // Same pattern as decal — when MC2_FRAMEGRAPH_EXECUTOR is ON the executor
        // pre-applies the TerrainOverlay pipeline (the sole entry render-state of
        // drawTerrainOverlays) and sets overlayStateAppliedByExecutor_; the body
        // then skips its own applyPipeline (one-shot). Gate OFF -> byte-identical.
        void executorApplyTerrainOverlayState();
        bool overlayStateAppliedByExecutor_ = false;
        // APPLY-STATE-WATER-1: fifth top-level scene-pass apply-state. ★BODY-SITE apply
        // (NOT a begin-seam pre-apply): renderWaterFastPath has a mid-body COMPUTE
        // dispatch (WaterStream::ComputeDispatchAndBindThinRecords -> glUseProgram(compute))
        // BETWEEN the begin seam and the applyPipeline(WaterArmed), so the executor apply is
        // dispatched at the live draw site (immediately before the body's :3276 applyPipeline,
        // AFTER the compute dispatch) and the body skips its own applyPipeline (one-shot).
        // Pipeline-only lift (water inherits the scene/MainColor FBO + viewport from the
        // caller). Gate OFF -> flag stays false -> body applies as before -> byte-identical.
        void executorApplyWaterState();
        bool waterStateAppliedByExecutor_ = false;
        // APPLY-STATE-SHADOW-1: first render-target-MODE apply-state consumer.
        // When MC2_FRAMEGRAPH_EXECUTOR is ON, the executor (at the dynamic-shadow seam,
        // beginDynamicShadowPass) applies the ShadowMech BASE pipeline + the forward-Z
        // depth clear (DepthForwardZ) via the EXTEND helper, and sets this flag; the body
        // then skips its own applyPipeline(ShadowMech) AND its forward-Z clear (one-shot
        // reset). FBO bind (ShadowDynamicMap) + viewport (ShadowMap size) stay body-owned
        // this slice (deferred to SHADOW-2). Gate OFF -> flag stays false -> body applies +
        // clears as before -> byte-identical. The capture/restore bracket, AMD feedback-unbind,
        // GL_TEXTURE_COMPARE_MODE flip, material/lsm upload, per-caster ShadowStaticProp, and
        // the noteRenderPass(ShadowCaster) seam all remain body-owned.
        void executorApplyShadowState();
        bool shadowStateAppliedByExecutor_ = false;
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

        int width_;
        int height_;
        graphics::RenderContextHandle ctx_h_;
        graphics::RenderWindowHandle win_h_;

        // fits vertices into viewport
        mat4 projection_;

		vec4 fog_color_;

        void initRenderStates();

        std::vector<gosTexture*> textureList_;
        std::vector<gosFont*> fontList_;
        std::vector<gosBuffer*> bufferList_;
        std::vector<gosVertexDeclaration*> vertexDeclarationList_;
        std::vector<gosRenderMaterial*> materialList_;
		using MaterialDBValue_t = std::map<uint32_t, gosRenderMaterial*>;
		using MaterialDB_t = std::map<std::string, MaterialDBValue_t>;
        MaterialDB_t materialDB_;

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
        
        gosMesh* quads_;
        gosMesh* tris_;
        gosMesh* indexed_tris_;
        gosMesh* lines_;
        gosMesh* points_;
        gosMesh* text_;
        gosRenderMaterial* basic_material_;
        gosRenderMaterial* basic_tex_material_;
        gosRenderMaterial* text_material_;

        gosRenderMaterial* basic_lighted_material_;
        gosRenderMaterial* basic_tex_lighted_material_;
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

        // Terrain extra VBO for world pos + normal
        GLuint terrain_extra_vb_ = 0;
        gos_TERRAIN_EXTRA* terrain_extra_data_ = nullptr;
        int terrain_extra_count_ = 0;
        int terrain_extra_capacity_ = 0;
        int terrain_extra_draw_offset_ = 0;  // consumed offset for per-batch alignment (legacy)

        // Per-batch extras from texture manager (replaces global offset)
        const gos_TERRAIN_EXTRA* terrain_batch_extras_ = nullptr;
        int terrain_batch_extras_count_ = 0;

        // Terrain tessellation MVP (world-to-NDC)
        mat4 terrain_mvp_;
        bool terrain_mvp_valid_ = false;
        vec4 terrain_camera_pos_;  // MC2 world space camera position for TCS LOD
        vec4 terrain_viewport_;    // (vmx, vmy, vax, vay) for EditorCamera shim

        // F1 Stage A-pre Task 7b: probe-only worldToClipGL cache. Written by the
        // now-retired probe setter; retained for diagnostic retirement batch.
        float probeWorldToClipGL_[16] = {};
        bool probeWorldToClipGLValid_ = false;

        gosRenderMaterial* terrain_material_ = nullptr;
        glsl_program* thin_terrain_prog_ = nullptr;  // gos_terrain_thin.vert + gos_terrain.frag
        glsl_program* terrain_surface_prog_ = nullptr;  // [TERRAIN_SURFACE] PR-2 gos_terrain_surface.vert + gos_terrain.frag
        glsl_program* water_fast_prog_   = nullptr;  // gos_terrain_water_fast.vert + gos_tex_vertex.frag
        glsl_program* mine_static_prog_  = nullptr;  // PR2c Stage 2c — gos_terrain_mine_static.vert + .frag
        glsl_program* mask_solid_prog_   = nullptr;  // B4 Stage 1b — gos_terrain_mask_solid.vert + gos_terrain.frag
        glsl_program* mask_water_prog_   = nullptr;  // B4 Stage 1c — gos_terrain_mask_water.vert + gos_tex_vertex.frag

        gosRenderMaterial* shadow_terrain_material_ = nullptr;
        gosRenderMaterial* shadow_object_material_ = nullptr;
        // GPU-driven dynamic sun shadow -- depth-only instanced programs (Phase 1).
        // Registered at init; called by Tasks 3+ to draw GPU-batched mechs/buildings
        // into the dynamic shadow FBO.
        glsl_program* shadow_mech_prog_        = nullptr;  // shadow_mech.vert + shadow_instanced.frag
        glsl_program* shadow_static_prop_prog_ = nullptr;  // shadow_static_prop.vert + shadow_instanced.frag
        bool shadow_mode_ = false;
        bool shadow_prepass_active_ = false;
        float terrain_shadow_softness_ = mc2ShadowCsmSoftness();  // env-overridable via MC2_SHADOW_CSM_SOFTNESS
        bool terrain_draw_enabled_ = true;
        GLint shadow_prepass_prev_fbo_ = 0;
        GLint shadow_prepass_prev_viewport_[4] = {0};
        const float* active_light_space_matrix_ = nullptr;  // routes static or dynamic LSM

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
        GLuint terrain_mat_normal_[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        GLuint terrain_normal_array_tex_   = 0;     // GL_TEXTURE_2D_ARRAY; 0 = not built
        bool   terrain_normal_array_dirty_ = false; // rebuild needed at next bind
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

        // Cached uniform locations for terrain shader (avoid per-draw glGetUniformLocation)
        struct TerrainUniformLocs {
            GLint tessLevel = -1, tessDistanceRange = -1, tessDisplace = -1;
            GLint cameraPos = -1, tessDebug = -1, terrainMVP = -1;
            GLint pathTint = -1;  // MC2_SHADER_PATH_TINT debug
            GLint terrainLightDir = -1, detailNormalTiling = -1, detailNormalStrength = -1;
            GLint pomParams = -1, terrainWorldScale = -1, cellBombParams = -1;
            GLint matNormal[5] = {-1, -1, -1, -1, -1};
            GLint matNormalArray = -1;   // sampler2DArray (TERRAIN_NORMAL_ARRAY path)
            GLint lightSpaceMatrix = -1, enableShadows = -1, shadowSoftness = -1, shadowMap = -1;
            GLint dynamicLightSpaceMatrix = -1, enableDynamicShadows = -1, dynamicShadowMap = -1;
            // Item 1 CSM: array-variant dynamic shadow uniforms (only valid when ON)
            GLint dynamicShadowArray = -1, dynamicCascadeMatrices = -1, dynamicCsmCount = -1;
            GLint dynamicCascadeTexelWorld = -1, csmDepthSpan = -1;  // Stage 3 texel bias
            // Per-cascade shadow resolution: separate full-map (last) cascade.
            GLint dynamicFullMapShadow = -1, dynamicFullMapTexelWorld = -1;
            GLint time = -1;
            GLint mapHalfExtent = -1;
            GLint terrainMaterialProfile = -1;  // C1 tactical (mclib/terrain.h)
            GLint worldToClipGL = -1;            // F1 Task 7b probe uniform
            GLint matNormalBoost = -1;           // per-material normal strength (vec4)
            GLint matTiling = -1;                // per-material UV tiling (vec4: rock,grass,dirt,concrete)
            GLint matTilingSnow = -1;            // snow UV tiling (float)
            GLint tintStrengthScale = -1;        // global tint blend scalar
            GLint snowBrightnessDampen = -1;     // <1 darkens detected snow
            // TERRAIN-TINT-UI-1
            GLint tintRock  = -1;                // material base tint (vec3)
            GLint tintGrass = -1;
            GLint tintDirt  = -1;
            // TERRAIN-CLASSIFY-TUNING-1
            GLint terrainClassGrass = -1;        // colormap RGB grass thresholds (vec4)
            GLint terrainClassDirt  = -1;        // colormap RGB dirt thresholds (vec4)
            // TERRAIN-NORMALS-FROM-HEIGHT-1
            GLint terrainHeightTex = -1;             // sampler2D, unit 11 (R32F)
            GLint terrainHeightParams = -1;          // vec4 (gridSide, 1/wuPerVertex, topLeftX, topLeftY)
            // CLIFF SHADOW FLOOR: lifts steep faces off near-black; 0=byte-identical
            GLint terrainCliffShadowFloor = -1;
            // TERRAIN-MATERIAL-LIB-1
            GLint tintConcrete = -1;              // promoted frag-literal (vec3)
            GLint tintSnow     = -1;               // promoted frag-literal (vec3)
            GLint matRoughness = -1;               // per-layer roughness scalar (vec4)
            GLint matAO        = -1;               // per-layer AO scalar (vec4)
            GLint useMaterialLib = -1;             // 0=off (byte-identical); 1=roughness/AO term applied
            // TERRAIN-CONTROLMAP-ALBEDO-1
            GLint controlAlbedoStrength = -1;      // 0=byte-identical, 1=full weight-composed albedo
            GLuint program = 0;
        } terrainLocs_;

        struct ThinTerrainUniformLocs {
            GLint terrainMVP = -1, mvp = -1;
            GLint cameraPos = -1, terrainLightDir = -1;
            GLint detailNormalTiling = -1, detailNormalStrength = -1;
            GLint pomParams = -1, terrainWorldScale = -1, cellBombParams = -1;
            GLint matNormal[5] = {-1,-1,-1,-1,-1};
            GLint matNormalArray = -1;
            GLint tex1 = -1;
            GLint lightSpaceMatrix = -1, enableShadows = -1, shadowSoftness = -1, shadowMap = -1;
            GLint dynamicLightSpaceMatrix = -1, enableDynamicShadows = -1, dynamicShadowMap = -1;
            // Item 1 CSM: array-variant dynamic shadow uniforms (only valid when ON)
            GLint dynamicShadowArray = -1, dynamicCascadeMatrices = -1, dynamicCsmCount = -1;
            GLint dynamicCascadeTexelWorld = -1, csmDepthSpan = -1;  // Stage 3 texel bias
            // Per-cascade shadow resolution: separate full-map (last) cascade.
            GLint dynamicFullMapShadow = -1, dynamicFullMapTexelWorld = -1;
            GLint time = -1, mapHalfExtent = -1;
            GLint ssboRecordBase = -1;
            GLint terrainMaterialProfile = -1;  // C1 tactical (mclib/terrain.h)
            GLint tessDebug = -1;               // shader debug-viz mode (frag mode 1..8)
            GLint pathTint = -1;                // MC2_SHADER_PATH_TINT debug
            GLint matNormalBoost = -1;          // per-material normal strength (vec4)
            GLint matTiling = -1;               // per-material UV tiling (vec4: rock,grass,dirt,concrete)
            GLint matTilingSnow = -1;           // snow UV tiling (float)
            GLint tintStrengthScale = -1;       // global tint blend scalar
            GLint snowBrightnessDampen = -1;    // <1 darkens detected snow
            // TERRAIN-TINT-UI-1
            GLint tintRock  = -1;               // material base tint (vec3)
            GLint tintGrass = -1;
            GLint tintDirt  = -1;
            // TERRAIN-CLASSIFY-TUNING-1
            GLint terrainClassGrass = -1;       // colormap RGB grass thresholds (vec4)
            GLint terrainClassDirt  = -1;       // colormap RGB dirt thresholds (vec4)
            // TERRAIN-NORMALS-FROM-HEIGHT-1
            GLint terrainHeightTex = -1;
            GLint terrainHeightParams = -1;
            GLint terrainCliffShadowFloor = -1;
            // TERRAIN-MATERIAL-LIB-1: shares gosRenderer members with the chunk
            // path (see file header); these locs stay -1 on gos_terrain.frag
            // until the uniforms are declared there too, so the upload calls
            // below are no-ops (>= 0 guarded) and this path is unaffected.
            GLint tintConcrete = -1;
            GLint tintSnow     = -1;
            GLint matRoughness = -1;
            GLint matAO        = -1;
            GLint useMaterialLib = -1;
            // TERRAIN-CONTROLMAP-ALBEDO-1: shares the same identity contract as
            // the chunk path above (default 0.0 -> byte-identical when unbound
            // too, since loc stays -1 until the uniform is declared here).
            GLint controlAlbedoStrength = -1;
            GLuint program = 0;
        } thinTerrainLocs_;

        struct ShadowUniformLocs {
            GLint lightSpaceMatrix = -1, tessLevel = -1, tessDistanceRange = -1;
            GLint tessDisplace = -1, cameraPos = -1, mvp = -1;
            GLint matNormal2 = -1, detailNormalTiling = -1, tex1 = -1;
            GLint matNormalArray = -1;
            GLint terrainMaterialProfile = -1;  // C1 tactical (mclib/terrain.h)
            GLuint program = 0;
        } shadowLocs_;

        void cacheTerrainUniformLocations(GLuint shp) {
            if (terrainLocs_.program == shp) return;  // already cached
            terrainLocs_.program = shp;
            // TERRAIN-TEX-UNIT-MAP-1: verify the highest unit we use fits on this GPU.
            {
                GLint maxUnits = 0;
                glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits);
                if (kTerrainMatNormalUnits[4] >= maxUnits || kTerrainTexUnitHeight >= maxUnits)
                    STOP(("GL_MAX_TEXTURE_IMAGE_UNITS=%d too small; terrain needs unit %d (height) and %d (snow normal)",
                          maxUnits, kTerrainTexUnitHeight, kTerrainMatNormalUnits[4]));
            }
            terrainLocs_.tessLevel = glGetUniformLocation(shp, "tessLevel");
            terrainLocs_.tessDistanceRange = glGetUniformLocation(shp, "tessDistanceRange");
            terrainLocs_.tessDisplace = glGetUniformLocation(shp, "tessDisplace");
            terrainLocs_.cameraPos = glGetUniformLocation(shp, "cameraPos");
            terrainLocs_.tessDebug = glGetUniformLocation(shp, "tessDebug");
            terrainLocs_.pathTint = glGetUniformLocation(shp, "u_pathTint");
            terrainLocs_.terrainMVP = glGetUniformLocation(shp, "u_worldToClipGL");
            terrainLocs_.terrainLightDir = glGetUniformLocation(shp, "terrainLightDir");
            terrainLocs_.detailNormalTiling = glGetUniformLocation(shp, "detailNormalTiling");
            terrainLocs_.detailNormalStrength = glGetUniformLocation(shp, "detailNormalStrength");
            terrainLocs_.pomParams = glGetUniformLocation(shp, "pomParams");
            terrainLocs_.terrainWorldScale = glGetUniformLocation(shp, "terrainWorldScale");
            terrainLocs_.cellBombParams = glGetUniformLocation(shp, "cellBombParams");
            terrainLocs_.matNormal[0] = glGetUniformLocation(shp, "matNormal0");
            terrainLocs_.matNormal[1] = glGetUniformLocation(shp, "matNormal1");
            terrainLocs_.matNormal[2] = glGetUniformLocation(shp, "matNormal2");
            terrainLocs_.matNormal[3] = glGetUniformLocation(shp, "matNormal3");
            terrainLocs_.matNormal[4] = glGetUniformLocation(shp, "matNormal4");
            terrainLocs_.matNormalArray = glGetUniformLocation(shp, "matNormalArray");
            terrainLocs_.lightSpaceMatrix = glGetUniformLocation(shp, "lightSpaceMatrix");
            terrainLocs_.enableShadows = glGetUniformLocation(shp, "enableShadows");
            terrainLocs_.shadowSoftness = glGetUniformLocation(shp, "shadowSoftness");
            terrainLocs_.shadowMap = glGetUniformLocation(shp, "shadowMap");
            terrainLocs_.dynamicLightSpaceMatrix = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
            terrainLocs_.enableDynamicShadows = glGetUniformLocation(shp, "enableDynamicShadows");
            terrainLocs_.dynamicShadowMap = glGetUniformLocation(shp, "dynamicShadowMap");
            terrainLocs_.dynamicShadowArray = glGetUniformLocation(shp, "dynamicShadowArray");
            terrainLocs_.dynamicCascadeMatrices = glGetUniformLocation(shp, "dynamicCascadeMatrices");
            terrainLocs_.dynamicCsmCount = glGetUniformLocation(shp, "dynamicCsmCount");
            terrainLocs_.dynamicCascadeTexelWorld = glGetUniformLocation(shp, "dynamicCascadeTexelWorld");
            terrainLocs_.dynamicFullMapShadow = glGetUniformLocation(shp, "dynamicFullMapShadow");
            terrainLocs_.dynamicFullMapTexelWorld = glGetUniformLocation(shp, "dynamicFullMapTexelWorld");
            terrainLocs_.csmDepthSpan = glGetUniformLocation(shp, "csmDepthSpan");
            terrainLocs_.time = glGetUniformLocation(shp, "time");
            terrainLocs_.mapHalfExtent = glGetUniformLocation(shp, "mapHalfExtent");
            terrainLocs_.terrainMaterialProfile = glGetUniformLocation(shp, "g_terrainMaterialProfile");
            // F1 Task 7b: probe uniform location (flat uniform, not UBO member).
            terrainLocs_.worldToClipGL    = glGetUniformLocation(shp, "u_worldToClipGL");
            terrainLocs_.matNormalBoost   = glGetUniformLocation(shp, "matNormalBoost");
            terrainLocs_.matTiling        = glGetUniformLocation(shp, "matTiling");
            terrainLocs_.matTilingSnow    = glGetUniformLocation(shp, "matTilingSnow");
            terrainLocs_.tintStrengthScale = glGetUniformLocation(shp, "tintStrengthScale");
            terrainLocs_.snowBrightnessDampen = glGetUniformLocation(shp, "snowBrightnessDampen");
            terrainLocs_.tintRock  = glGetUniformLocation(shp, "tintRock");
            terrainLocs_.tintGrass = glGetUniformLocation(shp, "tintGrass");
            terrainLocs_.tintDirt  = glGetUniformLocation(shp, "tintDirt");
            terrainLocs_.terrainClassGrass = glGetUniformLocation(shp, "terrainClassGrass");
            terrainLocs_.terrainClassDirt  = glGetUniformLocation(shp, "terrainClassDirt");
            terrainLocs_.terrainHeightTex             = glGetUniformLocation(shp, "terrainHeightTex");
            terrainLocs_.terrainHeightParams          = glGetUniformLocation(shp, "terrainHeightParams");
            terrainLocs_.terrainCliffShadowFloor =
                glGetUniformLocation(shp, "u_terrainCliffShadowFloor");
            terrainLocs_.tintConcrete   = glGetUniformLocation(shp, "tintConcrete");
            terrainLocs_.tintSnow       = glGetUniformLocation(shp, "tintSnow");
            terrainLocs_.matRoughness   = glGetUniformLocation(shp, "matRoughness");
            terrainLocs_.matAO          = glGetUniformLocation(shp, "matAO");
            terrainLocs_.useMaterialLib = glGetUniformLocation(shp, "u_useMaterialLib");
            terrainLocs_.controlAlbedoStrength = glGetUniformLocation(shp, "u_controlAlbedoStrength");
        }

        void cacheThinTerrainUniformLocations(GLuint shp) {
            if (thinTerrainLocs_.program == shp) return;
            thinTerrainLocs_.program            = shp;
            thinTerrainLocs_.terrainMVP         = glGetUniformLocation(shp, "u_worldToClipGL");
            thinTerrainLocs_.cameraPos          = glGetUniformLocation(shp, "cameraPos");
            thinTerrainLocs_.terrainLightDir    = glGetUniformLocation(shp, "terrainLightDir");
            thinTerrainLocs_.detailNormalTiling   = glGetUniformLocation(shp, "detailNormalTiling");
            thinTerrainLocs_.detailNormalStrength = glGetUniformLocation(shp, "detailNormalStrength");
            thinTerrainLocs_.pomParams          = glGetUniformLocation(shp, "pomParams");
            thinTerrainLocs_.terrainWorldScale  = glGetUniformLocation(shp, "terrainWorldScale");
            thinTerrainLocs_.cellBombParams     = glGetUniformLocation(shp, "cellBombParams");
            thinTerrainLocs_.matNormal[0]       = glGetUniformLocation(shp, "matNormal0");
            thinTerrainLocs_.matNormal[1]       = glGetUniformLocation(shp, "matNormal1");
            thinTerrainLocs_.matNormal[2]       = glGetUniformLocation(shp, "matNormal2");
            thinTerrainLocs_.matNormal[3]       = glGetUniformLocation(shp, "matNormal3");
            thinTerrainLocs_.matNormal[4]       = glGetUniformLocation(shp, "matNormal4");
            thinTerrainLocs_.matNormalArray     = glGetUniformLocation(shp, "matNormalArray");
            thinTerrainLocs_.tex1               = glGetUniformLocation(shp, "tex1");
            thinTerrainLocs_.lightSpaceMatrix   = glGetUniformLocation(shp, "lightSpaceMatrix");
            thinTerrainLocs_.enableShadows      = glGetUniformLocation(shp, "enableShadows");
            thinTerrainLocs_.shadowSoftness     = glGetUniformLocation(shp, "shadowSoftness");
            thinTerrainLocs_.shadowMap          = glGetUniformLocation(shp, "shadowMap");
            thinTerrainLocs_.dynamicLightSpaceMatrix = glGetUniformLocation(shp, "dynamicLightSpaceMatrix");
            thinTerrainLocs_.enableDynamicShadows    = glGetUniformLocation(shp, "enableDynamicShadows");
            thinTerrainLocs_.dynamicShadowMap        = glGetUniformLocation(shp, "dynamicShadowMap");
            thinTerrainLocs_.dynamicShadowArray      = glGetUniformLocation(shp, "dynamicShadowArray");
            thinTerrainLocs_.dynamicCascadeMatrices  = glGetUniformLocation(shp, "dynamicCascadeMatrices");
            thinTerrainLocs_.dynamicCsmCount         = glGetUniformLocation(shp, "dynamicCsmCount");
            thinTerrainLocs_.dynamicCascadeTexelWorld = glGetUniformLocation(shp, "dynamicCascadeTexelWorld");
            thinTerrainLocs_.dynamicFullMapShadow = glGetUniformLocation(shp, "dynamicFullMapShadow");
            thinTerrainLocs_.dynamicFullMapTexelWorld = glGetUniformLocation(shp, "dynamicFullMapTexelWorld");
            thinTerrainLocs_.csmDepthSpan            = glGetUniformLocation(shp, "csmDepthSpan");
            thinTerrainLocs_.time               = glGetUniformLocation(shp, "time");
            thinTerrainLocs_.mapHalfExtent      = glGetUniformLocation(shp, "mapHalfExtent");
            thinTerrainLocs_.ssboRecordBase     = glGetUniformLocation(shp, "ssboRecordBase");
            thinTerrainLocs_.terrainMaterialProfile = glGetUniformLocation(shp, "g_terrainMaterialProfile");
            thinTerrainLocs_.tessDebug          = glGetUniformLocation(shp, "tessDebug");
            thinTerrainLocs_.pathTint           = glGetUniformLocation(shp, "u_pathTint");
            thinTerrainLocs_.matNormalBoost     = glGetUniformLocation(shp, "matNormalBoost");
            thinTerrainLocs_.matTiling          = glGetUniformLocation(shp, "matTiling");
            thinTerrainLocs_.matTilingSnow      = glGetUniformLocation(shp, "matTilingSnow");
            thinTerrainLocs_.tintStrengthScale  = glGetUniformLocation(shp, "tintStrengthScale");
            thinTerrainLocs_.snowBrightnessDampen = glGetUniformLocation(shp, "snowBrightnessDampen");
            thinTerrainLocs_.tintRock  = glGetUniformLocation(shp, "tintRock");
            thinTerrainLocs_.tintGrass = glGetUniformLocation(shp, "tintGrass");
            thinTerrainLocs_.tintDirt  = glGetUniformLocation(shp, "tintDirt");
            thinTerrainLocs_.terrainClassGrass = glGetUniformLocation(shp, "terrainClassGrass");
            thinTerrainLocs_.terrainClassDirt  = glGetUniformLocation(shp, "terrainClassDirt");
            thinTerrainLocs_.terrainHeightTex             = glGetUniformLocation(shp, "terrainHeightTex");
            thinTerrainLocs_.terrainHeightParams          = glGetUniformLocation(shp, "terrainHeightParams");
            thinTerrainLocs_.terrainCliffShadowFloor =
                glGetUniformLocation(shp, "u_terrainCliffShadowFloor");
            // TERRAIN-CONTROLMAP-ALBEDO-1: stays -1 until gos_terrain_thin's frag
            // declares the uniform too (thin path is the dead/legacy indirect
            // shader per recon) -- upload call below is >=0 guarded, no-op.
            thinTerrainLocs_.controlAlbedoStrength =
                glGetUniformLocation(shp, "u_controlAlbedoStrength");
        }

        void cacheShadowUniformLocations(GLuint shp) {
            if (shadowLocs_.program == shp) return;
            shadowLocs_.program = shp;
            shadowLocs_.lightSpaceMatrix = glGetUniformLocation(shp, "lightSpaceMatrix");
            shadowLocs_.tessLevel = glGetUniformLocation(shp, "tessLevel");
            shadowLocs_.tessDistanceRange = glGetUniformLocation(shp, "tessDistanceRange");
            shadowLocs_.tessDisplace = glGetUniformLocation(shp, "tessDisplace");
            shadowLocs_.cameraPos = glGetUniformLocation(shp, "cameraPos");
            shadowLocs_.mvp = glGetUniformLocation(shp, "mvp");
            shadowLocs_.matNormal2 = glGetUniformLocation(shp, "matNormal2");
            shadowLocs_.matNormalArray = glGetUniformLocation(shp, "matNormalArray");
            shadowLocs_.detailNormalTiling = glGetUniformLocation(shp, "detailNormalTiling");
            shadowLocs_.tex1 = glGetUniformLocation(shp, "tex1");
            shadowLocs_.terrainMaterialProfile = glGetUniformLocation(shp, "g_terrainMaterialProfile");
        }

        // ── World-space overlay batches ────────────────────────────────────────
        // Both terrain overlays (cement perimeter) and decals (craters, footprints)
        // use the same vertex layout (WorldOverlayVert, 28 bytes) and VAO setup.
        // Per-draw texture grouping: multiple addTriangle calls within a frame with
        // different texHandle values are batched into separate draw entries.
        struct OverlayBatchEntry_ {
            unsigned int texHandle;
            unsigned int firstVert;    // index into verts vector
            unsigned int vertCount;
        };
        struct OverlayBatch_ {
            GLuint vbo = 0, vao = 0;
            std::vector<WorldOverlayVert> verts;
            std::vector<OverlayBatchEntry_> draws;
        };

        OverlayBatch_ terrainOverlayBatch_;
        OverlayBatch_ decalBatch_;

        glsl_program* overlayProg_ = nullptr;  // terrain_overlay.vert + terrain_overlay.frag
        glsl_program* decalProg_   = nullptr;  // terrain_overlay.vert + decal.frag

        struct OverlayUniformLocs_ {
            GLint terrainMVP     = -1;
            GLint mvp            = -1;
            GLint tex1           = -1;
            GLint fog_color      = -1;
            GLint time           = -1;
            GLint cameraPos      = -1;
            GLint surfaceDebugMode = -1;
            GLint pathTint        = -1;  // MC2_SHADER_PATH_TINT debug
            GLint terrainLightDir = -1;
            GLint mapHalfExtent  = -1;
            // TERRAIN-DECAL-LIGHTING-1a: shared terrain-lighting uniforms on
            // the cement overlay program. Cached for both overlayProg_ and
            // decalProg_ via the same lambda — decalProg_'s shader doesn't
            // declare these, so glGetUniformLocation returns -1 there and
            // the helper skips them (no behavioral change to decal path).
            GLint terrainHeightTex                   = -1;
            GLint terrainHeightParams                = -1;
            GLint terrainCliffShadowFloor            = -1;
            // CEMENT-NIGHT-LIGHT-1: mission flat-pad light (overlayProg_ only;
            // decal.frag does not declare it -> stays -1 -> skipped).
            GLint missionFlatLight                   = -1;  // vec3
            // OVERLAY-BIAS-LINEARIZE-1: bias-mode selector (terrain_overlay.vert,
            // declared on BOTH overlayProg_ and decalProg_ — shared VS).
            GLint overlayBiasLegacy                  = -1;  // int
            // NIGHT-LIGHT-POOLS-2b: light-pool uniforms (terrain_overlay.frag
            // only; decal.frag leaves these -1 -> skipped).
            GLint numPoolLights                      = -1;  // int
            GLint lightPoolSoft                      = -1;  // int
            GLint lightPoolMoverDim                  = -1;  // float
            GLint lightPoolCompress                  = -1;  // int (NIGHT-LIGHT-POOLS-3)
            GLint lightPoolMoverScale                = -1;  // float (NIGHT-LIGHT-POOLS-4)
            GLint lightPoolMoverMax                  = -1;  // int (NIGHT-LIGHT-POOLS-4)
            // PAD-AREA-GLOW-1: pad-area night lift (terrain_overlay.frag only;
            // decal.frag leaves these -1 -> skipped).
            GLint padGlow                            = -1;  // vec3 (premultiplied)
            GLint padGlowSpill                       = -1;  // float (wu)
            GLint padGlowMapSide                     = -1;  // int (0 = grid absent)
            GLint overlayCementAdopt                 = -1;  // OVERLAY-CEMENT-ADOPT-1
            GLint cementAtlasOv                      = -1;  // OVERLAY-CEMENT-ADOPT-1
            GLint cementGridSideOv                   = -1;  // OVERLAY-CEMENT-ADOPT-1
            GLint cementWUPTOv                       = -1;  // OVERLAY-CEMENT-ADOPT-1
            GLint cementSeamFixOv                    = -1;  // OVERLAY-CEMENT-SEAM-PARITY-1
            GLint padGlowHalfMap                     = -1;  // float (wu)
        };
        OverlayUniformLocs_ overlayLocs_;
        OverlayUniformLocs_ decalLocs_;
        uint64_t timeStart_ = 0;          // shared epoch for all time-animated shaders

        void pushToOverlayBatch_(OverlayBatch_& b, const WorldOverlayVert* v3, unsigned int texHandle);
        void uploadOverlayUniforms_(GLuint shp, const OverlayUniformLocs_& L, float elapsed, const float* terrainMvpOverride = nullptr);
};

#endif // GAMEOS_GRAPHICS_RENDERER_INTERNAL_H
