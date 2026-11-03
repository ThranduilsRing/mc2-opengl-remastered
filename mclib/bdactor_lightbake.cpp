// BldgAppearance / TreeAppearance: static-actor light bake gate (mc2CacheOrBakeStaticGpuLight) and the cone ground-hit march.
// Declared in the class headers; shared names in bdactor_internal.h.
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include "bdactor_internal.h"
#include "diagnostic_trace.h"

// [BAKEORD v1] PROP-BAKE-ORDER-VS-TIME-1 -----------------------------------
// PROP-RECIPE-BAKE-1 measured that among SUNLIT props the orientation-free
// ambient term splits per INSTANCE, not per type (Mesh01 28 bright / 132 dim,
// Barrels 2/8), bright = 0.7529 and dim = 0.2471 = 0x3F -- a 3x difference in
// the exact term that lights these props. It proposed a bake-ORDER cause (a
// prop baking inside TreeAppearance's global sun-off window inherits
// sun-inactive, so decomposeFirstActiveLightColor overwrites the AMBIENT slot
// instead of the sun slot, collapsing it, frozen by the per-recipe bake).
// It explicitly did NOT prove that, and named a competing cause the data
// cannot exclude: the split may be by bake TIME -- before vs after the
// LIGHTBAKE-REBAKE-ON-READY-1 soft invalidation -- with no tree involvement at
// all. Both hypotheses predict per-instance within-type variation, so the
// outcome table alone cannot separate them.
// ★ PROP-BAKE-SUNWINDOW-1 (2026-07-30) -- THE ORDER HYPOTHESIS IS REFUTED AT
// SOURCE. Do not build a fix on it; three files had copied it forward as fact.
// Two independent links fail:
//   (1) NOTHING ELSE CAN BAKE INSIDE THE WINDOW. `getWorldLight(0)->active =
//       false` occurs at exactly ONE site repo-wide (mech3d.cpp is a
//       per-actor pointLight, not the sun) and is restored in the same
//       function. Between them there is no early return (the only `return` in
//       TreeAppearance::update inside it is the tail `return
//       TRUE`) and no call that touches another actor -- only this tree's own
//       SetLightList / TransformMultiShape* / mc2CacheOrBakeStaticGpuLight. And
//       appearance updates are serial on a default launch (MC2_FRAME_JOBS is
//       default-OFF), so no worker thread can bake against the mutated global
//       either. The window is global in TYPE, confined in EXTENT.
//   (2) THE "COLLAPSE" STEP IS A NO-OP. GatherLightsParameters COMPACTS --
//       num_lights advances only inside the `active` branch (txmmgr.cpp,
//       ++) -- so with the sun filtered out row slot 0 already holds
//       the AMBIENT, and decomposeFirstActiveLightColor (txmmgr.cpp) picks
//       the first ACTIVE light off the SAME list and writes those same bytes
//       back. Proven by execution, not prose:
//       tools/object_light_row_harness ::decompose_slot0_is_a_noop_after_gather
//       (every class policy x light count x inactive-light hole), which FAILS
//       when the compaction is removed from the model.
// A tree bake therefore yields nL=1 (ambient only, no INFINITE) -- the intended
// foliage look -- and that is ALL the window does. It does not dim any
// neighbouring prop, so the BRIGHT/DIM split above is NOT explained by it, and
// the competing bake-TIME hypothesis is the one left standing.
// WHERE THE NEXT LANE SHOULD LOOK (leads, verified as SOURCE FACTS; the
// mechanism itself is NOT proven -- do not repeat this file's mistake of
// promoting a plausible chain to fact):
//   * The AMBIENT the DIM term reports has exactly ONE per-frame producer:
//     `eye->setLightColor(1, (ambientRed<<16)|(ambientGreen<<8)|ambientBlue)`,
//     gamecam.cpp, inside GameCamera::render(), re-evaluated every
//     frame from Camera::ambientRed/Green/Blue.
//   * Camera::ambientRed IS mutated during a mission: weather.cpp divides
//     it by rainLightLevel while raining and restore it; and
//     Camera::updateDaylight writes it at camera.cpp from the sun pitch --
//     though that body is entirely inside `if (day2NightTransitionTime > 0)`
//     (camera.cpp) and missions with no DayToNightTime park that at -1, so
//     it does not run on every mission. CHECK PER MISSION before assuming it.
//   * A per-recipe bake freezes whatever value was live, permanently
//     (mc2StaticLightEnvironmentGeneration is a constant 0), and the only
//     invalidation is the single LIGHTBAKE-REBAKE-ON-READY-1 pass above. So any
//     recipe first rendered while the ambient differed keeps that ambient for
//     the mission -- which is exactly a per-instance, within-type, frozen,
//     zoom-invariant split.
//   * mission.cpp also swings the ambient to 0xFFFFFF, but only under
//     `KillAmbientLight`, which is false unless the `-killambient` command-line
//     switch is passed (mechcmd2.cpp) -- NOT a default-launch factor.
// The `sunActive=` field below therefore no longer discriminates anything; it is
// kept because a NON-tree bake reading sunActive=0 would falsify (1) and that is
// worth continuing to watch for.
// This probe therefore logs BOTH discriminators at every bake, or it would
// confirm whichever hypothesis it was pointed at:
//   ord=            monotonic bake ordinal      -> ORDER
//   frame=          bake frame                  -> TIME
//   postInvalidate= bake is after the rebake    -> TIME, relative to the
//                                                  invalidation specifically
//   sunActive=      getWorldLight(0)->active    -> ORDER, the tree-window state
// plus the resulting amb/sun so each bake carries its own BRIGHT/DIM outcome.
// Diagnostic only, read-only, default OFF. Reuses the
// MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE gate (no new env var). That gate was
// described here as "already-registered" but was in fact registered NOWHERE
// until GATE-HYGIENE-CLOSE-1 added it to kAuxEnvVars, docs/tier1_env_vars.md
// and run_smoke.py's passthrough allowlist. NOTE: MC2_DIAGF
// is additionally gated on MC2_DIAG_VERBOSE=1 -- without it this emits nothing.
// ★★ PROP-BAKE-TIME-1 (2026-07-30) -- THE TIME HYPOTHESIS IS CONFIRMED, AND THE
// MUTATOR IS WEATHER. Both field constants are now derived, to the bit, from
// mc2_10's own authored bytes. This closes PROP-BAKE-ORDER-VS-TIME-1: ORDER was
// refuted above, TIME is confirmed here, and no third hypothesis is needed.
//   * mc2_10.fit [Cameras] `uc AmbientRed/Green/Blue = 192` and [Weather]
//     `f StartingRainLevel = 3.0`, `l ChanceOfRain = 100`, `ul MaxRainDrops = 500`
//     (verified in mc2srcdata-fresh/missions/mc2_10.fit, which md5-matches
//     tools/mc2data/stock_missions.md5).
//   * BRIGHT 0.7529 == 192/255 EXACTLY == the authored ambient with NO weather
//     division. Camera::update() publishes setLightColor(1, ambient) at
//     camera.cpp and mission.cpp orders eye->update() BEFORE
//     weather->update() BEFORE ObjectManager->update() (every
//     appearance bake). So the frame-1 bake reads an ambient published one stage
//     ahead of the weather that modifies it -- a value the mission never renders
//     a frame with.
//   * DIM 0.2471 == 63/255 EXACTLY == trunc(192 / rainLightLevel) for
//     rainLightLevel in (3.0, 3.0476] -- i.e. weather.cpp one tick past
//     mc2_10's authored StartingRainLevel. Camera::ambientRed is `unsigned char`,
//     so the quotient truncates.
//   * ChanceOfRain 100 makes invRainChance 0 at weather.cpp, so the `!= 0.0f`
//     guard never fires and rainTrend never flips: mc2_10 is a permanent
//     storm that ramps rainLevel to the 4.0 cap and stays there. The
//     LIVE ambient therefore SETTLES at 192/4 = 48/255,
//     while the frozen rows keep 63/255 (post-rebake) or 192/255 (frame 1).
// DIRECTION, and why this is NOT the standing "props render too dark" report.
// Scoped precisely to the shader's orientation-free ambientTerm
// (static_prop.vert sums it, adds it at coefficient 1.0): the
// freeze leaves that term ABOVE the live scene's, never below -- +15/255
// post-rebake, +144/255 for a frame-1 recipe. Fixing it DARKENS props on
// mc2_10. This says nothing about the sun slot or the final pixel, only about
// the one term this lane measured.
// SCOPE: one mission. Enumerated at source, the writers of Camera::ambientRed are
// camera.cpp (init, once), camera.cpp (updateDaylight),
// weather.cpp, and mission.cpp (-killambient only). Of those:
//   - camera.cpp is UNREACHABLE in a mission. Correcting the bullet above:
//     the gate is not "missions with no DayToNightTime" -- all 46 stock missions
//     that have a [Cameras] block AUTHOR the key (43 with a value <= 0), so the
//     readIdFloat fallback is dead data. The real reason is dispatch:
//     updateDaylight's per-frame call site is commented out (camera.cpp) and
//     its only other caller is Camera::activate() (camera.cpp) -- which
//     `eye` never reaches, because activate() is virtual (camera.h),
//     `eye = new GameCamera` (mission.cpp), and GameCamera::activate
//     (gamecam.cpp) fully overrides it without chaining, with its own
//     updateDaylight(true) commented out at gamecam.cpp. All three game-side
//     callers (mechcmd2.cpp, logistics.cpp, saveload.cpp) dispatch
//     there. Only Camera::setDaylightFraction (the MC2_UI_COMMAND_FILE driver)
//     can still reach it.
//   - weather's write is an IDENTITY assignment when rainLevel == 0.
//   Leaving WEATHER as the only default-launch mutator, and mc2_10 as the only
//   stock mission that can ever rain. Scanned across the whole stock corpus,
//   mc2_10 is the ONLY .fit with MaxRainDrops > 0 and a non-zero
//   StartingRainLevel/ChanceOfRain (mc2_19 authors StartingRainLevel 3.0 but
//   MaxRainDrops 0, and weather.cpp clears weatherActive when maxDrops == 0).
//   And rain cannot start spontaneously either: Weather's ctor seeds
//   rainTrend = +1, rainFactor = 0 (weather.h), so with rainLevel 0 the
//   only branch that can arm rain -- `(rainLevel <= 1.5f) && (rainTrend < 0.0f)`
//   at weather.cpp -- is unreachable and rainLevel stays pinned at 0.
//   On the other 45 missions Camera::ambient* is constant from Camera::init to
//   mission end, so NO bake moment can produce a divergent row there.
// AND THE INVALIDATION CANNOT REACH MOST PROPS ANYWAY -- this is independent of
// weather and is the harder half. Every live caller of
// mc2CacheOrBakeStaticGpuLight() sits inside an update(): BldgAppearance::update
// and TreeAppearance::update (the gpuEligible and
// full-bake arms of each). The only other callers are the default-OFF
// prewarmStaticLightBake pair ( bldg, tree). The touch() family never
// calls it: BldgAppearance touch/touchSerialCommit reach only
// ResubmitCachedGpuLightData() and TreeAppearance only (
//) -- and that function (mclib/msl.cpp) re-submits the leaf's
// ALREADY-CACHED lightData_. It never re-gathers from eye->getWorldLights() and
// never consults s_bakedStaticLight. MC2_STATIC_UPDATE_SKIP defaults TRUE
// (terrobj.cpp), so once a prop's IsStaticNow() holds, terrobj.cpp routes it
// to touch() for the rest of the mission. A prop that went static before
// mission_ready+8 therefore keeps its FIRST-EVER row forever: clearing the cache
// map is a no-op for it, because nothing will ever ask the map again.
// So the honest answer to "is the bake ever redone?" is: only for props still on
// the update() path at the instant of the single invalidation.
// WHAT IS STILL UNKNOWN: which props end up in which cohort. The 28/132 and 2/8
// partitions cannot be derived offline -- they depend on per-class routing
// (buildings vs terrain objects) and on when each IsStaticNow() first holds.
// The [BAKEORD v1] probe below already logs `frame=` and `postInvalidate=` per
// bake, which is exactly the discriminator; it needs one gated run, not more
// reading. Do not promote a guess about the partition to fact here.
// EXECUTABLE PROOF (no launch, no GL, no engine link):
//   tools/object_light_row_harness ::ambient_state_changes_the_baked_row
//                                  ::mc2_10_storm_ambient_is_not_mission_constant
//                                  ::non_rainy_missions_are_immune_to_bake_time
// The third is the scope bound AND the blindness guard: it holds the same three
// bake moments against six real authored ambients at rainLevel 0 and requires
// byte-identical rows, then injects the rain those missions do not author and
// requires the rows to stop agreeing. Both regressions were injected into this
// tree and observed RED before the tests were kept.
// THE FIX, DESIGNED BUT DELIBERATELY NOT SHIPPED. The seam already exists:
// mc2StaticLightEnvironmentGeneration (bdactor.cpp) returns a constant 0
// and is already consulted by the STABLE_LIGHT_SKIP eligibility test. Deriving
// it from the published scene lighting (Camera::ambient{Red,Green,Blue} +
// light{Red,Green,Blue}) and invalidating the bake cache on change repairs the
// invariant txmmgr.cpp asserts, and self-quiesces: those are 8-bit values,
// so a full storm ramp costs ~16 re-bakes and a non-rainy mission costs zero.
// It is NOT flipped here because its only visible effect on stock data is to
// darken props on mc2_10, which is the opposite direction to the user's standing
// report. That is a user call, not an agent call.
static bool s_lightBakeRebakeDone = false;
static uint32_t s_lightBakeReadyFrame = 0;

