// MC_TextureManager::renderLists phases 1-7 (state setup, light and scene-data upload, 3D objects,
// static-shadow build, static-prop registry flush); see txmmgr_render.cpp for the order they run in.

#include "txmmgr_render_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"

mat4 gos2my(Stuff::Matrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		m.entries[12], m.entries[13], m.entries[14], m.entries[15]);
	return m2;
}

mat4 gos2my(Stuff::LinearMatrix4D& m)
{
	mat4 m2(
		m.entries[0], m.entries[1], m.entries[2], m.entries[3],
		m.entries[4], m.entries[5], m.entries[6], m.entries[7],
		m.entries[8], m.entries[9], m.entries[10], m.entries[11],
		0.0f, 0.0f, 0.0f, 1.0f);
	return m2;
}


////////////////////////////////////////////////////////////////////////////////
class ShapeRenderer {

	mat4* world_;
	mat4* view_;
	mat4* wvp_;
	float* viewport_;
	HGOSBUFFER lights_data_;

public:

	void setup(mat4* world, mat4* view, mat4* wvp, float* viewport)
	{
		gosASSERT(world && view && wvp && viewport);
		world_ = world;
		view_ = view;
		wvp_ = wvp;
		viewport_ = viewport;
	}

	void set_lights_data(const HGOSBUFFER lights_data)
	{
		lights_data_ = lights_data;
	}

