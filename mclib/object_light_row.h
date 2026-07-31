//-----------------------------------------------------------------------------
// mclib/object_light_row.h — LIGHT-ROW-PURE-1
//
// ONE pure implementation of object light-row PRODUCTION: the CPU side that
// decides WHAT LIGHTS EACH OBJECT RECEIVES. This is ARCH-LIGHTING slice 1
// (.claude/ARCH-LIGHTING.md §5). It deliberately does NOT touch shading — no
// formula, no floor, no clamp, no sun-direction policy. Row production only.
//
// WHAT IT REPLACES
// ----------------
// Every appearance class used to produce its row by mutating the ONE shared
// global light list that `eye` owns, gathering, and (sometimes) restoring:
//
//   Building  eye->setLightColor(0, terrainRGB)   -- sun slot recolour, NEVER restored
//   Tree      eye->setLightColor(1, terrainRGB)   -- ambient slot recolour
//             eye->getWorldLight(0)->active=false -- sun DISABLED across its own bake
//             ... gather ...                         then active=true, slot 1 restored
//   Mech      eye->setLightColor(0, terrainRGB)   -- per frame
//   Vehicle   eye->setLightColor(0, terrainRGB)   -- per frame
//
// The policy was therefore implicit in the ORDER of mutations, and the tree's
// sun-off window was GLOBAL: anything that baked inside it inherited
// sun-inactive. Here the same policies are an EXPLICIT DECLARED INPUT
// (Query::sunSlot / Query::ambientSlot), applied per-light inside the gather
// loop, mutating nothing. No window exists, so nothing can bake inside one.
//
// WHAT THIS DOES AND DOES NOT SAY ABOUT PROP-BAKE-ORDER-VS-TIME-1
// (mclib/bdactor.cpp:3134-3156). That defect is unresolved because bake ORDER
// and bake TIME are indistinguishable after the fact — the evidence is a
// per-instance 3x split in the ambient term (0.7529 bright vs 0.2471 dim) and
// both hypotheses predict it.
//
// ★ PROP-BAKE-SUNWINDOW-1 CORRECTION (2026-07-30). An earlier revision of this
// comment stated the ORDER mechanism as fact: "GatherLightsParameters skips
// inactive lights, so inside the tree's sun-off window the AMBIENT becomes row
// slot 0; decomposeFirstActiveLightColor then unconditionally overwrites
// lightColor[0] ... that is the 3x collapse". The first half is true; the
// conclusion is FALSE, and it was copied onward into two other files and into a
// fix brief before anyone executed it. Verified at source:
//   * The gather DOES compact — num_lights advances only inside the
//     `active` branch (mclib/txmmgr.cpp:2835 and the ++ at :2983). So with the
//     sun filtered out, row slot 0 ALREADY holds the ambient.
//   * decomposeFirstActiveLightColor (mclib/txmmgr.cpp:1480) selects the FIRST
//     ACTIVE light off THE SAME list the gather read, so it writes back the
//     bytes the gather just wrote. It is a byte NO-OP, not a collapse.
//     Executed, not narrated: tools/object_light_row_harness
//     ::decompose_slot0_is_a_noop_after_gather, over every class policy x light
//     count x inactive-light hole, with an injection proof (removing the
//     compaction above makes it fail).
//   * The tree window is also lexically CONFINED: `active=false`
//     (bdactor.cpp:7110) and `active=true` (:7242) sit in one function with no
//     early return between them and no other actor's bake inside, and appearance
//     updates are serial by default (MC2_FRAME_JOBS is default-OFF). No non-tree
//     prop can bake inside it.
// The real effect of the tree policy on a row is simply that the SUN IS ABSENT
// (numLights_ drops by one, no INFINITE slot) — which for foliage is the
// intended stock look.
//
// Slot 0 IS clobberable, but only across a light-state change: a row gathered in
// one state and decomposed in another (mclib/txmmgr.cpp:2086, the per-frame
// template copy). sceneLightTemplateKey (:1512) is what forbids that, by hashing
// every light's `active` bit and every non-actor light's aRGB. That invariant is
// pinned by ::decompose_clobbers_slot0_only_across_a_light_state_change.
//
// What this file contributes is still real and still worth having: a pure
// function takes order and time as explicit inputs, so no window with a lifetime
// exists to reason about at all.
//
// DEPENDENCY-FREE BY CONSTRUCTION
// -------------------------------
// Same idiom as mclib/camera_frustum_math.h: header-only pure math over PODs,
// which production delegates to and a standalone contract harness
// (tools/object_light_row_harness/) can compile with NO engine, NO GL and NO
// Stuff types. That is what makes this slice verifiable while game launches are
// blocked. The engine-coupled part (TG_Light / Stuff::LinearMatrix4D ->
// LightInput) is a flat adapter in mclib/txmmgr.cpp and nothing else.
//
// LOCKSTEP: Row mirrors TG_HWLightsData (mclib/tgl.h:331). txmmgr.cpp
// static_asserts size AND every member offset both ways — see kRowLockstep
// there. Do not add fields to either side alone.
//-----------------------------------------------------------------------------
#ifndef MCLIB_OBJECT_LIGHT_ROW_H
#define MCLIB_OBJECT_LIGHT_ROW_H

