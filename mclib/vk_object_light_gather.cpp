// Object-light gathering and diagnostics for the native Vulkan feeds.
#include "mc2_czstring.h"
#include "render_scene_support.h"
#include "mc2fmt.h"
#include <algorithm>

// T1.15 [SPOT_DIAG v1] pack-probe state (GatherLightsParameters).
// First-shape always-on (one stderr line on the first call after process start).
// Per-summary every 600 GatherLightsParameters calls when env=1. `calls=N`
// because GatherLightsParameters runs once per submitMultiShape (many per frame).
static const bool s_spotDiagPackEnabled = false;
static bool          s_spotDiagPackFirstHit  = false;
static unsigned long s_spotDiagPackCalls     = 0;
static unsigned long s_spotDiagPackActiveSum = 0;
static unsigned long s_spotDiagPackInactSum  = 0;
static unsigned long s_spotDiagPackPointSum  = 0;

// MC2_LIGHT_CLAMP_PROBE (default OFF) — pure observation. Tracks the per-object
// HIGH-WATER populated light count across all GatherLightsParameters calls and
// emits a summary line. Answers "do stock missions ever exceed 16 lights/object?"
// Does NOT change behavior when ON.
static const bool    s_lightClampProbe        = false;
static uint32_t      s_lightClampProbeHighWater = 0;
static unsigned long s_lightClampProbeCalls     = 0;

// MC2_LIGHT_CLAMP_FIXTURE (default OFF) — synthetic injector of deterministic
// extra point lights into slots [populated..32) to prove slots >16 ever render.
// NEVER writes past ABI cap 32 (clamps injection to slot 31). NEVER changes
// stock behavior unless explicitly enabled. Yells a clear log line when active.
static const bool    s_lightClampFixture      = mc2::gate::get<"MC2_LIGHT_CLAMP_FIXTURE">();
static bool          s_lightClampFixtureYelled = false;

// MC2_LIGHT_POS_COLUMN_FIX (default ON as of VEHICLE-LIGHTING-PATH-1) —
// LIGHT-POS-COLUMN-1.
// Writes the light's world translation into the mat4 slots the GLSL/SPIR-V
// consumers actually read as the translation column. See the long comment at
// the memcpy in GatherLightsParameters for the full derivation. Set
// MC2_LIGHT_POS_COLUMN_FIX=0 to restore the shipped (0,0,0) upload.
// PROMOTED TO DEFAULT-ON. Its prior "sub-perceptual, 1/255 delta" measurement
// NO LONGER CHARACTERISES THIS GATE and must not be cited as grounds to demote
// it. That number was taken at sun-dominated fixture poses, where the only
// meaningful contributor is the INFINITE sun -- which has no position term and
// is therefore structurally immune to this bug. It measured the one condition
// under which the fix cannot matter.
// It becomes load-bearing here. With the vehicle light row now populated
// (VEHICLE-LIGHTING-PATH-1), vehicles are ground-level MOVERS that sit close to
// exactly the light classes this bug breaks -- POINT (carnage.cpp explosions,
// weaponbolt.cpp bolts) and SPOT (turret.cpp lamps, guard-tower cones) -- all
// of which were being evaluated at the world origin, so their falloff tracked
// distance-from-origin instead of distance-from-lamp. Requires re-measurement
// at vehicle-adjacent night poses, not at the old fixtures.
static const bool    s_lightPosColumnFix      = mc2::gate::get<"MC2_LIGHT_POS_COLUMN_FIX">();