static thread_local bool s_lastBakeFresh = false;  // [LIGHT_CONTRACT v1] set by the bake gate: true = real gather, false = shared HIT row

void mc2CacheOrBakeStaticGpuLight(TG_MultiShape* shape,
                                         bool registered, int32_t recipeIndex)
{
	s_lastBakeFresh = true;
	struct SlotIRec { TG_MultiShape* s; ~SlotIRec() { extern void mc2RecordSlotIFwd(TG_MultiShape*); mc2RecordSlotIFwd(s); } } slotIRec{shape};
	extern bool mc2LightBakeEnabled();
	extern bool mc2GetBakedStaticLight(int32_t, TG_HWLightsData&);
	extern void mc2SetBakedStaticLight(int32_t, const TG_HWLightsData&);
	extern void mc2WriteStaticLightSlot(int32_t, const TG_HWLightsData&);  // [LIGHTBAKE v2]
	extern bool mc2IsBakedStaticLightPresent(int32_t);
	if (!shape) return;
	if (!mc2LightBakeEnabled() || !registered || recipeIndex < 0) {
		shape->CacheGpuLightData();                  // unchanged D2/legacy path
		return;
	}
	// LIGHTBAKE-REBAKE-ON-READY-1 (batch3 item D): per-recipe bakes are frozen
	// at FIRST-render time and never invalidated (mc2StaticLightEnvironmentGeneration
	// is a constant 0). Buildings whose recipe baked during boot/load -- before
	// the mission's night lighting fully applied -- keep a too-bright day-ish
	// row forever: stable per-building, zoom-invariant (user report, mc2_10
	// night; proven by MC2_LIGHTBAKE=0 vs default pixel diff at saved_212019).
	// Fix: ONE soft invalidation of the bake CACHE (recipe slots + high-water
	// preserved -- entries lazily re-gather into the same permanent slots) a
	// few frames after mission-ready, when mission lighting is final.
	// Killswitch: MC2_LIGHTBAKE_REBAKE_ON_READY=0.
	{
		static const bool s_rebakeOnReady = mc2::gate::get<"MC2_LIGHTBAKE_REBAKE_ON_READY">();
		bool&            s_rebakeDone = s_lightBakeRebakeDone;  // [BAKEORD v1] file-scope
		uint32_t&        s_readyFrame = s_lightBakeReadyFrame;  // file-scope: re-armed per mission
		if (s_rebakeOnReady && !s_rebakeDone && SmokeMode::missionHasStarted()) {
			if (s_readyFrame == 0) {
				s_readyFrame = g_mc2FrameCounter.load(std::memory_order_relaxed) ? g_mc2FrameCounter.load(std::memory_order_relaxed) : 1;
			} else if (g_mc2FrameCounter.load(std::memory_order_relaxed) >= s_readyFrame + 8) {
				extern void mc2InvalidateBakedStaticLightCache();
				mc2InvalidateBakedStaticLightCache();
				s_rebakeDone = true;
				if (mc2_diag::tagRequested("LIGHTBAKE"))
					std::fprintf(stderr, "[LIGHTBAKE v1] event=rebake_on_ready frame=%u\n", static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed)));
				MC2_DIAGF("[LIGHTBAKE v1] event=rebake_on_ready frame=%u\n",
				       static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed)));
				fflush(stdout);
			}
		}
	}
	TG_HWLightsData baked;
	if (mc2GetBakedStaticLight(recipeIndex, baked)) {
		s_lastBakeFresh = false;
		shape->EmitBakedGpuLightData(recipeIndex, baked);    // HIT: recompute retired
	} else {
		shape->CacheGpuLightData();                          // MISS: real gather (frame 1 / post-invalidate)
		// C1 (adversarial review): CacheGpuLightData leaves
		// cachedGpuLightIndex_ at the 0xFFFFFFFF sentinel when the multi has
		// no SHAPE_NODE leaf (leaf->lightData_ stale). Only persist
		// the bake if the gather actually ran (valid index) -- else leave
		// uncached so it retries next frame; never persist a no-op
		// snapshot (would poison s_bakedStaticLight until invalidate).
		const TG_HWLightsData* leaf = shape->peekCachedLeafLightData();
		if (leaf && shape->getCachedGpuLightIndex() != 0xFFFFFFFFu) {
			mc2SetBakedStaticLight(recipeIndex, *leaf);      // mission source-of-truth (re-bake/invalidate)
			// [LIGHTBAKE v2] write the PERMANENT static slot once
			// (lightData_[recipeIndex] CPU mirror + advance S), then
			// point this multi at it -- identical end-state to the HIT
			// path, so from this frame on there is NO per-frame
			// addLightDataStructure for this recipe.
			mc2WriteStaticLightSlot(recipeIndex, *leaf);
			// [BAKEORD v1] order-vs-time discriminator. See the block comment
			// above this function. One line per real bake (MISS path only --
			// a HIT recomputes nothing, so it has no bake state to attribute).
			{
				static const bool s_bakeOrdProbe = mc2::gate::get<"MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE">();
				if (s_bakeOrdProbe) {
					static unsigned long long s_bakeOrd = 0;
					const unsigned long long ord = s_bakeOrd++;
					// Sun-enable state AT BAKE TIME. TreeAppearance::update
					// clears getWorldLight(0)->active across its own bake and
					// restores it; a non-tree prop baking inside that window is
					// the ORDER hypothesis' mechanism.
					int sunActive = -1;
					if (eye) {
						TG_LightPtr l0 = eye->getWorldLight(0);
						if (l0) sunActive = l0->active ? 1 : 0;
					}
					// Outcome, computed off the SAME leaf that is being frozen:
					// ambient sum vs first infinite (sun) colour, exactly the way
					// static_prop.vert consumes the row.
					float amb[3] = {0.0f, 0.0f, 0.0f};
					float sun[3] = {0.0f, 0.0f, 0.0f};
					bool sunFound = false;
					const int n = (leaf->numLights_ < MAX_HW_LIGHTS_IN_WORLD)
					            ? leaf->numLights_ : MAX_HW_LIGHTS_IN_WORLD;
					for (int i = 0; i < n; ++i) {
						const int lt = static_cast<int>(leaf->lightDir[i][3]);
						if (lt == TG_LIGHT_AMBIENT) {
							amb[0] += leaf->lightColor[i][0];
							amb[1] += leaf->lightColor[i][1];
							amb[2] += leaf->lightColor[i][2];
						} else if (!sunFound &&
						           (lt == TG_LIGHT_INFINITE ||
						            lt == TG_LIGHT_INFINITEWITHFALLOFF)) {
							sun[0] = leaf->lightColor[i][0];
							sun[1] = leaf->lightColor[i][1];
							sun[2] = leaf->lightColor[i][2];
							sunFound = true;
						}
					}
					MC2_DIAGF("[BAKEORD v1] ri=%d ord=%llu frame=%u "
					          "postInvalidate=%d sunActive=%d nl=%d "
					          "amb=%.4f,%.4f,%.4f sun=%.4f,%.4f,%.4f\n",
					          static_cast<int>(recipeIndex), ord,
					          static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed)),
					          s_lightBakeRebakeDone ? 1 : 0,
					          sunActive, static_cast<int>(leaf->numLights_),
					          amb[0], amb[1], amb[2],
					          sun[0], sun[1], sun[2]);
				}
			}
			// Slot write skipped (light feed inactive): keep the per-frame dynamic index CacheGpuLightData just produced.
			if (!mc2IsBakedStaticLightPresent(recipeIndex)) return;
			shape->EmitBakedGpuLightData(recipeIndex, *leaf);
		}
	}
}