	void render(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, DWORD texture_id, int light_index, bool isHudElement = false, const TG_RenderShape* renderShape = nullptr)
	{
		gos_SetRenderState(gos_State_Texture, texture_id);
		gos_SetRenderViewport(viewport_[2], viewport_[3], viewport_[0], viewport_[1]);

		HGOSRENDERMATERIAL mat;
		const bool usePbrOverride = renderShape &&
			renderShape->programOverride_ &&
			renderShape->pbrNormalTexture_ &&
			renderShape->pbrOrmTexture_ &&
			renderShape->pbrMaterialSsbo_;
		if (usePbrOverride) {
			mat = renderShape->programOverride_;
		} else if (texture_id == 0) {
			static const HGOSRENDERMATERIAL s_matVertexLighted = gos_getRenderMaterial("gos_vertex_lighted");
			mat = s_matVertexLighted;
		} else {
			static const HGOSRENDERMATERIAL s_matTexVertexLighted = gos_getRenderMaterial("gos_tex_vertex_lighted");
			mat = s_matTexVertexLighted;
		}

		gos_SetRenderMaterialParameterMat4(mat, "world_", (const float*)*world_);
		//gos_SetRenderMaterialParameterMat4(mat, "view_", (const float*)*view_);
		gos_SetRenderMaterialParameterMat4(mat, "wvp_", (const float*)*wvp_);

        float ld[4] = { (float)light_index, 0.0f, 0.0f, 0.0f};
		gos_SetRenderMaterialParameterFloat4(mat, "light_offset_", ld);

		// GPU projection via terrainMVP (skip for HUD elements which need legacy viewport projection)
		gos_SetRenderMaterialParameterInt(mat, "gpuProjection", isHudElement ? 0 : 1);

		// VEHICLE-NIGHT-LIT-1 killswitch: feed calc_light MC2-frame position +
		// normal (matching mech.vert / static_prop.vert) instead of the legacy
		// Stuff-frame pair.
		// See the u_legacyLitMc2Frame block in shaders/gos_tex_vertex_lighted.vert
		// for the full root-cause writeup (vehicles received no point/spot light
		// because the frames were mixed inside GetFalloff's distance test).
		// VEHICLE-LIGHTING-PATH-1: PROMOTED TO DEFAULT-ON, and this is a
		// CORRECTNESS REQUIREMENT of the CacheGpuLightData() call added at
		// gvactor.cpp -- NOT an optional enhancement. DO NOT "clean up" this
		// gate as inert.
		// Why it looked inert before: until this slice, vehicles never
		// populated lightData_ at all, so calc_light took its numLights==0
		// early-return (lighting.hglsl) and returned vec3(1,1,1)
		// regardless of frame. Correcting the FRAME of a row containing ZERO
		// lights is necessarily a no-op -- which is exactly why
		// VEHICLE-NIGHT-LIT-1 measured as "disproven" on its own. It was never
		// wrong, only unreachable.
		// Now that the row is populated, the frame becomes load-bearing:
		// feeding Stuff-frame position/normal against MC2-frame light rows
		// makes length(vertex_world_pos - light_pos) meaningless inside
		// GetFalloff and puts the sun NdotL ~90 degrees off. Shipping the
		// gather with this OFF would produce confidently-wrong lighting.
		// Blast radius is now self-limiting: this program is shared with other
		// legacy lit shapes, but any shape that does not gather still has
		// numLights==0 and returns before the frame is ever read. Only shapes
		// that actually carry lights -- i.e. the vehicles this slice fixes --
		// can observe the flip.
		static const bool s_legacyLitMc2Frame = mc2::gate::get<"MC2_LEGACY_LIT_MC2_FRAME">();
		gos_SetRenderMaterialParameterInt(mat, "u_legacyLitMc2Frame",
			s_legacyLitMc2Frame ? 1 : 0);

		// SUN-AXIS-FIX-1. ObjectLights.light_dir is a STUFF-frame vector --
		// GetLocalForwardInWorld() on a BuildRotation(EulerAngles(pitch,
		// yaw+135, 0)) transform (this file, mclib/camera.cpp/
		//) returns matrix row 2 = (cosP*sinY, -sinP, cosP*cosY).
		// Applying the same Stuff->MC2 swap used for normals and negating for
		// the toward-sun convention gives (cosP*cos a, cosP*sin a, sinP) with
		// a = lightYaw + 45 -- the true spherical sun. The +90deg between
		// yaw+135 and the terrain sun's yaw+45 is EXACTLY cancelled by the
		// swap, which is why the one consumer that swaps (mech.vert,
		// specular) has always been right and the ones that do not are wrong.
		// Proven on all 24 shipping missions offline by
		// tools/class_light_band_harness Stage B/D -- no launch required.
		// Only meaningful on the u_legacyLitMc2Frame=1 branch, which is where
		// the shader hands calc_light an MC2-frame normal; the shader applies
		// it there and nowhere else.
		// DEFAULT OFF. This is a correctness fix but a LARGE and TWO-SIDED
		// visual one -- 22 of 24 missions move, up to 0.53 of full scale, and
		// almost exactly as many surface normals get DARKER as brighter. No
		// human has seen it (engine launches are hard-blocked in this lane), so
		// it ships opt-in: options.cfg SunAxisFix = TRUE enables.
		// Canonical DEFAULT-OFF OPT-IN idiom `(v && v[0] && v[0] != '0')`, so
		// scripts/check-gate-default-agreement.py can read the default OUT of
		// the reader. An unparseable idiom is a FAILURE for a Feature gate.
		static const bool s_sunAxisMc2Fix = mc2::options::current().sunAxisFix;
		gos_SetRenderMaterialParameterInt(mat, "u_sunAxisMc2Fix",
			s_sunAxisMc2Fix ? 1 : 0);

		// VEHICLE-AMBIENT-FLOOR-1: DEFAULT-ON. Finishes VEHICLE-LIGHTING-PATH-1
		// by giving this path the two terms mech.vert has and it did not --
		// the kAmbientFloor = 0.35 seed handed to calc_light, and the
		// clamp(...,0,1) on the result (shaders/mech.vert).
		// VEHICLE-LIGHTING-PATH-1 fixed "vehicles have no light row". It did
		// NOT fix "the truck is darker than the mech next to it", because a
		// vehicle computed base_light + A + S*ndl, unclamped, while a mech
		// computes clamp(0.35 + A + S*ndl, 0, 1). base_light is the per-vertex
		// aRGBLight seed: measured across the 8 stock ground-vehicle .tgl
		// binaries it is 0x000000 on 56-110 verts/file, 0x010101 on 3-6, and
		// 0x646257 (0.392, the only value above the 0.35 floor) on at most 2.
		// See the base_light block in shaders/gos_tex_vertex_lighted.vert.
		// On mc2_10 --
		// the map in the user's report -- that is 0.753 against 1.0 on shadow
		// faces, so the reported symptom survived the fix intended to cure it;
		// on mc2_03 the vehicle peak is 0.796, below 1.0 at EVERY normal, so
		// every vehicle pixel darkened. With this on, the vehicle expression is
		// arithmetically identical to the mech expression for the same row.
		// Blast radius is self-limiting for the same reason as
		// MC2_LEGACY_LIT_MC2_FRAME above: a shape that never gathers still has
		// numLights == 0, so calc_light returns vec3(1,1,1) before it reads the
		// seed (lighting.hglsl), and clamp(vec3(1),0,1) == vec3(1).
		// Only shapes carrying a real light row can observe the flip.
		// MC2_VEHICLE_AMBIENT_FLOOR=0 -> u_vehicleAmbientFloor=0 -> no floor,
		// no clamp -> byte-identical to the pre-slice shipped build.
		static const bool s_vehicleAmbientFloor = mc2::gate::get<"MC2_VEHICLE_AMBIENT_FLOOR">();
		gos_SetRenderMaterialParameterInt(mat, "u_vehicleAmbientFloor",
			s_vehicleAmbientFloor ? 1 : 0);

		// [LIGHTSSBO v1] FORK-2: LightsData is now an SSBO; the UBO-
		// reflection bind below would silently no-op (SSBO blocks are not
		// in GL_ACTIVE_UNIFORM_BLOCKS) -> legacy lit meshes would render
		// garbage lighting. Bind the storage block explicitly instead.
		// SceneData stays a UBO.
		gos_SetRenderMaterialUniformBlockBindingPoint(mat, "SceneData", SCENE_DATA_ATTACHMENT_SLOT);

		// GLSTATE-SSBO-SLOT5-RESTORE-1: save prior slot 5 binding so endShadowPass
		// restores it correctly instead of hard-zeroing (NVIDIA may generate
		// GL_INVALID_OPERATION on a shader read from a zero-bound SSBO slot).
		GLuint savedSsbo5 = 0;
		if (usePbrOverride) {
			static int s_pbrRenderTrace = 0;
			if (mc2::gate::get<"MC2_BUILDING_PBR_TRACE">() && s_pbrRenderTrace < 64) {
				++s_pbrRenderTrace;
				mc2::fmt::print_err("[BUILDING_PBR_TRACE] ShapeRenderer::render vb={:016X} ib={:016X} tex={:d} normal={:d} orm={:d} ssbo={:d} light={:d}\n", mc2::fmt::as_ptr((void*)vb), mc2::fmt::as_ptr((void*)ib), mc2::fmt::as_u32((unsigned long)texture_id), mc2::fmt::as_u32((unsigned long)renderShape->pbrNormalTexture_), mc2::fmt::as_u32((unsigned long)renderShape->pbrOrmTexture_), mc2::fmt::as_u32((unsigned long)renderShape->pbrMaterialSsbo_), mc2::fmt::as_i32(light_index));
				fflush(stderr);
			}
			float controls[4] = {
				renderShape->pbrTileScale_,
				renderShape->pbrRoughnessBias_,
				renderShape->pbrMetallicInfluence_,
				0.0f
			};
			gos_SetRenderMaterialParameterFloat4(mat, "u_buildingPbrControls", controls);
			gos_SetRenderMaterialSamplerUnit(mat, "tex1", 0);
			gos_SetRenderMaterialSamplerUnit(mat, "u_normalTex", 1);
			gos_SetRenderMaterialSamplerUnit(mat, "u_ormTex", 2);
			gos_SetRenderState(gos_State_Texture2, renderShape->pbrNormalTexture_);
			gos_SetRenderState(gos_State_Texture3, renderShape->pbrOrmTexture_);
			GLint q5 = 0;
			glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 5, &q5);
			savedSsbo5 = static_cast<GLuint>(q5);
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, (GLuint)renderShape->pbrMaterialSsbo_);
			gos_SetRenderState(gos_State_Culling, gos_Cull_None);
		}

		gos_ApplyRenderMaterial(mat);
		gos_BindLightDataStorageBlock(mat);

		// VEHICLE-LIGHTING-PATH-1: night light-pool parity. mech.frag and
		// static_prop.frag both consume the pool; this legacy lit program
		// was the only object shader with no pool term and no upload, so
		// vehicles received zero contribution from building pools, turret
		// lamps and their own headlights at night. Must come after
		// gos_ApplyRenderMaterial (needs the active program).
		// Gated off (MC2_LIGHT_POOL_OBJECTS) this
		// force-uploads u_numPoolLights=0 -> calcLightPools returns +0.0
		// exactly -> byte-identical.
		gos_UploadObjectLightPools(mat);

		// Bind shadow maps + terrainMVP after apply() (requires active program)
		if (!isHudElement) {
			gos_SetupObjectShadows(mat);
		}

		gos_RenderIndexedArray(ib, vb, vdecl);

		if (usePbrOverride) {
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, savedSsbo5);
			gos_SetRenderState(gos_State_Texture2, 0);
			gos_SetRenderState(gos_State_Texture3, 0);
			gos_SetRenderState(gos_State_Culling, gos_Cull_CW);
		}

	}

};

