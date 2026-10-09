//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"gosfxheaders.hpp"
#include<mlr/mlrcardcloud.hpp>

// B1 Stage 2' C11: subclass-Start routing into the GPU particle pipeline.
#include"particles/batcher.h"
#include"particles/spawn.h"
#include"particles/cardcloud_sim.h"   // VFX-GPU-SIM-CARDCLOUD-BUFFER-1
#include<cmath>    // VFX-ORIGINAL-RENDER-ANIM-FIELDS-1: std::atan2
#include<cstdio>   // VFX-ORACLE diagnostics
#include<vector>

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification::CardCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	SpinningCloud__Specification(gosFX::CardCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(m_class == CardCloudClassID);
	Verify(gos_GetCurrentHeap() == Heap);

	m_halfHeight.Load(stream, gfx_version);
	m_aspectRatio.Load(stream, gfx_version);

	//
	//-------------------------------------------------------------------
	// If we are reading an old version of the card cloud, ignore all the
	// animation on the UV channels
	//-------------------------------------------------------------------
	//
	if (gfx_version < 10)
	{
		m_pIndex.m_ageCurve.SetCurve(0.0f);
		m_pIndex.m_seedCurve.SetCurve(1.0f);
		m_pIndex.m_seeded = false;

		SeededCurveOf<ComplexCurve, LinearCurve,Curve::e_ComplexLinearType> temp;
		temp.Load(stream, gfx_version);
		Stuff::Scalar v = temp.ComputeValue(0.0f, 0.0f);
		m_UOffset.SetCurve(v);

		temp.Load(stream, gfx_version);
		v = temp.ComputeValue(0.0f, 0.0f);
		m_VOffset.SetCurve(v);

		m_USize.Load(stream, gfx_version);
		m_VSize.Load(stream, gfx_version);

		m_animated = false;
	}

	//
	//------------------------------
	// Otherwise, read in the curves
	//------------------------------
	//
	else
	{
		m_pIndex.Load(stream, gfx_version);
		m_UOffset.Load(stream, gfx_version);
		m_VOffset.Load(stream, gfx_version);
		m_USize.Load(stream, gfx_version);
		m_VSize.Load(stream, gfx_version);
		*stream >> m_animated;
	}
	SetWidth();

	m_totalParticleSize = gosFX::CardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::CardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification::CardCloud__Specification():
	SpinningCloud__Specification(gosFX::CardCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_animated = false;
	m_width = 1;
	m_totalParticleSize = gosFX::CardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::CardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification*
	gosFX::CardCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	CardCloud__Specification *spec =
		new gosFX::CardCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	SpinningCloud__Specification::Save(stream);

	m_halfHeight.Save(stream);
	m_aspectRatio.Save(stream);
	m_pIndex.Save(stream);
	m_UOffset.Save(stream);
	m_VOffset.Save(stream);
	m_USize.Save(stream);
	m_VSize.Save(stream);
	*stream << m_animated;
}
//------------------------------------------------------------------------------
//

void 
	gosFX::CardCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	SpinningCloud__Specification::BuildDefaults();

	m_halfHeight.m_ageCurve.SetCurve(1.0f);
	m_halfHeight.m_seeded = false;
	m_halfHeight.m_seedCurve.SetCurve(1.0f);

	m_aspectRatio.m_ageCurve.SetCurve(1.0f);
	m_aspectRatio.m_seeded = false;
	m_aspectRatio.m_seedCurve.SetCurve(1.0f);

	m_pIndex.m_ageCurve.SetCurve(0.0f);
	m_pIndex.m_seeded = false;
	m_pIndex.m_seedCurve.SetCurve(1.0f);

	m_UOffset.SetCurve(0.0f);
	m_VOffset.SetCurve(0.0f);
	m_USize.SetCurve(1.0f);
	m_VSize.SetCurve(1.0f);

	m_animated = false;
	m_width = 1;
}

//------------------------------------------------------------------------------
//