// CONE-SPILL-COUPLING-1 (2026-07-25). User, third framing of the same
// complaint: "the actual 'light triangles' need to be more transparent, and
// where they intersect the ground needs to be center of light spill on the
// ground."
// LIGHT-CONE-MODELS-1 already moved the pool off the tower base to the beam's
// far end, but it computed that point as `apex + aim*len` and then SNAPPED z
// to getTerrainElevation — a vertical DROP, not an intersection. For a tower
// searchlight raked across rising ground the beam meets terrain well BEFORE
// its far end, so the pool landed past (or beside) the bright patch the cone
// visibly makes: the "cone points one way, pool sits elsewhere" decoupling.
// This is the real thing: march the beam ray against the heightfield and
// return the FIRST crossing. f(t) = ray.z - terrainHeight(ray.xy) changes sign
// exactly at the surface, so a coarse march to bracket + bisection to refine
// lands on the true intersection for any terrain profile.
// Cost is bounded and independent of beam length: a FIXED step count (not a
// fixed step size), so a long beam takes coarser steps rather than more
// samples. ~34 heightfield lookups per fixture per frame worst case, and it
// returns early on the common near-hit.
// Returns false when the beam never meets ground within its own length —
// aimed at a wall, over a cliff edge, or off the map. Callers must then emit
// NO pool at all; planting one anyway is the phantom-pool artefact.
bool mc2ConeGroundHit (const glm::vec3& apex,
                              const glm::vec3& aim,
                              float beamLen,
                              glm::vec3& hitOut)
{
	if (!land || !(beamLen > 0.0f))
		return false;

	// Fixed step COUNT keeps the per-fixture cost flat no matter how long the
	// authored beam is. 24 coarse steps over a beam is ~1/2 terrain cell at
	// the stock cone lengths seen on tower lamps.
	const int   kCoarseSteps = 24;
	const int   kRefineIters = 10;
	const float dt = beamLen / static_cast<float>(kCoarseSteps);

	glm::vec3 prev = apex;
	// Height is only meaningful on-map; an apex already outside the playfield
	// has nothing to intersect.
	bool prevInside = Terrain::IsGameSelectTerrainPosition(prev);
	float prevF = prevInside ? (prev.z - land->getTerrainElevation(prev)) : 1.0f;

	for (int i = 1; i <= kCoarseSteps; ++i)
	{
		const float t = dt * static_cast<float>(i);
		glm::vec3 cur;
		cur.x = apex.x + aim.x * t;
		cur.y = apex.y + aim.y * t;
		cur.z = apex.z + aim.z * t;

		// Off-map: the beam has left the heightfield. Stop rather than
		// extrapolate — sampling outside the map is what produced pools on
		// ground the fixture cannot possibly light.
		if (!Terrain::IsGameSelectTerrainPosition(cur))
			return false;

		const float curF = cur.z - land->getTerrainElevation(cur);

		// Sign change (or exact touch) brackets the surface between prev/cur.
		if (prevInside && prevF > 0.0f && curF <= 0.0f)
		{
			// Bisect the bracket. f is continuous over the bilinear
			// heightfield, so this converges on the crossing.
			glm::vec3 lo = prev, hi = cur;
			for (int r = 0; r < kRefineIters; ++r)
			{
				glm::vec3 mid;
				mid.x = (lo.x + hi.x) * 0.5f;
				mid.y = (lo.y + hi.y) * 0.5f;
				mid.z = (lo.z + hi.z) * 0.5f;
				if ((mid.z - land->getTerrainElevation(mid)) > 0.0f)
					lo = mid;
				else
					hi = mid;
			}
			hitOut.x = (lo.x + hi.x) * 0.5f;
			hitOut.y = (lo.y + hi.y) * 0.5f;
			hitOut.z = land->getTerrainElevation(hitOut);
			return true;
		}

		prev = cur;
		prevF = curF;
		prevInside = true;
	}

	// Beam ran its full authored length without reaching the surface.
	return false;
}

