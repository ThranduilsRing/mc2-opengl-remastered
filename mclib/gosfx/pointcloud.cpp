#include"gosfxheaders.hpp"
#include<mlr/mlrpointcloud.hpp>

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline.
#include"particles/batcher.h"
#include"particles/spawn.h"

#include<cstdio>   // VFX-POINTCLOUD-ORACLE diagnostics
#include<cmath>    // std::min / std::max fallback (also in gosfxheaders)

//==========================================================================//
// File:	 gosFX_PointCloud.cpp											//
// Contents: Base gosFX::PointCloud Component								//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
//############################################################################
//########################  gosFX::PointCloud__Specification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::PointCloud__Specification::PointCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	ParticleCloud__Specification(gosFX::PointCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Verify(m_class == PointCloudClassID);
	Verify(gos_GetCurrentHeap() == Heap);
	m_totalParticleSize = gosFX::PointCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::PointCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud__Specification::PointCloud__Specification():
	ParticleCloud__Specification(gosFX::PointCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_totalParticleSize = gosFX::PointCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::PointCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud__Specification*
	gosFX::PointCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	PointCloud__Specification *spec =
		new gosFX::PointCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//############################################################################
//##############################  gosFX::PointCloud  ################################
//############################################################################

gosFX::PointCloud::ClassData*
	gosFX::PointCloud::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			PointCloudClassID,
			"gosFX::PointCloud",
			ParticleCloud::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud::PointCloud(
	Specification *spec,
	unsigned flags
):
	ParticleCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

	gos_PushCurrentHeap(MidLevelRenderer::Heap);
	m_cloudImplementation =
		new MidLevelRenderer::MLRPointCloud(spec->m_maxParticleCount);
	Register_Object(m_cloudImplementation);
	gos_PopCurrentHeap();

	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_localTranslation = Cast_Pointer(Stuff::Point3D*, &m_data[index]);
	index += spec->m_maxParticleCount*sizeof(Stuff::Point3D);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);

	m_cloudImplementation->SetData(
		Cast_Pointer(const int *, &m_activeParticleCount),
		m_P_localTranslation,
		m_P_color
	);
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud::~PointCloud()
{
	Unregister_Object(m_cloudImplementation);
	delete m_cloudImplementation;
}

