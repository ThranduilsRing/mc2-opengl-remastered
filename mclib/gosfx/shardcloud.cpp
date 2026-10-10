#include"gosfxheaders.hpp"
#include<mlr/mlrtrianglecloud.hpp>

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline.
#include"particles/batcher.h"
#include"particles/spawn.h"
#include<cstdio>   // VFX-ORACLE diagnostics
#include<cmath>    // std::atan2 for spin extraction (VFX-SHARDCLOUD-SPIN-ASPECT-1)

//==========================================================================//
// File:	 gosFX_ShardCloud.cpp											//
// Contents: Base gosFX::ShardCloud Component								//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//------------------------------------------------------------------------------
//
gosFX::ShardCloud__Specification::ShardCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	SpinningCloud__Specification(gosFX::ShardCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);
	Verify(m_class == ShardCloudClassID);

	m_size.Load(stream, gfx_version);
	m_angularity.Load(stream, gfx_version);

	m_totalParticleSize = gosFX::ShardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::ShardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud__Specification::ShardCloud__Specification():
	SpinningCloud__Specification(gosFX::ShardCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_totalParticleSize = gosFX::ShardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::ShardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud__Specification*
	gosFX::ShardCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	ShardCloud__Specification *spec =
		new gosFX::ShardCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	SpinningCloud__Specification::Save(stream);

	m_size.Save(stream);
	m_angularity.Save(stream);
}

//------------------------------------------------------------------------------
//
void 
	gosFX::ShardCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	SpinningCloud__Specification::BuildDefaults();

	m_size.m_ageCurve.SetCurve(1.0f);
	m_size.m_seeded = false;
	m_size.m_seedCurve.SetCurve(1.0f);

	m_angularity.m_ageCurve.SetCurve(1.0f);
	m_angularity.m_seeded = false;
	m_angularity.m_seedCurve.SetCurve(1.0f);

}

//------------------------------------------------------------------------------
//
bool 
	gosFX::ShardCloud__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);
return		SpinningCloud__Specification::IsDataValid(fix_data);
/*
	m_size.m_ageCurve.SetCurve(1.0f);
	m_size.m_seeded = false;
	m_size.m_seedCurve.SetCurve(1.0f);

	m_angularity.m_ageCurve.SetCurve(1.0f);
	m_angularity.m_seeded = false;
	m_angularity.m_seedCurve.SetCurve(1.0f);
*/
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud__Specification::Copy(ShardCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	SpinningCloud__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_size = spec->m_size;
	m_angularity = spec->m_angularity;
	gos_PopCurrentHeap();
}

//############################################################################
//##############################  gosFX::ShardCloud  ################################
//############################################################################

gosFX::ShardCloud::ClassData*
	gosFX::ShardCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			ShardCloudClassID,
			"gosFX::ShardCloud",
			SpinningCloud::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud::ShardCloud(
	Specification *spec,
	unsigned flags
):
	SpinningCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

	gos_PushCurrentHeap(MidLevelRenderer::Heap);
	m_cloudImplementation =
		new MidLevelRenderer::MLRTriangleCloud(spec->m_maxParticleCount);
	Register_Object(m_cloudImplementation);
	gos_PopCurrentHeap();

	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_vertices = Cast_Pointer(Stuff::Point3D*, &m_data[index]);
	index += 3*spec->m_maxParticleCount*sizeof(Stuff::Point3D);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);

	m_cloudImplementation->SetData(
		Cast_Pointer(const int *, &m_activeParticleCount),
		m_P_vertices,
		m_P_color
	);
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud::~ShardCloud()
{
	Unregister_Object(m_cloudImplementation);
	delete m_cloudImplementation;
}

