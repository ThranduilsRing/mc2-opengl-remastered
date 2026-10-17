#include"gosfxheaders.hpp"
#include "mlr_cloud_draw.hpp"
#include<mlr/mlrtrianglecloud.hpp>

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline.
#include"particles/batcher.h"
#include<cstdio>   // VFX-ORACLE diagnostics
#include<cmath>    // std::atan2 for spin extraction (VFX-SHARDCLOUD-SPIN-ASPECT-1)
#include "mc2fmt.h"
#include "math/stuff_glm_bridge.h"

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
		mc2::math::sinD(spec->m_angularity.ComputeValue(m_age, particle->m_seed));
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

	// Live particles retain birth randomness; rendering must not resample it.
	// An empty cloud emits nothing but still draws its child effects.
	if (mc2::particles::Batcher::is_enabled()) {
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
			Stuff::LinearMatrix4D local_to_world;
			local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);

			mc2::particles::Batcher &batcher = mc2::particles::Batcher::Instance();
			batcher.BeginGroup(mlrTex, 0.0f, 0.0f, 1.0f, 1.0f, blendMode, 0u);

			int harvested = 0;
			float minA    =  3.0e38f, maxA    = -3.0e38f;
			float minSize =  3.0e38f, maxSize = -3.0e38f;
			float minSpin =  3.0e38f, maxSpin = -3.0e38f;
			float minAge = 1.0f, maxAge = 0.0f;
			for (int i = 0; i < m_activeParticleCount; ++i) {
				Particle *p = GetParticle(i);
				Check_Object(p);
				if (p->m_age >= 1.0f) continue;   // skip dead slots (legacy filter)
				Stuff::Point3D wc;
				wc.Multiply(p->m_localTranslation, local_to_world);
				// Shards remain billboards; triangle-shape parity is a separate migration.
				const Stuff::Scalar radius = p->m_scale * p->m_radius;
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
				gp.age         = static_cast<float>(p->m_age);  // normalized [0,1]
				gp.lifetime    = 1.0f;                           // normalized sentinel (real seconds = 1.0f/p->m_ageRate)
				gp.atlasIndex  = 0u;
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
				if (gp.age < minAge) minAge = gp.age;
				if (gp.age > maxAge) maxAge = gp.age;
			}

			if (mc2::particles::Batcher::is_log_enabled()) {
				static bool s_first = false;
				if (!s_first && harvested > 0) {
					s_first = true;
					const double szLo = (harvested > 0) ? static_cast<double>(minSize) : 0.0;
					const double szHi = (harvested > 0) ? static_cast<double>(maxSize) : 0.0;
					const double spLo = (harvested > 0) ? static_cast<double>(minSpin)  : 0.0;
					const double spHi = (harvested > 0) ? static_cast<double>(maxSpin)  : 0.0;
					const double ageLo = static_cast<double>(minAge);
					const double ageHi = static_cast<double>(maxAge);
					mc2::fmt::print_err("[VFX_ORACLE v1] class=ShardCloud FIRST_HARVEST spec=\"{}\" active={:d} harvested={:d} alpha=[{:.3f},{:.3f}] uvRect=[0.000,0.000,1.000,1.000] atlasColumns=0 sizeRange=[{:.3f},{:.3f}] spinRange=[{:.3f},{:.3f}] ageRange=[{:.3f},{:.3f}]\n", mc2::fmt::safe(static_cast<const char*>(spec->m_name)), mc2::fmt::as_i32(m_activeParticleCount), mc2::fmt::as_i32(harvested), static_cast<double>(minA), static_cast<double>(maxA), szLo, szHi, spLo, spHi, ageLo, ageHi);
					std::fflush(stderr);
				}
				static unsigned long long s_calls = 0, s_harvTotal = 0;
				s_harvTotal += static_cast<unsigned>(harvested);
				if ((++s_calls % 240ull) == 0ull) {
					const double aLo   = (harvested > 0) ? static_cast<double>(minA)   : 0.0;
					const double aHi   = (harvested > 0) ? static_cast<double>(maxA)   : 0.0;
					const double ageLo = (harvested > 0) ? static_cast<double>(minAge) : 0.0;
					const double ageHi = (harvested > 0) ? static_cast<double>(maxAge) : 0.0;
					mc2::fmt::print_err("[VFX_ORACLE v1] class=ShardCloud calls={:d} active_this_call={:d} harvested_this_call={:d} emitted_this_call={:d} harvestedTotal={:d} fallback=0 alpha_this_call=[{:.3f},{:.3f}] age_this_call=[{:.3f},{:.3f}]\n", mc2::fmt::as_u64(s_calls), mc2::fmt::as_i32(m_activeParticleCount), mc2::fmt::as_i32(harvested), mc2::fmt::as_i32(harvested), mc2::fmt::as_u64(s_harvTotal), aLo, aHi, ageLo, ageHi);
					std::fflush(stderr);
				}
			}
		}
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
		Specification *spec = GetSpecification();
		Check_Object(spec);
		MlrCloudDraw submission(info, spec->m_state, m_cloudImplementation, m_localToParent);
		const auto& local_to_world = submission.localToWorld;

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
				camera_in_cloud = mc2::math::toPoint3D(mc2::math::transformPoint(mc2::math::toGlm(camera_in_world), glm::inverse(mc2::math::toGlm(local_to_world))));

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
				camera_in_cloud = mc2::math::toPoint3D(mc2::math::transformPoint(mc2::math::toGlm(camera_in_world), glm::inverse(mc2::math::toGlm(local_to_world))));

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
			camera_in_cloud = mc2::math::toPoint3D(mc2::math::transformPoint(mc2::math::toGlm(camera_in_world), glm::inverse(mc2::math::toGlm(local_to_world))));

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
		submission.submit(info);
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
// Both render arms retain the same particle birth and lifetime state.
void
	gosFX::ShardCloud::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	SpinningCloud::Start(info);
}