bool 
	gosFX::CardCloud__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);

		Stuff::Scalar max_offset, min_offset;
	Stuff::Scalar max_scale, min_scale;
	m_USize.ExpensiveComputeRange(&min_scale, &max_scale);
	Stuff::Scalar lower = min_scale;
	if (lower > 0.0f)
		lower = 0.0f;
	Stuff::Scalar upper = max_scale;

	//
	//------------------------------------
	// Calculate the worst case UV offsets
	//------------------------------------
	//
	m_VOffset.ExpensiveComputeRange(&min_offset, &max_offset);
	lower += min_offset;
	upper += max_offset;

	if (upper > 99.0f || lower < -99.0f)
	{
		if(fix_data)
		{
			m_VOffset.SetCurve(0.0f);
			PAUSE(("Warning: Curve \"VOffset\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

		}
			else
		return false;
	}
	
	m_VSize.ExpensiveComputeRange(&min_scale, &max_scale);
	lower = min_scale;
	if (lower > 0.0f)
		lower = 0.0f;
	upper = max_scale;

	//
	//------------------------------------
	// Calculate the worst case UV offsets
	//------------------------------------
	//
	//max_offset, min_offset;
	m_UOffset.ExpensiveComputeRange(&min_offset, &max_offset);
	lower += min_offset;
	upper += max_offset;

	if (upper > 99.0f || lower < -99.0f)
	{
		if(fix_data)
		{
			m_UOffset.SetCurve(0.0f);
			PAUSE(("Warning: Curve \"UOffset\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

		}
		else
			return false;
	}

	
	return	SpinningCloud__Specification::IsDataValid(fix_data);

}


//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::Copy(CardCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	SpinningCloud__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_halfHeight = spec->m_halfHeight;
	m_aspectRatio = spec->m_aspectRatio;
	m_pIndex = spec->m_pIndex;
	m_UOffset = spec->m_UOffset;
	m_VOffset = spec->m_VOffset;
	m_USize = spec->m_USize;
	m_VSize = spec->m_VSize;
	m_animated = spec->m_animated;
	m_width = spec->m_width;
	gos_PopCurrentHeap();
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::SetWidth()
{
	m_width =
		static_cast<BYTE>(1.0f / m_USize.ComputeValue(0.0f, 0.0f));
}

//############################################################################
//##############################  gosFX::CardCloud  ################################
//############################################################################

gosFX::CardCloud::ClassData*
	gosFX::CardCloud::DefaultData = NULL;

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			CardCloudClassID,
			"gosFX::CardCloud",
			SpinningCloud::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = NULL;
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud::CardCloud(
	Specification *spec,
	unsigned flags
):
	SpinningCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

	gos_PushCurrentHeap(MidLevelRenderer::Heap);
	m_cloudImplementation =
		new MidLevelRenderer::MLRCardCloud(spec->m_maxParticleCount);
	Register_Object(m_cloudImplementation);
	gos_PopCurrentHeap();

	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_vertices = Cast_Pointer(Stuff::Point3D*, &m_data[index]);
	index += 4*spec->m_maxParticleCount*sizeof(Stuff::Point3D);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);
	index += spec->m_maxParticleCount * sizeof(Stuff::RGBAColor);
	m_P_uvs = Cast_Pointer(Stuff::Vector2DOf<Stuff::Scalar>*, &m_data[index]);

	m_cloudImplementation->SetData(
		Cast_Pointer(const int *, &m_activeParticleCount),
		m_P_vertices,
		m_P_color,
		m_P_uvs
	);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud::~CardCloud()
{
	Unregister_Object(m_cloudImplementation);
	delete m_cloudImplementation;
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud*
	gosFX::CardCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	CardCloud *cloud = new gosFX::CardCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::CreateNewParticle(
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
	particle->m_halfY =
		spec->m_halfHeight.ComputeValue(m_age, particle->m_seed);
	particle->m_halfX =
		particle->m_halfY * spec->m_aspectRatio.ComputeValue(m_age, particle->m_seed);
	particle->m_radius =
		Stuff::Sqrt(
			particle->m_halfX * particle->m_halfX
			 + particle->m_halfY * particle->m_halfY
		);
}

//------------------------------------------------------------------------------
//
bool
	gosFX::CardCloud::AnimateParticle(
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
	Set_Statistic(Card_Count, Card_Count+1);
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
	m_P_color[index].red = spec->m_pRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pAlpha.ComputeValue(age, seed);

	//
	//----------------
	// Animate the uvs
	//----------------
	//
	Stuff::Scalar u = spec->m_UOffset.ComputeValue(age, seed);
	Stuff::Scalar v = spec->m_VOffset.ComputeValue(age, seed);
	Stuff::Scalar u2 = spec->m_USize.ComputeValue(age, seed);
	Stuff::Scalar v2 = spec->m_VSize.ComputeValue(age, seed);

	//
	//--------------------------------------------------------------
	// If we are animated, figure out the row/column to be displayed
	//--------------------------------------------------------------
	//
	if (spec->m_animated)
	{
		BYTE columns =
			Stuff::Truncate_Float_To_Byte(
				spec->m_pIndex.ComputeValue(age, seed)
			);
		BYTE rows = static_cast<BYTE>(columns / spec->m_width);
		columns = static_cast<BYTE>(columns - rows*spec->m_width);

		//
		//---------------------------
		// Now compute the end points
		//---------------------------
		//
		u += u2*columns;
		v += v2*rows;
	}
	u2 += u;
	v2 += v;

	index *= 4;
	m_P_uvs[index].x = u;
	m_P_uvs[index].y = v2;
	m_P_uvs[++index].x = u2;
	m_P_uvs[index].y = v2;
	m_P_uvs[++index].x = u2;
	m_P_uvs[index].y = v;
	m_P_uvs[++index].x = u;
	m_P_uvs[index].y = v;
	return true;
}

//------------------------------------------------------------------------------
//
void gosFX::CardCloud::DestroyParticle(unsigned index)
{
	Check_Object(this);

	m_cloudImplementation->TurnOff(index);
	Verify(!m_cloudImplementation->IsOn(index));
	SpinningCloud::DestroyParticle(index);
}

//------------------------------------------------------------------------------
//
void gosFX::CardCloud::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	// GPU render path (Stage 2'): re-emit current cloud to the batcher on
	// every Draw() call. This matches the legacy path which submits card
	// geometry to MLR render lists each frame. SpawnCardCloud samples spec
	// curves at resolveSampleAge(m_age) (0.5 unless MC2_VFX_AGE_SAMPLE=1)
	// (per-frame CPU-animated positions are a B2 polish item).
	// SpinningCloud::Draw propagates up the chain for EffectCloud children;
	// it does not render card geometry.
	//
	// NOTE: GPU spawn moved from Start() to here. Start()-based emission only
	// filled the batcher for one frame (until the first Flush()), leaving the
	// batcher empty for all subsequent frames.
	if (mc2::particles::Batcher::is_enabled()) {
		// VFX-GPU-SIM-CARDCLOUD-BUFFER-1: observe-only handoff of the live CPU
		// sim state into the persistent GPU sim SSBO. Independent of the oracle
		// render gate; gathers + submits ONLY (no integration, no readback, no
		// render). CPU sim stays authoritative; this does not alter the frame.
		// Nested under is_enabled() (MC2_GPU_PARTICLES, default OFF): if particles
		// are disabled (the default) this gather is a no-op. Per-instance submit
		// (last-writer-wins in the SSBO this slice; per-frame accumulation is
		// COMPUTE-1). COMPUTE-1 adds the compute integration + readback + parity.
		if (mc2::particles::Batcher::is_gpu_sim_cardcloud_enabled()
		    && m_activeParticleCount > 0 && m_P_color) {
			Stuff::LinearMatrix4D local_to_world;
			local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
			// Reused across frames (single-threaded render path) to avoid
			// per-frame allocation.
			static std::vector<mc2::particles::CardCloudSimParticle> s_simRecords;
			s_simRecords.clear();
			s_simRecords.reserve(static_cast<size_t>(m_activeParticleCount));
			for (int i = 0; i < m_activeParticleCount; ++i) {
				Particle *p = GetParticle(i);
				Check_Object(p);
				if (p->m_age >= 1.0f) continue;   // skip dead (compacted live list)
				Stuff::Point3D wc;
				wc.Multiply(p->m_localTranslation, local_to_world);
				const Stuff::Scalar radius =
					p->m_scale * Stuff::Sqrt(p->m_halfX * p->m_halfX + p->m_halfY * p->m_halfY);
				const Stuff::RGBAColor &c = m_P_color[i];
				mc2::particles::CardCloudSimParticle r = {};
				r.position[0] = wc.x;
				r.position[1] = wc.y;
				r.position[2] = wc.z;
				r.ageRate     = static_cast<float>(p->m_ageRate);
				r.velocity[0] = p->m_worldLinearVelocity.x;
				r.velocity[1] = p->m_worldLinearVelocity.y;
				r.velocity[2] = p->m_worldLinearVelocity.z;
				r.age         = static_cast<float>(p->m_age);
				r.color[0]    = c.red;
				r.color[1]    = c.green;
				r.color[2]    = c.blue;
				r.color[3]    = c.alpha;
				r.size        = static_cast<float>(radius);
				r.lifetime    = (p->m_ageRate > 0.0f)
				                  ? static_cast<float>(1.0 / p->m_ageRate) : 0.0f;
				r.flags       = mc2::particles::kCardCloudSimFlagAlive;
				s_simRecords.push_back(r);
			}
			gos_cardcloud_sim_submit(s_simRecords.data(),
			                         static_cast<unsigned>(s_simRecords.size()),
			                         static_cast<unsigned>(m_activeParticleCount));
		}

		if (mc2::particles::Batcher::is_oracle_render_enabled()) {
			// VFX-ORIGINAL-RECORD-ABI-1 (Phase 1, CardCloud): CPU-oracle render.
			// ParticleCloud::Execute (which runs every frame under GPU mode)
			// keeps each live particle current: m_localTranslation and m_scale
			// are advanced per-frame, m_P_color[i] is recomputed in
			// CardCloud::AnimateParticle, and m_halfX/Y are per-particle
			// birth-time extents (set in CreateNewParticle). All are valid at Draw.
			// Emit one GPU billboard per LIVE particle from that real data. CPU
			// sim stays authoritative; the legacy MLR draw stays skipped (no
			// dual-draw). Per-particle rotation (m_localRotation), aspect
			// (halfX vs halfY) and per-particle UV frame are NOT representable in
			// the existing 64B GpuParticle record — deferred to the ABI-extension
			// follow-up (docs/vfx-originals-restoration-design.md).
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
				// Same effect->world transform the legacy CardCloud::Draw feeds
				// to MLR as effectToWorld.
				Stuff::LinearMatrix4D local_to_world;
				local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);

				mc2::particles::Batcher &batcher = mc2::particles::Batcher::Instance();

				// VFX-FLIPBOOK-ASSET-TABLE-1: pass real per-tile UV rect from spec
				// instead of the old placeholder (0,0,1,1). For animated atlases,
				// pass atlasColumns=m_width so the shader can apply per-particle
				// frame offsets (col=frame%columns, row=frame/columns).
				// m_UOffset/m_VOffset are the tile origin within the atlas page;
				// m_USize/m_VSize are the per-tile dimensions (both [0,1] normalized).
				const float tileU0 = static_cast<float>(
					spec->m_UOffset.ComputeValue(0.0f, 0.0f));
				const float tileV0 = static_cast<float>(
					spec->m_VOffset.ComputeValue(0.0f, 0.0f));
				float tileUs = static_cast<float>(
					spec->m_USize.ComputeValue(0.0f, 0.0f));
				float tileVs = static_cast<float>(
					spec->m_VSize.ComputeValue(0.0f, 0.0f));
				// Safety: degenerate tile size falls back to full page.
				if (tileUs <= 0.0f) tileUs = 1.0f;
				if (tileVs <= 0.0f) tileVs = 1.0f;
				// atlasColumns: >1 enables per-particle frame offset in shader.
				// Non-animated effects (m_animated=false or m_width<=1) get 0 —
				// shader reads u_atlasColumns==0 and skips the frame-offset path.
				const uint32_t atlasColumns =
					(spec->m_animated && spec->m_width > 1u)
					? static_cast<uint32_t>(spec->m_width) : 0u;

				batcher.BeginGroup(mlrTex, tileU0, tileV0, tileUs, tileVs,
				                   blendMode, atlasColumns);

				int harvested = 0;
				uint32_t minFrame = 0xFFFFFFFFu, maxFrame = 0u;
				float minA =  3.0e38f, maxA = -3.0e38f;
				// VFX-ORIGINAL-RENDER-ANIM-FIELDS-1: sizeX and spin range trackers.
				float minSizeX = 3.0e38f, maxSizeX = -3.0e38f;
				float minSpin  = 3.0e38f, maxSpin  = -3.0e38f;
				// VFX-AGE-LIFETIME-UPLOAD-1: age range tracker for FIRST_HARVEST log.
				float minAge = 1.0f, maxAge = 0.0f;
				for (int i = 0; i < m_activeParticleCount; ++i) {
					Particle *p = GetParticle(i);
					Check_Object(p);
					if (p->m_age >= 1.0f) continue;   // skip dead slots (legacy filter)
					Stuff::Point3D wc;
					wc.Multiply(p->m_localTranslation, local_to_world);
					// Billboard radius from per-particle scaled half-extents.
					// Aspect (halfX vs halfY) collapsed to a bounding radius —
					// same idiom as the placeholder; per-particle aspect deferred.
					const Stuff::Scalar radius =
						p->m_scale *
						Stuff::Sqrt(p->m_halfX * p->m_halfX + p->m_halfY * p->m_halfY);
					const Stuff::RGBAColor &c = m_P_color[i];

					// VFX-FLIPBOOK-ASSET-TABLE-1: per-particle animated atlas frame.
					// m_pIndex is a SeededCurve returning float frame index [0, N).
					// Match CPU truncation: Truncate_Float_To_Byte (floor toward zero).
					// Seed (m_seed) and age (m_age) are both in ParticleCloud__Particle.
					// Non-animated or degenerate: atlasFrame=0 (first tile, no offset).
					uint32_t atlasFrame = 0u;
					if (atlasColumns > 1u) {
						const float fIdx = static_cast<float>(
							spec->m_pIndex.ComputeValue(
								static_cast<Stuff::Scalar>(p->m_age),
								static_cast<Stuff::Scalar>(p->m_seed)));
						const int fi = static_cast<int>(fIdx);  // truncate (matches CPU)
						atlasFrame = (fi > 0) ? static_cast<uint32_t>(fi) : 0u;
						if (atlasFrame < minFrame) minFrame = atlasFrame;
						if (atlasFrame > maxFrame) maxFrame = atlasFrame;
					}

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
					// atlasIndex carries per-particle frame index (not texture handle).
					// Texture binding happens via GroupInfo.handle (set in BeginGroup above).
					gp.atlasIndex  = atlasFrame;
					// VFX-ORIGINAL-RENDER-ANIM-FIELDS-1: pack (sizeX, sizeY, spinAngle)
					// into velocity. velocity is zero-initialized by gp={} above; the
					// oracle path fills it here. The placeholder path (Spawn) never sets
					// velocity, so the shader fallback (velocity.x==0) applies there.
					// velocity[0] = scale * halfX  (half-width in world units)
					// velocity[1] = scale * halfY  (half-height in world units)
					// velocity[2] = world-space spin angle for the view-aligned billboard.
					// m_localRotation is in the effect's local frame; when the effect has a
					// rotation applied (e.g. Effect_Against_Motion for missiles), we must
					// compose with the parent's world rotation to get the correct screen-space
					// spin. Use UnitQuaternion::operator=(LinearMatrix4D) + Multiply(q1,q2).
					gp.velocity[0] = static_cast<float>(p->m_scale * p->m_halfX);
					gp.velocity[1] = static_cast<float>(p->m_scale * p->m_halfY);
					{
						Stuff::UnitQuaternion parentRot, worldRot;
						parentRot = *info->m_parentToWorld;
						worldRot.Multiply(p->m_localRotation, parentRot);
						gp.velocity[2] = 2.0f * std::atan2(
						    static_cast<float>(worldRot.z),
						    static_cast<float>(worldRot.w));
					}
					batcher.Emit(gp);
					++harvested;
					if (c.alpha < minA) minA = c.alpha;
					if (c.alpha > maxA) maxA = c.alpha;
					// VFX-AGE-LIFETIME-UPLOAD-1: track age range
					if (gp.age < minAge) minAge = gp.age;
					if (gp.age > maxAge) maxAge = gp.age;
					// VFX-ORIGINAL-RENDER-ANIM-FIELDS-1: track sizeX and spin ranges
					if (gp.velocity[0] < minSizeX) minSizeX = gp.velocity[0];
					if (gp.velocity[0] > maxSizeX) maxSizeX = gp.velocity[0];
					if (gp.velocity[2] < minSpin)  minSpin  = gp.velocity[2];
					if (gp.velocity[2] > maxSpin)  maxSpin  = gp.velocity[2];
				}

				// [VFX_ORACLE v1] diagnostics. harvested = LIVE particle count
				// (active minus already-dead m_age>=1 slots) == emitted; no
				// fabrication, no fallback. One-shot on first harvest so sparse
				// clouds are provable; 240-call summary thereafter.
				if (mc2::particles::Batcher::is_log_enabled()) {
					static bool s_first = false;
					if (!s_first && harvested > 0) {
						s_first = true;
						// VFX-FLIPBOOK-ASSET-TABLE-1: log atlasColumns + frame range
						// on first harvest to confirm animated flipbook is active.
						// VFX-ORIGINAL-RENDER-ANIM-FIELDS-1: also log spec name,
						// m_animated, sizeRange, and spinRange.
						const uint32_t frameHi =
							(atlasColumns > 1u && minFrame <= maxFrame) ? maxFrame : 0u;
						const double sizeXLo = (harvested > 0) ? static_cast<double>(minSizeX) : 0.0;
						const double sizeXHi = (harvested > 0) ? static_cast<double>(maxSizeX) : 0.0;
						const double spinLo  = (harvested > 0) ? static_cast<double>(minSpin)  : 0.0;
						const double spinHi  = (harvested > 0) ? static_cast<double>(maxSpin)  : 0.0;
						// VFX-AGE-LIFETIME-UPLOAD-1: age range (min/max after dead-slot skip)
						const double ageLo = static_cast<double>(minAge);
						const double ageHi = static_cast<double>(maxAge);
						std::fprintf(stderr,
							"[VFX_ORACLE v1] class=CardCloud FIRST_HARVEST spec=\"%s\" animated=%d active=%d harvested=%d alpha=[%.3f,%.3f] atlasColumns=%u frameRange=[%u,%u] sizeRange=[%.2f,%.2f] spinRange=[%.3f,%.3f] ageRange=[%.3f,%.3f]\n",
							static_cast<const char*>(spec->m_name),
							static_cast<int>(spec->m_animated),
							m_activeParticleCount, harvested,
							static_cast<double>(minA), static_cast<double>(maxA),
							atlasColumns,
							(atlasColumns > 1u && minFrame <= maxFrame) ? minFrame : 0u,
							frameHi,
							sizeXLo, sizeXHi,
							spinLo, spinHi,
							ageLo, ageHi);
						std::fflush(stderr);
					}
					static unsigned long long s_calls = 0, s_harvTotal = 0;
					s_harvTotal += static_cast<unsigned>(harvested);
					if ((++s_calls % 240ull) == 0ull) {
						// minA/maxA are sentinels if this call harvested 0 live
						// particles; report 0 in that case (avoid 3e38 in the log).
						const double aLo   = (harvested > 0) ? static_cast<double>(minA)   : 0.0;
						const double aHi   = (harvested > 0) ? static_cast<double>(maxA)   : 0.0;
						const double ageLo = (harvested > 0) ? static_cast<double>(minAge) : 0.0;
						const double ageHi = (harvested > 0) ? static_cast<double>(maxAge) : 0.0;
						std::fprintf(stderr,
							"[VFX_ORACLE v1] class=CardCloud calls=%llu active_this_call=%d harvested_this_call=%d emitted_this_call=%d harvestedTotal=%llu fallback=0 alpha_this_call=[%.3f,%.3f] age_this_call=[%.3f,%.3f]\n",
							s_calls, m_activeParticleCount, harvested, harvested,
							s_harvTotal, aLo, aHi, ageLo, ageHi);
						std::fflush(stderr);
					}
				}
			}
		} else {
			(void)mc2::particles::Spawn(GetSpecification(), &m_localToWorld, (float)m_seed, (float)m_age);
		}
		// VFX-WEAPON-FX-RESTORE-OPUS-1: call Effect::Draw (not SpinningCloud::Draw) to
		// propagate to EffectCloud children without re-submitting this cloud to MLR.
		// SpinningCloud::Draw → MLR submission would double-draw now that MLR is re-enabled.
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
						Stuff::LinearMatrix4D card_to_cloud;
						card_to_cloud.BuildRotation(particle->m_localRotation);
						card_to_cloud.AlignLocalAxisToWorldVector(
							direction_in_cloud,
							Stuff::Z_Axis,
							Stuff::Y_Axis,
							Stuff::X_Axis
						);
						card_to_cloud.BuildTranslation(
							particle->m_localTranslation
						);

						//
						//-------------------------------------------------
						// Figure out the scale, then build the four points
						//-------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
					}
					else
						vert += 4;
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
						Stuff::LinearMatrix4D card_to_cloud;
						card_to_cloud.BuildRotation(particle->m_localRotation);
						card_to_cloud.AlignLocalAxisToWorldVector(
							direction_in_cloud,
							Stuff::Z_Axis,
							Stuff::X_Axis,
							-1
						);
						card_to_cloud.BuildTranslation(particle->m_localTranslation);

						//
						//-------------------------------------------------
						// Figure out the scale, then build the four points
						//-------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
					}
					else
						vert += 4;
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
					Stuff::LinearMatrix4D card_to_cloud;
					card_to_cloud.BuildRotation(particle->m_localRotation);
					card_to_cloud.AlignLocalAxisToWorldVector(
						direction_in_cloud,
						Stuff::Z_Axis,
						Stuff::Y_Axis,
						-1
					);
					card_to_cloud.BuildTranslation(particle->m_localTranslation);

					//
					//-------------------------------------------------
					// Figure out the scale, then build the four points
					//-------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
				}
				else
					vert += 4;
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
					Stuff::LinearMatrix4D card_to_cloud;
					card_to_cloud.BuildRotation(particle->m_localRotation);
					card_to_cloud.BuildTranslation(particle->m_localTranslation);

					//
					//-------------------------------------------------
					// Figure out the scale, then build the four points
					//-------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
				}
				else
					vert += 4;
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
	gosFX::CardCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//------------------------------------------------------------------------------
// B1 Stage 2' C11 — route to GPU particle pipeline when env-gated on.
// See pointcloud.cpp Start for the full rationale; same pattern as
// ShardCloud (C5/C8). C10 diagnostic showed CardCloud is 45.9% of stock
// Effect::Start calls in mc2_10 — the dominant per-class spawn type.
//
void
	gosFX::CardCloud::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	// C9 fix: ALWAYS call SpinningCloud::Start (which resolves to the
	// inherited ParticleCloud::Start) so per-particle structures are
	// initialized. Skipping the parent under env-on corrupts heap state
	// because the destructor and legacy Execute walk garbage memory.
	SpinningCloud::Start(info);

	// GPU spawn moved to Draw() for per-frame emission. Start()-based spawning
	// only filled the batcher for a single frame; Draw() re-emits each frame
	// while the effect is alive, matching the legacy MLR submission cadence.
}