//------------------------------------------------------------------------------
//
gosFX::PointCloud*
	gosFX::PointCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	PointCloud *cloud = new gosFX::PointCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::PointCloud::Execute(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	//
	//----------------------------------------
	// If we aren't supposed to execute, don't
	//----------------------------------------
	//
	if (!IsExecuted())
		return false;

	//
	//------------------------------------------------------------------
	// Animate the particles.  If it is time for us to die, return false
	//------------------------------------------------------------------
	//
	if (!ParticleCloud::Execute(info))
		return false;

	//
	//-----------------------------------------------------------------------
	// If there are active particles to animate, get the current center point
	// of the bounds
	//-----------------------------------------------------------------------
	//
	if (m_activeParticleCount > 0)
	{
		Stuff::ExtentBox box(Stuff::Point3D::Identity, Stuff::Point3D::Identity);
		Stuff::Point3D *vertex = &m_P_localTranslation[0];
		unsigned i=0;

		//
		//-------------------------------------------------------------------
		// If there is no bounds yet, we need to create our extent box around
		// the first legal point we find
		//-------------------------------------------------------------------
		//
		while (i<m_activeParticleCount)
		{
			Particle *particle = GetParticle(i);
			Check_Object(particle);
			if (particle->m_age < 1.0f)
			{
				Check_Object(vertex);
				box.maxX = vertex->x;
				box.minX = vertex->x;
				box.maxY = vertex->y;
				box.minY = vertex->y;
				box.maxZ = vertex->z;
				box.minZ = vertex->z;
				++vertex;
				++i;
				break;
			}
			++vertex;
			++i;
		}

		//
		//-----------------------------
		// Look for the other particles
		//-----------------------------
		//
		while (i<m_activeParticleCount)
		{
			Particle *particle = GetParticle(i);
			Check_Object(particle);
			if (particle->m_age < 1.0f)
			{
				Check_Object(vertex);
				if (vertex->x > box.maxX)
					box.maxX = vertex->x;
				else if (vertex->x < box.minX)
					box.minX = vertex->x;

				if (vertex->y > box.maxY)
					box.maxY = vertex->y;
				else if (vertex->y < box.minY)
					box.minY = vertex->y;

				if (vertex->z > box.maxZ)
					box.maxZ = vertex->z;
				else if (vertex->z < box.minZ)
					box.minZ = vertex->z;
			}
			++vertex;
			++i;
		}

		//
		//------------------------------------
		// Now, build a info->m_bounds around this box
		//------------------------------------
		//
		Verify(box.maxX >= box.minX);
		Verify(box.maxY >= box.minY);
		Verify(box.maxZ >= box.minZ);
		Stuff::OBB local_bounds = Stuff::OBB::Identity;
		local_bounds.axisExtents.x = 0.5f * (box.maxX - box.minX);
		local_bounds.axisExtents.y = 0.5f * (box.maxY - box.minY);
		local_bounds.axisExtents.z = 0.5f * (box.maxZ - box.minZ);
		local_bounds.localToParent(3,0) = box.minX + local_bounds.axisExtents.x;
		local_bounds.localToParent(3,1) = box.minY + local_bounds.axisExtents.y;
		local_bounds.localToParent(3,2) = box.minZ + local_bounds.axisExtents.z;
		local_bounds.sphereRadius = local_bounds.axisExtents.GetLength();
		if (local_bounds.sphereRadius < Stuff::SMALL)
			local_bounds.sphereRadius = 0.01f;
		Stuff::OBB parent_bounds;
		parent_bounds.Multiply(local_bounds, m_localToParent);
		info->m_bounds->Union(*info->m_bounds, parent_bounds);
	}

	//
	//----------------------------------------------
	// Tell our caller that we get to keep executing
	//----------------------------------------------
	//
	return true;
}

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::CreateNewParticle(
		unsigned index,
		Stuff::Point3D *translation
	)
{
	Check_Object(this);
	Check_Pointer(translation);

	//
	//-----------------------------------------------------------------------
	// Let our parent do creation, then turn on the particle in the cloud and
	// set its position
	//-----------------------------------------------------------------------
	//
	ParticleCloud::CreateNewParticle(index, translation);
	m_cloudImplementation->TurnOn(index);
	Verify(m_cloudImplementation->IsOn(index));
	m_P_localTranslation[index] = *translation;
}