//---------------------------------------------------------------------------
// [LIGHT_CONTRACT v1] terrain light I that each light-data row was gathered with. The retired
// shader read the baked row, so a recipe row carries the first baker's I; the contract must
// reproduce that exactly (per-actor slope-following I would be a look change). I is recovered
// from the freshly gathered row against the S/A in force at gather time.
static std::mutex s_slotIMutex;
static std::vector<float> s_slotI;
static std::vector<unsigned> s_slotEpoch;      // light-environment epoch each row was gathered in
static std::vector<unsigned> s_slotBakeFrame;  // frame counter at gather

// Light-environment epoch: bumps whenever the published sun/ambient (S,A) differs from the previous query.
// A row gathered in an older epoch is STALE (predates the last light change), not a contract mismatch.
static unsigned lightEnvEpoch(const int S[3], const int A[3])
{
	static std::mutex m; static int last[6]; static bool have = false; static unsigned epoch = 1;
	std::lock_guard<std::mutex> g(m);
	const int now[6] = {S[0], S[1], S[2], A[0], A[1], A[2]};
	if (!have) { std::copy_n(now, 6, last); have = true; }
	else if (!std::equal(now, now + 6, last)) { std::copy_n(now, 6, last); ++epoch; }
	return epoch;
}

void mc2RecordSlotIFwd(TG_MultiShape* shape)
{
	if (!shape || !eye || !s_lastBakeFresh) return;
	const uint32_t idx = shape->getCachedGpuLightIndex();
	if (idx == 0xFFFFFFFFu || idx > (1u << 22)) return;
	const TG_HWLightsData* leaf = shape->peekCachedLeafLightData();
	if (!leaf) return;
	int found = -1;
	for (int i = 0; i < leaf->numLights_ && i < kGpuObjectLightsMax; ++i) {
		const int type = static_cast<int>(leaf->lightDir[i][3]);
		if (type == 1 || type == 2) { found = i; break; }
	}
	float I = -1.0f;
	int S[3], A[3], F; float D[3];
	eye->getPropLightContract(S, A, F, D);
	const unsigned epoch = lightEnvEpoch(S, A);
	if (found >= 0) {
		float lo = -1e9f, hi = 1e9f;
		for (int k = 0; k < 3; ++k) {
			if (S[k] <= 0) continue;
			const int c = static_cast<int>(leaf->lightColor[found][k] * 255.0f + 0.5f);
			lo = std::max(lo, static_cast<float>(c - A[k]) / S[k]);
			// A saturated channel (255) only bounds I from below; the row stores the clamped value.
			if (c < 255) hi = std::min(hi, static_cast<float>(c + 1 - A[k]) / S[k]);
		}
		if (lo > -1e8f) I = std::clamp(hi < 1e8f ? 0.5f * (lo + hi) : lo, 0.0f, 1.0f);
	}
	std::lock_guard<std::mutex> g(s_slotIMutex);
	if (s_slotI.size() <= idx) { s_slotI.resize(idx + 1, -1.0f); s_slotEpoch.resize(idx + 1, 0u); s_slotBakeFrame.resize(idx + 1, 0u); }
	s_slotI[idx] = I;
	s_slotEpoch[idx] = epoch;
	s_slotBakeFrame[idx] = static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed));
}