void MC_TextureManager::renderListsPreamble (void)
{
	{
	ZoneScopedN("RenderLists.Preamble");
	GosGlLostDrawsScope _rlLost_("preamble");
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 1);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 1);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterBiLinear);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 1);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	DWORD fogColor = eye->fogColor;
	// FOG time.  Set Render state to FOG on!
	if (useFog)
	{
		//gos_SetRenderState( gos_State_Fog, (int)&fogColor);
		gos_SetRenderState( gos_State_Fog, fogColor); // sebi
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	gos_SetRenderState(gos_State_Culling, gos_Cull_CW);
	} // end RenderLists.Preamble
}

void MC_TextureManager::renderListsLightDataUpload (void)
{
	{
	ZoneScopedN("RenderLists.LightDataUpload");
	GosGlLostDrawsScope _rlLost_("light_upload");
    // copy global list of light data into GPU buffer

    // [LIGHTSSBO v1] Upload-size FLOOR retained (NOT a removable UBO-window
    // artifact — falsified 2026-05-17). The engine deliberately tolerates
    // transient over-count lightDataIndex for cull-stale actors whose
    // update() was skipped offscreen (see gos_static_prop_registry.cpp
    // comment "...points at a slot ... beyond the upload count"). The
    // floor guarantees those indices still read valid backing memory
    // instead of past-end (-> zero -> black props). std::max keeps the
    // old max(count, 64) semantics; lightData_ is capacity(128)-sized so
    // sourcing 64 entries is in-bounds.
    // [LIGHTSSBO v2] kLightUploadFloor=64 is PRESERVED: totalBytes stays
    // max(count,64)*stride. When count<64 the padded tail [count..64) lands in
    // the per-frame SUFFIX [prefixBytes..totalBytes) and is uploaded every frame,
    // so the deliberate tolerance of transient over-count lightDataIndex for
    // cull-stale offscreen actors still reads valid backing memory — identical
    // in-bounds guarantee to the legacy whole-buffer upload. The split only
    // changes WHICH bytes re-upload each frame, never the uploaded extent.
    constexpr uint32_t kLightUploadFloor = 64u;
    const size_t lightUploadCount =
        std::max<uint32_t>(lightDataStructuresCount, kLightUploadFloor);
    const size_t totalBytes = lightUploadCount * sizeof(TG_HWLightsData);
    // mc2StaticLightHighWater / mc2ConsumeStaticLightPrefixDirty / the split gate
    // are all file-scope fns defined earlier in this TU — call directly.
    if (mc2StaticLightUploadSplitEnabled() && mc2LightBakeEnabled()) {
        // Prefix = permanent static table [0..S), clamped to the floored count.
        const uint32_t S = mc2StaticLightHighWater();
        const size_t prefixCount = (S < lightUploadCount) ? S : lightUploadCount;
        const size_t prefixBytes = prefixCount * sizeof(TG_HWLightsData);
        const bool prefixDirty = mc2ConsumeStaticLightPrefixDirty();
        gos_LightDataSsbo_UploadSplit(lightData_.data(), prefixBytes, totalBytes, prefixDirty);
    } else {
        gos_LightDataSsbo_Upload(lightData_.data(), totalBytes);   // legacy whole-buffer
    }
	} // end RenderLists.LightDataUpload
}

