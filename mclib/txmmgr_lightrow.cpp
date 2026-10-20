// MC_TextureManager: pure object light row, GatherLightsParameters and the light-clamp / light-row diagnostics.
// Declared in the class headers; shared names in txmmgr_internal.h.
#include "txmmgr_internal.h"
#include "mc2fmt.h"

// T1.15 [SPOT_DIAG v1] pack-probe state (GatherLightsParameters).
// First-shape always-on (one stderr line on the first call after process start).
// Per-summary every 600 GatherLightsParameters calls when env=1. `calls=N`
// because GatherLightsParameters runs once per submitMultiShape (many per frame).
static const bool s_spotDiagPackEnabled = mc2::gate::get<"MC2_SPOT_DIAG">();
static bool          s_spotDiagPackFirstHit  = false;
static unsigned long s_spotDiagPackCalls     = 0;
static unsigned long s_spotDiagPackActiveSum = 0;
static unsigned long s_spotDiagPackInactSum  = 0;
static unsigned long s_spotDiagPackPointSum  = 0;

// MC2_LIGHT_CLAMP_PROBE (default OFF) — pure observation. Tracks the per-object
// HIGH-WATER populated light count across all GatherLightsParameters calls and
// emits a summary line. Answers "do stock missions ever exceed 16 lights/object?"
// Does NOT change behavior when ON.
static const bool    s_lightClampProbe        = mc2::gate::get<"MC2_LIGHT_CLAMP_PROBE">();
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

// MC2_LIGHT_ROW_PURE (default OFF — see WHY OFF below) — LIGHT-ROW-PURE-1,
// ARCH-LIGHTING slice 1.
// Object light-ROW PRODUCTION is delegated to the one pure function
// object_light_row::BuildObjectLightRow (mclib/object_light_row.h): the row
// that ships is built from an EXPLICIT query (light list + declared per-class
// policy) instead of being a by-product of the mutate-gather-restore dance.
// =0 restores the legacy in-place packing loop below, byte-for-byte.
// WHY DEFAULT-OFF, and what would justify flipping it:
// tools/object_light_row_harness proves the ALGORITHM byte-identical — that a
// declared policy reproduces the mutation it replaces, across every class,
// light count 0..42, hole position and env combination. It CANNOT prove the
// engine-coupled half: flattenLight() below, which turns a TG_Light plus a
// Stuff::LinearMatrix4D into a LightInput, has no offline test because the
// harness deliberately links no engine. If that adapter mis-copies a matrix or
// a forward vector, the pure row diverges from the legacy one and default-ON
// would ship it unobserved. Game launches are blocked, so in-engine identity is
// UNPROVEN, and an unproven default is exactly the "documented default lie"
// class this repo keeps finding. Flip to default-ON when one run with
// MC2_LIGHT_ROW_PURE=1 MC2_LIGHT_ROW_PURE_VERIFY=1 reports
// checked>0 mismatched=0 across tier1 — that is the missing evidence, and
// nothing else is.
static const bool    s_lightRowPure           = mc2::gate::get<"MC2_LIGHT_ROW_PURE">();

// MC2_LIGHT_ROW_PURE_VERIFY (default OFF) — the in-engine half of the proof.
// Runs BOTH producers on the SAME frame and memcmps the 3600-byte rows: the
// legacy loop (which reads the globally MUTATED light list) against the pure
// function (which reads the same list plus the class's DECLARED policy). Equal
// rows mean the declaration matches what the class actually did to the world;
// a mismatch means a class's published policy has drifted from its mutation,
// which is exactly the divergence this slice exists to make impossible to ship
// silently. Observation only — never changes which row is used.
static const bool    s_lightRowPureVerify     = mc2::gate::get<"MC2_LIGHT_ROW_PURE_VERIFY">();
static unsigned long s_lightRowPureChecked    = 0;
static unsigned long s_lightRowPureMismatch   = 0;

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
static const bool    s_lightRowCensus = mc2::gate::get<"MC2_LIGHT_ROW_CENSUS">();
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
static void lightRowCensusEmit(const char* tag, unsigned t) {
    const LightRowCensusRec& r = s_lightRowCensusTab[t & 7];
    mc2::fmt::print_err("[LIGHT_ROW_CENSUS v1] {} type={:d}({}) seen={:d} src=({:.3f},{:.3f},{:.3f}) up=({:.3f},{:.3f},{:.3f}) maxSrcAbs={:.3f} falloff=({:.1f},{:.1f}) fix={:d}\n", mc2::fmt::safe(tag), mc2::fmt::as_u32(t), mc2::fmt::safe(lightRowCensusTypeName(t)), mc2::fmt::as_u32(r.seen), r.src[0], r.src[1], r.src[2], r.up[0], r.up[1], r.up[2], r.maxSrcAbs, r.falloff[0], r.falloff[1], mc2::fmt::as_i32(s_lightPosColumnFix ? 1 : 0));
    std::fflush(stderr);
}
}