#include <stdint.h>
#include <string.h>

namespace object_light_row {

// ABI cap, lockstep with MAX_HW_LIGHTS_IN_WORLD (mclib/tgl.h:309) and the
// ObjectLights SSBO in shaders/include/lighting.hglsl.
enum : uint32_t { kMaxLights = 32 };

// Light type ids, lockstep with the TG_LIGHT_* enum (mclib/tgl.h). Mirrored
// rather than included so this header stays engine-free; txmmgr.cpp
// static_asserts each one against the real enum.
// Values are load-bearing: the packer writes the type into lightDir[3], which
// every consumer switches on (shaders/include/lighting.hglsl). txmmgr.cpp
// static_asserts each against the real #define -- and did catch these being
// wrong once, which is why the asserts are there rather than a comment.
enum : uint32_t {
	kTypeNone      = 0xffffffffu,
	kTypeAmbient   = 0,
	kTypeInfinite  = 1,
	kTypeInfiniteWithFalloff = 2,
	kTypePoint     = 3,
	kTypeSpot      = 4,
	kTypeTerrain   = 5
};

//-----------------------------------------------------------------------------
// One source light, flattened from TG_Light by the engine-side adapter.
// Everything the row production reads and nothing else — this list IS the
// answer to "what does a light row actually depend on?", which was previously
// spread across a global list, two arrays and four mutation sites.
//-----------------------------------------------------------------------------
struct LightInput {
	uint32_t type;        // TG_LIGHT_*
	bool     active;      // TG_Light::active AFTER owner/cull composition
	uint32_t aRGB;        // TG_Light::GetaRGB()
	float    l2w[12];     // Stuff::LinearMatrix4D raw (adapter substitutes
	                      //   Identity for AMBIENT, exactly as the legacy gather)
	float    fwd[3];      // light2world.GetLocalForwardInWorld()
	float    falloff[3];  // closeDistance, farDistance, oneOverDistance
};

//-----------------------------------------------------------------------------
// PER-CLASS POLICY — the whole point of the slice.
//
// Which class this row is for. Carried so the row's policy is self-describing
// in traces and so the contract checker can assert the declared shape per
// class. It does NOT select behaviour: the behaviour is entirely in the two
// SlotOverrides below. A policy that changed behaviour by enum value would be
// the old implicit-order problem wearing a new hat.
//-----------------------------------------------------------------------------
enum ClassPolicy : uint32_t {
	kClassNone        = 0,   // no policy: raw gather (legacy GatherLightsParameters)
	kClassBuilding    = 1,   // sun-slot recolour  (terrain light sample)
	kClassTree        = 2,   // ambient-slot recolour + sun DISABLED
	kClassMech        = 3,   // sun-slot recolour, per frame
	kClassVehicle     = 4,   // sun-slot recolour, per frame
	kClassGenericProp = 5    // building pattern
};

// A declared override of ONE light in the source list. Replaces exactly one
// legacy mutation of the shared list. `index` is the light's position in
// Query::lights, resolved by the adapter from the TG_Light* the legacy code
// mutated (pointer identity in, index out) so the header stays POD.
struct SlotOverride {
	int32_t  index         = -1;     // <0 => unused
	bool     recolour      = false;  // substitute aRGB (was: SetaRGB on the shared light)
	uint32_t aRGB          = 0;
	bool     forceInactive = false;  // exclude from the row (was: ->active = false)
};

//-----------------------------------------------------------------------------
// The explicit query. Position and the terrain-light sample are inputs to the
// CALLER's policy decision (they produce the aRGB below); they are carried here
// so a row is fully self-describing for tracing and so the harness can build
// one without an engine.
//-----------------------------------------------------------------------------
struct Query {
	const LightInput* lights      = nullptr;
	uint32_t          numLights   = 0;