void MC_TextureManager::renderListsSceneDataUpload (void)
{
    // update scene data uniform buffer
    Stuff::Vector3D cp = eye->getCameraOrigin();
    {
        ZoneScopedN("Camera.SceneDataUpload");
        GosGlLostDrawsScope _rlLost_("scene_data");
        sceneData_->fog_start = eye->fogStart;
        sceneData_->fog_end = eye->fogFull;
        sceneData_->min_haze_dist = Camera::MinHazeDistance;
        sceneData_->dist_factor = Camera::DistanceFactor;
        sceneData_->cam_pos[0] = cp.x;
        sceneData_->cam_pos[1] = cp.y;
        sceneData_->cam_pos[2] = cp.z;
        sceneData_->cam_pos[3] = 1.0f;
        vec4 fc = uint32_to_vec4(eye->fogColor);
        sceneData_->fog_color[0] = fc.z;
        sceneData_->fog_color[1] = fc.y;
        sceneData_->fog_color[2] = fc.x;
        sceneData_->fog_color[3] = fc.w;
#ifdef MC2_VULKAN_ISLAND
        // VK-MECH-MATERIAL-LIGHTING-1: cache the RGB fog color the mech VS reads as
        // g_scene.fogColor.rgb so the VK mech island can feed the same value into its
        // per-frame UBO (parity with mech.frag's fog mix). Snapshotted at the SAME site
        // GL uploads the scene UBO — no phase drift. Accessor: mc2GetSceneFogColorRGB().
        extern void mc2SetSceneFogColorRGB(float, float, float);
        mc2SetSceneFogColorRGB(sceneData_->fog_color[0], sceneData_->fog_color[1], sceneData_->fog_color[2]);
#endif
        sceneData_->baseVertexColor = uint32_to_vec4(BaseVertexColor).zyxw();
        gos_UpdateBuffer(sceneDataBuffer_, sceneData_, 0, sizeof(TG_HWSceneData));
    }
}