// LIGHT-ROW-PURE-1 — ARCH-LIGHTING slice 1. See mclib/object_light_row.h for
// the design; this is the engine-coupled adapter and nothing else.
// LOCKSTEP: object_light_row::Row mirrors TG_HWLightsData field for field. If
// either side gains a member, these fire at compile time rather than shipping a
// row whose bytes the GPU reads differently than the harness compares.
namespace {

namespace olr = object_light_row;

static_assert(sizeof(olr::Row) == sizeof(TG_HWLightsData),
              "object_light_row::Row must match TG_HWLightsData size");
static_assert(offsetof(olr::Row, lightToWorld) == offsetof(TG_HWLightsData, lightToWorld),
              "lightToWorld offset drift");
static_assert(offsetof(olr::Row, lightDir)     == offsetof(TG_HWLightsData, lightDir),
              "lightDir offset drift");
static_assert(offsetof(olr::Row, lightColor)   == offsetof(TG_HWLightsData, lightColor),
              "lightColor offset drift");
static_assert(offsetof(olr::Row, lightFalloff) == offsetof(TG_HWLightsData, lightFalloff),
              "lightFalloff offset drift");
static_assert(offsetof(olr::Row, numLights_)   == offsetof(TG_HWLightsData, numLights_),
              "numLights_ offset drift");
static_assert(offsetof(olr::Row, pad)          == offsetof(TG_HWLightsData, pad),
              "pad offset drift");
static_assert(olr::kMaxLights == MAX_HW_LIGHTS_IN_WORLD, "ABI light-count drift");

// The mirrored type ids must equal the real enum, or the packed lightDir[3]
// discriminator the shaders switch on would silently mean something else.
static_assert(static_cast<uint32_t>(olr::kTypeNone)     == static_cast<uint32_t>(TG_LIGHT_NONE),     "type drift");
static_assert(static_cast<uint32_t>(olr::kTypeAmbient)  == static_cast<uint32_t>(TG_LIGHT_AMBIENT),  "type drift");
static_assert(static_cast<uint32_t>(olr::kTypeInfinite) == static_cast<uint32_t>(TG_LIGHT_INFINITE), "type drift");
static_assert(static_cast<uint32_t>(olr::kTypeInfiniteWithFalloff) ==
              static_cast<uint32_t>(TG_LIGHT_INFINITEWITHFALLOFF), "type drift");
static_assert(static_cast<uint32_t>(olr::kTypePoint)    == static_cast<uint32_t>(TG_LIGHT_POINT),    "type drift");
static_assert(static_cast<uint32_t>(olr::kTypeSpot)     == static_cast<uint32_t>(TG_LIGHT_SPOT),     "type drift");
static_assert(static_cast<uint32_t>(olr::kTypeTerrain)  == static_cast<uint32_t>(TG_LIGHT_TERRAIN),  "type drift");

// The policy the appearance class currently being updated has DECLARED. It is
// published immediately alongside the legacy mutation and cleared after, so it
// describes exactly one class's row.
// WHY THE MUTATIONS ARE STILL THERE (do not "finish the job" without redoing
// the census): a reader census over this tree found the shared-list mutations
// are load-bearing for a consumer the row diff CANNOT see —
// TG_Shape::MultiTransformShape, the CPU per-vertex lighting kernel
// (mclib/tgl.cpp), reads the same TG_Light objects' active/GetaRGB
// and runs INSIDE the tree's sun-off window (mclib/bdactor.cpp, and
// under dual-emit). It is live in this build: useVertexLighting is true
// (mclib/terrain.cpp) and Environment.Renderer is 0, not 3
// (code/mechcmd2.cpp). Deleting the mutations would therefore change baked
// vertex colours — listOfColors / listOfTriangles[].aRGBLight — while every
// gathered row stayed byte-identical. That is the one failure mode a row-memcmp
// is structurally blind to, so the mutations stay and the policy is published
// as a DECLARATION of them. Because a recolour publishes the same aRGB the
// mutation wrote and a disable publishes the same active=false, applying the
// policy on top of the already-mutated list is idempotent — which is what makes
// MC2_LIGHT_ROW_PURE_VERIFY a continuous check that the declaration still
// matches reality.
struct PublishedPolicy {
	bool                 valid = false;
	olr::ClassPolicy     cls   = olr::kClassNone;
	bool                 sunRecolour = false;
	uint32_t             sunARGB     = 0;
	bool                 sunDisabled = false;
	bool                 ambRecolour = false;
	uint32_t             ambARGB     = 0;
	float                worldPos[3] = {0.0f, 0.0f, 0.0f};
	float                terrainSample = 1.0f;
};
// thread_local for the same FRAME-JOBS-2 reason as the scratch buffer below,
// and because it fails in the safe direction: if a future worker thread runs
// the gather without having published, it simply sees no policy and does a raw
// gather of the (still-mutated) list -- which is byte-identical to the legacy
// row. A shared static could instead be read half-written and produce a wrong
// one.
static thread_local PublishedPolicy s_publishedPolicy;

// Flatten one TG_Light exactly as the legacy gather reads it.
inline void flattenLight(const TG_LightPtr src, olr::LightInput& dst)
{
	dst.type   = static_cast<uint32_t>(src->lightType);
	dst.active = (src->active != 0);
	dst.aRGB   = static_cast<uint32_t>(src->GetaRGB());

	// Same AMBIENT->Identity substitution as the legacy path.
	glm::mat4 light2world;
	if (TG_LIGHT_AMBIENT != src->lightType)
		light2world = src->lightToWorld;
	else
		light2world = glm::mat4(1.0f);

	memcpy(dst.l2w, static_cast<const float*>(glm::value_ptr(light2world)), 12 * sizeof(float));

	glm::vec3 uVec;
	uVec = mc2::math::localForwardInWorld(light2world);
	dst.fwd[0] = uVec.x;
	dst.fwd[1] = uVec.y;
	dst.fwd[2] = uVec.z;

	dst.falloff[0] = src->closeDistance;
	dst.falloff[1] = src->farDistance;
	dst.falloff[2] = src->oneOverDistance;
}

// Resolve the declared policy's target lights BY ROLE rather than by array
// index. The classes mutate worldLights[0] (the sun) and worldLights[1] (the
// ambient) — allocated as TG_LIGHT_INFINITE and TG_LIGHT_AMBIENT at
// mclib/camera.cpp — but they GATHER against activeLights[], a
// separately compacted view (mclib/camera.cpp). Role resolution is
// correct under any compaction order; an index would silently target the wrong
// light the day a TG_LIGHT_TERRAIN sorts ahead of the sun.
inline int32_t findFirstOfType(const olr::LightInput* lights, uint32_t n, uint32_t type)
{
	for (uint32_t i = 0; i < n; ++i)
		if (lights[i].type == type)
			return static_cast<int32_t>(i);
	return -1;
}

// Build the explicit query from the current global light list plus whatever
// policy the class published. This is the ONE place the globals are read.
inline bool buildQueryFromGlobals(std::vector<olr::LightInput>& scratch, olr::Query& q)
{
	const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
	const DWORD        numLights    = TG_Shape::s_numLights;

	scratch.clear();
	if (listOfLights) {
		scratch.reserve(numLights);
		for (DWORD i = 0; i < numLights; ++i) {
			olr::LightInput li{};
			if (listOfLights[i] != nullptr) {
				flattenLight(listOfLights[i], li);
			} else {
				li.active = false;   // NULL entries are skipped by both packers
				li.type   = static_cast<uint32_t>(TG_LIGHT_NONE);
			}
			scratch.push_back(li);
		}
	}

	q.lights       = scratch.empty() ? nullptr : scratch.data();
	q.numLights    = static_cast<uint32_t>(scratch.size());
	q.posColumnFix = s_lightPosColumnFix;
	q.clampFixture = s_lightClampFixture;

	const PublishedPolicy& p = s_publishedPolicy;
	if (!p.valid)
		return false;

	q.policy             = p.cls;
	q.terrainLightSample = p.terrainSample;
	q.worldPos[0] = p.worldPos[0];
	q.worldPos[1] = p.worldPos[1];
	q.worldPos[2] = p.worldPos[2];

	if (p.sunRecolour || p.sunDisabled) {
		q.sunSlot.index         = findFirstOfType(q.lights, q.numLights, TG_LIGHT_INFINITE);
		q.sunSlot.recolour      = p.sunRecolour;
		q.sunSlot.aRGB          = p.sunARGB;
		q.sunSlot.forceInactive = p.sunDisabled;
	}
	if (p.ambRecolour) {
		q.ambientSlot.index    = findFirstOfType(q.lights, q.numLights, TG_LIGHT_AMBIENT);
		q.ambientSlot.recolour = p.ambRecolour;
		q.ambientSlot.aRGB     = p.ambARGB;
	}
	return true;
}

// Substitute the pure row for the legacy one (gate ON), and/or compare them
// (verify ON). Called once at the tail of GatherLightsParameters, after the
// legacy loop and every in-loop diagnostic has run untouched.
void applyPureObjectLightRow(TG_HWLightsData* lights)
{
	// A published policy is consumed by exactly ONE gather and then dropped.
	// This is deliberately the safe failure direction: a class whose policy is
	// missed falls back to a raw gather of the (still-mutated) list, which is
	// byte-identical to the legacy row; whereas a policy left standing could be
	// applied to some unrelated object's gather and produce a wrong row. Single
	// -shot consumption makes the second case unreachable — and it is the same
	// stale-global hazard this slice exists to remove, so it must not be
	// reintroduced by the mechanism that removes it.
	struct ConsumePolicy {
		~ConsumePolicy() { s_publishedPolicy.valid = false; }
	} _consume;

	if (!s_lightRowPure && !s_lightRowPureVerify)
		return;                     // NEGATIVE CONTROL: not one row byte changed

	// thread_local, not a plain static: this file already carries FRAME-JOBS-2
	// preconditions (s_lightDataMapMu, atomic shadow counters) because worker
	// threads are expected to reach this code. The surrounding per-call
	// diagnostic counters are plain statics and race benignly on an int; a
	// shared std::vector would not -- concurrent clear()/push_back() is heap
	// corruption, not a skewed tally. Per-thread scratch costs one buffer.
	static thread_local std::vector<olr::LightInput> scratch;
	olr::Query q;
	buildQueryFromGlobals(scratch, q);

	// Seed from the legacy row so the deliberately-untouched tail slots and
	// pad[] compare equal — the packer writes only [0, numLights).
	olr::Row pure;
	memcpy(&pure, lights, sizeof(pure));
	olr::BuildObjectLightRow(q, &pure);

	if (s_lightRowPureVerify) {
		++s_lightRowPureChecked;
		if (memcmp(&pure, lights, sizeof(pure)) != 0) {
			++s_lightRowPureMismatch;
			// SAMPLE-CAP 8: only the first 8 mismatches print in full. The
			// running totals in the periodic summary below are the census; these
			// lines are a sample and say so, so a truncated stream can never be
			// mistaken for "only 8 rows ever diverged".
			if (s_lightRowPureMismatch <= 8) {
				mc2::fmt::print_err("[LIGHT_ROW_PURE] MISMATCH #{:d} (sample=first-8 of an uncapped count; see the periodic summary for the total) class={:d} legacyLights={:d} pureLights={:d} (declared policy does not reproduce the mutation)\n", mc2::fmt::as_u32(s_lightRowPureMismatch), mc2::fmt::as_u32(static_cast<unsigned>(q.policy)), mc2::fmt::as_i32(lights->numLights_), mc2::fmt::as_i32(pure.numLights_));
				std::fflush(stderr);
			}
		}
		if ((s_lightRowPureChecked % 5000) == 0) {
			mc2::fmt::print_err("[LIGHT_ROW_PURE] verify: checked={:d} mismatched={:d}\n", mc2::fmt::as_u32(s_lightRowPureChecked), mc2::fmt::as_u32(s_lightRowPureMismatch));
			std::fflush(stderr);
		}
	}

	if (s_lightRowPure)
		memcpy(lights, &pure, sizeof(pure));
}

}