	ClassPolicy       policy      = kClassNone;
	SlotOverride      sunSlot;        // building/mech/vehicle recolour; tree disable
	SlotOverride      ambientSlot;    // tree recolour

	// Environment-derived row-production switches, passed EXPLICITLY rather than
	// read from file statics, so the harness can exercise both settings.
	bool              posColumnFix  = true;   // MC2_LIGHT_POS_COLUMN_FIX (default ON)
	bool              clampFixture  = false;  // MC2_LIGHT_CLAMP_FIXTURE  (default OFF)

	// Carried for tracing/self-description only. Never read by the packer.
	float             worldPos[3]        = {0.0f, 0.0f, 0.0f};
	float             terrainLightSample = 1.0f;
};

//-----------------------------------------------------------------------------
// Byte-identical mirror of TG_HWLightsData (mclib/tgl.h:331), 3600 B at N=32.
//-----------------------------------------------------------------------------
struct Row {
	float   lightToWorld[kMaxLights][16];
	float   lightDir[kMaxLights][4];
	float   lightColor[kMaxLights][4];
	float   lightFalloff[kMaxLights][4];
	int32_t numLights_;
	int32_t pad[3];
};

static_assert(sizeof(Row) == 3600,
              "object_light_row::Row must be 3600 B (N=32) — lockstep with TG_HWLightsData");

//-----------------------------------------------------------------------------
// BuildObjectLightRow — THE pure function.
//
// Pure in the sense that matters here: output depends only on `q`, and nothing
// outside `*out` is written. No globals read, no globals written, no light
// object mutated, no env read, no allocation, no logging.
//
// SLOTS BEYOND numLights_ ARE DELIBERATELY LEFT UNTOUCHED. The legacy
// GatherLightsParameters writes only [0, num_lights) into a caller-owned
// struct that is frequently REUSED (leaf->lightData_), so zeroing the tail here
// would produce rows that differ from the shipped ones in exactly the bytes a
// memcmp harness is watching. This is a byte-identity requirement, not an
// oversight — see the harness's reuse test.
//-----------------------------------------------------------------------------
inline void BuildObjectLightRow(const Query& q, Row* out)
{
	if (!out)
		return;

	uint32_t num_lights = 0;
	const uint32_t max_num_lights = kMaxLights;

	for (uint32_t iLight = 0; iLight < q.numLights; ++iLight)
	{
		if (num_lights == max_num_lights)
			break;
		if (!q.lights)
			break;

		const LightInput& L = q.lights[iLight];

		// ---- DECLARED POLICY, applied here instead of by mutating the world ----
		// Exactly reproduces the legacy mutations:
		//   recolour      <=> eye->setLightColor(slot, aRGB)  (TG_Light::SetaRGB)
		//   forceInactive <=> eye->getWorldLight(0)->active = false
		// Order note: an override is matched by index, so two overrides naming the
		// same light is a caller bug; sun is consulted first, matching the legacy
		// sequence in which the sun mutation always preceded the ambient one.
		bool     lightActive = L.active;
		uint32_t lightARGB   = L.aRGB;

		if (q.sunSlot.index == (int32_t)iLight) {
			if (q.sunSlot.forceInactive) lightActive = false;
			if (q.sunSlot.recolour)      lightARGB   = q.sunSlot.aRGB;
		}
		if (q.ambientSlot.index == (int32_t)iLight) {
			if (q.ambientSlot.forceInactive) lightActive = false;
			if (q.ambientSlot.recolour)      lightARGB   = q.ambientSlot.aRGB;
		}

		if (!lightActive)
			continue;

		float* const l2wOut = out->lightToWorld[num_lights];

		// LIGHT-POS-COLUMN-1 (see mclib/txmmgr.cpp for the full derivation):
		// Stuff::AffineMatrix4D is 3 columns x 4 rows, so the world translation
		// lives at entries [3]/[7]/[11]; every GPU consumer decodes the origin as
		// m[3].xyz == floats [12][13][14]. posColumnFix copies them across;
		// otherwise those three floats are zeroed, exactly as shipped pre-fix.
		memcpy(l2wOut, L.l2w, 12 * sizeof(float));
		if (q.posColumnFix) {
			l2wOut[12] = l2wOut[3];
			l2wOut[13] = l2wOut[7];
			l2wOut[14] = l2wOut[11];
		} else {
			l2wOut[12] = l2wOut[13] = l2wOut[14] = 0.0f;
		}
		l2wOut[15] = 1.0f;

		out->lightDir[num_lights][0] = L.fwd[0];
		out->lightDir[num_lights][1] = L.fwd[1];
		out->lightDir[num_lights][2] = L.fwd[2];
		out->lightDir[num_lights][3] = (float)L.type;

		out->lightColor[num_lights][0] = ((lightARGB >> 16) & 0x000000ffu) / 255.0f;
		out->lightColor[num_lights][1] = ((lightARGB >>  8) & 0x000000ffu) / 255.0f;
		out->lightColor[num_lights][2] = ((lightARGB      ) & 0x000000ffu) / 255.0f;
		out->lightColor[num_lights][3] = 1.0f;

		out->lightFalloff[num_lights][0] = L.falloff[0];
		out->lightFalloff[num_lights][1] = L.falloff[1];
		out->lightFalloff[num_lights][2] = L.falloff[2];
		out->lightFalloff[num_lights][3] = 0.0f;

		++num_lights;
	}

	// MC2_LIGHT_CLAMP_FIXTURE — deterministic synthetic point lights filling
	// [num_lights, 32) to prove slots >16 populate and render. Reproduced here
	// bit-for-bit because it is part of what the shipped gather emits when the
	// gate is on, and a row harness that skipped it would be blind to the one
	// path that fills the top half of the row.
	if (q.clampFixture && num_lights < kMaxLights) {
		while (num_lights < kMaxLights) {
			const uint32_t s = num_lights;

			for (int c = 0; c < 16; ++c)
				out->lightToWorld[s][c] = (c % 5 == 0) ? 1.0f : 0.0f;  // identity

			out->lightDir[s][0] = 0.0f;
			out->lightDir[s][1] = -1.0f;
			out->lightDir[s][2] = 0.0f;
			out->lightDir[s][3] = (float)kTypePoint;

			out->lightColor[s][0] = ((s & 1) ? 1.0f : 0.25f);
			out->lightColor[s][1] = ((s & 2) ? 1.0f : 0.25f);
			out->lightColor[s][2] = ((s & 4) ? 1.0f : 0.25f);
			out->lightColor[s][3] = 1.0f;

			out->lightFalloff[s][0] = 0.0f;
			out->lightFalloff[s][1] = 1000.0f;
			out->lightFalloff[s][2] = 1.0f / 1000.0f;
			out->lightFalloff[s][3] = 0.0f;

			++num_lights;
		}
	}

	out->numLights_ = (int32_t)num_lights;
}

//-----------------------------------------------------------------------------
// LEGACY REFERENCE — the mutate-the-shared-list-then-gather dance, modelled.
//
// This exists so the byte-identity proof can be run OFFLINE, with no engine and
// no game launch: `applyLegacyMutations` does to a COPY of the light list
// exactly what the appearance class did to the shared global one, and then the
// row is produced with NO policy. If that row memcmps equal to the pure row
// built from the UNMUTATED list plus the declared policy, then the policy
// faithfully replaces the mutation — which is the claim the slice rests on.
//
// It is reference/test scaffolding, not a second production path: production
// calls BuildObjectLightRow only.
//-----------------------------------------------------------------------------
inline void applyLegacyMutations(LightInput* lights, uint32_t numLights,
                                 const Query& q)
{
	if (!lights) return;
	// Mirrors SetaRGB (aRGB = OEMaRGB = new) and `light->active = false`.
	if (q.sunSlot.index >= 0 && (uint32_t)q.sunSlot.index < numLights) {
		if (q.sunSlot.recolour)      lights[q.sunSlot.index].aRGB   = q.sunSlot.aRGB;
		if (q.sunSlot.forceInactive) lights[q.sunSlot.index].active = false;
	}
	if (q.ambientSlot.index >= 0 && (uint32_t)q.ambientSlot.index < numLights) {
		if (q.ambientSlot.recolour)      lights[q.ambientSlot.index].aRGB   = q.ambientSlot.aRGB;
		if (q.ambientSlot.forceInactive) lights[q.ambientSlot.index].active = false;
	}
}

// Strip the declared policy, leaving a raw gather — what the legacy path ran
// after it had already mutated the world.
inline Query withoutPolicy(const Query& q)
{
	Query r = q;
	r.policy      = kClassNone;
	r.sunSlot     = SlotOverride();
	r.ambientSlot = SlotOverride();
	return r;
}

} // namespace object_light_row

#endif // MCLIB_OBJECT_LIGHT_ROW_H