void MC_TextureManager::renderListsObjects3D (void)
{
	static bool bSkip = true;
	{
		ZoneScopedN("Render.3DObjects");
		TracyGpuZone("Render.3DObjects");
		GosGlLostDrawsScope _rlLost_("obj3d");
		// SAME-ORDER-EXECUTOR-SLICE-2: validate-only ownership of MechOpaque top-level
		// pass. Body sets its own state (no apply); this just validates ambient+FBO.
		render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::OpaqueObject,
		                                          "renderLists_Render.3DObjects");
	for (size_t i = 0; i<nextAvailableHardwareVertexNode; i++)
	{
		if ((masterHardwareVertexNodes[i].flags & MC2_DRAWSOLID) &&
			(masterHardwareVertexNodes[i].shapes))
		{
			gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState(gos_State_AlphaTest,
				(masterHardwareVertexNodes[i].flags & MC2_ALPHATEST) ? 1 : 0);
			if (masterHardwareVertexNodes[i].flags & MC2_ISTERRAIN)
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
			else
				gos_SetRenderState(gos_State_TextureAddress, gos_TextureWrap);

			uint32_t totalShapes = masterHardwareVertexNodes[i].numShapes;
			// in case less shapes were addded in Render() that it was "promised" in Update(), generally etter to investigate and remove all such cases
			if (masterHardwareVertexNodes[i].currentShape != (masterHardwareVertexNodes[i].shapes + masterHardwareVertexNodes[i].numShapes))
			{
				totalShapes = masterHardwareVertexNodes[i].currentShape - masterHardwareVertexNodes[i].shapes;
			}
			for (uint32_t sh = 0; sh < totalShapes; ++sh)
			{
				DWORD textureIndex = masterHardwareVertexNodes[i].textureIndex;
				if (textureIndex == 1227 && bSkip)
					continue;

				static bool b_old_way = false;
				if (b_old_way)
				{
					gos_SetRenderState(gos_State_Texture, masterTextureNodes[textureIndex].get_gosTextureHandle());
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;
					gos_SetRenderViewport(rs->viewport_[2], rs->viewport_[3], rs->viewport_[0], rs->viewport_[1]);

					//gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
					// TODO: set mvp_ in a separate function, like gos_set_render_camera(mvp_)...
					gos_RenderIndexedArray(rs->ib_, rs->vb_, rs->vdecl_, (const float*)rs->mvp_);
				}
				else
				{
					DWORD texture = masterTextureNodes[textureIndex].get_gosTextureHandle();
					TG_RenderShape* rs = masterHardwareVertexNodes[i].shapes + sh;

					mat4 view_mat = gos2my(TG_Shape::s_worldToCamera);
					mat4 world_mat = gos2my(rs->mw_);
					mat4 wvp_mat = gos2my(rs->mvp_);

					ShapeRenderer shape_renderer;
					shape_renderer.setup(&world_mat, &view_mat, &wvp_mat, rs->viewport_);
					shape_renderer.render(rs->vb_, rs->ib_, rs->vdecl_, texture, rs->light_data_buffer_index_, rs->isHudElement_, rs);
				}

			}

			//Reset the list to zero length to avoid drawing more then once!
			//Also comes in handy if gameLogic is not called.
			masterHardwareVertexNodes[i].currentShape = masterHardwareVertexNodes[i].shapes;
			//masterHardwareVertexNodes[i].numShapes = 0;
		}
	}
	gos_SetRenderState(gos_State_AlphaTest, 0);
	} // end Render.3DObjects zone
}