// LIGHT-ROW-PURE-1 public policy declaration API (declared in render_submissions.h).
// An appearance class publishes the policy it is about to apply, immediately
// beside the legacy mutation, and clears it when its gather is done. Publishing
// is inert by itself: it changes no light, no row and no pixel unless
// MC2_LIGHT_ROW_PURE is on, and even then it reproduces the mutation exactly.
void mc2PublishObjectLightPolicy(uint32_t classPolicy,
                                 bool sunRecolour, uint32_t sunARGB,
                                 bool sunDisabled,
                                 bool ambientRecolour, uint32_t ambientARGB,
                                 float posX, float posY, float posZ,
                                 float terrainSample)
{
	s_publishedPolicy.valid         = true;
	s_publishedPolicy.cls           = (olr::ClassPolicy)classPolicy;
	s_publishedPolicy.sunRecolour   = sunRecolour;
	s_publishedPolicy.sunARGB       = sunARGB;
	s_publishedPolicy.sunDisabled   = sunDisabled;
	s_publishedPolicy.ambRecolour   = ambientRecolour;
	s_publishedPolicy.ambARGB       = ambientARGB;
	s_publishedPolicy.worldPos[0]   = posX;
	s_publishedPolicy.worldPos[1]   = posY;
	s_publishedPolicy.worldPos[2]   = posZ;
	s_publishedPolicy.terrainSample = terrainSample;
}