// MC2_LIGHT_ROW_CENSUS (default OFF) — LIGHT-POS-COMBAT-1. Pure observation.
// The pre-existing MC2_OBJECT_PARITY_TRACE dump latches after the FIRST
// GatherLightsParameters call of the session, so it can only ever sample a
// static opening frame. The lights this lane cares about — carnage.cpp
// explosions (POINT), weaponbolt.cpp bolts (POINT), turret.cpp turret lamps
// (SPOT, falloff 50/450) — are transient and only exist during combat, so the
// latched trace structurally CANNOT observe them. This census instead keeps a
// running per-light-type tally for the whole session and emits:
//   * one FIRST line the first time each positional type (INFINITEWITHFALLOFF
//     /POINT/SPOT) is ever gathered, and
//   * a periodic full dump every kCensusPeriod calls.
// It records BOTH the source translation (dest [3]/[7]/[11], where
// BuildTranslation left it) and the uploaded position ([12]/[13]/[14], what
// every shader actually decodes as the light origin), so the receipt shows
// directly whether the position survives the upload under either gate state.
// Observation only: never changes a single uploaded float.
static const bool    s_lightRowCensus = false;
namespace {
struct LightRowCensusRec {
    unsigned long seen;
    bool          firstEmitted;
    float         src[3];   // dest[3]/[7]/[11] — where BuildTranslation put it
    float         up[3];    // dest[12]/[13]/[14] — what the shaders read
    float         falloff[2];
    float         maxSrcAbs; // largest |component| of src ever seen for this type
};
// TG_LIGHT_* ids are 0..5 (tgl.h); index defensively.
static LightRowCensusRec s_lightRowCensusTab[8] = {};
static unsigned long     s_lightRowCensusCalls  = 0;
static const char* lightRowCensusTypeName(unsigned t) {
    switch (t) {
    case 0: return "AMBIENT";
    case 1: return "INFINITE";
    case 2: return "INFINITEWITHFALLOFF";
    case 3: return "POINT";
    case 4: return "SPOT";
    case 5: return "TERRAIN";
    default: return "UNKNOWN";
    }
}
static void lightRowCensusEmit(mc2::czstring tag, unsigned t) {
    const LightRowCensusRec& r = s_lightRowCensusTab[t & 7];
    mc2::fmt::print_err("[LIGHT_ROW_CENSUS v1] {} type={:d}({}) seen={:d} src=({:.3f},{:.3f},{:.3f}) up=({:.3f},{:.3f},{:.3f}) maxSrcAbs={:.3f} falloff=({:.1f},{:.1f}) fix={:d}\n", mc2::fmt::safe(tag), mc2::fmt::as_u32(t), mc2::fmt::safe(lightRowCensusTypeName(t)), mc2::fmt::as_u32(r.seen), r.src[0], r.src[1], r.src[2], r.up[0], r.up[1], r.up[2], r.maxSrcAbs, r.falloff[0], r.falloff[1], mc2::fmt::as_i32(s_lightPosColumnFix ? 1 : 0));
    std::fflush(stderr);
}
}

void GatherLightsParameters(TG_HWLightsData* lights)
{
	GatherLightsParameters(TG_Shape::lightContext(), lights);
}