//------------------------------------------------------------------------------
//
bool
	gosFX::PointCloud::AnimateParticle(
		unsigned index,
		const Stuff::LinearMatrix4D *world_to_new_local,
		Stuff::Time till
	)
{
	Check_Object(this);

	//
	//-----------------------------------------------------------------------
	// If this cloud is unparented, we need to transform the point from local
	// space into world space and set the internal position/velocity pointers
	// to these temporary values
	//-----------------------------------------------------------------------
	//
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	Stuff::Scalar age = particle->m_age;
	if (age >= 1.0f)
		return false;
	Set_Statistic(Point_Count, Point_Count+1);
	Stuff::Vector3D *velocity = &particle->m_localLinearVelocity;
	Stuff::Point3D *translation = &m_P_localTranslation[index];
	int sim_mode = GetSimulationMode();
	if (sim_mode == DynamicWorldSpaceSimulationMode)
	{
		Check_Object(translation);
		Check_Object(velocity);
		particle->m_worldLinearVelocity.Multiply(*velocity, m_localToWorld);
		particle->m_worldTranslation.Multiply(*translation, m_localToWorld);
		translation = &particle->m_worldTranslation;
		velocity = &particle->m_worldLinearVelocity;
	}
	Check_Object(translation);
	Check_Object(velocity);

	//
	//------------------------------------------------------------------
	// First, calculate the drag on the particle.  Drag can never assist
	// velocity
	//------------------------------------------------------------------
	//
	Stuff::Scalar seed = particle->m_seed;
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Stuff::Vector3D ether;
	ether.x = spec->m_pEtherVelocityX.ComputeValue(age, seed);
	ether.y = spec->m_pEtherVelocityY.ComputeValue(age, seed);
	ether.z = spec->m_pEtherVelocityZ.ComputeValue(age, seed);
	Stuff::Vector3D accel(Stuff::Vector3D::Identity);

	//
	//-------------------------------------------------------------------
	// Deal with pseudo-world simulation.  In this mode, we interpret the
	// forces as if they are already in worldspace, and we transform them
	// back to local space
	//-------------------------------------------------------------------
	//
	Stuff::Scalar drag = -spec->m_pDrag.ComputeValue(age, seed);
	Max_Clamp(drag, 0.0f);
	if (sim_mode == StaticWorldSpaceSimulationMode)
	{
		Stuff::LinearMatrix4D world_to_effect;
		world_to_effect.Invert(m_localToWorld);
		Stuff::Vector3D local_ether;
		local_ether.MultiplyByInverse(ether, world_to_effect);
		Stuff::Vector3D rel_vel;
		rel_vel.Subtract(*velocity, local_ether);
		accel.Multiply(rel_vel, drag);

		//
		//-----------------------------------------
		// Now, add in acceleration of the particle
		//-----------------------------------------
		//
		Stuff::Vector3D world_accel;
		world_accel.x = spec->m_pAccelerationX.ComputeValue(age, seed);
		world_accel.y = spec->m_pAccelerationY.ComputeValue(age, seed);
		world_accel.z = spec->m_pAccelerationZ.ComputeValue(age, seed);
		Stuff::Vector3D local_accel;
		local_accel.Multiply(world_accel, world_to_effect);
		accel += local_accel;
	}

	//
	//----------------------------------------------------------------------
	// Otherwise, just add the forces in the same space the particles are in
	//----------------------------------------------------------------------
	//
	else
	{
		Stuff::Vector3D rel_vel;
		rel_vel.Subtract(*velocity, ether);
		accel.Multiply(rel_vel, drag);

		//
		//-----------------------------------------
		// Now, add in acceleration of the particle
		//-----------------------------------------
		//
		accel.x += spec->m_pAccelerationX.ComputeValue(age, seed);
		accel.y += spec->m_pAccelerationY.ComputeValue(age, seed);
		accel.z += spec->m_pAccelerationZ.ComputeValue(age, seed);
	}

	//
	//-------------------------------------------------
	// Compute the particle's new velocity and position
	//-------------------------------------------------
	//
	Stuff::Scalar time_slice =
		static_cast<Stuff::Scalar>(till - m_lastRan);
	velocity->AddScaled(*velocity, accel, time_slice);
	translation->AddScaled(*translation, *velocity, time_slice);

	//
	//---------------------------------------------------------------------
	// If we are unparented, we need to transform the velocity and position
	// data back into the NEW local space
	//---------------------------------------------------------------------
	//
	if (sim_mode == DynamicWorldSpaceSimulationMode)
	{
		Check_Object(world_to_new_local);
		particle->m_localLinearVelocity.Multiply(
			particle->m_worldLinearVelocity,
			*world_to_new_local
		);
		m_P_localTranslation[index].Multiply(
			particle->m_worldTranslation,
			*world_to_new_local
		);
	}

	//
	//------------------
	// Animate the color
	//------------------
	//
	Check_Pointer(m_P_color);
	m_P_color[index].red = spec->m_pRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pAlpha.ComputeValue(age, seed);
	return true;
}

//------------------------------------------------------------------------------
//
void gosFX::PointCloud::DestroyParticle(unsigned index)
{
	Check_Object(this);

	m_cloudImplementation->TurnOff(index);
	Verify(!m_cloudImplementation->IsOn(index));
	ParticleCloud::DestroyParticle(index);
}