void mc2ClearObjectLightPolicy(void)
{
	s_publishedPolicy = PublishedPolicy();
}

void GatherLightsParameters(TG_HWLightsData* lights)
{
	gosASSERT(lights);

	// Stage 2.D.2 diagnostic: dump gathered lights when MC2_OBJECT_PARITY_TRACE=1.
	// Fired once per session to avoid per-frame spam. Shows what GPU UBO gets.
	static bool s_lightDumpDone = false;
	const bool doLightTrace = (!s_lightDumpDone && mc2::gate::get<"MC2_OBJECT_PARITY_TRACE">());

	uint32_t num_lights = 0;
	// LIGHT-CLAMP-RAISE-STAGE1-1: runtime population now reaches the full GPU ABI
	// cap (MAX_HW_LIGHTS_IN_WORLD = 32). Stage 0 widened the struct/stride to 32;
	// this slice "allows putting 32 in the box". Safe because every shader per-light
	// loop is min(numLights.x, MAX_LIGHTS_IN_WORLD)-bound (cost/reads follow the
	// populated count), and the break below (last write = slot 31) is the overrun
	// guard. The break loop is cap-agnostic — it just trips later now.
	static constexpr uint32_t kRuntimeLightClamp = MAX_HW_LIGHTS_IN_WORLD; // 32
	const uint32_t max_num_lights = kRuntimeLightClamp;

	const TG_LightPtr* listOfLights = TG_Shape::s_listOfLights;
	const DWORD numLights = TG_Shape::s_numLights;

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
				for (int c = 0; c < 3; ++c) {
					const float a = rec.src[c] < 0.0f ? -rec.src[c] : rec.src[c];
					if (a > rec.maxSrcAbs) rec.maxSrcAbs = a;
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

	// LIGHT-ROW-PURE-1 — the row that actually ships. Placed at the TAIL, after
	// the legacy loop and every in-loop diagnostic above has run untouched, so
	// this slice cannot regress instrumentation (debug_instrumentation_rule:
	// demote-not-delete) and so MC2_LIGHT_ROW_PURE=0 is a byte-exact revert to
	// the loop above. With both gates off this is a single predictable branch.
	applyPureObjectLightRow(lights);
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

	TG_LightPtr* const savedList  = TG_Shape::s_listOfLights;
	const DWORD        savedCount = TG_Shape::s_numLights;

	// Mirror SetLightList's own null-normalisation (mclib/tgl.cpp):
	// a NULL list forces the count to 0 there, and the gather's
	// `listOfLights[iLight]` deref would otherwise run against a null base.
	if (lights) {
		TG_Shape::s_listOfLights = lights;
		TG_Shape::s_numLights    = static_cast<DWORD>(nLights);
	} else {
		TG_Shape::s_listOfLights = nullptr;
		TG_Shape::s_numLights    = 0;
	}

	GatherLightsParameters(out);

	TG_Shape::s_listOfLights = savedList;
	TG_Shape::s_numLights    = savedCount;
}