// Mission unload (mc2ClearAllBakedStaticLight): light indices and recipe rows restart, so drop the I/epoch
// side tables and re-arm the one-shot rebake-on-ready, otherwise a mission loaded from a save keeps rows
// baked before its night lighting was applied (the row-stale case the parity check reports as stale_rows).
void mc2OnMissionLightBakeReset()
{
	{
		std::lock_guard<std::mutex> g(s_slotIMutex);
		s_slotI.clear(); s_slotEpoch.clear(); s_slotBakeFrame.clear();
	}
	s_lightBakeRebakeDone = false;
	s_lightBakeReadyFrame = 0;
}

static std::atomic<unsigned> s_slotIHit{0}, s_slotIFallback{0};
float mc2PropSlotI(uint32_t lightIndex, float fallbackI)
{
	std::lock_guard<std::mutex> g(s_slotIMutex);
	if (lightIndex < s_slotI.size() && s_slotI[lightIndex] >= 0.0f) { ++s_slotIHit; return s_slotI[lightIndex]; }
	++s_slotIFallback;
	return fallbackI;
}

//---------------------------------------------------------------------------
// [LIGHT_CONTRACT v1] parity counters: the row's first infinite light colour (what the
// retired shader row read returned) against contractSun() for the actor.
static std::atomic<unsigned> s_contractChecks{0}, s_contractMismatch{0};
// Shared (HIT) rows reuse the first baker's terrain light I: informational delta, ceiling 8 levels.
static std::atomic<unsigned> s_sharedRows{0}, s_sharedMaxDelta{0}, s_offByOne{0};
// Stale rows (gathered before the last S/A change): informational, converge after the rebake.
static std::atomic<unsigned> s_staleRows{0}, s_staleMaxDelta{0}, s_staleLastFrame{0};