//------------------------------------------------------------------------------
//
void gosFX::PointCloud::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	// GPU render path (Stage 2'): re-emit current cloud to the batcher on
	// every Draw() call. This matches the legacy path which submits point
	// geometry to MLR render lists each frame. SpawnPoint samples spec
	// curves at resolveSampleAge(m_age) (0.5 unless MC2_VFX_AGE_SAMPLE=1).
	// ParticleCloud::Draw propagates up the chain for EffectCloud children;
	// it does not render point geometry.
	//
	// NOTE: GPU spawn moved from Start() to here. Start()-based emission only
	// filled the batcher for one frame (until the first Flush()), leaving the
	// batcher empty for all subsequent frames.
	if (mc2::particles::Batcher::is_enabled()) {
		if (mc2::particles::Batcher::is_oracle_render_enabled() && m_activeParticleCount > 0) {
			// VFX-POINTCLOUD-ORACLE-HARVEST-1: harvest live CPU PointCloud particles.
			// PointCloud has no per-particle spin, no aspect ratio, no UV sub-rect —
			// simpler than CardCloud. Position from m_P_localTranslation[i] (LOCAL
			// space) transformed to world via local_to_world. Color from per-particle
			// animated array m_P_color[i]. No per-particle size — spec has no size
			// curve; use a 4.0f world-unit constant (kept below the shader's oracle
			// size floor of max(size,8) for velocity=0 path — acceptable for muzzle
			// flare dots). velocity stays (0,0,0): no spin, no aspect. atlasIndex=0.
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
			// Same effect->world transform as the legacy Draw() path.
			Stuff::LinearMatrix4D local_to_world;
			local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);

			mc2::particles::Batcher &batcher = mc2::particles::Batcher::Instance();
			// Full-page UV; PointCloud has no atlas sub-rect.
			batcher.BeginGroup(mlrTex, 0.0f, 0.0f, 1.0f, 1.0f, blendMode, 0u);

			int harvested = 0;
			float minAlpha =  3.0e38f, maxAlpha = -3.0e38f;
			// VFX-AGE-LIFETIME-UPLOAD-1: age range tracker for FIRST_HARVEST log.
			float minAge = 1.0f, maxAge = 0.0f;

			for (int i = 0; i < m_activeParticleCount; ++i) {
				Particle *p = GetParticle(i);
				Check_Object(p);
				if (p->m_age >= 1.0f) continue;   // skip dead slots

				// Transform local-space position to world space.
				Stuff::Point3D wc;
				wc.Multiply(m_P_localTranslation[i], local_to_world);

				const Stuff::RGBAColor &c = m_P_color[i];

				mc2::particles::GpuParticle gp = {};
				gp.position[0] = wc.x;
				gp.position[1] = wc.y;
				gp.position[2] = wc.z;
				gp.color[0]    = c.red;
				gp.color[1]    = c.green;
				gp.color[2]    = c.blue;
				gp.color[3]    = c.alpha;
				// PointCloud has no per-particle size curve; 4.0f world units is a
				// reasonable visible constant for muzzle-flash dots. The oracle
				// billboard path (velocity=(0,0,0)) floors size at max(size,8.0) in
				// the shader, so this results in ~8 wu dot — acceptable.
				gp.size        = 4.0f;
				// VFX-AGE-LIFETIME-UPLOAD-1: upload normalized age and lifetime sentinel.
				gp.age         = static_cast<float>(p->m_age);  // normalized [0,1]
				gp.lifetime    = 1.0f;                           // normalized sentinel (real seconds = 1.0f/p->m_ageRate)
				// velocity stays zero-initialized: no spin, no aspect for PointCloud.
				// atlasIndex stays 0: no animated atlas.

				batcher.Emit(gp);
				++harvested;
				if (c.alpha < minAlpha) minAlpha = c.alpha;
				if (c.alpha > maxAlpha) maxAlpha = c.alpha;
				// VFX-AGE-LIFETIME-UPLOAD-1: track age range
				if (gp.age < minAge) minAge = gp.age;
				if (gp.age > maxAge) maxAge = gp.age;
			}

			// [VFX_ORACLE v1] diagnostics: one-shot on first harvest, then 240-call
			// summary — same cadence as CardCloud oracle to stay consistent.
			if (mc2::particles::Batcher::is_log_enabled()) {
				static bool s_first = false;
				if (!s_first && harvested > 0) {
					s_first = true;
					// VFX-AGE-LIFETIME-UPLOAD-1: age range
					const double ageLo = static_cast<double>(minAge);
					const double ageHi = static_cast<double>(maxAge);
					std::fprintf(stderr,
						"[VFX_ORACLE v1] class=PointCloud FIRST_HARVEST spec=\"%s\" active=%d harvested=%d alpha=[%.3f,%.3f] size=4.0 spin=none atlas=none ageRange=[%.3f,%.3f]\n",
						static_cast<const char*>(spec->m_name),
						m_activeParticleCount, harvested,
						static_cast<double>(minAlpha), static_cast<double>(maxAlpha),
						ageLo, ageHi);
					std::fflush(stderr);
				}
				static unsigned long long s_calls = 0, s_harvTotal = 0;
				s_harvTotal += static_cast<unsigned>(harvested);
				if ((++s_calls % 240ull) == 0ull) {
					const double aLo = (harvested > 0) ? static_cast<double>(minAlpha) : 0.0;
					const double aHi = (harvested > 0) ? static_cast<double>(maxAlpha) : 0.0;
					std::fprintf(stderr,
						"[VFX_ORACLE v1] class=PointCloud calls=%llu active_this_call=%d harvested_this_call=%d emitted_this_call=%d harvestedTotal=%llu fallback=0 alpha_this_call=[%.3f,%.3f]\n",
						s_calls, m_activeParticleCount, harvested, harvested,
						s_harvTotal, aLo, aHi);
					std::fflush(stderr);
				}
			}

			// VFX-WEAPON-FX-RESTORE-OPUS-1: Effect::Draw only; skip MLR re-submission.
			Effect::Draw(info);
			return;
		}

		(void)mc2::particles::Spawn(GetSpecification(), &m_localToWorld, (float)m_seed, (float)m_age);
		// VFX-WEAPON-FX-RESTORE-OPUS-1: Effect::Draw only; skip MLR re-submission.
		Effect::Draw(info);
		return;
	}

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
	
	 	info->m_clipper->DrawEffect(&dInfo);
	}
	ParticleCloud::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::PointCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//------------------------------------------------------------------------------