void MC_TextureManager::renderListsPostObjectsStateRestore (void)
{
	{
	ZoneScopedN("RenderLists.PostObjectsStateRestore");
	GosGlLostDrawsScope _rlLost_("state_restore");
	// [Moved in Phase 4 debug] flush() was originally here (after
	// Render.3DObjects). But Render.TerrainSolid runs AFTER us on line
	// ~1287, so terrain was overwriting our building pixels. Flush is
	// now relocated further down, after Render.TerrainSolid completes.

	// restore state as all old-style geometry is culled on CPU and all vertices are already pretransformed
	gos_SetRenderState(gos_State_Culling, gos_Cull_None);

	// restore viewport
	gos_SetRenderViewport(0, 0, Environment.drawableWidth, Environment.drawableHeight);
	} // end RenderLists.PostObjectsStateRestore
}

void MC_TextureManager::renderListsStaticShadowBuild (void)
{
	// VPL-#shadow Phase 1+2 (arch-doc docs/plans/static-terrain-shadow-
	// architecture.md): build the static terrain shadow from the FULL map
	// ONCE. Was: a prime + a camera-windowed accumulate behind a >100u
	// camera-move gate -> the shadow FBO was fed only the ~110 visible
	// terrain nodes and never cleared -> a near-empty depth atlas ->
	// soft half-map shadow wash. Root cause is feed-scope and was
	// probe-proven: the world-fixed ortho light matrix is correct and
	// built-once ([SHADOWFRUSTUM v1] n=1 mapHalfExtent=6400
	// orthoHalf=9503.5; build & sample share getLightSpaceMatrix()).
	// Phase 1 retires the prime block, the camera-motion gate, and the
	// gos_*ShadowRebuild* API (this was their only caller). The build is
	// gated solely by the gos_StaticLightMatrixBuilt() latch, which
	// Terrain::destroy re-arms per mission (C-1) so mission 2+ rebuilds
	// against fresh blocks[]. The MapData full-map feed is stock-safe
	// (no-ops if blocks[] unallocated -> shadow simply absent, never a
	// crash, never worse than a missing shadow).
	// ISSUE-34: keyed on the modern shadow path (shadow_terrain material),
	// not tessellation — static shadow map still builds when tess is off.
	if (gos_IsModernShadowPathActive() && !gos_StaticLightMatrixBuilt() &&
	    Terrain::mapData) {
		ZoneScopedN("Shadow.StaticFullMapBuild");
		TracyGpuZone("Shadow.StaticFullMapBuild");
		GosGlLostDrawsScope _rlLost_("static_shadow_build");

		gos_BuildStaticLightMatrix();   // world-fixed, camera-independent
		gos_MarkStaticLightMatrixBuilt();

		// Any valid terrain colormap: the shadow prepass is depth-only
		// (shadow_terrain.tese = plain lightSpaceMatrix*worldPos, not
		// sampled for depth), so the exact texture is irrelevant -- bind
		// the first terrain node's, else the solid default (idx 0).
		unsigned long shTex = masterTextureNodes[0].get_gosTextureHandle();
		for (long si = 0; si < nextAvailableVertexNode; si++) {
			if ((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
				(masterVertexNodes[si].flags & MC2_ISTERRAIN)) {
				shTex = masterTextureNodes[masterVertexNodes[si].textureIndex].get_gosTextureHandle();
				break;
			}
		}

		gos_BeginShadowPrePass(true);   // one-shot clear (no accumulate)
		Terrain::mapData->renderStaticTerrainShadowFullMap(indexArray, shTex);
		gos_EndShadowPrePass();

		// SHADOW-STATIC-BUILDINGS-2: append rigid BUILDING casters to the world-
		// fixed static shadow map, ONCE (same one-shot build block as terrain).
		// Source = the FULL registry (all buildings, visibility-independent, baked
		// at mission-load registerStatic), NOT per-frame visible buckets (Option B's
		// failure). Trees excluded by the Building population filter. Appends to the
		// terrain depth (gos_BeginShadowPrePass(false) = no clear). Relies on C-pre min-combine
		// (7ea32b83) so a building in both this and the dynamic bounded-near map does
		// not double-darken terrain. Only runs when the prepass actually activates.
		if (gos_BeginShadowPrePass(false)) {  // append, no clear
			// Local (not static): runs once per mission; getBuildingShadowInstances
			// clears+fills it. No need to retain capacity for the process lifetime.
			std::vector<GpuStaticPropInstance> bldgShadowInstances;
			GpuStaticPropRegistry::getBuildingShadowInstances(bldgShadowInstances);
			GpuStaticPropBatcher::instance().drawStaticBuildingShadows(bldgShadowInstances);
			gos_EndShadowPrePass();
		}
	}
}

void MC_TextureManager::renderListsStaticPropRegistryFlush (void)
{
	// STATIC-PROP SHADOW-ORDER FIX (2026-05-29): inject static-registry instances
	// into batcher buckets BEFORE flushShadow() below. flushShadow() calls
	// uploadAllBucketsIfNeeded(), which uploads s_bucketsByType and LOCKS the
	// per-frame SSBO slot (s_lastUploadedSlot=s_frameSlot). If the registry flush
	// ran only at its old site (after this shadow block, before the main flush()),
	// terrain-object instances (trees/fences/prop-buildings) arrived AFTER the slot
	// was locked; the idempotency guard then skipped re-upload, so their typeIDs
	// were absent from s_typeRanges -> zero instances -> INVISIBLE whenever
	// MC2_SHADOW_ENABLE=1. (True buildings submit via submitMultiShape during
	// Render.3DObjects, so they were already in the buckets and unaffected.)
	// Running the registry flush here makes the single shadow-time upload include
	// terrain objects (they now also cast static shadows). Gate mirrors the
	// Render.GpuStaticProps block; runs regardless of tessellation/mech state.
	{
		ZoneScopedN("RenderLists.StaticPropRegistryFlush");
		GosGlLostDrawsScope _rlLost_("sp_registry_flush");
		extern bool g_useGpuStaticProps;
		extern bool g_useGpuObjects;
		if (g_useGpuStaticProps || g_useGpuObjects) {
			GpuStaticPropRegistry::flush();
		}
	} // end RenderLists.StaticPropRegistryFlush
}