//------------------------------------------------------------------------------
//
gosFX::ShardCloud*
	gosFX::ShardCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	ShardCloud *cloud = new gosFX::ShardCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::ShardCloud::AnimateParticle(
		unsigned index,
		const Stuff::LinearMatrix4D *world_to_new_local,
		Stuff::Time till
	)
{
	Check_Object(this);

	//
	//-----------------------------------------
	// Animate the parent then get our pointers
	//-----------------------------------------
	//
	if (!SpinningCloud::AnimateParticle(index, world_to_new_local, till))
		return false;
	Set_Statistic(Shard_Count, Shard_Count+1);
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	Stuff::Scalar seed = particle->m_seed;
	Stuff::Scalar age = particle->m_age;

	//
	//------------------
	// Animate the color
	//------------------
	//
	Check_Pointer(m_P_color);
	index *= 3;
	m_P_color[index].red = spec->m_pRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pAlpha.ComputeValue(age, seed);
	m_P_color[index+2] = m_P_color[index+1] = m_P_color[index];
	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud::CreateNewParticle(
		unsigned index,
		Stuff::Point3D *translation
	)
{
	Check_Object(this);

	//
	//-------------------------------------------------------------------
	// Let our parent do creation, then turn on the particle in the cloud
	//-------------------------------------------------------------------
	//
	SpinningCloud::CreateNewParticle(index, translation);
	m_cloudImplementation->TurnOn(index);
	Verify(m_cloudImplementation->IsOn(index));

	//
	//-----------------------------
	// Figure out the particle size
	//-----------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	particle->m_radius = spec->m_size.ComputeValue(m_age, particle->m_seed);
	particle->m_angle =
		Stuff::Sin(spec->m_angularity.ComputeValue(m_age, particle->m_seed));
}

//------------------------------------------------------------------------------
//
void gosFX::ShardCloud::DestroyParticle(unsigned index)
{
	Check_Object(this);

	m_cloudImplementation->TurnOff(index);
	Verify(!m_cloudImplementation->IsOn(index));
	SpinningCloud::DestroyParticle(index);
}

//------------------------------------------------------------------------------
//
void gosFX::ShardCloud::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	// GPU render path (Stage 2'): re-emit current cloud to the batcher on
	// every Draw() call. This matches the legacy path which submits shard
	// geometry to MLR render lists each frame. SpawnShard samples spec
	// curves at resolveSampleAge(m_age) (0.5 unless MC2_VFX_AGE_SAMPLE=1).
	// SpinningCloud::Draw propagates up the chain for EffectCloud children;
	// it does not render shard geometry.
	//
	// NOTE: GPU spawn moved from Start() to here. Start()-based emission only
	// filled the batcher for one frame (until the first Flush()), leaving the
	// batcher empty for all subsequent frames.
	if (mc2::particles::Batcher::is_enabled()) {
		if (mc2::particles::Batcher::is_oracle_render_enabled()) {
			// VFX-ORIGINAL-SHARDCLOUD-ABI-1 (Phase 1, ShardCloud): CPU-oracle
			// render, mirroring the CardCloud bridge. ParticleCloud::Execute
			// (runs every frame under GPU mode) keeps each live particle current:
			// m_localTranslation + m_scale are advanced per-frame, m_P_color is
			// recomputed in ShardCloud::AnimateParticle (per-VERTEX, 3 per shard,
			// so particle i's color is m_P_color[i*3]), and m_radius/m_angle are
			// per-particle birth-time values (CreateNewParticle). Emit one GPU
			// billboard per LIVE shard (m_age<1) from that real data. CPU sim
			// stays authoritative; legacy MLR draw stays skipped (no dual-draw).
			// DEFERRED to the ABI-extension follow-up: the shard TRIANGLE shape
			// (m_angle), per-particle rotation (m_localRotation), and per-particle
			// UV frame — not representable in the existing 64B GpuParticle record.
			// This slice renders an approximate billboard (center+color+size).
			if (m_activeParticleCount > 0 && m_P_color) {
				Specification *spec = GetSpecification();
				Check_Object(spec);
				const uint32_t mlrTex =
					static_cast<uint32_t>(spec->m_state.GetTextureHandle());
				int blendMode = 0;
				{
					const MidLevelRenderer::MLRState::AlphaMode am =
						spec->m_state.GetAlphaMode();
					if (am == MidLevelRenderer::MLRState::OneOneMode ||
					    am == MidLevelRenderer::MLRState::AlphaOneMode)
						blendMode = 1;
				}
				// Same effect->world transform the legacy ShardCloud::Draw feeds
				// to MLR as effectToWorld.
				Stuff::LinearMatrix4D local_to_world;
				local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);

				mc2::particles::Batcher &batcher = mc2::particles::Batcher::Instance();
				// VFX-SHARDCLOUD-UV-RECT-1: ShardCloud has no m_UOffset/VOffset/USize/VSize
				// spec fields (it uses MLRTriangleCloud, not MLRCardCloud). The UV is always
				// full-page (0,0,1,1) and there is no atlas animation. Pass atlasColumns=0
				// explicitly to match the 7-arg BeginGroup signature from VFX-FLIPBOOK-ASSET-TABLE-1.
				batcher.BeginGroup(mlrTex, 0.0f, 0.0f, 1.0f, 1.0f, blendMode, 0u);

				int harvested = 0;
				float minA    =  3.0e38f, maxA    = -3.0e38f;
				// VFX-SHARDCLOUD-SPIN-ASPECT-1: track size and spin ranges for FIRST_HARVEST log.
				float minSize =  3.0e38f, maxSize = -3.0e38f;
				float minSpin =  3.0e38f, maxSpin = -3.0e38f;
				// VFX-AGE-LIFETIME-UPLOAD-1: age range tracker for FIRST_HARVEST log.
				float minAge = 1.0f, maxAge = 0.0f;
				for (int i = 0; i < m_activeParticleCount; ++i) {
					Particle *p = GetParticle(i);
					Check_Object(p);
					if (p->m_age >= 1.0f) continue;   // skip dead slots (legacy filter)
					Stuff::Point3D wc;
					wc.Multiply(p->m_localTranslation, local_to_world);
					// Billboard radius from per-particle scaled shard radius.
					// ShardCloud particles are triangles (m_angle is the angularity),
					// not quads; there is no separate halfX/halfY. Treat as a square
					// billboard of side = scale*radius (same bounding radius both axes).
					const Stuff::Scalar radius = p->m_scale * p->m_radius;
					// Color array is per-VERTEX (3 per shard); particle i -> i*3.
					const Stuff::RGBAColor &c = m_P_color[i * 3];
					mc2::particles::GpuParticle gp = {};
					gp.position[0] = wc.x;
					gp.position[1] = wc.y;
					gp.position[2] = wc.z;
					gp.color[0]    = c.red;
					gp.color[1]    = c.green;
					gp.color[2]    = c.blue;
					gp.color[3]    = c.alpha;
					gp.size        = static_cast<float>(radius);
					// VFX-AGE-LIFETIME-UPLOAD-1: upload normalized age and lifetime sentinel.
					gp.age         = static_cast<float>(p->m_age);  // normalized [0,1]
					gp.lifetime    = 1.0f;                           // normalized sentinel (real seconds = 1.0f/p->m_ageRate)
					// atlasIndex carries per-particle frame index; 0 for non-animated ShardCloud.
					gp.atlasIndex  = 0u;
					// VFX-SHARDCLOUD-SPIN-ASPECT-1: pack (sizeX, sizeY, spinAngle) into velocity.
					// SpinningCloud__Particle has m_localRotation (UnitQuaternion q=(x,y,z,w)).
					// ShardCloud particles are treated as circular billboards (no halfX/halfY),
					// so sizeX == sizeY == scale*radius.  Spin = 2*atan2(q.z, q.w) — the in-plane
					// rotation component around the world-up axis, matching the CardCloud convention.
					gp.velocity[0] = static_cast<float>(radius);
					gp.velocity[1] = static_cast<float>(radius);
					gp.velocity[2] = 2.0f * std::atan2(
					    static_cast<float>(p->m_localRotation.z),
					    static_cast<float>(p->m_localRotation.w));
					batcher.Emit(gp);
					++harvested;
					if (c.alpha < minA)           minA     = c.alpha;
					if (c.alpha > maxA)           maxA     = c.alpha;
					if (gp.velocity[0] < minSize) minSize  = gp.velocity[0];
					if (gp.velocity[0] > maxSize) maxSize  = gp.velocity[0];
					if (gp.velocity[2] < minSpin) minSpin  = gp.velocity[2];
					if (gp.velocity[2] > maxSpin) maxSpin  = gp.velocity[2];
					// VFX-AGE-LIFETIME-UPLOAD-1: track age range
					if (gp.age < minAge) minAge = gp.age;
					if (gp.age > maxAge) maxAge = gp.age;
				}

				// [VFX_ORACLE v1] diagnostics (mirror CardCloud). harvested =
				// LIVE count (active minus dead m_age>=1 slots) == emitted.
				if (mc2::particles::Batcher::is_log_enabled()) {
					static bool s_first = false;
					if (!s_first && harvested > 0) {
						s_first = true;
						// VFX-SHARDCLOUD-UV-RECT-1: log UV rect (always full-page for ShardCloud)
						// VFX-SHARDCLOUD-SPIN-ASPECT-1: add sizeRange and spinRange
						// VFX-SHARDCLOUD-ORACLE-POLISH-1: add spec name (matches CardCloud format)
						const double szLo = (harvested > 0) ? static_cast<double>(minSize) : 0.0;
						const double szHi = (harvested > 0) ? static_cast<double>(maxSize) : 0.0;
						const double spLo = (harvested > 0) ? static_cast<double>(minSpin)  : 0.0;
						const double spHi = (harvested > 0) ? static_cast<double>(maxSpin)  : 0.0;
						// VFX-AGE-LIFETIME-UPLOAD-1: age range
						const double ageLo = static_cast<double>(minAge);
						const double ageHi = static_cast<double>(maxAge);
						std::fprintf(stderr,
							"[VFX_ORACLE v1] class=ShardCloud FIRST_HARVEST spec=\"%s\" active=%d harvested=%d alpha=[%.3f,%.3f] uvRect=[0.000,0.000,1.000,1.000] atlasColumns=0 sizeRange=[%.3f,%.3f] spinRange=[%.3f,%.3f] ageRange=[%.3f,%.3f]\n",
							static_cast<const char*>(spec->m_name),
							m_activeParticleCount, harvested,
							static_cast<double>(minA), static_cast<double>(maxA),
							szLo, szHi, spLo, spHi,
							ageLo, ageHi);
						std::fflush(stderr);
					}
					static unsigned long long s_calls = 0, s_harvTotal = 0;
					s_harvTotal += static_cast<unsigned>(harvested);
					if ((++s_calls % 240ull) == 0ull) {
						const double aLo   = (harvested > 0) ? static_cast<double>(minA)   : 0.0;
						const double aHi   = (harvested > 0) ? static_cast<double>(maxA)   : 0.0;
						const double ageLo = (harvested > 0) ? static_cast<double>(minAge) : 0.0;
						const double ageHi = (harvested > 0) ? static_cast<double>(maxAge) : 0.0;
						std::fprintf(stderr,
							"[VFX_ORACLE v1] class=ShardCloud calls=%llu active_this_call=%d harvested_this_call=%d emitted_this_call=%d harvestedTotal=%llu fallback=0 alpha_this_call=[%.3f,%.3f] age_this_call=[%.3f,%.3f]\n",
							s_calls, m_activeParticleCount, harvested, harvested,
							s_harvTotal, aLo, aHi, ageLo, ageHi);
						std::fflush(stderr);
					}
				}
			}
		} else {
			(void)mc2::particles::Spawn(GetSpecification(), &m_localToWorld, (float)m_seed, (float)m_age);
		}
		// VFX-WEAPON-FX-RESTORE-OPUS-1: Effect::Draw propagates to children only;
		// avoids re-submitting to MLR now that the gate is re-enabled.
		Effect::Draw(info);
		return;
	}

	//
	//---------------------------------------------------------
	// If we have active particles, set up the draw information
	//---------------------------------------------------------
	//
	if (m_activeParticleCount)
	{
		MidLevelRenderer::DrawEffectInformation dInfo;
		dInfo.effect = m_cloudImplementation;
		Specification *spec = GetSpecification();
		Check_Object(spec);
		dInfo.state.Combine(info->m_state, spec->m_state);
		dInfo.clippingFlags = info->m_clippingFlags;
		Stuff::LinearMatrix4D local_to_world;
		local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
		dInfo.effectToWorld = &local_to_world;

		//
		//--------------------------------------------------------------
		// Check the orientation mode.  The first case is XY orientation
		//--------------------------------------------------------------
		//
		unsigned i;
		unsigned vert=0;
		if (spec->m_alignZUsingX)
		{
			if (spec->m_alignZUsingY)
			{
				//
				//-----------------------------------------
				// Get the camera location into local space
				//-----------------------------------------
				//
				Stuff::Point3D
					camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
				Stuff::Point3D camera_in_cloud;
				camera_in_cloud.MultiplyByInverse(
					camera_in_world,
					local_to_world
				);

				//
				//--------------------------------------
				// Spin through all the active particles
				//--------------------------------------
				//
				for (i = 0; i < m_activeParticleCount; i++)
				{
					Particle *particle = GetParticle(i);
					Check_Object(particle);
					if (particle->m_age < 1.0f)
					{

						//
						//--------------------------------
						// Build the local to cloud matrix
						//--------------------------------
						//
						Stuff::Vector3D direction_in_cloud;
						direction_in_cloud.Subtract(
							camera_in_cloud,
							particle->m_localTranslation
						);
						Stuff::LinearMatrix4D shard_to_cloud;
						shard_to_cloud.BuildRotation(particle->m_localRotation);
						shard_to_cloud.AlignLocalAxisToWorldVector(
							direction_in_cloud,
							Stuff::Z_Axis,
							Stuff::Y_Axis,
							Stuff::X_Axis
						);
						shard_to_cloud.BuildTranslation(particle->m_localTranslation);

						//
						//--------------------------------------------------
						// Figure out the scale, then build the three points
						//--------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								0.0f,
								-0.5f*scale*particle->m_radius,
								0.0f
							),
							shard_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							),
							shard_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							),
							shard_to_cloud
						);
					}
					else
						vert += 3;
				}
			}

			//
			//-----------------------
			// Handle X-only rotation
			//-----------------------
			//
			else
			{
				//
				//-----------------------------------------
				// Get the camera location into local space
				//-----------------------------------------
				//
				Stuff::Point3D
					camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
				Stuff::Point3D camera_in_cloud;
				camera_in_cloud.MultiplyByInverse(
					camera_in_world,
					local_to_world
				);

				//
				//--------------------------------------
				// Spin through all the active particles
				//--------------------------------------
				//
				for (i = 0; i < m_activeParticleCount; i++)
				{
					Particle *particle = GetParticle(i);
					Check_Object(particle);
					if (particle->m_age < 1.0f)
					{

						//
						//--------------------------------
						// Build the local to cloud matrix
						//--------------------------------
						//
						Stuff::Vector3D direction_in_cloud;
						direction_in_cloud.Subtract(
							camera_in_cloud,
							particle->m_localTranslation
						);
						Stuff::LinearMatrix4D shard_to_cloud;
						shard_to_cloud.BuildRotation(particle->m_localRotation);
						shard_to_cloud.AlignLocalAxisToWorldVector(
							direction_in_cloud,
							Stuff::Z_Axis,
							Stuff::X_Axis,
							-1
						);
						shard_to_cloud.BuildTranslation(particle->m_localTranslation);

						//
						//--------------------------------------------------
						// Figure out the scale, then build the three points
						//--------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								0.0f,
								-0.5f*scale*particle->m_radius,
								0.0f
							),
							shard_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							),
							shard_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_angle*particle->m_radius,
								scale*particle->m_radius*0.5f,
								0.0f
							),
							shard_to_cloud
						);
					}
					else
						vert += 3;
				}
			}
		}

		//
		//-------------------------------------------------------
		// Each matrix needs to be aligned to the camera around Y
		//-------------------------------------------------------
		//
		else if (spec->m_alignZUsingY)
		{
			//
			//-----------------------------------------
			// Get the camera location into local space
			//-----------------------------------------
			//
			Stuff::Point3D
				camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
			Stuff::Point3D camera_in_cloud;
			camera_in_cloud.MultiplyByInverse(
				camera_in_world,
				local_to_world
			);

			//
			//--------------------------------------
			// Spin through all the active particles
			//--------------------------------------
			//
			for (i = 0; i < m_activeParticleCount; i++)
			{
				Particle *particle = GetParticle(i);
				Check_Object(particle);
				if (particle->m_age < 1.0f)
				{

					//
					//--------------------------------
					// Build the local to cloud matrix
					//--------------------------------
					//
					Stuff::Vector3D direction_in_cloud;
					direction_in_cloud.Subtract(
						camera_in_cloud,
						particle->m_localTranslation
					);
					Stuff::LinearMatrix4D shard_to_cloud;
					shard_to_cloud.BuildRotation(particle->m_localRotation);
					shard_to_cloud.AlignLocalAxisToWorldVector(
						direction_in_cloud,
						Stuff::Z_Axis,
						Stuff::Y_Axis,
						-1
					);
					shard_to_cloud.BuildTranslation(particle->m_localTranslation);

					//
					//--------------------------------------------------
					// Figure out the scale, then build the three points
					//--------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							0.0f,
							-0.5f*scale*particle->m_radius,
							0.0f
						),
						shard_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						),
						shard_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						),
						shard_to_cloud
					);
				}
				else
					vert += 3;
			}
		}

		//
		//---------------------------------------------------------------
		// No alignment is necessary, so just multiply out all the active
		// particles
		//---------------------------------------------------------------
		//
		else
		{
			for (i = 0; i < m_activeParticleCount; i++)
			{
				Particle *particle = GetParticle(i);
				Check_Object(particle);
				if (particle->m_age < 1.0f)
				{

					//
					//--------------------------------
					// Build the local to cloud matrix
					//--------------------------------
					//
					Stuff::LinearMatrix4D shard_to_cloud;
					shard_to_cloud.BuildRotation(particle->m_localRotation);
					shard_to_cloud.BuildTranslation(particle->m_localTranslation);

					//
					//--------------------------------------------------
					// Figure out the scale, then build the three points
					//--------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							0.0f,
							-0.5f*scale*particle->m_radius,
							0.0f
						),
						shard_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						),
						shard_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_angle*particle->m_radius,
							scale*particle->m_radius*0.5f,
							0.0f
						),
						shard_to_cloud
					);
				}
				else
					vert += 3;
			}
		}

		//
		//---------------------
		// Now just do the draw
		//---------------------
		//
	 	info->m_clipper->DrawEffect(&dInfo);
	}

	SpinningCloud::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::ShardCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//------------------------------------------------------------------------------
// B1 Stage 2' C8 — route to GPU particle pipeline when env-gated on.
// See pointcloud.cpp Start for the full rationale; same pattern.
//
void
	gosFX::ShardCloud::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	// C9 fix: ALWAYS call SpinningCloud::Start (which resolves to the
	// inherited ParticleCloud::Start) so per-particle structures are
	// initialized. C8 skipped this under env-on and the destructor /
	// legacy Execute walked garbage memory, corrupting heap state. See
	// pointcloud.cpp Start for the full rationale.
	SpinningCloud::Start(info);

	// GPU spawn moved to Draw() so it fires every frame (not just once at Start).
}