// B1 Stage 2' C8 — route to GPU particle pipeline when env-gated on.
// Subclass-Start routing catches BOTH top-level direct spawns AND
// children-inside-composites (EffectCloud iterates its children and calls
// child Start which lands here). After ParticleCloud::Start has resolved
// m_seed / m_age / m_localToWorld we hand off to mc2::particles::Spawn
// and skip the legacy newbie-accumulator path; the GPU pipeline owns
// rendering. Lifecycle (Execute / IsExecuted / HasFinished / Kill)
// remains on this subclass.
//
void
	gosFX::PointCloud::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	// C9 fix: ALWAYS call ParticleCloud::Start to initialize per-particle
	// structures (m_birthAccumulator, m_activeParticleCount, etc.) so the
	// destructor and the legacy Execute()/AnimateParticle() per-frame loop
	// walk valid state. C8 skipped this under env-on by calling only
	// Effect::Start; subsequent reads of garbage fields and writes to
	// uninitialized offsets corrupted the heap adjacent to mcTextureManager
	// state, ultimately crashing in MC_TextureNode::get_gosTextureHandle at
	// frame ~3542. Under env-on we ADDITIONALLY emit a particle record into
	// the SSBO batcher; legacy ParticleCloud per-frame work continues
	// (B2 polish will skip Execute when env-on). Legacy ::Draw is A2-gated
	// at the MLR work-leaves so it no-ops.
	ParticleCloud::Start(info);

	// GPU spawn moved to Draw() so it fires every frame (not just once at Start).
}