void mc2PropLightContractCheck(TG_MultiShape* shape, float terrainLight, uint32_t policy)
{
	if (!shape || !eye) return;
	const TG_HWLightsData* leaf = shape->peekCachedLeafLightData();
	if (!leaf) return;
	int S[3], A[3], F; float D[3];
	eye->getPropLightContract(S, A, F, D);
	const mc2::prop_light::SunRgb want = mc2::prop_light::contractSun(S, A, F, terrainLight, policy);
	const unsigned epoch = lightEnvEpoch(S, A);
	unsigned rowEpoch = 0, rowFrame = 0;
	{
		const uint32_t idx = shape->getCachedGpuLightIndex();
		std::lock_guard<std::mutex> g(s_slotIMutex);
		if (idx < s_slotEpoch.size()) { rowEpoch = s_slotEpoch[idx]; rowFrame = s_slotBakeFrame[idx]; }
	}
	int found = -1;
	for (int i = 0; i < leaf->numLights_ && i < kGpuObjectLightsMax; ++i) {
		const int type = static_cast<int>(leaf->lightDir[i][3]);
		if (type == 1 || type == 2) { found = i; break; }
	}
	const bool fresh = s_lastBakeFresh;
	s_lastBakeFresh = true;   // consume: direct (non-gate) bakes are fresh
	if (!fresh) ++s_sharedRows;   // informational: row reused from the recipe bake
	bool ok;
	if (!want.sunOn) {
		ok = (found < 0);
	} else if (found < 0) {
		ok = false;
	} else {
		unsigned d = 0;
		for (int k = 0; k < 3; ++k) {
			const int dk = static_cast<int>(leaf->lightColor[found][k] * 255.0f + 0.5f) - want.c[k];
			const unsigned ad = static_cast<unsigned>(dk < 0 ? -dk : dk);
			if (ad > d) d = ad;
		}
		ok = (d == 0);
		if ((rowEpoch != 0 && rowEpoch < epoch) || (policy & mc2::prop_light::kLatchStale)) {
			// Row predates the last light change: informational stale row, not a parity failure.
			const unsigned n = ++s_staleRows;
			unsigned cur = s_staleMaxDelta.load();
			while (d > cur && !s_staleMaxDelta.compare_exchange_weak(cur, d)) {}
			s_staleLastFrame = static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed));
			if (n <= 6 && d > 0 && mc2_diag::tagRequested("LIGHT_CONTRACT"))
				std::fprintf(stderr, "[LIGHT_CONTRACT v1] stale#%u frame=%u bake_frame=%u row_epoch=%u epoch=%u delta=%u\n",
				             n, s_staleLastFrame.load(), rowFrame, rowEpoch, epoch, d);
			return;
		}
		if (d > 0) ++s_offByOne;
		unsigned cur = s_sharedMaxDelta.load();
		while (d > cur && !s_sharedMaxDelta.compare_exchange_weak(cur, d)) {}
	}
	++s_contractChecks;
	if (!ok) {
		const unsigned n = ++s_contractMismatch;
		if (n <= 12 && mc2_diag::tagRequested("LIGHT_CONTRACT")) {
			const float* c = found >= 0 ? leaf->lightColor[found] : nullptr;
			std::fprintf(stderr, "[LIGHT_CONTRACT v1] mismatch#%u diag idx=%u slots=%zu row_epoch=%u epoch=%u fresh=%d frame=%u\n",
			             n, shape->getCachedGpuLightIndex(), s_slotEpoch.size(), rowEpoch, epoch, fresh ? 1 : 0,
			             static_cast<unsigned>(g_mc2FrameCounter.load(std::memory_order_relaxed)));
			std::fprintf(stderr, "[LIGHT_CONTRACT v1] mismatch#%u policy=%u I=%.4f want=%d,%d,%d on=%d S=%d,%d,%d A=%d,%d,%d F=%d row=%s%.1f,%.1f,%.1f\n",
			             n, policy, terrainLight, want.c[0], want.c[1], want.c[2], want.sunOn ? 1 : 0,
			             S[0], S[1], S[2], A[0], A[1], A[2], F, found >= 0 ? "" : "none ",
			             c ? c[0] * 255.0f : 0.0f, c ? c[1] * 255.0f : 0.0f, c ? c[2] * 255.0f : 0.0f);
		}
	}
}

void mc2PropLightContractReport(bool final)
{
	if (!mc2_diag::tagRequested("LIGHT_CONTRACT")) return;
	extern unsigned mc2CameraLightChangedCount();
	std::fprintf(stderr, "[LIGHT_CONTRACT v1] checks=%u mismatches=%u camera_light_changed=%u shared_row_delta=%u max_delta=%u inexact_rows=%u slotI_hit=%u slotI_fallback=%u stale_rows=%u stale_max=%u stale_last_frame=%u%s\n",
	          s_contractChecks.load(), s_contractMismatch.load(), mc2CameraLightChangedCount(),
	          s_sharedRows.load(), s_sharedMaxDelta.load(), s_offByOne.load(), s_slotIHit.load(), s_slotIFallback.load(),
	          s_staleRows.load(), s_staleMaxDelta.load(), s_staleLastFrame.load(), final ? " final=1" : "");
	fflush(stderr);
}