void GatherLightsParameters(const TG_LightContext& ctx, TG_HWLightsData* lights)
{
	gosASSERT(lights);

	// Stage 2.D.2 diagnostic: dump gathered lights when MC2_OBJECT_PARITY_TRACE=1.
	// Fired once per session to avoid per-frame spam. Shows what GPU UBO gets.
	static bool s_lightDumpDone = false;
	const bool doLightTrace = (!s_lightDumpDone && false);

	uint32_t num_lights = 0;
	// LIGHT-CLAMP-RAISE-STAGE1-1: runtime population now reaches the full GPU ABI
	// cap (MAX_HW_LIGHTS_IN_WORLD = 32). Stage 0 widened the struct/stride to 32;
	// this slice "allows putting 32 in the box". Safe because every shader per-light
	// loop is min(numLights.x, MAX_LIGHTS_IN_WORLD)-bound (cost/reads follow the
	// populated count), and the break below (last write = slot 31) is the overrun
	// guard. The break loop is cap-agnostic — it just trips later now.
	static constexpr uint32_t kRuntimeLightClamp = MAX_HW_LIGHTS_IN_WORLD; // 32
	const uint32_t max_num_lights = kRuntimeLightClamp;

	const TG_LightPtr* listOfLights = ctx.list;
	const DWORD numLights = ctx.count;

	// T1.15 [SPOT_DIAG v1] per-call active/inactive/point_active tally.
	unsigned diagActiveLights = 0;
	unsigned diagInactiveLights = 0;
	unsigned diagPointActive = 0;

	for (uint32_t iLight = 0; iLight < numLights; iLight++)
	{
		if (num_lights == max_num_lights)
			break;

		// T1.15 [SPOT_DIAG v1] count active/inactive lights in the source list
		// before the active-filter culls them. Read `active` ONCE here; the
		// canonical filter below reads it again, but the field is plain DWORD
		// and not externally mutated between these two reads in this loop.
		if (listOfLights[iLight] != nullptr) {
			const DWORD t = listOfLights[iLight]->lightType;
			if (listOfLights[iLight]->active) {
				++diagActiveLights;
				if (t == TG_LIGHT_POINT) ++diagPointActive;
			} else {
				++diagInactiveLights;
			}
		}

		if ((listOfLights[iLight] != nullptr) && (listOfLights[iLight]->active))
		{

			const DWORD type = listOfLights[iLight]->lightType;

			glm::mat4 light2world;
			if (TG_LIGHT_AMBIENT != type)
				light2world = listOfLights[iLight]->lightToWorld;
			else
				light2world = glm::mat4(1.0f);

			// LIGHT-POS-COLUMN-1 — light world POSITION never reaches the GPU.
			// Stuff::AffineMatrix4D is 3 columns x 4 rows, indexed
			// entries[(column<<2)+row] (mclib/stuff/affinematrix.hpp), and
			// BuildTranslation writes (W_Axis, {X,Y,Z}_Axis) — i.e. the world
			// translation lives at entries[3], [7], [11]. BuildRotation only
			// touches rows 0..2 (mclib/stuff/affinematrix.cpp), so a
			// BuildTranslation-then-BuildRotation producer (bdactor.cpp /
			//, carnage.cpp, turret.cpp, weaponbolt.cpp)
			// really does leave the position in those three slots.
			// The memcpy below therefore lands the translation at GPU floats
			// [3]/[7]/[11] — which, read as a column-major GLSL mat4, is
			// m[0].w / m[1].w / m[2].w. But EVERY consumer decodes the light
			// origin as m[3].xyz == floats [12][13][14]:
			//     shaders/include/lighting.hglsl (POINT/SPOT/
			//                                     INFINITEWITHFALLOFF)
			//     shaders/vulkan/mech_tier1.vert
			//     shaders/lightgrid_build.comp
			// and the line below explicitly ZEROES exactly those three slots.
			// Net effect in the shipped build: every object-path POINT / SPOT /
			// INFINITEWITHFALLOFF light is evaluated as if it sat at the world
			// origin.
			// WHY THE FIX IS HERE AND NOT IN THE SHADERS: this is the single
			// chokepoint feeding all seven read sites across three shader
			// languages (GLSL, SPIR-V, compute), so one edit corrects them
			// together and needs no SPIR-V rebake. It also CANNOT double-correct:
			// the value currently uploaded is the constant (0,0,0), which carries
			// no information, so no downstream consumer can possibly be
			// compensating for it.
			// NOT TOUCHED: lightDir is gathered separately below via
			// GetLocalForwardInWorld and is already correct; the CPU mirror
			// (msl.cpp) sources POINT position from TG_Light::direction and
			// never reads this matrix. The rotation 3x3 left in [0..11] stays as
			// shipped — no consumer reads it.
			memcpy(lights->lightToWorld[num_lights], static_cast<const float*>(glm::value_ptr(light2world)), 12*sizeof(float));
			if (s_lightPosColumnFix)
			{
				lights->lightToWorld[num_lights][12] = lights->lightToWorld[num_lights][3];
				lights->lightToWorld[num_lights][13] = lights->lightToWorld[num_lights][7];
				lights->lightToWorld[num_lights][14] = lights->lightToWorld[num_lights][11];
			}
			else
			{
				lights->lightToWorld[num_lights][12] = lights->lightToWorld[num_lights][13] = lights->lightToWorld[num_lights][14] = 0.0f;
			}
			lights->lightToWorld[num_lights][15] = 1.0f;

			glm::vec3 uVec;
			uVec = mc2::math::localForwardInWorld(light2world);
			lights->lightDir[num_lights][0] = uVec.x;
			lights->lightDir[num_lights][1] = uVec.y;
			lights->lightDir[num_lights][2] = uVec.z;

			lights->lightDir[num_lights][3] = static_cast<float>(type);

			DWORD startLight = listOfLights[iLight]->GetaRGB();

			lights->lightColor[num_lights][0] = ((startLight >> 16) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][1] = ((startLight >> 8) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][2] = ((startLight) & 0x000000ff) / 255.0f;
			lights->lightColor[num_lights][3] = 1.0f;

			// Slice 2 (object-offload) — Stage 2.C: per-light falloff fields.
			// GLSL `GetFalloff` reads .x=closeDistance, .y=farDistance,
			// .z=oneOverDistance. Source on the CPU side is
			// TG_Light::{closeDistance,farDistance,oneOverDistance} at
			// mclib/tgl.h. AMBIENT lights don't use distance falloff
			// (the GLSL kernel hits the AMBIENT case before reading falloff),
			// but populate the fields anyway for cache uniformity.
			lights->lightFalloff[num_lights][0] = listOfLights[iLight]->closeDistance;
			lights->lightFalloff[num_lights][1] = listOfLights[iLight]->farDistance;
			lights->lightFalloff[num_lights][2] = listOfLights[iLight]->oneOverDistance;
			lights->lightFalloff[num_lights][3] = 0.0f;

			switch (type)
			{
			case TG_LIGHT_AMBIENT:
				break;
			case TG_LIGHT_INFINITE:
			case TG_LIGHT_INFINITEWITHFALLOFF:
				break;
			case TG_LIGHT_POINT:
				break;
			case TG_LIGHT_TERRAIN:
				break;
			case TG_LIGHT_SPOT:
				break;
			default:
				STOP(("Unknown light type id: %d", type));
			}

			if (doLightTrace) {
				mc2::fmt::print_err("[PARITY_DIAG v2] GatherLightsParameters iLight={:d} type={:d} aRGB=0x{:08X} dir=({:.4f},{:.4f},{:.4f}) color=({:.4f},{:.4f},{:.4f})\n", mc2::fmt::as_u32(num_lights), mc2::fmt::as_u32(static_cast<unsigned>(type)), mc2::fmt::as_u32(static_cast<unsigned>(listOfLights[iLight]->GetaRGB())), lights->lightDir[num_lights][0], lights->lightDir[num_lights][1], lights->lightDir[num_lights][2], lights->lightColor[num_lights][0], lights->lightColor[num_lights][1], lights->lightColor[num_lights][2]);
				std::fflush(stderr);
			}

			// MC2_LIGHT_ROW_CENSUS (default OFF) — LIGHT-POS-COMBAT-1.
			// Observation only; records what this row carried in and what it
			// uploaded out. See the header comment near s_lightRowCensus.
			if (s_lightRowCensus) {
				const unsigned ti = (static_cast<unsigned>(type)) & 7u;
				LightRowCensusRec& rec = s_lightRowCensusTab[ti];
				const float* row = lights->lightToWorld[num_lights];
				rec.seen++;
				rec.src[0] = row[3];  rec.src[1] = row[7];  rec.src[2] = row[11];
				rec.up[0]  = row[12]; rec.up[1]  = row[13]; rec.up[2]  = row[14];
				rec.falloff[0] = lights->lightFalloff[num_lights][0];
				rec.falloff[1] = lights->lightFalloff[num_lights][1];
				for (const auto srcElem : rec.src) {
					const float a = srcElem < 0.0f ? -srcElem : srcElem;
					rec.maxSrcAbs = std::max(rec.maxSrcAbs, a);
				}
				// One line the first time a POSITIONAL type ever appears — this
				// is the event the whole lane is waiting on.
				if (!rec.firstEmitted &&
				    (type == TG_LIGHT_INFINITEWITHFALLOFF ||
				     type == TG_LIGHT_POINT ||
				     type == TG_LIGHT_SPOT)) {
					rec.firstEmitted = true;
					lightRowCensusEmit("FIRST", ti);
				}
			}

			num_lights++;
		}
	}

	if (s_lightRowCensus) {
		// Periodic full dump so a long combat run leaves a running receipt
		// rather than a single sample. Period is in GatherLightsParameters
		// calls (many per frame), so this is a few lines per second at most.
		static const unsigned long kCensusPeriod = 5000;
		if ((++s_lightRowCensusCalls % kCensusPeriod) == 0) {
			for (unsigned t = 0; t < 8; ++t) {
				if (s_lightRowCensusTab[t].seen)
					lightRowCensusEmit("DUMP", t);
			}
		}
	}

	if (doLightTrace) {
		mc2::fmt::print_err("[PARITY_DIAG v2] GatherLightsParameters numLights={:d}\n", mc2::fmt::as_u32(num_lights));
		std::fflush(stderr);
		s_lightDumpDone = true;
	}

	// MC2_LIGHT_CLAMP_FIXTURE (default OFF) — deterministic synthetic point lights
	// injected into slots [num_lights..32) to PROVE slots >16 ever populate AND
	// render. NEVER writes past slot 31 (ABI cap 32). NEVER fires unless the env
	// gate is "1". Deterministic: fixed color/dir/falloff derived from slot index.
	if (s_lightClampFixture && num_lights < MAX_HW_LIGHTS_IN_WORLD) {
		const uint32_t injectStart = num_lights;
		// Fill to the cap; the loop bound (< MAX_HW_LIGHTS_IN_WORLD) guarantees the
		// last written slot is 31 — never an overrun of the 32-wide arrays.
		while (num_lights < MAX_HW_LIGHTS_IN_WORLD) {
			const uint32_t s = num_lights;

			// Identity lightToWorld (point light at origin-relative offset baked
			// into dir is unused; point lights use position via lightToWorld).
			for (int c = 0; c < 16; ++c)
				lights->lightToWorld[s][c] = (c % 5 == 0) ? 1.0f : 0.0f; // identity

			// Deterministic direction (unused for POINT but populate for uniformity).
			lights->lightDir[s][0] = 0.0f;
			lights->lightDir[s][1] = -1.0f;
			lights->lightDir[s][2] = 0.0f;
			lights->lightDir[s][3] = static_cast<float>(TG_LIGHT_POINT);

			// Deterministic bright color that cycles by slot so the extra lights
			// are visibly distinct when they contribute.
			lights->lightColor[s][0] = ((s & 1) ? 1.0f : 0.25f);
			lights->lightColor[s][1] = ((s & 2) ? 1.0f : 0.25f);
			lights->lightColor[s][2] = ((s & 4) ? 1.0f : 0.25f);
			lights->lightColor[s][3] = 1.0f;

			// Wide falloff so the synthetic lights actually reach the object.
			lights->lightFalloff[s][0] = 0.0f;       // closeDistance
			lights->lightFalloff[s][1] = 1000.0f;    // farDistance
			lights->lightFalloff[s][2] = 1.0f / 1000.0f; // oneOverDistance
			lights->lightFalloff[s][3] = 0.0f;

			++num_lights;
		}

		if (!s_lightClampFixtureYelled) {
			s_lightClampFixtureYelled = true;
			mc2::fmt::print_err("[LIGHT_CLAMP_FIXTURE] ON: injected {:d} synthetic point lights into object {:016X} slots {:d}..{:d} (ABI cap {:d}). This DELIBERATELY changes rendered lighting — gate MC2_LIGHT_CLAMP_FIXTURE.\n", mc2::fmt::as_u32(num_lights - injectStart), mc2::fmt::as_ptr(static_cast<void*>(lights)), mc2::fmt::as_u32(injectStart), mc2::fmt::as_u32(num_lights - 1), mc2::fmt::as_i32(MAX_HW_LIGHTS_IN_WORLD));
			std::fflush(stderr);
		}
	}

	lights->numLights_ = num_lights;

	// MC2_LIGHT_CLAMP_PROBE (default OFF) — pure observation, no behavior change.
	// Track per-object high-water populated light count; emit on each new high
	// plus a periodic summary so the per-mission peak is visible in logs.
	if (s_lightClampProbe) {
		++s_lightClampProbeCalls;
		if (num_lights > s_lightClampProbeHighWater) {
			s_lightClampProbeHighWater = num_lights;
			mc2::fmt::print_err("[LIGHT_CLAMP_PROBE] new high-water populated lights/object = {:d} (ABI cap {:d}, runtime clamp {:d}) after {:d} calls\n", mc2::fmt::as_u32(s_lightClampProbeHighWater), mc2::fmt::as_i32(MAX_HW_LIGHTS_IN_WORLD), mc2::fmt::as_u32(static_cast<unsigned>(max_num_lights)), mc2::fmt::as_u32(s_lightClampProbeCalls));
			std::fflush(stderr);
		}
		if ((s_lightClampProbeCalls % 2000) == 0) {
			mc2::fmt::print_err("[LIGHT_CLAMP_PROBE] summary: high-water={:d} over {:d} calls (exceeds_16={})\n", mc2::fmt::as_u32(s_lightClampProbeHighWater), mc2::fmt::as_u32(s_lightClampProbeCalls), mc2::fmt::safe((s_lightClampProbeHighWater > 16) ? "YES" : "no"));
			std::fflush(stderr);
		}
	}

	// T1.15 [SPOT_DIAG v1] pack-probe emit. First-call always-on; summary
	// every 600 calls when env=1.
	++s_spotDiagPackCalls;
	s_spotDiagPackActiveSum += diagActiveLights;
	s_spotDiagPackInactSum  += diagInactiveLights;
	s_spotDiagPackPointSum  += diagPointActive;
	if (!s_spotDiagPackFirstHit) {
		s_spotDiagPackFirstHit = true;
		MC2_DIAGFE(stderr,
			"[SPOT_DIAG v1] event=pack_first_shape shape=%p active_lights=%u "
			"inactive_lights=%u point_lights_active=%u\n",
			static_cast<void*>(lights), diagActiveLights, diagInactiveLights, diagPointActive);
		std::fflush(stderr);
	}
	if (s_spotDiagPackEnabled && (s_spotDiagPackCalls % 600) == 0) {
		double avgA = static_cast<double>(s_spotDiagPackActiveSum) / 600.0;
		double avgI = static_cast<double>(s_spotDiagPackInactSum)  / 600.0;
		double avgP = static_cast<double>(s_spotDiagPackPointSum)  / 600.0;
		mc2::fmt::print_err("[SPOT_DIAG v1] event=pack_summary calls={:d} avg_active_per_shape={:.3f} avg_inactive_per_shape={:.3f} avg_point_active_per_shape={:.3f}\n", mc2::fmt::as_u32(s_spotDiagPackCalls), avgA, avgI, avgP);
		std::fflush(stderr);
		s_spotDiagPackActiveSum = 0;
		s_spotDiagPackInactSum  = 0;
		s_spotDiagPackPointSum  = 0;
	}

}

// RENDERER-INPUT-READBACK-1: see the contract comment in mclib/tgl.h.
// Placed HERE, directly under GatherLightsParameters, because it must observe
// the same TG_Shape statics from the same translation unit and because any
// future edit to the gather must be seen by whoever maintains the probe.
// The save/restore is the load-bearing part. TG_Shape::s_listOfLights is live
// renderer state: whatever actor drew last owns it, and the next
// submitMultiShape()/GatherGpuObjectLightDataOnly() read will use it. A probe
// that installed a list and walked away would silently re-light the next
// object drawn -- an observation tool that changes the thing observed. Both
// statics go back byte-for-byte before this returns.
void ProbeGatherLightsForList(TG_LightPtr* lights, unsigned long nLights,
                              TG_HWLightsData* out)
{
	if (!out)
		return;
	*out = TG_HWLightsData();   // ctor zeroes arrays and sets numLights_ = 0

	// R2 item 1: the probe's list rides in its own context; TG_Shape's statics are never written (null list =
	// zero count, same normalisation as SetLightList).
	GatherLightsParameters(TG_LightContext::make(lights ? std::span<TG_LightPtr>(lights, nLights) : std::span<TG_LightPtr>()), out);
}
